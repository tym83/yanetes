package nanny

import (
	"context"
	"encoding/json"
	"fmt"
	"hash/fnv"
	"maps"
	"sort"
	"strconv"
	"time"

	appsv1 "k8s.io/api/apps/v1"
	corev1 "k8s.io/api/core/v1"
	apiequality "k8s.io/apimachinery/pkg/api/equality"
	apierrors "k8s.io/apimachinery/pkg/api/errors"
	metav1 "k8s.io/apimachinery/pkg/apis/meta/v1"
	"k8s.io/apimachinery/pkg/labels"
	"k8s.io/apimachinery/pkg/types"
	"k8s.io/apimachinery/pkg/util/intstr"
	"k8s.io/apimachinery/pkg/util/rand"
	"k8s.io/client-go/informers"
	"k8s.io/client-go/kubernetes"
	appslisters "k8s.io/client-go/listers/apps/v1"
	corelisters "k8s.io/client-go/listers/core/v1"
	"k8s.io/client-go/tools/cache"
	"k8s.io/utils/ptr"
)

const (
	annRevision        = "deployment.kubernetes.io/revision"
	annDesiredReplicas = "deployment.kubernetes.io/desired-replicas"
	annMaxReplicas     = "deployment.kubernetes.io/max-replicas"
	labelTemplateHash  = appsv1.DefaultDeploymentUniqueLabelKey
)

// Deployments rolls replica sets: a new one per template, the old ones
// scaled down as the strategy allows.
type Deployments struct {
	client      kubernetes.Interface
	deployments appslisters.DeploymentLister
	sets        appslisters.ReplicaSetLister
	pods        corelisters.PodLister
	loop        *loop
}

func NewDeployments(client kubernetes.Interface, factory informers.SharedInformerFactory) *Deployments {
	d := &Deployments{
		client:      client,
		deployments: factory.Apps().V1().Deployments().Lister(),
		sets:        factory.Apps().V1().ReplicaSets().Lister(),
		pods:        factory.Core().V1().Pods().Lister(),
	}
	d.loop = newLoop("deployment", 10, d.sync)
	_, _ = factory.Apps().V1().Deployments().Informer().AddEventHandler(d.loop.handlers())
	byOwner := func(obj any) {
		o, ok := unwrap(obj).(metav1.Object)
		if !ok {
			return
		}
		if ref := metav1.GetControllerOf(o); ref != nil && ref.Kind == "Deployment" {
			d.loop.add(o.GetNamespace() + "/" + ref.Name)
			return
		}
		// An orphan set may be adopted.
		if rs, ok := o.(*appsv1.ReplicaSet); ok {
			deps, _ := d.deployments.Deployments(rs.Namespace).List(labels.Everything())
			for _, dep := range deps {
				sel, err := metav1.LabelSelectorAsSelector(dep.Spec.Selector)
				if err == nil && !sel.Empty() && sel.Matches(labels.Set(rs.Labels)) {
					d.loop.add(dep.Namespace + "/" + dep.Name)
				}
			}
		}
	}
	_, _ = factory.Apps().V1().ReplicaSets().Informer().AddEventHandler(cache.ResourceEventHandlerFuncs{
		AddFunc: byOwner, UpdateFunc: func(_, obj any) { byOwner(obj) }, DeleteFunc: byOwner,
	})
	// Recreate waits for the old pods to be gone.
	_, _ = factory.Core().V1().Pods().Informer().AddEventHandler(cache.ResourceEventHandlerFuncs{
		DeleteFunc: func(obj any) {
			pod, ok := unwrap(obj).(*corev1.Pod)
			if !ok {
				return
			}
			if ref := metav1.GetControllerOf(pod); ref != nil && ref.Kind == "ReplicaSet" {
				if rs, err := d.sets.ReplicaSets(pod.Namespace).Get(ref.Name); err == nil {
					byOwner(rs)
				}
			}
		},
	})
	return d
}

