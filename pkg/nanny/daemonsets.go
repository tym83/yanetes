package nanny

import (
	"context"
	"fmt"
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
	"k8s.io/apimachinery/pkg/util/intstr"
	"k8s.io/client-go/informers"
	"k8s.io/client-go/kubernetes"
	appslisters "k8s.io/client-go/listers/apps/v1"
	corelisters "k8s.io/client-go/listers/core/v1"
	"k8s.io/client-go/tools/cache"
	corev1helpers "k8s.io/component-helpers/scheduling/corev1"
	"k8s.io/component-helpers/scheduling/corev1/nodeaffinity"
	"k8s.io/klog/v2"
	"k8s.io/utils/ptr"
)

var kindDaemonSet = schemaKind{"apps", "v1", "DaemonSet"}

// DaemonSets runs one pod of each daemon set on every node it fits.
type DaemonSets struct {
	client kubernetes.Interface
	sets   appslisters.DaemonSetLister
	pods   corelisters.PodLister
	nodes  corelisters.NodeLister
	revs   revisions
	exp    *expectations
	loop   *loop
}

func NewDaemonSets(client kubernetes.Interface, factory informers.SharedInformerFactory) *DaemonSets {
	d := &DaemonSets{
		client: client,
		sets:   factory.Apps().V1().DaemonSets().Lister(),
		pods:   factory.Core().V1().Pods().Lister(),
		nodes:  factory.Core().V1().Nodes().Lister(),
		revs:   revisions{client: client, lister: factory.Apps().V1().ControllerRevisions().Lister()},
		exp:    newExpectations(),
	}
	d.loop = newLoop("daemonset", 5, d.sync)
	_, _ = factory.Apps().V1().DaemonSets().Informer().AddEventHandler(d.loop.handlers())
	_ = factory.Apps().V1().ControllerRevisions().Informer()
	_, _ = factory.Core().V1().Pods().Informer().AddEventHandler(cache.ResourceEventHandlerFuncs{
		AddFunc: func(obj any) {
			if key := d.ownerKey(obj.(*corev1.Pod)); key != "" {
				d.exp.observe(key, 1, 0)
				d.loop.add(key)
			} else {
				d.wakeAll(obj.(*corev1.Pod).Namespace)
			}
		},
		UpdateFunc: func(old, obj any) {
			if key := d.ownerKey(obj.(*corev1.Pod)); key != "" {
				// A pod being deleted is as good as gone for the count.
				if old.(*corev1.Pod).DeletionTimestamp == nil && obj.(*corev1.Pod).DeletionTimestamp != nil {
					d.exp.observe(key, 0, 1)
				}
				d.loop.add(key)
			} else {
				d.wakeAll(obj.(*corev1.Pod).Namespace)
			}
			if key := d.ownerKey(old.(*corev1.Pod)); key != "" {
				d.loop.add(key)
			}
		},
		DeleteFunc: func(obj any) {
			pod, ok := unwrap(obj).(*corev1.Pod)
			if !ok {
				return
			}
			if key := d.ownerKey(pod); key != "" {
				if pod.DeletionTimestamp == nil {
					d.exp.observe(key, 0, 1)
				}
				d.loop.add(key)
			}
		},
	})
	nodeChanged := func(obj any) { d.wakeAll("") }
	_, _ = factory.Core().V1().Nodes().Informer().AddEventHandler(cache.ResourceEventHandlerFuncs{
		AddFunc: nodeChanged, UpdateFunc: func(_, obj any) { nodeChanged(obj) }, DeleteFunc: nodeChanged,
	})
	return d
}

func (d *DaemonSets) Run(ctx context.Context) { d.loop.run(ctx) }

func (d *DaemonSets) ownerKey(pod *corev1.Pod) string {
	ref := metav1.GetControllerOf(pod)
	if ref == nil || ref.Kind != "DaemonSet" {
		return ""
	}
	return pod.Namespace + "/" + ref.Name
}

func (d *DaemonSets) wakeAll(ns string) {
	sets, _ := d.sets.List(labels.Everything())
	for _, ds := range sets {
		if ns == "" || ds.Namespace == ns {
			d.loop.add(ds.Namespace + "/" + ds.Name)
		}
	}
}

