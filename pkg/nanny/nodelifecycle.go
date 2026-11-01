package nanny

import (
	"context"
	"slices"
	"time"

	corev1 "k8s.io/api/core/v1"
	apiequality "k8s.io/apimachinery/pkg/api/equality"
	apierrors "k8s.io/apimachinery/pkg/api/errors"
	metav1 "k8s.io/apimachinery/pkg/apis/meta/v1"
	"k8s.io/apimachinery/pkg/labels"
	"k8s.io/client-go/informers"
	"k8s.io/client-go/kubernetes"
	coordinationlisters "k8s.io/client-go/listers/coordination/v1"
	corelisters "k8s.io/client-go/listers/core/v1"
	"k8s.io/client-go/tools/cache"
	"k8s.io/klog/v2"
)

const nodeGracePeriod = 40 * time.Second

// NodeLifecycle watches node heartbeats, keeps the condition taints in
// step with the conditions, and evicts pods that do not tolerate a
// NoExecute taint, or tolerate it only for a while.
type NodeLifecycle struct {
	client kubernetes.Interface
	nodes  corelisters.NodeLister
	leases coordinationlisters.LeaseLister
	pods   corelisters.PodLister
	loop   *loop
	evict  *loop
	podsOn cache.Indexer
}

func NewNodeLifecycle(client kubernetes.Interface, factory informers.SharedInformerFactory) *NodeLifecycle {
	podInformer := factory.Core().V1().Pods().Informer()
	_ = podInformer.AddIndexers(cache.Indexers{"node": func(obj any) ([]string, error) {
		return []string{obj.(*corev1.Pod).Spec.NodeName}, nil
	}})
	n := &NodeLifecycle{
		client: client,
		nodes:  factory.Core().V1().Nodes().Lister(),
		leases: factory.Coordination().V1().Leases().Lister(),
		pods:   factory.Core().V1().Pods().Lister(),
		podsOn: podInformer.GetIndexer(),
	}
	n.loop = newLoop("nodelifecycle", 4, n.sync)
	n.evict = newLoop("taint-eviction", 8, n.syncPod)
	_, _ = factory.Core().V1().Nodes().Informer().AddEventHandler(cache.ResourceEventHandlerFuncs{
		AddFunc: n.loop.addObject,
		UpdateFunc: func(old, obj any) {
			n.loop.addObject(obj)
			if !apiequality.Semantic.DeepEqual(old.(*corev1.Node).Spec.Taints, obj.(*corev1.Node).Spec.Taints) {
				n.wakePods(obj.(*corev1.Node).Name)
			}
		},
	})
	_, _ = podInformer.AddEventHandler(cache.ResourceEventHandlerFuncs{
		AddFunc:    n.evict.addObject,
		UpdateFunc: func(_, obj any) { n.evict.addObject(obj) },
	})
	return n
}

func (n *NodeLifecycle) Run(ctx context.Context) {
	go n.evict.run(ctx)
	go func() {
		t := time.NewTicker(5 * time.Second)
		defer t.Stop()
		for {
			select {
			case <-ctx.Done():
				return
			case <-t.C:
				nodes, _ := n.nodes.List(labels.Everything())
				for _, nd := range nodes {
					n.loop.add(nd.Name)
				}
			}
		}
	}()
	n.loop.run(ctx)
}

func (n *NodeLifecycle) wakePods(node string) {
	pods, _ := n.podsOn.ByIndex("node", node)
	for _, p := range pods {
		n.evict.addObject(p)
	}
}

func condition(node *corev1.Node, t corev1.NodeConditionType) *corev1.NodeCondition {
	for i := range node.Status.Conditions {
		if node.Status.Conditions[i].Type == t {
			return &node.Status.Conditions[i]
		}
	}
	return nil
}

func (n *NodeLifecycle) sync(ctx context.Context, name string) error {
	node, err := n.nodes.Get(name)
	if apierrors.IsNotFound(err) {
		return nil
	}
	if err != nil {
		return err
	}
	// A heartbeat is the newer of the lease and the Ready condition.
	beat := time.Time{}
	if ready := condition(node, corev1.NodeReady); ready != nil {
		beat = ready.LastHeartbeatTime.Time
	}
	if lease, err := n.leases.Leases(corev1.NamespaceNodeLease).Get(name); err == nil && lease.Spec.RenewTime != nil && lease.Spec.RenewTime.After(beat) {
		beat = lease.Spec.RenewTime.Time
	}
	if beat.IsZero() {
		beat = node.CreationTimestamp.Time
	}
	if time.Since(beat) > nodeGracePeriod {
		if err := n.markUnknown(ctx, node); err != nil {
			return err
		}
		node, _ = n.nodes.Get(name)
		if node == nil {
			return nil
		}
	}
	return n.syncTaints(ctx, node)
}

