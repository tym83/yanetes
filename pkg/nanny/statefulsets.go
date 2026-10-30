package nanny

import (
	"context"
	"fmt"
	"maps"
	"sort"
	"strconv"
	"strings"
	"time"

	appsv1 "k8s.io/api/apps/v1"
	corev1 "k8s.io/api/core/v1"
	apiequality "k8s.io/apimachinery/pkg/api/equality"
	apierrors "k8s.io/apimachinery/pkg/api/errors"
	metav1 "k8s.io/apimachinery/pkg/apis/meta/v1"
	"k8s.io/apimachinery/pkg/labels"
	"k8s.io/client-go/informers"
	"k8s.io/client-go/kubernetes"
	appslisters "k8s.io/client-go/listers/apps/v1"
	corelisters "k8s.io/client-go/listers/core/v1"
	"k8s.io/client-go/tools/cache"
	"k8s.io/utils/ptr"
)

var kindStatefulSet = schemaKind{"apps", "v1", "StatefulSet"}

// StatefulSets keeps numbered pods with stable names and claims, made and
// removed in order unless the set says Parallel.
type StatefulSets struct {
	client kubernetes.Interface
	sets   appslisters.StatefulSetLister
	pods   corelisters.PodLister
	claims corelisters.PersistentVolumeClaimLister
	revs   revisions
	loop   *loop
}

func NewStatefulSets(client kubernetes.Interface, factory informers.SharedInformerFactory) *StatefulSets {
	s := &StatefulSets{
		client: client,
		sets:   factory.Apps().V1().StatefulSets().Lister(),
		pods:   factory.Core().V1().Pods().Lister(),
		claims: factory.Core().V1().PersistentVolumeClaims().Lister(),
		revs:   revisions{client: client, lister: factory.Apps().V1().ControllerRevisions().Lister()},
	}
	s.loop = newLoop("statefulset", 5, s.sync)
	_, _ = factory.Apps().V1().StatefulSets().Informer().AddEventHandler(s.loop.handlers())
	_ = factory.Apps().V1().ControllerRevisions().Informer()
	byPod := func(obj any) {
		pod, ok := unwrap(obj).(*corev1.Pod)
		if !ok {
			return
		}
		if ref := metav1.GetControllerOf(pod); ref != nil {
			if ref.Kind == "StatefulSet" {
				s.loop.add(pod.Namespace + "/" + ref.Name)
			}
			return
		}
		sets, _ := s.sets.StatefulSets(pod.Namespace).List(labels.Everything())
		for _, set := range sets {
			sel, err := metav1.LabelSelectorAsSelector(set.Spec.Selector)
			if err == nil && !sel.Empty() && sel.Matches(labels.Set(pod.Labels)) {
				s.loop.add(set.Namespace + "/" + set.Name)
			}
		}
	}
	_, _ = factory.Core().V1().Pods().Informer().AddEventHandler(cache.ResourceEventHandlerFuncs{
		AddFunc: byPod, UpdateFunc: func(_, obj any) { byPod(obj) }, DeleteFunc: byPod,
	})
	_ = factory.Core().V1().PersistentVolumeClaims().Informer()
	return s
}

func (s *StatefulSets) Run(ctx context.Context) { s.loop.run(ctx) }

// ordinalOf reads the number off a pod named <set>-<n>.
func ordinalOf(set *appsv1.StatefulSet, pod *corev1.Pod) (int, bool) {
	rest, ok := strings.CutPrefix(pod.Name, set.Name+"-")
	if !ok {
		return 0, false
	}
	n, err := strconv.Atoi(rest)
	return n, err == nil && n >= 0 && strconv.Itoa(n) == rest
}

func claimName(set *appsv1.StatefulSet, tmpl *corev1.PersistentVolumeClaim, ordinal int) string {
	return fmt.Sprintf("%s-%s-%d", tmpl.Name, set.Name, ordinal)
}

func runningAndReady(pod *corev1.Pod) bool {
	return pod.Status.Phase == corev1.PodRunning && podReady(pod) && pod.DeletionTimestamp == nil
}

