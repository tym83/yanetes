package nanny

import (
	"context"
	"time"

	corev1 "k8s.io/api/core/v1"
	apiequality "k8s.io/apimachinery/pkg/api/equality"
	apierrors "k8s.io/apimachinery/pkg/api/errors"
	metav1 "k8s.io/apimachinery/pkg/apis/meta/v1"
	"k8s.io/apimachinery/pkg/labels"
	"k8s.io/client-go/informers"
	"k8s.io/client-go/kubernetes"
	corelisters "k8s.io/client-go/listers/core/v1"
	"k8s.io/client-go/tools/cache"
	"k8s.io/utils/ptr"
)

var kindReplicationController = schemaKind{"", "v1", "ReplicationController"}

// ReplicationControllers is the replica set controller for the older
// core/v1 kind with a plain map selector.
type ReplicationControllers struct {
	client kubernetes.Interface
	rcs    corelisters.ReplicationControllerLister
	pods   corelisters.PodLister
	exp    *expectations
	loop   *loop
}

func NewReplicationControllers(client kubernetes.Interface, factory informers.SharedInformerFactory) *ReplicationControllers {
	r := &ReplicationControllers{
		client: client,
		rcs:    factory.Core().V1().ReplicationControllers().Lister(),
		pods:   factory.Core().V1().Pods().Lister(),
		exp:    newExpectations(),
	}
	r.loop = newLoop("replicationcontroller", 5, r.sync)
	_, _ = factory.Core().V1().ReplicationControllers().Informer().AddEventHandler(r.loop.handlers())
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

func (r *ReplicationControllers) Run(ctx context.Context) { r.loop.run(ctx) }

func (r *ReplicationControllers) ownerKey(pod *corev1.Pod) string {
	ref := metav1.GetControllerOf(pod)
	if ref == nil || ref.Kind != kindReplicationController.kind {
		return ""
	}
	rc, err := r.rcs.ReplicationControllers(pod.Namespace).Get(ref.Name)
	if err != nil || rc.UID != ref.UID {
		return ""
	}
	return pod.Namespace + "/" + rc.Name
}

func (r *ReplicationControllers) wakeMatching(pod *corev1.Pod) {
	if metav1.GetControllerOf(pod) != nil {
		return
	}
	rcs, _ := r.rcs.ReplicationControllers(pod.Namespace).List(labels.Everything())
	for _, rc := range rcs {
		if len(rc.Spec.Selector) > 0 && labels.SelectorFromSet(rc.Spec.Selector).Matches(labels.Set(pod.Labels)) {
			r.loop.add(pod.Namespace + "/" + rc.Name)
		}
	}
}

func (r *ReplicationControllers) sync(ctx context.Context, key string) error {
	ns, name, _ := cache.SplitMetaNamespaceKey(key)
	rc, err := r.rcs.ReplicationControllers(ns).Get(name)
	if apierrors.IsNotFound(err) {
		r.exp.forget(key)
		return nil
	}
	if err != nil {
		return err
	}
	if len(rc.Spec.Selector) == 0 || rc.Spec.Template == nil {
		return nil
	}
	selector := labels.SelectorFromSet(rc.Spec.Selector)
	all, err := r.pods.Pods(ns).List(labels.Everything())
	if err != nil {
		return err
	}
	owned, err := claimPods(ctx, r.client, rc, kindReplicationController, selector, all)
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
	if rc.DeletionTimestamp == nil && r.exp.satisfied(key) {
		scaleErr = r.scale(ctx, key, rc, running)
	}
	return firstErr(r.updateStatus(ctx, rc, running, scaleErr), scaleErr)
}

func (r *ReplicationControllers) scale(ctx context.Context, key string, rc *corev1.ReplicationController, running []*corev1.Pod) error {
	diff := int(ptr.Deref(rc.Spec.Replicas, 1)) - len(running)
	switch {
	case diff > 0:
		diff = min(diff, burstLimit)
		r.exp.expect(key, diff, 0)
		created := 0
		for batch := 1; created < diff; batch = min(batch*2, diff-created) {
			batch = min(batch, diff-created)
			errs := make(chan error, batch)
			for range batch {
				go func() { errs <- createPod(ctx, r.client, rc, kindReplicationController, rc.Spec.Template, rc.Name+"-") }()
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

// updateStatus also carries a create or delete failure as the
// ReplicaFailure condition, as upstream does for a quota that is exceeded.
func (r *ReplicationControllers) updateStatus(ctx context.Context, rc *corev1.ReplicationController, running []*corev1.Pod, scaleErr error) error {
	now := time.Now()
	templateLabels := labels.Set(rc.Spec.Template.Labels).AsSelectorPreValidated()
	status := corev1.ReplicationControllerStatus{
		Replicas:           int32(len(running)),
		ObservedGeneration: rc.Generation,
	}
	var recheck time.Duration
	for _, p := range running {
		if templateLabels.Matches(labels.Set(p.Labels)) {
			status.FullyLabeledReplicas++
		}
		if podReady(p) {
			status.ReadyReplicas++
		}
		ok, wait := availableFor(p, rc.Spec.MinReadySeconds, now)
		if ok {
			status.AvailableReplicas++
		} else if wait > 0 && (recheck == 0 || wait < recheck) {
			recheck = wait
		}
	}
	for _, c := range rc.Status.Conditions {
		if c.Type != corev1.ReplicationControllerReplicaFailure {
			status.Conditions = append(status.Conditions, c)
		}
	}
	if scaleErr != nil && !apierrors.IsNotFound(scaleErr) {
		reason := "FailedCreate"
		if int(ptr.Deref(rc.Spec.Replicas, 1)) < len(running) {
			reason = "FailedDelete"
		}
		cond := corev1.ReplicationControllerCondition{
			Type: corev1.ReplicationControllerReplicaFailure, Status: corev1.ConditionTrue,
			Reason: reason, Message: scaleErr.Error(), LastTransitionTime: metav1.Now(),
		}
		for _, c := range rc.Status.Conditions {
			if c.Type == cond.Type && c.Status == cond.Status && c.Reason == cond.Reason {
				cond.LastTransitionTime = c.LastTransitionTime
			}
		}
		status.Conditions = append(status.Conditions, cond)
	}
	if !apiequality.Semantic.DeepEqual(rc.Status, status) {
		rc = rc.DeepCopy()
		rc.Status = status
		if _, err := r.client.CoreV1().ReplicationControllers(rc.Namespace).UpdateStatus(ctx, rc, metav1.UpdateOptions{}); err != nil && !apierrors.IsNotFound(err) {
			return err
		}
	}
	if recheck > 0 {
		return later(recheck + 100*time.Millisecond)
	}
	return nil
}