// markUnknown says what upstream says of a silent kubelet.
func (n *NodeLifecycle) markUnknown(ctx context.Context, node *corev1.Node) error {
	node = node.DeepCopy()
	now := metav1.Now()
	changed := false
	for _, t := range []corev1.NodeConditionType{corev1.NodeReady, corev1.NodeMemoryPressure, corev1.NodeDiskPressure, corev1.NodePIDPressure} {
		c := condition(node, t)
		if c == nil {
			node.Status.Conditions = append(node.Status.Conditions, corev1.NodeCondition{
				Type: t, Status: corev1.ConditionUnknown, Reason: "NodeStatusNeverUpdated",
				Message: "Kubelet never posted node status.", LastHeartbeatTime: node.CreationTimestamp, LastTransitionTime: now,
			})
			changed = true
			continue
		}
		if c.Status != corev1.ConditionUnknown {
			c.Status, c.Reason, c.Message, c.LastTransitionTime = corev1.ConditionUnknown, "NodeStatusUnknown", "Kubelet stopped posting node status.", now
			changed = true
		}
	}
	if !changed {
		return nil
	}
	if _, err := n.client.CoreV1().Nodes().UpdateStatus(ctx, node, metav1.UpdateOptions{}); err != nil {
		return err
	}
	// Its pods serve nothing now.
	pods, _ := n.podsOn.ByIndex("node", node.Name)
	for _, obj := range pods {
		pod := obj.(*corev1.Pod).DeepCopy()
		for i, c := range pod.Status.Conditions {
			if c.Type == corev1.PodReady && c.Status != corev1.ConditionFalse {
				pod.Status.Conditions[i].Status, pod.Status.Conditions[i].LastTransitionTime = corev1.ConditionFalse, now
				_, _ = n.client.CoreV1().Pods(pod.Namespace).UpdateStatus(ctx, pod, metav1.UpdateOptions{})
			}
		}
	}
	return nil
}

// syncTaints derives the condition taints from the conditions.
func (n *NodeLifecycle) syncTaints(ctx context.Context, node *corev1.Node) error {
	want := map[string]corev1.TaintEffect{}
	add := func(key string, effects ...corev1.TaintEffect) {
		for _, e := range effects {
			want[key+"/"+string(e)] = e
		}
	}
	if ready := condition(node, corev1.NodeReady); ready != nil {
		switch ready.Status {
		case corev1.ConditionFalse:
			add(corev1.TaintNodeNotReady, corev1.TaintEffectNoSchedule, corev1.TaintEffectNoExecute)
		case corev1.ConditionUnknown:
			add(corev1.TaintNodeUnreachable, corev1.TaintEffectNoSchedule, corev1.TaintEffectNoExecute)
		}
	} else {
		add(corev1.TaintNodeNotReady, corev1.TaintEffectNoSchedule)
	}
	for t, key := range map[corev1.NodeConditionType]string{
		corev1.NodeMemoryPressure: corev1.TaintNodeMemoryPressure, corev1.NodeDiskPressure: corev1.TaintNodeDiskPressure,
		corev1.NodePIDPressure: corev1.TaintNodePIDPressure, corev1.NodeNetworkUnavailable: corev1.TaintNodeNetworkUnavailable,
	} {
		if c := condition(node, t); c != nil && c.Status == corev1.ConditionTrue {
			add(key, corev1.TaintEffectNoSchedule)
		}
	}
	if node.Spec.Unschedulable {
		add(corev1.TaintNodeUnschedulable, corev1.TaintEffectNoSchedule)
	}
	managed := []string{corev1.TaintNodeNotReady, corev1.TaintNodeUnreachable, corev1.TaintNodeMemoryPressure,
		corev1.TaintNodeDiskPressure, corev1.TaintNodePIDPressure, corev1.TaintNodeNetworkUnavailable, corev1.TaintNodeUnschedulable}
	var taints []corev1.Taint
	have := map[string]bool{}
	for _, t := range node.Spec.Taints {
		k := t.Key + "/" + string(t.Effect)
		if slices.Contains(managed, t.Key) && want[k] == "" {
			continue
		}
		have[k] = true
		taints = append(taints, t)
	}
	now := metav1.Now()
	for k, e := range want {
		if have[k] {
			continue
		}
		t := corev1.Taint{Key: k[:len(k)-len(e)-1], Effect: e}
		if e == corev1.TaintEffectNoExecute {
			t.TimeAdded = &now
		}
		taints = append(taints, t)
	}
	if apiequality.Semantic.DeepEqual(taints, node.Spec.Taints) || (len(taints) == 0 && len(node.Spec.Taints) == 0) {
		return nil
	}
	node = node.DeepCopy()
	node.Spec.Taints = taints
	_, err := n.client.CoreV1().Nodes().Update(ctx, node, metav1.UpdateOptions{})
	if apierrors.IsConflict(err) {
		return later(time.Second)
	}
	return err
}