func (s *StatefulSets) newPod(set *appsv1.StatefulSet, template *corev1.PodTemplateSpec, revision string, ordinal int) *corev1.Pod {
	t := template.DeepCopy()
	name := fmt.Sprintf("%s-%d", set.Name, ordinal)
	podLabels := maps.Clone(t.Labels)
	if podLabels == nil {
		podLabels = map[string]string{}
	}
	podLabels[appsv1.StatefulSetPodNameLabel] = name
	podLabels[appsv1.PodIndexLabel] = strconv.Itoa(ordinal)
	podLabels[labelRevisionHash] = revision
	spec := t.Spec
	spec.Hostname = name
	spec.Subdomain = set.Spec.ServiceName
	for _, c := range set.Spec.VolumeClaimTemplates {
		v := corev1.Volume{Name: c.Name, VolumeSource: corev1.VolumeSource{
			PersistentVolumeClaim: &corev1.PersistentVolumeClaimVolumeSource{ClaimName: claimName(set, &c, ordinal)},
		}}
		replaced := false
		for i := range spec.Volumes {
			if spec.Volumes[i].Name == c.Name {
				spec.Volumes[i], replaced = v, true
			}
		}
		if !replaced {
			spec.Volumes = append(spec.Volumes, v)
		}
	}
	return &corev1.Pod{
		ObjectMeta: metav1.ObjectMeta{
			Name:            name,
			Namespace:       set.Namespace,
			Labels:          podLabels,
			Annotations:     t.Annotations,
			Finalizers:      t.Finalizers,
			OwnerReferences: []metav1.OwnerReference{*metav1.NewControllerRef(set, kindStatefulSet.gvk())},
		},
		Spec: spec,
	}
}

// ensureClaims makes the pod's claims before the pod, so the pod never
// waits on a claim nobody will create.
func (s *StatefulSets) ensureClaims(ctx context.Context, set *appsv1.StatefulSet, ordinal int) error {
	for _, tmpl := range set.Spec.VolumeClaimTemplates {
		name := claimName(set, &tmpl, ordinal)
		if _, err := s.claims.PersistentVolumeClaims(set.Namespace).Get(name); err == nil {
			continue
		}
		claimLabels := maps.Clone(tmpl.Labels)
		if claimLabels == nil {
			claimLabels = map[string]string{}
		}
		if set.Spec.Selector != nil {
			maps.Copy(claimLabels, set.Spec.Selector.MatchLabels)
		}
		pvc := &corev1.PersistentVolumeClaim{
			ObjectMeta: metav1.ObjectMeta{Name: name, Namespace: set.Namespace, Labels: claimLabels, Annotations: tmpl.Annotations},
			Spec:       *tmpl.Spec.DeepCopy(),
		}
		policy := set.Spec.PersistentVolumeClaimRetentionPolicy
		if policy != nil && policy.WhenDeleted == appsv1.DeletePersistentVolumeClaimRetentionPolicyType {
			ref := metav1.NewControllerRef(set, kindStatefulSet.gvk())
			ref.Controller = ptr.To(false)
			pvc.OwnerReferences = []metav1.OwnerReference{*ref}
		}
		_, err := s.client.CoreV1().PersistentVolumeClaims(set.Namespace).Create(ctx, pvc, metav1.CreateOptions{})
		if err != nil && !apierrors.IsAlreadyExists(err) {
			return err
		}
	}
	return nil
}

