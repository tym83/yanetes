package nanny

import (
	"context"
	"encoding/json"
	"fmt"
	"sort"
	"sync"
	"time"

	appsv1 "k8s.io/api/apps/v1"
	corev1 "k8s.io/api/core/v1"
	apiequality "k8s.io/apimachinery/pkg/api/equality"
	apierrors "k8s.io/apimachinery/pkg/api/errors"
	metav1 "k8s.io/apimachinery/pkg/apis/meta/v1"
	"k8s.io/apimachinery/pkg/labels"
	"k8s.io/apimachinery/pkg/types"
	"k8s.io/client-go/informers"
	"k8s.io/client-go/kubernetes"
	appslisters "k8s.io/client-go/listers/apps/v1"
	corelisters "k8s.io/client-go/listers/core/v1"
	"k8s.io/client-go/tools/cache"
	"k8s.io/utils/ptr"
)

// expectations counts pod creations and deletions a controller has asked
// for and not yet seen in its cache, so it does not ask twice.
type expectations struct {
	mu      sync.Mutex
	pending map[string]*expected
}

type expected struct {
	adds, dels int
	at         time.Time
}

func newExpectations() *expectations { return &expectations{pending: map[string]*expected{}} }

func (e *expectations) expect(key string, adds, dels int) {
	e.mu.Lock()
	defer e.mu.Unlock()
	e.pending[key] = &expected{adds: adds, dels: dels, at: time.Now()}
}

func (e *expectations) observe(key string, adds, dels int) {
	e.mu.Lock()
	defer e.mu.Unlock()
	if x, ok := e.pending[key]; ok {
		x.adds -= adds
		x.dels -= dels
	}
}

// satisfied: everything asked for was seen, or long enough ago to stop waiting.
func (e *expectations) satisfied(key string) bool {
	e.mu.Lock()
	defer e.mu.Unlock()
	x, ok := e.pending[key]
	return !ok || (x.adds <= 0 && x.dels <= 0) || time.Since(x.at) > 5*time.Minute
}

func (e *expectations) forget(key string) {
	e.mu.Lock()
	defer e.mu.Unlock()
	delete(e.pending, key)
}

// ReplicaSets keeps the number of pods a replica set asks for.
type ReplicaSets struct {
	client kubernetes.Interface
	sets   appslisters.ReplicaSetLister
	pods   corelisters.PodLister
	exp    *expectations
	loop   *loop
}

// burstLimit caps how many pods one sync creates at once; the batches grow
// from one, so a template that cannot run fails one pod, not a thousand.
const burstLimit = 500

func NewReplicaSets(client kubernetes.Interface, factory informers.SharedInformerFactory) *ReplicaSets {
	r := &ReplicaSets{
		client: client,
		sets:   factory.Apps().V1().ReplicaSets().Lister(),
		pods:   factory.Core().V1().Pods().Lister(),
		exp:    newExpectations(),
	}
	r.loop = newLoop("replicaset", 10, r.sync)
	_, _ = factory.Apps().V1().ReplicaSets().Informer().AddEventHandler(r.loop.handlers())
	_, _ = factory.Core().V1().Pods().Informer().AddEventHandler(cache.ResourceEventHandlerFuncs{
		AddFunc: func(obj any) {
			pod := obj.(*corev1.Pod)
			if key := r.ownerKey(pod); key != "" {
				r.exp.observe(key, 1, 0)
				r.loop.add(key)
				return
			}
			r.wakeMatching(pod)
		},
		UpdateFunc: func(old, obj any) {
			oldPod, pod := old.(*corev1.Pod), obj.(*corev1.Pod)
			if key := r.ownerKey(pod); key != "" {
				r.loop.add(key)
			}
			if key := r.ownerKey(oldPod); key != "" && key != r.ownerKey(pod) {
				r.loop.add(key)
			}
			if r.ownerKey(pod) == "" || !labels.Equals(oldPod.Labels, pod.Labels) {
				r.wakeMatching(pod)
			}
		},
		DeleteFunc: func(obj any) {
			pod, ok := unwrap(obj).(*corev1.Pod)
			if !ok {
				return
			}
			if key := r.ownerKey(pod); key != "" {
				r.exp.observe(key, 0, 1)
				r.loop.add(key)
			}
		},
	})
	return r
}

func (r *ReplicaSets) Run(ctx context.Context) { r.loop.run(ctx) }

func (r *ReplicaSets) ownerKey(pod *corev1.Pod) string {
	ref := metav1.GetControllerOf(pod)
	if ref == nil || ref.Kind != "ReplicaSet" {
		return ""
	}
	rs, err := r.sets.ReplicaSets(pod.Namespace).Get(ref.Name)
	if err != nil || rs.UID != ref.UID {
		return ""
	}
	return pod.Namespace + "/" + rs.Name
}