func (d *Deployments) Run(ctx context.Context) { d.loop.run(ctx) }

// templateHash names a template's replica set, as pod-template-hash does.
func templateHash(t *corev1.PodTemplateSpec, collisions *int32) string {
	h := fnv.New32a()
	body, _ := json.Marshal(t)
	h.Write(body)
	if collisions != nil {
		h.Write([]byte(strconv.Itoa(int(*collisions))))
	}
	return rand.SafeEncodeString(strconv.FormatUint(uint64(h.Sum32()), 10))
}

func sameTemplate(a, b *corev1.PodTemplateSpec) bool {
	a, b = a.DeepCopy(), b.DeepCopy()
	delete(a.Labels, labelTemplateHash)
	delete(b.Labels, labelTemplateHash)
	return apiequality.Semantic.DeepEqual(a, b)
}

func revision(o metav1.Object) int64 {
	v, _ := strconv.ParseInt(o.GetAnnotations()[annRevision], 10, 64)
	return v
}

func replicas(rs *appsv1.ReplicaSet) int32 { return ptr.Deref(rs.Spec.Replicas, 0) }

// surgeAndUnavailable resolves the rolling update bounds the way upstream
// does: surge rounds up, unavailability down, and both zero means one.
func surgeAndUnavailable(dep *appsv1.Deployment) (int32, int32) {
	want := int(ptr.Deref(dep.Spec.Replicas, 1))
	ru := dep.Spec.Strategy.RollingUpdate
	surgeV, unavailV := intstr.FromString("25%"), intstr.FromString("25%")
	if ru != nil && ru.MaxSurge != nil {
		surgeV = *ru.MaxSurge
	}
	if ru != nil && ru.MaxUnavailable != nil {
		unavailV = *ru.MaxUnavailable
	}
	surge, _ := intstr.GetScaledValueFromIntOrPercent(&surgeV, want, true)
	unavail, _ := intstr.GetScaledValueFromIntOrPercent(&unavailV, want, false)
	if surge == 0 && unavail == 0 {
		unavail = 1
	}
	return int32(surge), int32(min(unavail, want))
}

func (d *Deployments) sync(ctx context.Context, key string) error {
	ns, name, _ := cache.SplitMetaNamespaceKey(key)
	dep, err := d.deployments.Deployments(ns).Get(name)
	if apierrors.IsNotFound(err) {
		return nil
	}
	if err != nil {
		return err
	}
	selector, err := metav1.LabelSelectorAsSelector(dep.Spec.Selector)
	if err != nil || selector.Empty() || dep.DeletionTimestamp != nil {
		return nil
	}
	sets, err := d.claimSets(ctx, dep, selector)
	if err != nil {
		return err
	}
	var newRS *appsv1.ReplicaSet
	var old []*appsv1.ReplicaSet
	for _, rs := range sets {
		if newRS == nil && sameTemplate(&rs.Spec.Template, &dep.Spec.Template) {
			newRS = rs
		} else {
			old = append(old, rs)
		}
	}
	sort.Slice(old, func(i, j int) bool { return old[i].CreationTimestamp.Before(&old[j].CreationTimestamp) })

	if dep.Spec.Paused {
		return d.status(ctx, dep, newRS, old, "")
	}
	recreate := dep.Spec.Strategy.Type == appsv1.RecreateDeploymentStrategyType
	if recreate {
		done, err := d.recreateOld(ctx, dep, old)
		if err != nil || !done {
			return firstErr(err, d.status(ctx, dep, newRS, old, ""))
		}
	}
	created := ""
	newRS, created, err = d.ensureNew(ctx, dep, newRS, old, recreate)
	if err != nil {
		return err
	}
	if !recreate {
		if err := d.rollOld(ctx, dep, newRS, old); err != nil {
			return err
		}
	}
	if err := d.cleanup(ctx, dep, old); err != nil {
		return err
	}
	return d.status(ctx, dep, newRS, old, created)
}

