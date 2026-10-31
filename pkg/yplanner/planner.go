// Package yplanner is the scheduler of Yandexnetes: it finds a node for
// every pod nobody placed yet and binds the pod there.
package yplanner

import (
	"context"
	"fmt"
	"log/slog"
	"maps"
	"math/rand/v2"
	"sort"
	"strings"
	"sync"
	"time"

	corev1 "k8s.io/api/core/v1"
	"k8s.io/apimachinery/pkg/api/equality"
	apierrors "k8s.io/apimachinery/pkg/api/errors"
	metav1 "k8s.io/apimachinery/pkg/apis/meta/v1"
	"k8s.io/apimachinery/pkg/labels"
	"k8s.io/apimachinery/pkg/types"
	"k8s.io/client-go/informers"
	"k8s.io/client-go/kubernetes"
	"k8s.io/client-go/kubernetes/scheme"
	typedcorev1 "k8s.io/client-go/kubernetes/typed/core/v1"
	corelisters "k8s.io/client-go/listers/core/v1"
	"k8s.io/client-go/tools/cache"
	"k8s.io/client-go/tools/record"
	"k8s.io/client-go/util/workqueue"
	resourcehelper "k8s.io/component-helpers/resource"
	corev1helpers "k8s.io/component-helpers/scheduling/corev1"
	"k8s.io/component-helpers/scheduling/corev1/nodeaffinity"
	"k8s.io/klog/v2"
)

// Planner places pods whose schedulerName is its own.
type Planner struct {
	client   kubernetes.Interface
	name     string
	pods     corelisters.PodLister
	nodes    corelisters.NodeLister
	queue    workqueue.TypedRateLimitingInterface[string]
	recorder record.EventRecorder

	// Pods bound but not yet seen bound in the cache: their room is taken.
	mu      sync.Mutex
	assumed map[types.UID]assumedPod
	// Pods that fit nowhere wait for the cluster to change.
	waiting map[string]struct{}
	// Where the next pod starts looking for nodes.
	nextStart int
	// Bindings in flight, at most as many as it holds.
	binding chan struct{}
	cache   *clusterCache
}

type assumedPod struct {
	pod  *corev1.Pod
	node string
	at   time.Time
}

func New(client kubernetes.Interface, factory informers.SharedInformerFactory, name string) *Planner {
	p := &Planner{
		client: client,
		name:   name,
		pods:   factory.Core().V1().Pods().Lister(),
		nodes:  factory.Core().V1().Nodes().Lister(),
		queue: workqueue.NewTypedRateLimitingQueueWithConfig(
			workqueue.NewTypedItemExponentialFailureRateLimiter[string](100*time.Millisecond, 10*time.Second),
			workqueue.TypedRateLimitingQueueConfig[string]{Name: "yplanner"},
		),
		assumed: map[types.UID]assumedPod{},
		waiting: map[string]struct{}{},
		binding: make(chan struct{}, 64),
		cache:   newClusterCache(),
	}
	broadcaster := record.NewBroadcaster()
	broadcaster.StartRecordingToSink(&typedcorev1.EventSinkImpl{Interface: client.CoreV1().Events("")})
	p.recorder = broadcaster.NewRecorder(scheme.Scheme, corev1.EventSource{Component: name})

	_, _ = factory.Core().V1().Pods().Informer().AddEventHandler(cache.ResourceEventHandlerFuncs{
		AddFunc: func(obj any) {
			p.cache.update(obj.(*corev1.Pod))
			p.podChanged(obj.(*corev1.Pod))
		},
		UpdateFunc: func(_, obj any) {
			p.cache.update(obj.(*corev1.Pod))
			p.podChanged(obj.(*corev1.Pod))
		},
		DeleteFunc: func(obj any) {
			if t, ok := obj.(cache.DeletedFinalStateUnknown); ok {
				obj = t.Obj
			}
			if pod, ok := obj.(*corev1.Pod); ok {
				p.cache.remove(pod.UID)
				p.forget(pod.UID)
				if pod.Spec.NodeName != "" {
					p.wakeWaiting()
				}
			}
		},
	})
	_, _ = factory.Core().V1().Nodes().Informer().AddEventHandler(cache.ResourceEventHandlerFuncs{
		AddFunc: func(obj any) {
			p.cache.upsertNode(obj.(*corev1.Node))
			p.wakeWaiting()
		},
		UpdateFunc: func(old, obj any) {
			p.cache.upsertNode(obj.(*corev1.Node))
			// Heartbeats change nothing a waiting pod cares about.
			if nodeSchedulingChanged(old.(*corev1.Node), obj.(*corev1.Node)) {
				p.wakeWaiting()
			}
		},
		DeleteFunc: func(obj any) {
			if t, ok := obj.(cache.DeletedFinalStateUnknown); ok {
				obj = t.Obj
			}
			if n, ok := obj.(*corev1.Node); ok {
				p.cache.deleteNode(n.Name)
			}
		},
	})
	return p
}

