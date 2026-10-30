package nanny

import (
	"context"
	"fmt"
	"time"

	corev1 "k8s.io/api/core/v1"
	policyv1 "k8s.io/api/policy/v1"
	apiequality "k8s.io/apimachinery/pkg/api/equality"
	apierrors "k8s.io/apimachinery/pkg/api/errors"
	metav1 "k8s.io/apimachinery/pkg/apis/meta/v1"
	"k8s.io/apimachinery/pkg/labels"
	"k8s.io/apimachinery/pkg/types"
	"k8s.io/apimachinery/pkg/util/intstr"
	"k8s.io/client-go/informers"
	"k8s.io/client-go/kubernetes"
	appslisters "k8s.io/client-go/listers/apps/v1"
	corelisters "k8s.io/client-go/listers/core/v1"
	policylisters "k8s.io/client-go/listers/policy/v1"
	"k8s.io/client-go/tools/cache"
	"k8s.io/utils/ptr"
)

// A pod the eviction API let go of counts as disrupted this long, or until
// it is seen going.
const disruptedPodTimeout = 2 * time.Minute

// Disruption keeps the status of PodDisruptionBudgets: how many pods are
// expected, healthy and may be disrupted now.
type Disruption struct {
	client      kubernetes.Interface
	budgets     policylisters.PodDisruptionBudgetLister
	pods        corelisters.PodLister
	rcs         corelisters.ReplicationControllerLister
	sets        appslisters.ReplicaSetLister
	deployments appslisters.DeploymentLister
	stateful    appslisters.StatefulSetLister
	loop        *loop
}

func NewDisruption(client kubernetes.Interface, factory informers.SharedInformerFactory) *Disruption {
	d := &Disruption{
		client:      client,
		budgets:     factory.Policy().V1().PodDisruptionBudgets().Lister(),
		pods:        factory.Core().V1().Pods().Lister(),
		rcs:         factory.Core().V1().ReplicationControllers().Lister(),
		sets:        factory.Apps().V1().ReplicaSets().Lister(),
		deployments: factory.Apps().V1().Deployments().Lister(),
		stateful:    factory.Apps().V1().StatefulSets().Lister(),
	}
	d.loop = newLoop("disruption", 5, d.sync)
	_, _ = factory.Policy().V1().PodDisruptionBudgets().Informer().AddEventHandler(d.loop.handlers())
	byPod := func(obj any) {
		pod, ok := unwrap(obj).(*corev1.Pod)
		if !ok {
			return
		}
		budgets, _ := d.budgets.PodDisruptionBudgets(pod.Namespace).List(labels.Everything())
		for _, b := range budgets {
			sel, err := metav1.LabelSelectorAsSelector(b.Spec.Selector)
			if err == nil && sel.Matches(labels.Set(pod.Labels)) {
				d.loop.add(b.Namespace + "/" + b.Name)
			}
		}
	}
	_, _ = factory.Core().V1().Pods().Informer().AddEventHandler(cache.ResourceEventHandlerFuncs{
		AddFunc: byPod, UpdateFunc: func(_, obj any) { byPod(obj) }, DeleteFunc: byPod,
	})
	// A scale change of a controller changes what a budget expects.
	byScalable := func(obj any) {
		o, ok := unwrap(obj).(metav1.Object)
		if !ok {
			return
		}
		budgets, _ := d.budgets.PodDisruptionBudgets(o.GetNamespace()).List(labels.Everything())
		for _, b := range budgets {
			d.loop.add(b.Namespace + "/" + b.Name)
		}
	}
	scaled := cache.ResourceEventHandlerFuncs{UpdateFunc: func(_, obj any) { byScalable(obj) }}
	_, _ = factory.Core().V1().ReplicationControllers().Informer().AddEventHandler(scaled)
	_, _ = factory.Apps().V1().ReplicaSets().Informer().AddEventHandler(scaled)
	_, _ = factory.Apps().V1().Deployments().Informer().AddEventHandler(scaled)
	_, _ = factory.Apps().V1().StatefulSets().Informer().AddEventHandler(scaled)
	return d
}

func (d *Disruption) Run(ctx context.Context) { d.loop.run(ctx) }

// expected sums the scale of the controllers of the pods; a pod with no
// scalable controller makes percentages and maxUnavailable impossible.
func (d *Disruption) expected(pods []*corev1.Pod) (int32, error) {
	seen := map[types.UID]bool{}
	var total int32
	for _, pod := range pods {
		ref := metav1.GetControllerOf(pod)
		if ref == nil {
			return 0, fmt.Errorf("found no controller ref for pod %q", pod.Name)
		}
		var uid types.UID
		var scale int32
		switch ref.Kind {
		case "ReplicaSet":
			rs, err := d.sets.ReplicaSets(pod.Namespace).Get(ref.Name)
			if err != nil || rs.UID != ref.UID {
				return 0, fmt.Errorf("found no controllers for pod %q", pod.Name)
			}
			uid, scale = rs.UID, ptr.Deref(rs.Spec.Replicas, 1)
			if owner := metav1.GetControllerOf(rs); owner != nil && owner.Kind == "Deployment" {
				dep, err := d.deployments.Deployments(pod.Namespace).Get(owner.Name)
				if err == nil && dep.UID == owner.UID {
					uid, scale = dep.UID, ptr.Deref(dep.Spec.Replicas, 1)
				}
			}
		case "ReplicationController":
			rc, err := d.rcs.ReplicationControllers(pod.Namespace).Get(ref.Name)
			if err != nil || rc.UID != ref.UID {
				return 0, fmt.Errorf("found no controllers for pod %q", pod.Name)
			}
			uid, scale = rc.UID, ptr.Deref(rc.Spec.Replicas, 1)
		case "StatefulSet":
			ss, err := d.stateful.StatefulSets(pod.Namespace).Get(ref.Name)
			if err != nil || ss.UID != ref.UID {
				return 0, fmt.Errorf("found no controllers for pod %q", pod.Name)
			}
			uid, scale = ss.UID, ptr.Deref(ss.Spec.Replicas, 1)
		default:
			return 0, fmt.Errorf("found no controllers for pod %q", pod.Name)
		}
		if !seen[uid] {
			seen[uid] = true
			total += scale
		}
	}
	return total, nil
}

