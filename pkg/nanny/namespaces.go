package nanny

import (
	"context"
	"fmt"
	"slices"
	"strings"
	"sync"
	"time"

	corev1 "k8s.io/api/core/v1"
	apierrors "k8s.io/apimachinery/pkg/api/errors"
	metav1 "k8s.io/apimachinery/pkg/apis/meta/v1"
	"k8s.io/apimachinery/pkg/runtime/schema"
	"k8s.io/client-go/discovery"
	"k8s.io/client-go/informers"
	"k8s.io/client-go/kubernetes"
	corelisters "k8s.io/client-go/listers/core/v1"
	"k8s.io/client-go/metadata"
	"k8s.io/client-go/tools/cache"
)

// Namespaces empties a namespace that is being deleted and then lets it go:
// upstream's namespace controller, minus the status conditions nobody reads.
type Namespaces struct {
	client    kubernetes.Interface
	meta      metadata.Interface
	discovery discovery.DiscoveryInterface
	lister    corelisters.NamespaceLister
	loop      *loop

	mu        sync.Mutex
	resources []schema.GroupVersionResource
	listedAt  time.Time
}

func NewNamespaces(client kubernetes.Interface, meta metadata.Interface, factory informers.SharedInformerFactory) *Namespaces {
	n := &Namespaces{
		client:    client,
		meta:      meta,
		discovery: client.Discovery(),
		lister:    factory.Core().V1().Namespaces().Lister(),
	}
	n.loop = newLoop("namespace", 5, n.sync)
	_, _ = factory.Core().V1().Namespaces().Informer().AddEventHandler(n.loop.handlers())
	return n
}

func (n *Namespaces) Run(ctx context.Context) { n.loop.run(ctx) }

// deletable lists the namespaced resources a namespace can hold, again at
// most every half a minute so new CRDs are seen.
func (n *Namespaces) deletable() ([]schema.GroupVersionResource, error) {
	n.mu.Lock()
	defer n.mu.Unlock()
	if time.Since(n.listedAt) < 30*time.Second && n.resources != nil {
		return n.resources, nil
	}
	lists, err := n.discovery.ServerPreferredNamespacedResources()
	if err != nil && len(lists) == 0 {
		return nil, err
	}
	var out []schema.GroupVersionResource
	for _, list := range lists {
		gv, err := schema.ParseGroupVersion(list.GroupVersion)
		if err != nil {
			continue
		}
		for _, r := range list.APIResources {
			if slices.Contains(r.Verbs, "list") && slices.Contains(r.Verbs, "delete") {
				out = append(out, gv.WithResource(r.Name))
			}
		}
	}
	n.resources, n.listedAt = out, time.Now()
	return out, nil
}

func (n *Namespaces) sync(ctx context.Context, name string) error {
	ns, err := n.lister.Get(name)
	if apierrors.IsNotFound(err) {
		return nil
	}
	if err != nil {
		return err
	}
	if ns.DeletionTimestamp == nil || !slices.Contains(ns.Spec.Finalizers, corev1.FinalizerKubernetes) {
		return nil
	}
	resources, err := n.deletable()
	if err != nil {
		return err
	}
	// Discovery that does not even list pods is broken, not empty: a
	// namespace finalized on it would leave all of its objects behind.
	if !slices.Contains(resources, schema.GroupVersionResource{Version: "v1", Resource: "pods"}) {
		n.mu.Lock()
		n.listedAt = time.Time{}
		n.mu.Unlock()
		return fmt.Errorf("discovery lists no pods; not finalizing %s", name)
	}
	// Pods go first and alone, as upstream orders it: what they use
	// (config maps, secrets, claims) stays until they are gone.
	pods := schema.GroupVersionResource{Version: "v1", Resource: "pods"}
	order := append([]schema.GroupVersionResource{pods}, slices.DeleteFunc(slices.Clone(resources), func(g schema.GroupVersionResource) bool { return g == pods })...)
	remaining := map[string]int{}
	finalizers := map[string]int{}
	var failure string
	for i, gvr := range order {
		list, err := n.meta.Resource(gvr).Namespace(name).List(ctx, metav1.ListOptions{})
		if err != nil {
			if apierrors.IsNotFound(err) || apierrors.IsMethodNotSupported(err) {
				continue
			}
			return err
		}
		if len(list.Items) == 0 {
			continue
		}
		key := gvr.Resource
		if gvr.Group != "" {
			key += "." + gvr.Group
		}
		remaining[key] += len(list.Items)
		for _, item := range list.Items {
			for _, f := range item.Finalizers {
				finalizers[f]++
			}
		}
		background := metav1.DeletePropagationBackground
		err = n.meta.Resource(gvr).Namespace(name).DeleteCollection(ctx,
			metav1.DeleteOptions{PropagationPolicy: &background}, metav1.ListOptions{})
		if err != nil && !apierrors.IsNotFound(err) {
			return err
		}
		if i == 0 {
			failure = fmt.Sprintf("Failed to delete all resource types, 1 remaining: unexpected items still remain in namespace: %s for gvr: /v1, Resource=pods", name)
			break
		}
	}
	if err := n.reportProgress(ctx, ns, remaining, finalizers, failure); err != nil {
		return err
	}
	if len(remaining) > 0 {
		// Pods leave when their kubelets say so; look again shortly.
		return later(2 * time.Second)
	}
	ns = ns.DeepCopy()
	ns.Spec.Finalizers = slices.DeleteFunc(ns.Spec.Finalizers, func(f corev1.FinalizerName) bool {
		return f == corev1.FinalizerKubernetes
	})
	_, err = n.client.CoreV1().Namespaces().Finalize(ctx, ns, metav1.UpdateOptions{})
	if apierrors.IsNotFound(err) {
		return nil
	}
	return err
}