func (p *Planner) podChanged(pod *corev1.Pod) {
	if pod.Spec.NodeName != "" {
		p.forget(pod.UID)
		if pod.Status.Phase == corev1.PodSucceeded || pod.Status.Phase == corev1.PodFailed || pod.DeletionTimestamp != nil {
			p.wakeWaiting()
		}
		return
	}
	if pod.Spec.SchedulerName != p.name || pod.DeletionTimestamp != nil {
		return
	}
	key, _ := cache.MetaNamespaceKeyFunc(pod)
	p.queue.Add(key)
}

func (p *Planner) forget(uid types.UID) {
	p.mu.Lock()
	delete(p.assumed, uid)
	p.mu.Unlock()
}

// wakeWaiting gives every pod that fit nowhere another try.
func (p *Planner) wakeWaiting() {
	p.mu.Lock()
	keys := make([]string, 0, len(p.waiting))
	for k := range p.waiting {
		keys = append(keys, k)
	}
	clear(p.waiting)
	p.mu.Unlock()
	for _, k := range keys {
		p.queue.Add(k)
	}
}

// Run places pods one at a time: two workers would each count the same
// free room.
func (p *Planner) Run(ctx context.Context) {
	defer p.queue.ShutDown()
	slog.Info("yplanner started", "scheduler", p.name)
	go func() {
		for {
			key, quit := p.queue.Get()
			if quit {
				return
			}
			err := p.schedule(ctx, key)
			if err != nil {
				slog.Warn("scheduling failed", "pod", key, "err", err)
				p.queue.AddRateLimited(key)
			} else {
				p.queue.Forget(key)
			}
			p.queue.Done(key)
		}
	}()
	// Assumed pods the informer never showed bound are let go after a while.
	go func() {
		t := time.NewTicker(30 * time.Second)
		defer t.Stop()
		for {
			select {
			case <-ctx.Done():
				return
			case <-t.C:
				p.mu.Lock()
				for uid, a := range p.assumed {
					if time.Since(a.at) > 30*time.Second {
						delete(p.assumed, uid)
						if live, err := p.pods.Pods(a.pod.Namespace).Get(a.pod.Name); err != nil || live.UID != uid || live.Spec.NodeName == "" {
							p.cache.remove(uid)
						}
					}
				}
				p.mu.Unlock()
			}
		}
	}()
	<-ctx.Done()
}

func (p *Planner) schedule(ctx context.Context, key string) error {
	ns, name, _ := cache.SplitMetaNamespaceKey(key)
	pod, err := p.pods.Pods(ns).Get(name)
	if apierrors.IsNotFound(err) {
		return nil
	}
	if err != nil {
		return err
	}
	if pod.Spec.NodeName != "" || pod.DeletionTimestamp != nil || pod.Spec.SchedulerName != p.name {
		return nil
	}
	// Placed already, its binding still on the way.
	p.mu.Lock()
	_, placed := p.assumed[pod.UID]
	p.mu.Unlock()
	if placed {
		return nil
	}
	p.cache.mu.Lock()
	state, names := p.cache.nodes, p.cache.names
	// Room promised to pods that preempted for it is taken, unless they
	// matter less than this one: without that, the pods just evicted come
	// back in their place before the preemptor does.
	cloned := false
	if pending, err := p.pods.List(labels.Everything()); err == nil {
		for _, other := range pending {
			if other.UID == pod.UID || other.Spec.NodeName != "" || other.Status.NominatedNodeName == "" ||
				corev1helpers.PodPriority(other) < corev1helpers.PodPriority(pod) {
				continue
			}
			if s := state[other.Status.NominatedNodeName]; s != nil {
				if !cloned {
					state, cloned = maps.Clone(state), true
				}
				promised := s.without(nil)
				promised.add(other)
				state[other.Status.NominatedNodeName] = promised
			}
		}
	}
	fits, reasons := p.filter(pod, state, names)
	if len(fits) == 0 {
		p.cache.mu.Unlock()
		p.unschedulable(ctx, pod, len(names), reasons)
		p.cache.mu.Lock()
		p.preempt(ctx, pod, state)
		p.cache.mu.Unlock()
		p.mu.Lock()
		p.waiting[key] = struct{}{}
		p.mu.Unlock()
		return nil
	}
	node := p.pick(pod, fits)
	binding := &corev1.Binding{
		ObjectMeta: metav1.ObjectMeta{Namespace: pod.Namespace, Name: pod.Name, UID: pod.UID},
		Target:     corev1.ObjectReference{Kind: "Node", Name: node.node.Name},
	}
	// The pod counts as placed from now on.
	p.cache.placeLocked(pod, node.node.Name)
	p.cache.mu.Unlock()
	p.mu.Lock()
	p.assumed[pod.UID] = assumedPod{pod: pod, node: node.node.Name, at: time.Now()}
	p.mu.Unlock()
	// The pod counts as placed from now on; the binding goes out on its own,
	// as upstream's binding cycle does, so the next pod need not wait for it.
	p.binding <- struct{}{}
	go func() {
		defer func() { <-p.binding }()
		if err := p.client.CoreV1().Pods(pod.Namespace).Bind(ctx, binding, metav1.CreateOptions{}); err != nil {
			p.forget(pod.UID)
			p.cache.remove(pod.UID)
			if !apierrors.IsNotFound(err) && !apierrors.IsConflict(err) {
				slog.Warn("binding failed", "pod", key, "err", err)
				p.queue.AddRateLimited(key)
			}
			return
		}
		p.recorder.Eventf(pod, corev1.EventTypeNormal, "Scheduled", "Successfully assigned %s/%s to %s", pod.Namespace, pod.Name, node.node.Name)
	}()
	return nil
}