// claimSets adopts orphan replica sets that match and returns those owned.
func (d *Deployments) claimSets(ctx context.Context, dep *appsv1.Deployment, selector labels.Selector) ([]*appsv1.ReplicaSet, error) {
	all, err := d.sets.ReplicaSets(dep.Namespace).List(labels.Everything())
	if err != nil {
		return nil, err
	}
	var owned []*appsv1.ReplicaSet
	for _, rs := range all {
		ref := metav1.GetControllerOf(rs)
		switch {
		case ref != nil && ref.UID == dep.UID:
			owned = append(owned, rs)
		case ref == nil && rs.DeletionTimestamp == nil && selector.Matches(labels.Set(rs.Labels)):
			body, _ := json.Marshal(map[string]any{"metadata": map[string]any{
				"ownerReferences": []metav1.OwnerReference{*metav1.NewControllerRef(dep, kindDeployment.gvk())},
				"uid":             rs.UID,
			}})
			got, err := d.client.AppsV1().ReplicaSets(rs.Namespace).Patch(ctx, rs.Name, types.StrategicMergePatchType, body, metav1.PatchOptions{})
			if err != nil {
				if apierrors.IsNotFound(err) {
					continue
				}
				return nil, err
			}
			owned = append(owned, got)
		}
	}
	return owned, nil
}

func (d *Deployments) scale(ctx context.Context, dep *appsv1.Deployment, rs *appsv1.ReplicaSet, to int32, annotate bool) (*appsv1.ReplicaSet, error) {
	surge, _ := surgeAndUnavailable(dep)
	want := ptr.Deref(dep.Spec.Replicas, 1)
	desired, maxR := strconv.Itoa(int(want)), strconv.Itoa(int(want+surge))
	if replicas(rs) == to && (!annotate || (rs.Annotations[annDesiredReplicas] == desired && rs.Annotations[annMaxReplicas] == maxR)) {
		return rs, nil
	}
	rs = rs.DeepCopy()
	rs.Spec.Replicas = &to
	if rs.Annotations == nil {
		rs.Annotations = map[string]string{}
	}
	rs.Annotations[annDesiredReplicas] = desired
	rs.Annotations[annMaxReplicas] = maxR
	return d.client.AppsV1().ReplicaSets(rs.Namespace).Update(ctx, rs, metav1.UpdateOptions{})
}

// recreateOld scales every old set to zero and says whether their pods are gone.
func (d *Deployments) recreateOld(ctx context.Context, dep *appsv1.Deployment, old []*appsv1.ReplicaSet) (bool, error) {
	for _, rs := range old {
		if replicas(rs) > 0 {
			if _, err := d.scale(ctx, dep, rs, 0, false); err != nil {
				return false, err
			}
		}
	}
	pods, err := d.pods.Pods(dep.Namespace).List(labels.Everything())
	if err != nil {
		return false, err
	}
	for _, p := range pods {
		ref := metav1.GetControllerOf(p)
		if ref == nil || p.Status.Phase == corev1.PodSucceeded || p.Status.Phase == corev1.PodFailed {
			continue
		}
		for _, rs := range old {
			if ref.UID == rs.UID {
				return false, nil
			}
		}
	}
	return true, nil
}

