package nanny

import (
	"context"
	"strings"
	"sync"
	"time"

	corev1 "k8s.io/api/core/v1"
	apierrors "k8s.io/apimachinery/pkg/api/errors"
	"k8s.io/apimachinery/pkg/api/resource"
	metav1 "k8s.io/apimachinery/pkg/apis/meta/v1"
	"k8s.io/apimachinery/pkg/labels"
	"k8s.io/apimachinery/pkg/runtime/schema"
	"k8s.io/apimachinery/pkg/selection"
	"k8s.io/client-go/informers"
	"k8s.io/client-go/kubernetes"
	corelisters "k8s.io/client-go/listers/core/v1"
	"k8s.io/client-go/metadata"
	"k8s.io/client-go/tools/cache"
	resourcehelper "k8s.io/component-helpers/resource"
)

const quotaResync = 30 * time.Second

// ResourceQuotas recomputes what each quota's namespace uses. yapi charges
// quotas as objects are admitted; this is the count that corrects drift.
type ResourceQuotas struct {
	client  kubernetes.Interface
	meta    metadata.Interface
	quotas  corelisters.ResourceQuotaLister
	pods    corelisters.PodLister
	svcs    corelisters.ServiceLister
	cms     corelisters.ConfigMapLister
	secrets corelisters.SecretLister
	pvcs    corelisters.PersistentVolumeClaimLister
	rcs     corelisters.ReplicationControllerLister
	loop    *loop

	mu        sync.Mutex
	versions  map[string]string // group -> preferred version
	listedAt  time.Time
	discovery func() (map[string]string, error)
}

func NewResourceQuotas(client kubernetes.Interface, meta metadata.Interface, factory informers.SharedInformerFactory) *ResourceQuotas {
	q := &ResourceQuotas{
		client:  client,
		meta:    meta,
		quotas:  factory.Core().V1().ResourceQuotas().Lister(),
		pods:    factory.Core().V1().Pods().Lister(),
		svcs:    factory.Core().V1().Services().Lister(),
		cms:     factory.Core().V1().ConfigMaps().Lister(),
		secrets: factory.Core().V1().Secrets().Lister(),
		pvcs:    factory.Core().V1().PersistentVolumeClaims().Lister(),
		rcs:     factory.Core().V1().ReplicationControllers().Lister(),
	}
	q.discovery = func() (map[string]string, error) {
		groups, err := client.Discovery().ServerGroups()
		if err != nil {
			return nil, err
		}
		out := map[string]string{}
		for _, g := range groups.Groups {
			out[g.Name] = g.PreferredVersion.Version
		}
		return out, nil
	}
	q.loop = newLoop("resourcequota", 5, q.sync)
	_, _ = factory.Core().V1().ResourceQuotas().Informer().AddEventHandler(q.loop.handlers())
	byNamespace := func(obj any) {
		if o, ok := unwrap(obj).(metav1.Object); ok {
			q.wake(o.GetNamespace())
		}
	}
	tracked := cache.ResourceEventHandlerFuncs{
		AddFunc: byNamespace, UpdateFunc: func(_, obj any) { byNamespace(obj) }, DeleteFunc: byNamespace,
	}
	for _, inf := range []cache.SharedIndexInformer{
		factory.Core().V1().Pods().Informer(), factory.Core().V1().Services().Informer(),
		factory.Core().V1().ConfigMaps().Informer(), factory.Core().V1().Secrets().Informer(),
		factory.Core().V1().PersistentVolumeClaims().Informer(), factory.Core().V1().ReplicationControllers().Informer(),
	} {
		_, _ = inf.AddEventHandler(tracked)
	}
	return q
}

func (q *ResourceQuotas) wake(ns string) {
	quotas, _ := q.quotas.ResourceQuotas(ns).List(labels.Everything())
	for _, rq := range quotas {
		q.loop.add(rq.Namespace + "/" + rq.Name)
	}
}