// daemonTolerations are the ones upstream gives every daemon pod: a
// daemon stays with its node whatever the node's condition.
func daemonTolerations(spec *corev1.PodSpec) []corev1.Toleration {
	out := append([]corev1.Toleration(nil), spec.Tolerations...)
	add := func(key string, effect corev1.TaintEffect) {
		for _, t := range out {
			if t.Key == key && t.Effect == effect && t.Operator == corev1.TolerationOpExists {
				return
			}
		}
		out = append(out, corev1.Toleration{Key: key, Operator: corev1.TolerationOpExists, Effect: effect})
	}
	add(corev1.TaintNodeNotReady, corev1.TaintEffectNoExecute)
	add(corev1.TaintNodeUnreachable, corev1.TaintEffectNoExecute)
	add(corev1.TaintNodeDiskPressure, corev1.TaintEffectNoSchedule)
	add(corev1.TaintNodeMemoryPressure, corev1.TaintEffectNoSchedule)
	add(corev1.TaintNodePIDPressure, corev1.TaintEffectNoSchedule)
	add(corev1.TaintNodeUnschedulable, corev1.TaintEffectNoSchedule)
	if spec.HostNetwork {
		add(corev1.TaintNodeNetworkUnavailable, corev1.TaintEffectNoSchedule)
	}
	return out
}

// fits says whether a daemon pod should run on the node, and whether one
// already there may stay: a NoSchedule taint keeps new pods off but does
// not evict.
func fits(template *corev1.PodTemplateSpec, node *corev1.Node) (run, stay bool) {
	pod := &corev1.Pod{Spec: *template.Spec.DeepCopy()}
	pod.Spec.Tolerations = daemonTolerations(&pod.Spec)
	if ok, _ := nodeaffinity.GetRequiredNodeAffinity(pod).Match(node); !ok {
		return false, false
	}
	untolerated := func(effects ...corev1.TaintEffect) bool {
		_, found := corev1helpers.FindMatchingUntoleratedTaint(klog.Background(), node.Spec.Taints, pod.Spec.Tolerations,
			func(t *corev1.Taint) bool {
				for _, e := range effects {
					if t.Effect == e {
						return true
					}
				}
				return false
			}, false)
		return found
	}
	if untolerated(corev1.TaintEffectNoExecute) {
		return false, false
	}
	return !untolerated(corev1.TaintEffectNoSchedule), true
}

// targetNode is where a daemon pod is or will be: its node, or the node
// its affinity pins it to before the scheduler binds it.
func targetNode(pod *corev1.Pod) string {
	if pod.Spec.NodeName != "" {
		return pod.Spec.NodeName
	}
	a := pod.Spec.Affinity
	if a == nil || a.NodeAffinity == nil || a.NodeAffinity.RequiredDuringSchedulingIgnoredDuringExecution == nil {
		return ""
	}
	for _, term := range a.NodeAffinity.RequiredDuringSchedulingIgnoredDuringExecution.NodeSelectorTerms {
		for _, f := range term.MatchFields {
			if f.Key == "metadata.name" && f.Operator == corev1.NodeSelectorOpIn && len(f.Values) == 1 {
				return f.Values[0]
			}
		}
	}
	return ""
}

func (d *DaemonSets) newPod(ds *appsv1.DaemonSet, hash, node string) *corev1.Pod {
	t := ds.Spec.Template.DeepCopy()
	podLabels := maps.Clone(t.Labels)
	if podLabels == nil {
		podLabels = map[string]string{}
	}
	podLabels[labelRevisionHash] = hash
	podLabels["pod-template-generation"] = strconv.FormatInt(ds.Generation, 10)
	spec := t.Spec
	spec.Tolerations = daemonTolerations(&spec)
	// Pinned through affinity, as upstream does, so the scheduler still
	// checks resources and ports before binding.
	if spec.Affinity == nil {
		spec.Affinity = &corev1.Affinity{}
	}
	if spec.Affinity.NodeAffinity == nil {
		spec.Affinity.NodeAffinity = &corev1.NodeAffinity{}
	}
	spec.Affinity.NodeAffinity.RequiredDuringSchedulingIgnoredDuringExecution = &corev1.NodeSelector{
		NodeSelectorTerms: []corev1.NodeSelectorTerm{{MatchFields: []corev1.NodeSelectorRequirement{
			{Key: "metadata.name", Operator: corev1.NodeSelectorOpIn, Values: []string{node}},
		}}},
	}
	return &corev1.Pod{
		ObjectMeta: metav1.ObjectMeta{
			GenerateName:    ds.Name + "-",
			Namespace:       ds.Namespace,
			Labels:          podLabels,
			Annotations:     t.Annotations,
			Finalizers:      t.Finalizers,
			OwnerReferences: []metav1.OwnerReference{*metav1.NewControllerRef(ds, kindDaemonSet.gvk())},
		},
		Spec: spec,
	}
}

type nodePlan struct {
	node        *corev1.Node
	run, stay   bool
	pods        []*corev1.Pod // live ones, oldest first
	failed      []*corev1.Pod
	updated     *corev1.Pod
	old         []*corev1.Pod
	available   bool // a live pod on the node is available
	newAvail    bool
	unavailable bool
}