// ensureNew creates or scales the replica set of the current template.
func (d *Deployments) ensureNew(ctx context.Context, dep *appsv1.Deployment, newRS *appsv1.ReplicaSet,
	old []*appsv1.ReplicaSet, recreate bool) (*appsv1.ReplicaSet, string, error) {
	want := ptr.Deref(dep.Spec.Replicas, 1)
	maxOld := int64(0)
	for _, rs := range old {
		maxOld = max(maxOld, revision(rs))
	}
	target := func(current int32) int32 {
		if recreate || current >= want {
			return want
		}
		surge, _ := surgeAndUnavailable(dep)
		total := int32(0)
		for _, rs := range old {
			total += replicas(rs)
		}
		total += current
		room := want + surge - total
		if room <= 0 {
			return current
		}
		return current + min(room, want-current)
	}
	if newRS != nil {
		// A template seen before comes back as the newest revision.
		if revision(newRS) <= maxOld {
			rs := newRS.DeepCopy()
			if rs.Annotations == nil {
				rs.Annotations = map[string]string{}
			}
			rs.Annotations[annRevision] = strconv.FormatInt(maxOld+1, 10)
			got, err := d.client.AppsV1().ReplicaSets(rs.Namespace).Update(ctx, rs, metav1.UpdateOptions{})
			if err != nil {
				return nil, "", err
			}
			newRS = got
		}
		rs, err := d.scale(ctx, dep, newRS, target(replicas(newRS)), true)
		return rs, "", err
	}
	hash := templateHash(&dep.Spec.Template, dep.Status.CollisionCount)
	template := *dep.Spec.Template.DeepCopy()
	template.Labels = maps.Clone(template.Labels)
	if template.Labels == nil {
		template.Labels = map[string]string{}
	}
	template.Labels[labelTemplateHash] = hash
	selector := dep.Spec.Selector.DeepCopy()
	if selector.MatchLabels == nil {
		selector.MatchLabels = map[string]string{}
	}
	selector.MatchLabels[labelTemplateHash] = hash
	annotations := map[string]string{}
	for k, v := range dep.Annotations {
		if k != annRevision && k != "kubectl.kubernetes.io/last-applied-configuration" {
			annotations[k] = v
		}
	}
	surge, _ := surgeAndUnavailable(dep)
	annotations[annRevision] = strconv.FormatInt(maxOld+1, 10)
	annotations[annDesiredReplicas] = strconv.Itoa(int(want))
	annotations[annMaxReplicas] = strconv.Itoa(int(want + surge))
	count := target(0)
	rs := &appsv1.ReplicaSet{
		ObjectMeta: metav1.ObjectMeta{
			Name:            dep.Name + "-" + hash,
			Namespace:       dep.Namespace,
			Labels:          template.Labels,
			Annotations:     annotations,
			OwnerReferences: []metav1.OwnerReference{*metav1.NewControllerRef(dep, kindDeployment.gvk())},
		},
		Spec: appsv1.ReplicaSetSpec{
			Replicas:        &count,
			MinReadySeconds: dep.Spec.MinReadySeconds,
			Selector:        selector,
			Template:        template,
		},
	}
	got, err := d.client.AppsV1().ReplicaSets(dep.Namespace).Create(ctx, rs, metav1.CreateOptions{})
	if apierrors.IsAlreadyExists(err) {
		existing, getErr := d.client.AppsV1().ReplicaSets(dep.Namespace).Get(ctx, rs.Name, metav1.GetOptions{})
		if getErr == nil && metav1.IsControlledBy(existing, dep) && sameTemplate(&existing.Spec.Template, &dep.Spec.Template) {
			return existing, "", nil
		}
		// Someone else's set has the name: count a collision and hash again.
		dep = dep.DeepCopy()
		dep.Status.CollisionCount = ptr.To(ptr.Deref(dep.Status.CollisionCount, 0) + 1)
		_, err = d.client.AppsV1().Deployments(dep.Namespace).UpdateStatus(ctx, dep, metav1.UpdateOptions{})
		return nil, "", firstErr(err, later(0))
	}
	if err != nil {
		return nil, "", err
	}
	return got, got.Name, nil
}