func (q *ResourceQuotas) Run(ctx context.Context) {
	go func() {
		t := time.NewTicker(quotaResync)
		defer t.Stop()
		for {
			select {
			case <-ctx.Done():
				return
			case <-t.C:
				quotas, _ := q.quotas.List(labels.Everything())
				for _, rq := range quotas {
					q.loop.add(rq.Namespace + "/" + rq.Name)
				}
			}
		}
	}()
	q.loop.run(ctx)
}

// usage adds up quantities, starting each at zero.
type usage map[corev1.ResourceName]resource.Quantity

func (u usage) add(name corev1.ResourceName, v resource.Quantity) {
	cur, ok := u[name]
	if !ok {
		cur = *resource.NewQuantity(0, v.Format)
	}
	cur.Add(v)
	u[name] = cur
}

func (u usage) count(name corev1.ResourceName, n int64) {
	u.add(name, *resource.NewQuantity(n, resource.DecimalSI))
}

func podTerminal(p *corev1.Pod) bool {
	if p.Status.Phase == corev1.PodSucceeded || p.Status.Phase == corev1.PodFailed {
		return true
	}
	// A pod past its deletion grace no longer holds its quota, as upstream counts it.
	if p.DeletionTimestamp != nil && p.DeletionGracePeriodSeconds != nil {
		end := p.DeletionTimestamp.Add(time.Duration(*p.DeletionGracePeriodSeconds) * time.Second)
		return time.Now().After(end)
	}
	return false
}

func podBestEffort(p *corev1.Pod) bool {
	if p.Status.QOSClass != "" {
		return p.Status.QOSClass == corev1.PodQOSBestEffort
	}
	for _, list := range [][]corev1.Container{p.Spec.InitContainers, p.Spec.Containers} {
		for _, c := range list {
			if len(c.Resources.Requests) > 0 || len(c.Resources.Limits) > 0 {
				return false
			}
		}
	}
	return true
}

func podCrossNamespaceAffinity(p *corev1.Pod) bool {
	a := p.Spec.Affinity
	if a == nil {
		return false
	}
	cross := func(t corev1.PodAffinityTerm) bool { return len(t.Namespaces) > 0 || t.NamespaceSelector != nil }
	var terms []corev1.PodAffinityTerm
	if a.PodAffinity != nil {
		terms = append(terms, a.PodAffinity.RequiredDuringSchedulingIgnoredDuringExecution...)
		for _, w := range a.PodAffinity.PreferredDuringSchedulingIgnoredDuringExecution {
			terms = append(terms, w.PodAffinityTerm)
		}
	}
	if a.PodAntiAffinity != nil {
		terms = append(terms, a.PodAntiAffinity.RequiredDuringSchedulingIgnoredDuringExecution...)
		for _, w := range a.PodAntiAffinity.PreferredDuringSchedulingIgnoredDuringExecution {
			terms = append(terms, w.PodAffinityTerm)
		}
	}
	for _, t := range terms {
		if cross(t) {
			return true
		}
	}
	return false
}

// scopeMatches tells whether a pod falls in one scope requirement.
func scopeMatches(p *corev1.Pod, req corev1.ScopedResourceSelectorRequirement) bool {
	switch req.ScopeName {
	case corev1.ResourceQuotaScopeTerminating:
		return p.Spec.ActiveDeadlineSeconds != nil && *p.Spec.ActiveDeadlineSeconds >= 0
	case corev1.ResourceQuotaScopeNotTerminating:
		return p.Spec.ActiveDeadlineSeconds == nil
	case corev1.ResourceQuotaScopeBestEffort:
		return podBestEffort(p)
	case corev1.ResourceQuotaScopeNotBestEffort:
		return !podBestEffort(p)
	case corev1.ResourceQuotaScopeCrossNamespacePodAffinity:
		return podCrossNamespaceAffinity(p)
	case corev1.ResourceQuotaScopePriorityClass:
		var op selection.Operator
		switch req.Operator {
		case corev1.ScopeSelectorOpIn:
			op = selection.In
		case corev1.ScopeSelectorOpNotIn:
			op = selection.NotIn
		case corev1.ScopeSelectorOpExists:
			op = selection.Exists
		case corev1.ScopeSelectorOpDoesNotExist:
			op = selection.DoesNotExist
		default:
			return false
		}
		r, err := labels.NewRequirement(string(req.ScopeName), op, req.Values)
		if err != nil {
			return false
		}
		set := labels.Set{}
		if p.Spec.PriorityClassName != "" {
			set[string(req.ScopeName)] = p.Spec.PriorityClassName
		}
		return r.Matches(set)
	}
	return false
}