// reportProgress writes the namespace's deletion conditions the way the
// upstream controller words them.
func (n *Namespaces) reportProgress(ctx context.Context, ns *corev1.Namespace, remaining, finalizers map[string]int, failure string) error {
	now := metav1.Now()
	cond := func(t corev1.NamespaceConditionType, on bool, yes, no, msg string) corev1.NamespaceCondition {
		c := corev1.NamespaceCondition{Type: t, Status: corev1.ConditionFalse, Reason: no, LastTransitionTime: now}
		if on {
			c.Status, c.Reason, c.Message = corev1.ConditionTrue, yes, msg
		} else if t == corev1.NamespaceDeletionContentFailure {
			c.Message = "All content successfully deleted, may be waiting on finalization"
		} else if t == corev1.NamespaceContentRemaining {
			c.Message = "All content successfully removed"
		} else if t == corev1.NamespaceFinalizersRemaining {
			c.Message = "All content-preserving finalizers finished"
		} else {
			c.Message = "All resources successfully discovered"
		}
		for _, old := range ns.Status.Conditions {
			if old.Type == t && old.Status == c.Status {
				c.LastTransitionTime = old.LastTransitionTime
			}
		}
		return c
	}
	var left, held []string
	for k, v := range remaining {
		left = append(left, fmt.Sprintf("%s has %d resource instances", k, v))
	}
	for k, v := range finalizers {
		held = append(held, fmt.Sprintf("%s in %d resource instances", k, v))
	}
	slices.Sort(left)
	slices.Sort(held)
	want := []corev1.NamespaceCondition{
		cond(corev1.NamespaceDeletionDiscoveryFailure, false, "", "ResourcesDiscovered", ""),
		cond(corev1.NamespaceDeletionGVParsingFailure, false, "", "ParsedGroupVersions", ""),
		cond(corev1.NamespaceDeletionContentFailure, failure != "", "ContentDeletionFailed", "ContentDeleted", failure),
		cond(corev1.NamespaceContentRemaining, len(left) > 0, "SomeResourcesRemain", "ContentRemoved", "Some resources are remaining: "+strings.Join(left, ", ")),
		cond(corev1.NamespaceFinalizersRemaining, len(held) > 0, "SomeFinalizersRemain", "ContentHasNoFinalizers", "Some content in the namespace has finalizers remaining: "+strings.Join(held, ", ")),
	}
	for i := range want {
		if want[i].Type == corev1.NamespaceDeletionGVParsingFailure {
			want[i].Message = "All legacy kube types successfully parsed"
		}
	}
	same := len(want) == len(ns.Status.Conditions)
	for i := 0; same && i < len(want); i++ {
		o := ns.Status.Conditions[i]
		same = o.Type == want[i].Type && o.Status == want[i].Status && o.Reason == want[i].Reason && o.Message == want[i].Message
	}
	if same {
		return nil
	}
	ns = ns.DeepCopy()
	ns.Status.Conditions = want
	_, err := n.client.CoreV1().Namespaces().UpdateStatus(ctx, ns, metav1.UpdateOptions{})
	if apierrors.IsNotFound(err) || apierrors.IsConflict(err) {
		return nil
	}
	return err
}