func (s *StatefulSets) sync(ctx context.Context, key string) error {
	ns, name, _ := cache.SplitMetaNamespaceKey(key)
	set, err := s.sets.StatefulSets(ns).Get(name)
	if apierrors.IsNotFound(err) {
		return nil
	}
	if err != nil {
		return err
	}
	selector, err := metav1.LabelSelectorAsSelector(set.Spec.Selector)
	if err != nil || selector.Empty() {
		return nil
	}
	revs, err := s.revs.owned(ctx, set, kindStatefulSet, selector)
	if err != nil {
		return err
	}
	update, collided, err := s.revs.ensure(ctx, set, kindStatefulSet, &set.Spec.Template, revs, set.Status.CollisionCount)
	if err != nil {
		return err
	}
	if collided {
		set = set.DeepCopy()
		set.Status.CollisionCount = ptr.To(ptr.Deref(set.Status.CollisionCount, 0) + 1)
		_, err := s.client.AppsV1().StatefulSets(ns).UpdateStatus(ctx, set, metav1.UpdateOptions{})
		return firstErr(err, later(0))
	}
	current := update
	for _, rev := range revs {
		if rev.Name == set.Status.CurrentRevision {
			current = rev
		}
	}
	currentTemplate, err := templateOf(current)
	if err != nil {
		currentTemplate, current = &set.Spec.Template, update
	}

	all, err := s.pods.Pods(ns).List(labels.Everything())
	if err != nil {
		return err
	}
	owned, err := claimPods(ctx, s.client, set, kindStatefulSet, selector, all)
	if err != nil {
		return err
	}

	start := 0
	if set.Spec.Ordinals != nil {
		start = int(set.Spec.Ordinals.Start)
	}
	want := int(ptr.Deref(set.Spec.Replicas, 1))
	slots := make([]*corev1.Pod, want)
	var condemned []*corev1.Pod
	for _, p := range owned {
		ord, ok := ordinalOf(set, p)
		if ok && ord >= start && ord < start+want {
			slots[ord-start] = p
		} else {
			condemned = append(condemned, p)
		}
	}
	sort.Slice(condemned, func(i, j int) bool {
		a, _ := ordinalOf(set, condemned[i])
		b, _ := ordinalOf(set, condemned[j])
		return a > b
	})

	now := time.Now()
	var recheck time.Duration
	err = s.reconcile(ctx, set, slots, condemned, start, current, update, currentTemplate, now, &recheck)
	status := firstErr(err, s.updateStatus(ctx, set, owned, current, update, now))
	live := map[string]bool{current.Name: true, update.Name: true}
	for _, p := range owned {
		live[p.Labels[labelRevisionHash]] = true
	}
	if err := firstErr(status, s.revs.trim(ctx, revs, ptr.Deref(set.Spec.RevisionHistoryLimit, 10), live)); err != nil {
		return err
	}
	if recheck > 0 {
		return later(recheck + 100*time.Millisecond)
	}
	return nil
}