// rollOld scales old sets down as far as availability allows.
func (d *Deployments) rollOld(ctx context.Context, dep *appsv1.Deployment, newRS *appsv1.ReplicaSet, old []*appsv1.ReplicaSet) error {
	want := ptr.Deref(dep.Spec.Replicas, 1)
	_, unavail := surgeAndUnavailable(dep)
	oldCount := int32(0)
	for _, rs := range old {
		oldCount += replicas(rs)
	}
	if oldCount == 0 {
		return nil
	}
	all := oldCount + replicas(newRS)
	minAvailable := want - unavail
	newUnavailable := replicas(newRS) - newRS.Status.AvailableReplicas
	budget := all - minAvailable - newUnavailable
	if budget <= 0 {
		return nil
	}
	// Unhealthy old pods go first: they serve nothing anyway.
	for _, rs := range old {
		if budget <= 0 {
			break
		}
		unhealthy := replicas(rs) - rs.Status.AvailableReplicas
		if unhealthy <= 0 {
			continue
		}
		cut := min(unhealthy, budget)
		if _, err := d.scale(ctx, dep, rs, replicas(rs)-cut, false); err != nil {
			return err
		}
		rs.Spec.Replicas = ptr.To(replicas(rs) - cut)
		budget -= cut
	}
	available := newRS.Status.AvailableReplicas
	for _, rs := range old {
		available += min(rs.Status.AvailableReplicas, replicas(rs))
	}
	cut := min(available-minAvailable, budget)
	for _, rs := range old {
		if cut <= 0 {
			break
		}
		n := min(replicas(rs), cut)
		if n == 0 {
			continue
		}
		if _, err := d.scale(ctx, dep, rs, replicas(rs)-n, false); err != nil {
			return err
		}
		cut -= n
	}
	return nil
}

// cleanup deletes empty old sets beyond the revision history limit.
func (d *Deployments) cleanup(ctx context.Context, dep *appsv1.Deployment, old []*appsv1.ReplicaSet) error {
	limit := int(ptr.Deref(dep.Spec.RevisionHistoryLimit, 10))
	var empty []*appsv1.ReplicaSet
	for _, rs := range old {
		if replicas(rs) == 0 && rs.Status.Replicas == 0 && rs.DeletionTimestamp == nil {
			empty = append(empty, rs)
		}
	}
	sort.Slice(empty, func(i, j int) bool { return revision(empty[i]) < revision(empty[j]) })
	for i := 0; i < len(empty)-limit; i++ {
		err := d.client.AppsV1().ReplicaSets(dep.Namespace).Delete(ctx, empty[i].Name, metav1.DeleteOptions{})
		if err != nil && !apierrors.IsNotFound(err) {
			return err
		}
	}
	return nil
}

func setCondition(conds []appsv1.DeploymentCondition, c appsv1.DeploymentCondition) []appsv1.DeploymentCondition {
	now := metav1.Now()
	c.LastUpdateTime, c.LastTransitionTime = now, now
	for i, old := range conds {
		if old.Type != c.Type {
			continue
		}
		if old.Status == c.Status {
			c.LastTransitionTime = old.LastTransitionTime
			if old.Reason == c.Reason && old.Message == c.Message {
				c.LastUpdateTime = old.LastUpdateTime
			}
		}
		conds[i] = c
		return conds
	}
	return append(conds, c)
}