func (d *DaemonSets) sync(ctx context.Context, key string) error {
	ns, name, _ := cache.SplitMetaNamespaceKey(key)
	ds, err := d.sets.DaemonSets(ns).Get(name)
	if apierrors.IsNotFound(err) {
		d.exp.forget(key)
		return nil
	}
	if err != nil {
		return err
	}
	selector, err := metav1.LabelSelectorAsSelector(ds.Spec.Selector)
	if err != nil || selector.Empty() {
		return nil
	}
	revs, err := d.revs.owned(ctx, ds, kindDaemonSet, selector)
	if err != nil {
		return err
	}
	current, collided, err := d.revs.ensure(ctx, ds, kindDaemonSet, &ds.Spec.Template, revs, ds.Status.CollisionCount)
	if err != nil {
		return err
	}
	if collided {
		ds = ds.DeepCopy()
		ds.Status.CollisionCount = ptr.To(ptr.Deref(ds.Status.CollisionCount, 0) + 1)
		_, err := d.client.AppsV1().DaemonSets(ns).UpdateStatus(ctx, ds, metav1.UpdateOptions{})
		return firstErr(err, later(0))
	}
	hash := current.Labels[labelRevisionHash]

	all, err := d.pods.Pods(ns).List(labels.Everything())
	if err != nil {
		return err
	}
	owned, err := claimPods(ctx, d.client, ds, kindDaemonSet, selector, all)
	if err != nil {
		return err
	}
	nodes, err := d.nodes.List(labels.Everything())
	if err != nil {
		return err
	}
	now := time.Now()
	var recheck time.Duration
	plans := map[string]*nodePlan{}
	for _, n := range nodes {
		run, stay := fits(&ds.Spec.Template, n)
		plans[n.Name] = &nodePlan{node: n, run: run, stay: stay}
	}
	var orphans []*corev1.Pod
	sort.Slice(owned, func(i, j int) bool { return owned[i].CreationTimestamp.Before(&owned[j].CreationTimestamp) })
	for _, p := range owned {
		plan := plans[targetNode(p)]
		if plan == nil {
			if p.DeletionTimestamp == nil {
				orphans = append(orphans, p)
			}
			continue
		}
		if p.DeletionTimestamp != nil {
			continue
		}
		if p.Status.Phase == corev1.PodFailed {
			plan.failed = append(plan.failed, p)
			continue
		}
		plan.pods = append(plan.pods, p)
		ok, wait := availableFor(p, ds.Spec.MinReadySeconds, now)
		if wait > 0 && (recheck == 0 || wait < recheck) {
			recheck = wait
		}
		if ok {
			plan.available = true
		}
		if p.Labels[labelRevisionHash] == hash {
			if plan.updated == nil {
				plan.updated = p
				plan.newAvail = ok
			}
		} else {
			plan.old = append(plan.old, p)
		}
	}

	var create []string
	var remove []*corev1.Pod
	remove = append(remove, orphans...)
	rolling := ds.Spec.UpdateStrategy.Type != appsv1.OnDeleteDaemonSetStrategyType
	surge, maxUnavailable := daemonBounds(ds, plans)
	unavailable, surging := 0, 0
	for _, plan := range plans {
		if plan.run && !plan.available {
			unavailable++
		}
		if plan.updated != nil && len(plan.old) > 0 {
			surging++
		}
	}
	names := make([]string, 0, len(plans))
	for n := range plans {
		names = append(names, n)
	}
	sort.Strings(names)
	for _, n := range names {
		plan := plans[n]
		// Failed pods go, and come back on the next pass.
		remove = append(remove, plan.failed...)
		if !plan.stay {
			remove = append(remove, plan.pods...)
			continue
		}
		if len(plan.pods) == 0 {
			if plan.run && len(plan.failed) == 0 {
				create = append(create, n)
			}
			continue
		}
		if !rolling || len(plan.old) == 0 {
			// More than one with nothing to roll: keep the oldest.
			if len(plan.pods) > 1 && plan.updated != nil {
				for _, p := range plan.pods {
					if p != plan.updated {
						remove = append(remove, p)
					}
				}
			} else if len(plan.pods) > 1 {
				remove = append(remove, plan.pods[1:]...)
			}
			continue
		}
		switch {
		case surge > 0:
			if plan.updated == nil {
				if !plan.available || surging < surge {
					// An old pod that serves nothing is replaced at once.
					if !plan.available {
						remove = append(remove, plan.old...)
					} else {
						surging++
					}
					create = append(create, n)
				}
			} else if plan.newAvail {
				remove = append(remove, plan.old...)
			}
		default:
			if plan.updated != nil {
				remove = append(remove, plan.old...)
				continue
			}
			if !plan.available {
				remove = append(remove, plan.old...)
			} else if unavailable < maxUnavailable {
				unavailable++
				remove = append(remove, plan.old...)
			}
		}
	}

	var actErr error
	if ds.DeletionTimestamp == nil && d.exp.satisfied(key) && (len(create) > 0 || len(remove) > 0) {
		d.exp.expect(key, len(create), len(remove))
		for _, node := range create {
			_, err := d.client.CoreV1().Pods(ns).Create(ctx, d.newPod(ds, hash, node), metav1.CreateOptions{})
			if err != nil {
				d.exp.observe(key, 1, 0)
				if !apierrors.HasStatusCause(err, corev1.NamespaceTerminatingCause) {
					actErr = firstErr(actErr, err)
				}
			}
		}
		for _, p := range remove {
			err := d.client.CoreV1().Pods(ns).Delete(ctx, p.Name, metav1.DeleteOptions{Preconditions: &metav1.Preconditions{UID: &p.UID}})
			if err != nil {
				d.exp.observe(key, 0, 1)
				if !apierrors.IsNotFound(err) && !apierrors.IsConflict(err) {
					actErr = firstErr(actErr, err)
				}
			}
		}
	}

	live := map[string]bool{current.Name: true}
	for _, rev := range revs {
		for _, p := range owned {
			if p.Labels[labelRevisionHash] == rev.Labels[labelRevisionHash] {
				live[rev.Name] = true
			}
		}
	}
	trimErr := d.revs.trim(ctx, revs, ptr.Deref(ds.Spec.RevisionHistoryLimit, 10), live)
	statusErr := d.updateStatus(ctx, ds, plans, hash, now)
	if err := firstErr(actErr, trimErr, statusErr); err != nil {
		return err
	}
	if recheck > 0 {
		return later(recheck + 100*time.Millisecond)
	}
	return nil
}