// syncPod evicts a pod from a NoExecute taint it does not tolerate, at
// once or when its toleration runs out.
func (n *NodeLifecycle) syncPod(ctx context.Context, key string) error {
	ns, name, _ := cache.SplitMetaNamespaceKey(key)
	pod, err := n.pods.Pods(ns).Get(name)
	if err != nil || pod.Spec.NodeName == "" || pod.DeletionTimestamp != nil {
		return nil
	}
	node, err := n.nodes.Get(pod.Spec.NodeName)
	if err != nil {
		return nil
	}
	var wait time.Duration
	for i := range node.Spec.Taints {
		taint := &node.Spec.Taints[i]
		if taint.Effect != corev1.TaintEffectNoExecute {
			continue
		}
		var matching []corev1.Toleration
		for _, t := range pod.Spec.Tolerations {
			if t.ToleratesTaint(klog.Background(), taint, false) {
				matching = append(matching, t)
			}
		}
		if len(matching) == 0 {
			return n.evictPod(ctx, pod)
		}
		for _, t := range matching {
			if t.TolerationSeconds == nil {
				wait = -1
				break
			}
			added := node.CreationTimestamp.Time
			if taint.TimeAdded != nil {
				added = taint.TimeAdded.Time
			}
			left := time.Until(added.Add(time.Duration(*t.TolerationSeconds) * time.Second))
			if left <= 0 {
				return n.evictPod(ctx, pod)
			}
			if wait >= 0 && (wait == 0 || left < wait) {
				wait = left
			}
		}
	}
	if wait > 0 {
		return later(wait)
	}
	return nil
}

func (n *NodeLifecycle) evictPod(ctx context.Context, pod *corev1.Pod) error {
	err := n.client.CoreV1().Pods(pod.Namespace).Delete(ctx, pod.Name, metav1.DeleteOptions{Preconditions: &metav1.Preconditions{UID: &pod.UID}})
	if apierrors.IsNotFound(err) || apierrors.IsConflict(err) {
		return nil
	}
	return err
}

// PodGC force-deletes pods nothing will ever finish: pods bound to nodes
// that are gone, and unscheduled pods already being deleted.
type PodGC struct {
	client kubernetes.Interface
	pods   corelisters.PodLister
	nodes  corelisters.NodeLister
}

func NewPodGC(client kubernetes.Interface, factory informers.SharedInformerFactory) *PodGC {
	return &PodGC{client: client, pods: factory.Core().V1().Pods().Lister(), nodes: factory.Core().V1().Nodes().Lister()}
}

func (g *PodGC) Run(ctx context.Context) {
	suspects := map[string]time.Time{}
	t := time.NewTicker(10 * time.Second)
	defer t.Stop()
	for {
		select {
		case <-ctx.Done():
			return
		case <-t.C:
		}
		pods, _ := g.pods.List(labels.Everything())
		now := time.Now()
		for _, pod := range pods {
			key := pod.Namespace + "/" + pod.Name
			orphan := false
			if pod.Spec.NodeName != "" {
				if _, err := g.nodes.Get(pod.Spec.NodeName); apierrors.IsNotFound(err) {
					// A node gone from the cache for one tick may be a cache
					// that lags; ask the API before forgetting its pods.
					if first, ok := suspects[key]; !ok {
						suspects[key] = now
					} else if now.Sub(first) > 20*time.Second {
						_, err := g.client.CoreV1().Nodes().Get(ctx, pod.Spec.NodeName, metav1.GetOptions{})
						orphan = apierrors.IsNotFound(err)
					}
				}
			} else if pod.DeletionTimestamp != nil {
				orphan = true
			}
			if !orphan {
				continue
			}
			delete(suspects, key)
			zero := int64(0)
			_ = g.client.CoreV1().Pods(pod.Namespace).Delete(ctx, pod.Name, metav1.DeleteOptions{
				GracePeriodSeconds: &zero, Preconditions: &metav1.Preconditions{UID: &pod.UID},
			})
		}
	}
}