// nodeSchedulingChanged says whether a node update may let a waiting pod
// in: anything but heartbeats.
func nodeSchedulingChanged(old, cur *corev1.Node) bool {
	if old.Spec.Unschedulable != cur.Spec.Unschedulable || !equality.Semantic.DeepEqual(old.Spec.Taints, cur.Spec.Taints) ||
		!equality.Semantic.DeepEqual(old.Labels, cur.Labels) || !equality.Semantic.DeepEqual(old.Status.Allocatable, cur.Status.Allocatable) {
		return true
	}
	ready := func(n *corev1.Node) corev1.ConditionStatus {
		for _, c := range n.Status.Conditions {
			if c.Type == corev1.NodeReady {
				return c.Status
			}
		}
		return ""
	}
	return ready(old) != ready(cur)
}

// unschedulable records why, as upstream does: a condition and an event.
func (p *Planner) unschedulable(ctx context.Context, pod *corev1.Pod, total int, reasons map[string]int) {
	var parts []string
	for r, n := range reasons {
		parts = append(parts, fmt.Sprintf("%d %s", n, r))
	}
	sort.Strings(parts)
	msg := fmt.Sprintf("0/%d nodes are available", total)
	if len(parts) > 0 {
		msg += ": " + strings.Join(parts, ", ")
	}
	msg += "."
	p.recorder.Event(pod, corev1.EventTypeWarning, "FailedScheduling", msg)
	for _, c := range pod.Status.Conditions {
		if c.Type == corev1.PodScheduled && c.Status == corev1.ConditionFalse && c.Message == msg {
			return
		}
	}
	pod = pod.DeepCopy()
	cond := corev1.PodCondition{
		Type: corev1.PodScheduled, Status: corev1.ConditionFalse, Reason: corev1.PodReasonUnschedulable,
		Message: msg, LastTransitionTime: metav1.Now(),
	}
	replaced := false
	for i, c := range pod.Status.Conditions {
		if c.Type == corev1.PodScheduled {
			pod.Status.Conditions[i], replaced = cond, true
		}
	}
	if !replaced {
		pod.Status.Conditions = append(pod.Status.Conditions, cond)
	}
	if _, err := p.client.CoreV1().Pods(pod.Namespace).UpdateStatus(ctx, pod, metav1.UpdateOptions{}); err != nil && !apierrors.IsNotFound(err) && !apierrors.IsConflict(err) {
		slog.Warn("pod condition not written", "pod", pod.Namespace+"/"+pod.Name, "err", err)
	}
}

// nodeState is a node with what its pods already take.
type nodeState struct {
	node      *corev1.Node
	requested corev1.ResourceList
	podCount  int
	hostPorts map[string]int // pods using each host port
	pods      []*corev1.Pod
}

func portKey(ip string, proto corev1.Protocol, port int32) string {
	if ip == "" {
		ip = "0.0.0.0"
	}
	return fmt.Sprintf("%s/%s/%d", ip, proto, port)
}

// filter finds nodes the pod fits on. Like upstream's scheduler it stops
// once it has found enough of them on a big cluster, and each pod starts
// looking where the last one stopped, so all nodes get their turn.
func (p *Planner) filter(pod *corev1.Pod, state map[string]*nodeState, names []string) ([]*nodeState, map[string]int) {
	reasons := map[string]int{}
	required := nodeaffinity.GetRequiredNodeAffinity(pod)
	requests := resourcehelper.PodRequests(pod, resourcehelper.PodResourcesOptions{})
	idx := buildAffinityIndex(pod, state)
	enough := feasibleToFind(len(names))
	var fits []*nodeState
	start := 0
	if len(names) > 0 {
		start = p.nextStart % len(names)
	}
	examined := 0
	for i := range names {
		s := state[names[(start+i)%len(names)]]
		examined++
		if why := p.misfit(pod, s, required, requests, idx); why != "" {
			reasons[why]++
			continue
		}
		fits = append(fits, s)
		if len(fits) >= enough {
			break
		}
	}
	p.nextStart = start + examined
	return fits, reasons
}