// wakeMatching queues the replica sets that could adopt an orphan.
func (r *ReplicaSets) wakeMatching(pod *corev1.Pod) {
	if metav1.GetControllerOf(pod) != nil {
		return
	}
	sets, _ := r.sets.ReplicaSets(pod.Namespace).List(labels.Everything())
	for _, rs := range sets {
		sel, err := metav1.LabelSelectorAsSelector(rs.Spec.Selector)
		if err == nil && !sel.Empty() && sel.Matches(labels.Set(pod.Labels)) {
			r.loop.add(pod.Namespace + "/" + rs.Name)
		}
	}
}

func active(pod *corev1.Pod) bool {
	return pod.DeletionTimestamp == nil && pod.Status.Phase != corev1.PodSucceeded && pod.Status.Phase != corev1.PodFailed
}

func podReady(pod *corev1.Pod) bool {
	for _, c := range pod.Status.Conditions {
		if c.Type == corev1.PodReady {
			return c.Status == corev1.ConditionTrue
		}
	}
	return false
}

// availableFor tells whether a pod has been ready for minReady seconds,
// and if not, how long until it will have been.
func availableFor(pod *corev1.Pod, minReady int32, now time.Time) (bool, time.Duration) {
	if !podReady(pod) {
		return false, 0
	}
	if minReady == 0 {
		return true, 0
	}
	for _, c := range pod.Status.Conditions {
		if c.Type == corev1.PodReady {
			at := c.LastTransitionTime.Add(time.Duration(minReady) * time.Second)
			if !at.After(now) {
				return true, 0
			}
			return false, at.Sub(now)
		}
	}
	return false, 0
}

// claim adopts orphans that match and lets go of pods that stopped matching.
func claimPods(ctx context.Context, client kubernetes.Interface, owner metav1.Object, kind schemaKind,
	selector labels.Selector, pods []*corev1.Pod) ([]*corev1.Pod, error) {
	var owned []*corev1.Pod
	for _, pod := range pods {
		ref := metav1.GetControllerOf(pod)
		matches := selector.Matches(labels.Set(pod.Labels))
		switch {
		case ref != nil && ref.UID == owner.GetUID():
			if matches || owner.GetDeletionTimestamp() != nil {
				owned = append(owned, pod)
				continue
			}
			patch := fmt.Sprintf(`{"metadata":{"ownerReferences":[{"$patch":"delete","uid":%q}],"uid":%q}}`, owner.GetUID(), pod.UID)
			if _, err := client.CoreV1().Pods(pod.Namespace).Patch(ctx, pod.Name, types.StrategicMergePatchType, []byte(patch), metav1.PatchOptions{}); err != nil && !apierrors.IsNotFound(err) {
				return nil, err
			}
		case ref == nil && matches && pod.DeletionTimestamp == nil && owner.GetDeletionTimestamp() == nil:
			body, _ := json.Marshal(map[string]any{"metadata": map[string]any{
				"ownerReferences": []metav1.OwnerReference{*metav1.NewControllerRef(owner, kind.gvk())},
				"uid":             pod.UID,
			}})
			if _, err := client.CoreV1().Pods(pod.Namespace).Patch(ctx, pod.Name, types.StrategicMergePatchType, body, metav1.PatchOptions{}); err != nil {
				if apierrors.IsNotFound(err) {
					continue
				}
				return nil, err
			}
			owned = append(owned, pod)
		}
	}
	return owned, nil
}

func (r *ReplicaSets) sync(ctx context.Context, key string) error {
	ns, name, _ := cache.SplitMetaNamespaceKey(key)
	rs, err := r.sets.ReplicaSets(ns).Get(name)
	if apierrors.IsNotFound(err) {
		r.exp.forget(key)
		return nil
	}
	if err != nil {
		return err
	}
	selector, err := metav1.LabelSelectorAsSelector(rs.Spec.Selector)
	if err != nil || selector.Empty() {
		return nil
	}
	all, err := r.pods.Pods(ns).List(labels.Everything())
	if err != nil {
		return err
	}
	owned, err := claimPods(ctx, r.client, rs, kindReplicaSet, selector, all)
	if err != nil {
		return err
	}
	var running []*corev1.Pod
	for _, p := range owned {
		if active(p) {
			running = append(running, p)
		}
	}
	var scaleErr error
	if rs.DeletionTimestamp == nil && r.exp.satisfied(key) {
		scaleErr = r.scale(ctx, key, rs, running)
	}
	return firstErr(scaleErr, r.updateStatus(ctx, rs, owned, running))
}