func (d *Deployments) status(ctx context.Context, dep *appsv1.Deployment, newRS *appsv1.ReplicaSet, old []*appsv1.ReplicaSet, created string) error {
	want := ptr.Deref(dep.Spec.Replicas, 1)
	_, unavail := surgeAndUnavailable(dep)
	s := appsv1.DeploymentStatus{
		ObservedGeneration: dep.Generation,
		CollisionCount:     dep.Status.CollisionCount,
		Conditions:         append([]appsv1.DeploymentCondition(nil), dep.Status.Conditions...),
	}
	all := old
	if newRS != nil {
		all = append([]*appsv1.ReplicaSet{newRS}, old...)
		s.UpdatedReplicas = newRS.Status.Replicas
	}
	terminating := int32(0)
	for _, rs := range all {
		s.Replicas += rs.Status.Replicas
		s.ReadyReplicas += rs.Status.ReadyReplicas
		s.AvailableReplicas += rs.Status.AvailableReplicas
		terminating += ptr.Deref(rs.Status.TerminatingReplicas, 0)
	}
	s.TerminatingReplicas = &terminating
	s.UnavailableReplicas = max(0, s.Replicas-s.AvailableReplicas)
	if s.AvailableReplicas >= want-unavail {
		s.Conditions = setCondition(s.Conditions, appsv1.DeploymentCondition{
			Type: appsv1.DeploymentAvailable, Status: corev1.ConditionTrue,
			Reason: "MinimumReplicasAvailable", Message: "Deployment has minimum availability.",
		})
	} else {
		s.Conditions = setCondition(s.Conditions, appsv1.DeploymentCondition{
			Type: appsv1.DeploymentAvailable, Status: corev1.ConditionFalse,
			Reason: "MinimumReplicasUnavailable", Message: "Deployment does not have minimum availability.",
		})
	}
	oldLeft := int32(0)
	for _, rs := range old {
		oldLeft += rs.Status.Replicas
	}
	switch {
	case dep.Spec.Paused:
		s.Conditions = setCondition(s.Conditions, appsv1.DeploymentCondition{
			Type: appsv1.DeploymentProgressing, Status: corev1.ConditionUnknown,
			Reason: "DeploymentPaused", Message: "Deployment is paused",
		})
	case newRS != nil && s.UpdatedReplicas == want && s.AvailableReplicas == want && s.Replicas == want && oldLeft == 0:
		s.Conditions = setCondition(s.Conditions, appsv1.DeploymentCondition{
			Type: appsv1.DeploymentProgressing, Status: corev1.ConditionTrue,
			Reason: "NewReplicaSetAvailable", Message: fmt.Sprintf("ReplicaSet %q has successfully progressed.", newRS.Name),
		})
	case created != "":
		s.Conditions = setCondition(s.Conditions, appsv1.DeploymentCondition{
			Type: appsv1.DeploymentProgressing, Status: corev1.ConditionTrue,
			Reason: "NewReplicaSetCreated", Message: fmt.Sprintf("Created new replica set %q", created),
		})
	case newRS != nil:
		s.Conditions = setCondition(s.Conditions, appsv1.DeploymentCondition{
			Type: appsv1.DeploymentProgressing, Status: corev1.ConditionTrue,
			Reason: "ReplicaSetUpdated", Message: fmt.Sprintf("ReplicaSet %q is progressing.", newRS.Name),
		})
	}
	if newRS != nil && dep.Annotations[annRevision] != newRS.Annotations[annRevision] {
		dep = dep.DeepCopy()
		if dep.Annotations == nil {
			dep.Annotations = map[string]string{}
		}
		dep.Annotations[annRevision] = newRS.Annotations[annRevision]
		got, err := d.client.AppsV1().Deployments(dep.Namespace).Update(ctx, dep, metav1.UpdateOptions{})
		if err != nil {
			return err
		}
		dep = got
	}
	// Conditions carry times: compare without them so an idle deployment is not rewritten.
	same := func(a, b appsv1.DeploymentStatus) bool {
		strip := func(s appsv1.DeploymentStatus) appsv1.DeploymentStatus {
			s.Conditions = append([]appsv1.DeploymentCondition(nil), s.Conditions...)
			for i := range s.Conditions {
				s.Conditions[i].LastUpdateTime, s.Conditions[i].LastTransitionTime = metav1.Time{}, metav1.Time{}
			}
			return s
		}
		return apiequality.Semantic.DeepEqual(strip(a), strip(b))
	}
	if same(dep.Status, s) {
		return nil
	}
	dep = dep.DeepCopy()
	dep.Status = s
	_, err := d.client.AppsV1().Deployments(dep.Namespace).UpdateStatus(ctx, dep, metav1.UpdateOptions{})
	if apierrors.IsNotFound(err) {
		return nil
	}
	if err == nil && !dep.Spec.Paused && newRS != nil && s.AvailableReplicas < want {
		// minReadySeconds counts in time, not events.
		return later(time.Duration(max(dep.Spec.MinReadySeconds, 1)) * time.Second)
	}
	return err
}