func quotaScopes(rq *corev1.ResourceQuota) []corev1.ScopedResourceSelectorRequirement {
	var reqs []corev1.ScopedResourceSelectorRequirement
	for _, s := range rq.Spec.Scopes {
		reqs = append(reqs, corev1.ScopedResourceSelectorRequirement{ScopeName: s, Operator: corev1.ScopeSelectorOpExists})
	}
	if rq.Spec.ScopeSelector != nil {
		reqs = append(reqs, rq.Spec.ScopeSelector.MatchExpressions...)
	}
	return reqs
}

// podResource tells whether a quota resource is one pods are charged for:
// a scoped quota counts only these.
func podResource(name corev1.ResourceName) bool {
	n := string(name)
	return n == "pods" || n == "count/pods" || n == "cpu" || n == "memory" || n == "ephemeral-storage" ||
		strings.HasPrefix(n, "requests.") || strings.HasPrefix(n, "limits.") || strings.HasPrefix(n, "hugepages-")
}

func (q *ResourceQuotas) groupVersion(group string) (string, error) {
	q.mu.Lock()
	defer q.mu.Unlock()
	if q.versions == nil || time.Since(q.listedAt) > quotaResync {
		v, err := q.discovery()
		if err != nil {
			return "", err
		}
		q.versions, q.listedAt = v, time.Now()
	}
	return q.versions[group], nil
}