func (r *ReplicaSets) scale(ctx context.Context, key string, rs *appsv1.ReplicaSet, running []*corev1.Pod) error {
	want := int(ptr.Deref(rs.Spec.Replicas, 1))
	diff := want - len(running)
	switch {
	case diff > 0:
		diff = min(diff, burstLimit)
		r.exp.expect(key, diff, 0)
		created := 0
		for batch := 1; created < diff; batch = min(batch*2, diff-created) {
			batch = min(batch, diff-created)
			errs := make(chan error, batch)
			for range batch {
				go func() { errs <- createPod(ctx, r.client, rs, kindReplicaSet, &rs.Spec.Template, rs.Name+"-") }()
			}
			failed := 0
			var first error
			for range batch {
				if err := <-errs; err != nil {
					failed++
					first = firstErr(first, err)
				}
			}
			created += batch
			if failed > 0 {
				// The pods never asked for are not expected either.
				r.exp.observe(key, failed+diff-created, 0)
				if apierrors.HasStatusCause(first, corev1.NamespaceTerminatingCause) {
					return nil
				}
				return first
			}
		}
	case diff < 0:
		victims := rankForDeletion(running, -diff)
		r.exp.expect(key, 0, len(victims))
		for _, pod := range victims {
			err := r.client.CoreV1().Pods(pod.Namespace).Delete(ctx, pod.Name, metav1.DeleteOptions{Preconditions: &metav1.Preconditions{UID: &pod.UID}})
			if err != nil {
				r.exp.observe(key, 0, 1)
				if !apierrors.IsNotFound(err) {
					return err
				}
			}
		}
	}
	return nil
}

// rankForDeletion picks the pods to go first: unscheduled, then pending,
// then not ready, then the youngest.
func rankForDeletion(pods []*corev1.Pod, n int) []*corev1.Pod {
	ranked := append([]*corev1.Pod(nil), pods...)
	score := func(p *corev1.Pod) int {
		switch {
		case p.Spec.NodeName == "":
			return 0
		case p.Status.Phase == corev1.PodPending:
			return 1
		case !podReady(p):
			return 2
		}
		return 3
	}
	sort.SliceStable(ranked, func(i, j int) bool {
		si, sj := score(ranked[i]), score(ranked[j])
		if si != sj {
			return si < sj
		}
		return ranked[i].CreationTimestamp.After(ranked[j].CreationTimestamp.Time)
	})
	return ranked[:min(n, len(ranked))]
}

func createPod(ctx context.Context, client kubernetes.Interface, owner metav1.Object, kind schemaKind,
	template *corev1.PodTemplateSpec, generateName string) error {
	pod := &corev1.Pod{
		ObjectMeta: metav1.ObjectMeta{
			GenerateName:    generateName,
			Namespace:       owner.GetNamespace(),
			Labels:          template.Labels,
			Annotations:     template.Annotations,
			Finalizers:      template.Finalizers,
			OwnerReferences: []metav1.OwnerReference{*metav1.NewControllerRef(owner, kind.gvk())},
		},
		Spec: *template.Spec.DeepCopy(),
	}
	_, err := client.CoreV1().Pods(owner.GetNamespace()).Create(ctx, pod, metav1.CreateOptions{})
	return err
}

func (r *ReplicaSets) updateStatus(ctx context.Context, rs *appsv1.ReplicaSet, owned, running []*corev1.Pod) error {
	now := time.Now()
	templateLabels := labels.Set(rs.Spec.Template.Labels).AsSelectorPreValidated()
	status := appsv1.ReplicaSetStatus{Replicas: int32(len(running)), ObservedGeneration: rs.Generation, Conditions: rs.Status.Conditions}
	var recheck time.Duration
	for _, p := range running {
		if templateLabels.Matches(labels.Set(p.Labels)) {
			status.FullyLabeledReplicas++
		}
		if podReady(p) {
			status.ReadyReplicas++
		}
		ok, wait := availableFor(p, rs.Spec.MinReadySeconds, now)
		if ok {
			status.AvailableReplicas++
		} else if wait > 0 && (recheck == 0 || wait < recheck) {
			recheck = wait
		}
	}
	terminating := int32(0)
	for _, p := range owned {
		if p.DeletionTimestamp != nil {
			terminating++
		}
	}
	status.TerminatingReplicas = &terminating
	if !apiequality.Semantic.DeepEqual(rs.Status, status) {
		rs = rs.DeepCopy()
		rs.Status = status
		if _, err := r.client.AppsV1().ReplicaSets(rs.Namespace).UpdateStatus(ctx, rs, metav1.UpdateOptions{}); err != nil && !apierrors.IsNotFound(err) {
			return err
		}
	}
	if recheck > 0 {
		return later(recheck + 100*time.Millisecond)
	}
	return nil
}

func firstErr(errs ...error) error {
	for _, e := range errs {
		if e != nil {
			return e
		}
	}
	return nil
}