// feasibleToFind is how many fitting nodes are enough, as upstream
// computes it: all of a small cluster, a shrinking share of a big one,
// never under a hundred nor under five percent.
func feasibleToFind(nodes int) int {
	if nodes < 100 {
		return nodes
	}
	percentage := max(50-nodes/125, 5)
	return max(nodes*percentage/100, 100)
}

func (p *Planner) misfit(pod *corev1.Pod, s *nodeState,
	required nodeaffinity.RequiredNodeAffinity, requests corev1.ResourceList, idx *affinityIndex) string {
	node := s.node
	tolerations := pod.Spec.Tolerations
	tolerated := func(key string, effect corev1.TaintEffect) bool {
		return corev1helpers.TolerationsTolerateTaint(klog.Background(), tolerations, &corev1.Taint{Key: key, Effect: effect}, false)
	}
	if node.Spec.Unschedulable && !tolerated(corev1.TaintNodeUnschedulable, corev1.TaintEffectNoSchedule) {
		return "node(s) were unschedulable"
	}
	ready := false
	for _, c := range node.Status.Conditions {
		if c.Type == corev1.NodeReady && c.Status == corev1.ConditionTrue {
			ready = true
		}
	}
	if !ready && !tolerated(corev1.TaintNodeNotReady, corev1.TaintEffectNoSchedule) {
		return "node(s) were not ready"
	}
	if taint, untolerated := corev1helpers.FindMatchingUntoleratedTaint(klog.Background(), node.Spec.Taints, tolerations, func(t *corev1.Taint) bool {
		return t.Effect == corev1.TaintEffectNoSchedule || t.Effect == corev1.TaintEffectNoExecute
	}, false); untolerated {
		return fmt.Sprintf("node(s) had untolerated taint {%s: %s}", taint.Key, taint.Value)
	}
	if match, _ := required.Match(node); !match {
		return "node(s) didn't match Pod's node affinity/selector"
	}
	allocatable := node.Status.Allocatable
	if maxPods, ok := allocatable[corev1.ResourcePods]; ok && int64(s.podCount+1) > maxPods.Value() {
		return "Too many pods"
	}
	for name, want := range requests {
		if want.IsZero() {
			continue
		}
		have, ok := allocatable[name]
		if !ok {
			return "Insufficient " + string(name)
		}
		used := s.requested[name]
		free := have.DeepCopy()
		free.Sub(used)
		if free.Cmp(want) < 0 {
			return "Insufficient " + string(name)
		}
	}
	for _, c := range pod.Spec.Containers {
		for _, port := range c.Ports {
			if port.HostPort <= 0 {
				continue
			}
			if s.hostPorts[portKey(port.HostIP, port.Protocol, port.HostPort)] > 0 ||
				s.hostPorts[portKey("0.0.0.0", port.Protocol, port.HostPort)] > 0 ||
				(port.HostIP == "" || port.HostIP == "0.0.0.0") && anyPortOn(s.hostPorts, port.Protocol, port.HostPort) {
				return "node(s) didn't have free ports for the requested pod ports"
			}
		}
	}
	if why := interPodAffinity(pod, s, idx); why != "" {
		return why
	}
	return ""
}

func anyPortOn(used map[string]int, proto corev1.Protocol, port int32) bool {
	suffix := fmt.Sprintf("/%s/%d", proto, port)
	for k := range used {
		if strings.HasSuffix(k, suffix) {
			return true
		}
	}
	return false
}

// pick scores the nodes that fit by how much room they would have left,
// cpu and memory alike, and takes the best; ties go to chance.
func (p *Planner) pick(pod *corev1.Pod, fits []*nodeState) *nodeState {
	requests := resourcehelper.PodRequests(pod, resourcehelper.PodResourcesOptions{})
	score := func(s *nodeState) float64 {
		total := 0.0
		for _, name := range []corev1.ResourceName{corev1.ResourceCPU, corev1.ResourceMemory} {
			have, ok := s.node.Status.Allocatable[name]
			if !ok || have.IsZero() {
				continue
			}
			used := s.requested[name].DeepCopy()
			want := requests[name]
			used.Add(want)
			total += 1 - float64(used.MilliValue())/float64(have.MilliValue())
		}
		return total
	}
	rand.Shuffle(len(fits), func(i, j int) { fits[i], fits[j] = fits[j], fits[i] })
	best := fits[0]
	bestScore := score(best)
	for _, s := range fits[1:] {
		if v := score(s); v > bestScore {
			best, bestScore = s, v
		}
	}
	return best
}