func (q *ResourceQuotas) sync(ctx context.Context, key string) error {
	ns, name, _ := cache.SplitMetaNamespaceKey(key)
	rq, err := q.quotas.ResourceQuotas(ns).Get(name)
	if apierrors.IsNotFound(err) {
		return nil
	}
	if err != nil {
		return err
	}
	scopes := quotaScopes(rq)
	used := usage{}
	want := func(n corev1.ResourceName) bool {
		_, ok := rq.Spec.Hard[n]
		return ok && (len(scopes) == 0 || podResource(n))
	}

	pods, _ := q.pods.Pods(ns).List(labels.Everything())
	for _, p := range pods {
		if podTerminal(p) {
			continue
		}
		matched := true
		for _, s := range scopes {
			matched = matched && scopeMatches(p, s)
		}
		if !matched {
			continue
		}
		used.count(corev1.ResourcePods, 1)
		used.count("count/pods", 1)
		requests := resourcehelper.PodRequests(p, resourcehelper.PodResourcesOptions{})
		limits := resourcehelper.PodLimits(p, resourcehelper.PodResourcesOptions{})
		for n, v := range requests {
			used.add(corev1.ResourceName("requests."+string(n)), v)
			if n == corev1.ResourceCPU || n == corev1.ResourceMemory || n == corev1.ResourceEphemeralStorage {
				used.add(n, v)
			}
		}
		for n, v := range limits {
			used.add(corev1.ResourceName("limits."+string(n)), v)
		}
	}

	if len(scopes) == 0 {
		svcs, _ := q.svcs.Services(ns).List(labels.Everything())
		for _, s := range svcs {
			used.count(corev1.ResourceServices, 1)
			used.count("count/services", 1)
			if s.Spec.Type == corev1.ServiceTypeLoadBalancer {
				used.count(corev1.ResourceServicesLoadBalancers, 1)
			}
			if s.Spec.Type == corev1.ServiceTypeNodePort || s.Spec.Type == corev1.ServiceTypeLoadBalancer {
				used.count(corev1.ResourceServicesNodePorts, int64(len(s.Spec.Ports)))
			}
		}
		cms, _ := q.cms.ConfigMaps(ns).List(labels.Everything())
		used.count(corev1.ResourceConfigMaps, int64(len(cms)))
		used.count("count/configmaps", int64(len(cms)))
		secrets, _ := q.secrets.Secrets(ns).List(labels.Everything())
		used.count(corev1.ResourceSecrets, int64(len(secrets)))
		used.count("count/secrets", int64(len(secrets)))
		rcs, _ := q.rcs.ReplicationControllers(ns).List(labels.Everything())
		used.count(corev1.ResourceReplicationControllers, int64(len(rcs)))
		used.count("count/replicationcontrollers", int64(len(rcs)))
		quotas, _ := q.quotas.ResourceQuotas(ns).List(labels.Everything())
		used.count(corev1.ResourceQuotas, int64(len(quotas)))
		used.count("count/resourcequotas", int64(len(quotas)))
		pvcs, _ := q.pvcs.PersistentVolumeClaims(ns).List(labels.Everything())
		for _, c := range pvcs {
			used.count(corev1.ResourcePersistentVolumeClaims, 1)
			used.count("count/persistentvolumeclaims", 1)
			storage, ok := c.Spec.Resources.Requests[corev1.ResourceStorage]
			if ok {
				used.add(corev1.ResourceRequestsStorage, storage)
			}
			if class := ptrString(c.Spec.StorageClassName); class != "" {
				prefix := class + ".storageclass.storage.k8s.io/"
				used.count(corev1.ResourceName(prefix+"persistentvolumeclaims"), 1)
				if ok {
					used.add(corev1.ResourceName(prefix+"requests.storage"), storage)
				}
			}
		}
		// count/<resource>.<group> of anything else, from metadata alone.
		for n := range rq.Spec.Hard {
			s := string(n)
			if !strings.HasPrefix(s, "count/") {
				continue
			}
			if _, done := used[n]; done {
				continue
			}
			res, group, _ := strings.Cut(strings.TrimPrefix(s, "count/"), ".")
			version := "v1"
			if group != "" {
				v, err := q.groupVersion(group)
				if err != nil {
					return err
				}
				if v == "" {
					continue
				}
				version = v
			}
			list, err := q.meta.Resource(schema.GroupVersionResource{Group: group, Version: version, Resource: res}).
				Namespace(ns).List(ctx, metav1.ListOptions{})
			if err != nil {
				if apierrors.IsNotFound(err) {
					continue
				}
				return err
			}
			used.count(n, int64(len(list.Items)))
		}
	}

	status := corev1.ResourceQuotaStatus{Hard: rq.Spec.Hard.DeepCopy(), Used: corev1.ResourceList{}}
	for n := range rq.Spec.Hard {
		if !want(n) {
			continue
		}
		if v, ok := used[n]; ok {
			status.Used[n] = v
		} else {
			status.Used[n] = *resource.NewQuantity(0, resource.DecimalSI)
		}
	}
	if quantitiesEqual(rq.Status.Hard, status.Hard) && quantitiesEqual(rq.Status.Used, status.Used) {
		return nil
	}
	rq = rq.DeepCopy()
	rq.Status = status
	_, err = q.client.CoreV1().ResourceQuotas(ns).UpdateStatus(ctx, rq, metav1.UpdateOptions{})
	if apierrors.IsConflict(err) {
		return later(time.Second)
	}
	if apierrors.IsNotFound(err) {
		return nil
	}
	return err
}

func quantitiesEqual(a, b corev1.ResourceList) bool {
	if len(a) != len(b) {
		return false
	}
	for n, v := range a {
		w, ok := b[n]
		if !ok || v.Cmp(w) != 0 {
			return false
		}
	}
	return true
}

func ptrString(s *string) string {
	if s == nil {
		return ""
	}
	return *s
}
