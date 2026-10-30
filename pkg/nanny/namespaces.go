package nanny

import (
	"context"
	"fmt"
	"slices"
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
	remaining := 0
	for _, gvr := range resources {
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
		remaining += len(list.Items)
		background := metav1.DeletePropagationBackground
		err = n.meta.Resource(gvr).Namespace(name).DeleteCollection(ctx,
			metav1.DeleteOptions{PropagationPolicy: &background}, metav1.ListOptions{})
		if err != nil && !apierrors.IsNotFound(err) {
			return err
		}
	}
	if remaining > 0 {
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