// ServiceAccounts keeps a default service account and the cluster CA in
// every namespace that is not going away.
type ServiceAccounts struct {
	client     kubernetes.Interface
	namespaces corelisters.NamespaceLister
	accounts   corelisters.ServiceAccountLister
	configMaps corelisters.ConfigMapLister
	rootCA     string
	loop       *loop
}

const rootCAConfigMap = "kube-root-ca.crt"

func NewServiceAccounts(client kubernetes.Interface, factory informers.SharedInformerFactory, rootCA string) *ServiceAccounts {
	s := &ServiceAccounts{
		client:     client,
		namespaces: factory.Core().V1().Namespaces().Lister(),
		accounts:   factory.Core().V1().ServiceAccounts().Lister(),
		configMaps: factory.Core().V1().ConfigMaps().Lister(),
		rootCA:     rootCA,
	}
	s.loop = newLoop("serviceaccount", 5, s.sync)
	_, _ = factory.Core().V1().Namespaces().Informer().AddEventHandler(s.loop.handlers())
	// A deleted default account or CA map brings its namespace back in.
	byNamespace := func(obj any) {
		if o, ok := unwrap(obj).(metav1.Object); ok {
			s.loop.add(o.GetNamespace())
		}
	}
	handler := cache.ResourceEventHandlerFuncs{UpdateFunc: func(_, obj any) { byNamespace(obj) }, DeleteFunc: byNamespace}
	_, _ = factory.Core().V1().ServiceAccounts().Informer().AddEventHandler(handler)
	_, _ = factory.Core().V1().ConfigMaps().Informer().AddEventHandler(handler)
	return s
}

func (s *ServiceAccounts) Run(ctx context.Context) { s.loop.run(ctx) }

func (s *ServiceAccounts) sync(ctx context.Context, name string) error {
	ns, err := s.namespaces.Get(name)
	if apierrors.IsNotFound(err) {
		return nil
	}
	if err != nil {
		return err
	}
	if ns.DeletionTimestamp != nil || ns.Status.Phase == corev1.NamespaceTerminating {
		return nil
	}
	if _, err := s.accounts.ServiceAccounts(name).Get("default"); apierrors.IsNotFound(err) {
		sa := &corev1.ServiceAccount{ObjectMeta: metav1.ObjectMeta{Name: "default", Namespace: name}}
		if _, err := s.client.CoreV1().ServiceAccounts(name).Create(ctx, sa, metav1.CreateOptions{}); err != nil && !apierrors.IsAlreadyExists(err) {
			return err
		}
	}
	if s.rootCA == "" {
		return nil
	}
	cm, err := s.configMaps.ConfigMaps(name).Get(rootCAConfigMap)
	if apierrors.IsNotFound(err) {
		cm = &corev1.ConfigMap{
			ObjectMeta: metav1.ObjectMeta{
				Name:        rootCAConfigMap,
				Namespace:   name,
				Annotations: map[string]string{"kubernetes.io/description": "Contains a CA bundle that can be used to verify the kube-apiserver when using internal endpoints such as the internal service IP or kubernetes.default.svc. No other usage is guaranteed across distributions of Kubernetes clusters."},
			},
			Data: map[string]string{"ca.crt": s.rootCA},
		}
		_, err = s.client.CoreV1().ConfigMaps(name).Create(ctx, cm, metav1.CreateOptions{})
		if apierrors.IsAlreadyExists(err) {
			return nil
		}
		return err
	}
	if err != nil {
		return err
	}
	if cm.Data["ca.crt"] != s.rootCA || len(cm.Data) != 1 {
		cm = cm.DeepCopy()
		cm.Data = map[string]string{"ca.crt": s.rootCA}
		_, err = s.client.CoreV1().ConfigMaps(name).Update(ctx, cm, metav1.UpdateOptions{})
	}
	return err
}