func (d *Disruption) sync(ctx context.Context, key string) error {
	ns, name, _ := cache.SplitMetaNamespaceKey(key)
	pdb, err := d.budgets.PodDisruptionBudgets(ns).Get(name)
	if apierrors.IsNotFound(err) {
		return nil
	}
	if err != nil {
		return err
	}
	sel, err := metav1.LabelSelectorAsSelector(pdb.Spec.Selector)
	var pods []*corev1.Pod
	if err == nil && pdb.Spec.Selector != nil {
		all, _ := d.pods.Pods(ns).List(sel)
		for _, p := range all {
			if p.Status.Phase != corev1.PodSucceeded && p.Status.Phase != corev1.PodFailed {
				pods = append(pods, p)
			}
		}
	}
	now := time.Now()
	status := policyv1.PodDisruptionBudgetStatus{
		ObservedGeneration: pdb.Generation,
		Conditions:         append([]metav1.Condition(nil), pdb.Status.Conditions...),
	}
	// Pods the eviction API released stay counted as gone until seen going.
	var recheck time.Duration
	disrupted := map[string]metav1.Time{}
	byName := map[string]*corev1.Pod{}
	for _, p := range pods {
		byName[p.Name] = p
	}
	for podName, at := range pdb.Status.DisruptedPods {
		p := byName[podName]
		if p == nil || p.DeletionTimestamp != nil {
			continue
		}
		left := at.Add(disruptedPodTimeout).Sub(now)
		if left <= 0 {
			continue
		}
		disrupted[podName] = at
		if recheck == 0 || left < recheck {
			recheck = left
		}
	}
	if len(disrupted) > 0 {
		status.DisruptedPods = disrupted
	}
	for _, p := range pods {
		if p.DeletionTimestamp == nil && podReady(p) {
			if _, gone := disrupted[p.Name]; !gone {
				status.CurrentHealthy++
			}
		}
	}

	var syncErr error
	switch {
	case pdb.Spec.MaxUnavailable != nil:
		expected, err := d.expected(pods)
		if err != nil {
			syncErr = err
			break
		}
		unavailable, err := intstr.GetScaledValueFromIntOrPercent(pdb.Spec.MaxUnavailable, int(expected), true)
		if err != nil {
			syncErr = err
			break
		}
		status.ExpectedPods = expected
		status.DesiredHealthy = max(0, expected-int32(unavailable))
	case pdb.Spec.MinAvailable != nil && pdb.Spec.MinAvailable.Type == intstr.Int:
		status.ExpectedPods = int32(len(pods))
		status.DesiredHealthy = pdb.Spec.MinAvailable.IntVal
	case pdb.Spec.MinAvailable != nil:
		expected, err := d.expected(pods)
		if err != nil {
			syncErr = err
			break
		}
		available, err := intstr.GetScaledValueFromIntOrPercent(pdb.Spec.MinAvailable, int(expected), true)
		if err != nil {
			syncErr = err
			break
		}
		status.ExpectedPods = expected
		status.DesiredHealthy = int32(available)
	default:
		status.ExpectedPods = int32(len(pods))
	}

	cond := metav1.Condition{Type: policyv1.DisruptionAllowedCondition, ObservedGeneration: pdb.Generation}
	if syncErr != nil {
		status.CurrentHealthy, status.DesiredHealthy, status.ExpectedPods, status.DisruptionsAllowed = 0, 0, 0, 0
		cond.Status, cond.Reason, cond.Message = metav1.ConditionFalse, policyv1.SyncFailedReason, syncErr.Error()
	} else {
		status.DisruptionsAllowed = max(0, status.CurrentHealthy-status.DesiredHealthy)
		if status.ExpectedPods == 0 && pdb.Spec.MaxUnavailable == nil && pdb.Spec.MinAvailable == nil {
			status.DisruptionsAllowed = 0
		}
		if status.DisruptionsAllowed > 0 {
			cond.Status, cond.Reason = metav1.ConditionTrue, policyv1.SufficientPodsReason
		} else {
			cond.Status, cond.Reason = metav1.ConditionFalse, policyv1.InsufficientPodsReason
		}
	}
	cond.LastTransitionTime = metav1.NewTime(now)
	replaced := false
	for i, c := range status.Conditions {
		if c.Type != cond.Type {
			continue
		}
		if c.Status == cond.Status {
			cond.LastTransitionTime = c.LastTransitionTime
		}
		status.Conditions[i], replaced = cond, true
	}
	if !replaced {
		status.Conditions = append(status.Conditions, cond)
	}

	if !apiequality.Semantic.DeepEqual(pdb.Status, status) {
		pdb = pdb.DeepCopy()
		pdb.Status = status
		_, err := d.client.PolicyV1().PodDisruptionBudgets(ns).UpdateStatus(ctx, pdb, metav1.UpdateOptions{})
		if apierrors.IsConflict(err) {
			return later(time.Second)
		}
		if err != nil && !apierrors.IsNotFound(err) {
			return err
		}
	}
	if recheck > 0 {
		return later(recheck)
	}
	return nil
}