func (s *StatefulSets) reconcile(ctx context.Context, set *appsv1.StatefulSet, slots, condemned []*corev1.Pod, start int,
	current, update *appsv1.ControllerRevision, currentTemplate *corev1.PodTemplateSpec, now time.Time, recheck *time.Duration) error {
	if set.DeletionTimestamp != nil {
		return nil
	}
	ordered := set.Spec.PodManagementPolicy != appsv1.ParallelPodManagement
	rolling := set.Spec.UpdateStrategy.Type != appsv1.OnDeleteStatefulSetStrategyType
	partition := 0
	if rolling && set.Spec.UpdateStrategy.RollingUpdate != nil {
		partition = int(ptr.Deref(set.Spec.UpdateStrategy.RollingUpdate.Partition, 0))
	}
	available := func(p *corev1.Pod) bool {
		ok, wait := availableFor(p, set.Spec.MinReadySeconds, now)
		if wait > 0 && (*recheck == 0 || wait < *recheck) {
			*recheck = wait
		}
		return ok && runningAndReady(p)
	}
	healthy := true
	for i, p := range slots {
		ord := start + i
		switch {
		case p == nil:
			// Below the partition a pod comes back as it was.
			template, rev := &set.Spec.Template, update.Name
			if rolling && i < partition {
				template, rev = currentTemplate, current.Name
			}
			if err := s.ensureClaims(ctx, set, ord); err != nil {
				return err
			}
			_, err := s.client.CoreV1().Pods(set.Namespace).Create(ctx, s.newPod(set, template, rev, ord), metav1.CreateOptions{})
			if err != nil && !apierrors.IsAlreadyExists(err) {
				if apierrors.HasStatusCause(err, corev1.NamespaceTerminatingCause) {
					return nil
				}
				return err
			}
			healthy = false
			if ordered {
				return nil
			}
		case p.Status.Phase == corev1.PodFailed || p.Status.Phase == corev1.PodSucceeded:
			if p.DeletionTimestamp == nil {
				err := s.client.CoreV1().Pods(set.Namespace).Delete(ctx, p.Name, metav1.DeleteOptions{Preconditions: &metav1.Preconditions{UID: &p.UID}})
				if err != nil && !apierrors.IsNotFound(err) && !apierrors.IsConflict(err) {
					return err
				}
			}
			healthy = false
			if ordered {
				return nil
			}
		case !available(p):
			healthy = false
			if ordered {
				return nil
			}
		}
	}
	for _, p := range condemned {
		if p.DeletionTimestamp != nil {
			if ordered {
				return nil
			}
			continue
		}
		if ordered && !healthy {
			return nil
		}
		err := s.client.CoreV1().Pods(set.Namespace).Delete(ctx, p.Name, metav1.DeleteOptions{Preconditions: &metav1.Preconditions{UID: &p.UID}})
		if err != nil && !apierrors.IsNotFound(err) && !apierrors.IsConflict(err) {
			return err
		}
		if ordered {
			return nil
		}
	}
	if !rolling || !healthy {
		return nil
	}
	// One pod at a time, from the top down to the partition.
	for i := len(slots) - 1; i >= partition && i >= 0; i-- {
		p := slots[i]
		if p == nil || p.DeletionTimestamp != nil {
			return nil
		}
		if p.Labels[labelRevisionHash] != update.Name {
			err := s.client.CoreV1().Pods(set.Namespace).Delete(ctx, p.Name, metav1.DeleteOptions{Preconditions: &metav1.Preconditions{UID: &p.UID}})
			if err != nil && !apierrors.IsNotFound(err) && !apierrors.IsConflict(err) {
				return err
			}
			return nil
		}
		if !available(p) {
			return nil
		}
	}
	return nil
}

func (s *StatefulSets) updateStatus(ctx context.Context, set *appsv1.StatefulSet, owned []*corev1.Pod,
	current, update *appsv1.ControllerRevision, now time.Time) error {
	st := appsv1.StatefulSetStatus{
		ObservedGeneration: set.Generation,
		CollisionCount:     set.Status.CollisionCount,
		Conditions:         set.Status.Conditions,
		CurrentRevision:    current.Name,
		UpdateRevision:     update.Name,
	}
	for _, p := range owned {
		if p.DeletionTimestamp != nil && p.Status.Phase != corev1.PodRunning {
			continue
		}
		st.Replicas++
		if runningAndReady(p) {
			st.ReadyReplicas++
			if ok, _ := availableFor(p, set.Spec.MinReadySeconds, now); ok {
				st.AvailableReplicas++
			}
		}
		switch p.Labels[labelRevisionHash] {
		case current.Name:
			st.CurrentReplicas++
		}
		if p.Labels[labelRevisionHash] == update.Name {
			st.UpdatedReplicas++
		}
	}
	// A finished rollout makes the update revision the current one.
	want := ptr.Deref(set.Spec.Replicas, 1)
	if st.UpdatedReplicas == want && st.ReadyReplicas == want && st.Replicas == want {
		st.CurrentRevision = update.Name
		st.CurrentReplicas = st.UpdatedReplicas
	}
	if apiequality.Semantic.DeepEqual(set.Status, st) {
		return nil
	}
	set = set.DeepCopy()
	set.Status = st
	_, err := s.client.AppsV1().StatefulSets(set.Namespace).UpdateStatus(ctx, set, metav1.UpdateOptions{})
	if apierrors.IsNotFound(err) {
		return nil
	}
	if apierrors.IsConflict(err) {
		return later(time.Second)
	}
	return err
}