// daemonBounds resolves maxSurge and maxUnavailable against the number of
// nodes that should run the daemon.
func daemonBounds(ds *appsv1.DaemonSet, plans map[string]*nodePlan) (int, int) {
	desired := 0
	for _, p := range plans {
		if p.run {
			desired++
		}
	}
	ru := ds.Spec.UpdateStrategy.RollingUpdate
	surgeV, unavailV := intstr.FromInt32(0), intstr.FromInt32(1)
	if ru != nil && ru.MaxSurge != nil {
		surgeV = *ru.MaxSurge
	}
	if ru != nil && ru.MaxUnavailable != nil {
		unavailV = *ru.MaxUnavailable
	}
	surge, _ := intstr.GetScaledValueFromIntOrPercent(&surgeV, desired, true)
	unavail, _ := intstr.GetScaledValueFromIntOrPercent(&unavailV, desired, true)
	if surge == 0 && unavail == 0 {
		unavail = 1
	}
	return surge, unavail
}

func (d *DaemonSets) updateStatus(ctx context.Context, ds *appsv1.DaemonSet, plans map[string]*nodePlan, hash string, now time.Time) error {
	s := appsv1.DaemonSetStatus{
		ObservedGeneration: ds.Generation,
		CollisionCount:     ds.Status.CollisionCount,
		Conditions:         ds.Status.Conditions,
	}
	for _, plan := range plans {
		if plan.run {
			s.DesiredNumberScheduled++
		}
		if len(plan.pods) == 0 {
			continue
		}
		if !plan.run {
			s.NumberMisscheduled++
			continue
		}
		s.CurrentNumberScheduled++
		p := plan.pods[0]
		if plan.updated != nil {
			p = plan.updated
			s.UpdatedNumberScheduled++
		}
		if podReady(p) {
			s.NumberReady++
		}
		if ok, _ := availableFor(p, ds.Spec.MinReadySeconds, now); ok {
			s.NumberAvailable++
		}
	}
	s.NumberUnavailable = max(0, s.DesiredNumberScheduled-s.NumberAvailable)
	if apiequality.Semantic.DeepEqual(ds.Status, s) {
		return nil
	}
	ds = ds.DeepCopy()
	ds.Status = s
	_, err := d.client.AppsV1().DaemonSets(ds.Namespace).UpdateStatus(ctx, ds, metav1.UpdateOptions{})
	if apierrors.IsNotFound(err) {
		return nil
	}
	if apierrors.IsConflict(err) {
		return later(time.Second)
	}
	if err != nil {
		return fmt.Errorf("daemon set status: %w", err)
	}
	return nil
}
