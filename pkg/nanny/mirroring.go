package nanny

import (
	"context"
	"maps"
	"sort"
	"strings"

	corev1 "k8s.io/api/core/v1"
	discoveryv1 "k8s.io/api/discovery/v1"
	apiequality "k8s.io/apimachinery/pkg/api/equality"
	apierrors "k8s.io/apimachinery/pkg/api/errors"
	metav1 "k8s.io/apimachinery/pkg/apis/meta/v1"
	"k8s.io/apimachinery/pkg/labels"
	"k8s.io/client-go/informers"
	"k8s.io/client-go/kubernetes"
	corelisters "k8s.io/client-go/listers/core/v1"
	discoverylisters "k8s.io/client-go/listers/discovery/v1"
	"k8s.io/client-go/tools/cache"
	"k8s.io/utils/ptr"
)

const (
	mirrorManagedBy = "endpointslicemirroring-controller.k8s.io"
	labelSkipMirror = discoveryv1.LabelSkipMirror
	// Upstream mirrors at most this many addresses of one Endpoints.
	maxMirrored = 1000
)

// EndpointSliceMirroring copies Endpoints that people write by hand, for
// services without a selector, into EndpointSlices that kube-proxy reads.
type EndpointSliceMirroring struct {
	client    kubernetes.Interface
	services  corelisters.ServiceLister
	endpoints corelisters.EndpointsLister
	slices    discoverylisters.EndpointSliceLister
	loop      *loop
}

func NewEndpointSliceMirroring(client kubernetes.Interface, factory informers.SharedInformerFactory) *EndpointSliceMirroring {
	m := &EndpointSliceMirroring{
		client:    client,
		services:  factory.Core().V1().Services().Lister(),
		endpoints: factory.Core().V1().Endpoints().Lister(),
		slices:    factory.Discovery().V1().EndpointSlices().Lister(),
	}
	m.loop = newLoop("endpointslicemirroring", 5, m.sync)
	_, _ = factory.Core().V1().Endpoints().Informer().AddEventHandler(m.loop.handlers())
	_, _ = factory.Core().V1().Services().Informer().AddEventHandler(m.loop.handlers())
	bySlice := func(obj any) {
		s, ok := unwrap(obj).(*discoveryv1.EndpointSlice)
		if ok && s.Labels[discoveryv1.LabelManagedBy] == mirrorManagedBy {
			m.loop.add(s.Namespace + "/" + s.Labels[discoveryv1.LabelServiceName])
		}
	}
	_, _ = factory.Discovery().V1().EndpointSlices().Informer().AddEventHandler(cache.ResourceEventHandlerFuncs{
		UpdateFunc: func(_, obj any) { bySlice(obj) }, DeleteFunc: bySlice,
	})
	return m
}

func (m *EndpointSliceMirroring) Run(ctx context.Context) { m.loop.run(ctx) }

type mirrorSlice struct {
	addressType discoveryv1.AddressType
	ports       []discoveryv1.EndpointPort
	endpoints   []discoveryv1.Endpoint
}

func (m *EndpointSliceMirroring) sync(ctx context.Context, key string) error {
	ns, name, _ := cache.SplitMetaNamespaceKey(key)
	var want []mirrorSlice
	var svc *corev1.Service
	ep, err := m.endpoints.Endpoints(ns).Get(name)
	if err != nil && !apierrors.IsNotFound(err) {
		return err
	}
	if ep != nil {
		s, err := m.services.Services(ns).Get(name)
		if err != nil && !apierrors.IsNotFound(err) {
			return err
		}
		if s != nil && s.Spec.Selector == nil && ep.Labels[labelSkipMirror] != "true" && ep.DeletionTimestamp == nil {
			svc = s
			want = mirrorOf(ep)
		}
	}
	sel := labels.SelectorFromSet(labels.Set{discoveryv1.LabelServiceName: name, discoveryv1.LabelManagedBy: mirrorManagedBy})
	existing, err := m.slices.EndpointSlices(ns).List(sel)
	if err != nil {
		return err
	}
	sort.Slice(existing, func(i, j int) bool { return existing[i].Name < existing[j].Name })
	var sliceLabels map[string]string
	if svc != nil {
		sliceLabels = map[string]string{}
		for k, v := range ep.Labels {
			if k != labelSkipMirror && k != corev1.IsHeadlessService {
				sliceLabels[k] = v
			}
		}
		sliceLabels[discoveryv1.LabelServiceName] = name
		sliceLabels[discoveryv1.LabelManagedBy] = mirrorManagedBy
	}
	annotations := map[string]string{}
	if ep != nil {
		for k, v := range ep.Annotations {
			if k != corev1.EndpointsLastChangeTriggerTime && k != corev1.LastAppliedConfigAnnotation {
				annotations[k] = v
			}
		}
	}
	for i, w := range want {
		if i < len(existing) {
			s := existing[i]
			if s.AddressType == w.addressType && apiequality.Semantic.DeepEqual(s.Endpoints, w.endpoints) &&
				apiequality.Semantic.DeepEqual(s.Ports, w.ports) && maps.Equal(s.Labels, sliceLabels) {
				continue
			}
			if s.AddressType != w.addressType {
				// The address type of a slice cannot change: replace it.
				if err := m.client.DiscoveryV1().EndpointSlices(ns).Delete(ctx, s.Name, metav1.DeleteOptions{}); err != nil && !apierrors.IsNotFound(err) {
					return err
				}
			} else {
				s = s.DeepCopy()
				s.Endpoints, s.Ports, s.Labels, s.Annotations = w.endpoints, w.ports, sliceLabels, annotations
				if _, err := m.client.DiscoveryV1().EndpointSlices(ns).Update(ctx, s, metav1.UpdateOptions{}); err != nil {
					return err
				}
				continue
			}
		}
		s := &discoveryv1.EndpointSlice{
			ObjectMeta: metav1.ObjectMeta{
				GenerateName:    name + "-",
				Namespace:       ns,
				Labels:          sliceLabels,
				Annotations:     annotations,
				OwnerReferences: []metav1.OwnerReference{*metav1.NewControllerRef(svc, kindService.gvk())},
			},
			AddressType: w.addressType,
			Endpoints:   w.endpoints,
			Ports:       w.ports,
		}
		if _, err := m.client.DiscoveryV1().EndpointSlices(ns).Create(ctx, s, metav1.CreateOptions{}); err != nil {
			return err
		}
	}
	for _, s := range existing[min(len(want), len(existing)):] {
		err := m.client.DiscoveryV1().EndpointSlices(ns).Delete(ctx, s.Name, metav1.DeleteOptions{})
		if err != nil && !apierrors.IsNotFound(err) {
			return err
		}
	}
	return nil
}

// mirrorOf turns each subset into slices, one per address family, as
// upstream's mirroring does.
func mirrorOf(ep *corev1.Endpoints) []mirrorSlice {
	var out []mirrorSlice
	total := 0
	for _, subset := range ep.Subsets {
		var ports []discoveryv1.EndpointPort
		for _, p := range subset.Ports {
			proto := p.Protocol
			if proto == "" {
				proto = corev1.ProtocolTCP
			}
			ports = append(ports, discoveryv1.EndpointPort{
				Name: ptr.To(p.Name), Port: ptr.To(p.Port), Protocol: ptr.To(proto), AppProtocol: p.AppProtocol,
			})
		}
		byType := map[discoveryv1.AddressType][]discoveryv1.Endpoint{}
		add := func(a corev1.EndpointAddress, ready bool) {
			if total >= maxMirrored {
				return
			}
			total++
			t := discoveryv1.AddressTypeIPv4
			if strings.Contains(a.IP, ":") {
				t = discoveryv1.AddressTypeIPv6
			}
			e := discoveryv1.Endpoint{
				Addresses:  []string{a.IP},
				Conditions: discoveryv1.EndpointConditions{Ready: ptr.To(ready)},
				TargetRef:  a.TargetRef,
				NodeName:   a.NodeName,
			}
			if a.Hostname != "" {
				e.Hostname = ptr.To(a.Hostname)
			}
			byType[t] = append(byType[t], e)
		}
		for _, a := range subset.Addresses {
			add(a, true)
		}
		for _, a := range subset.NotReadyAddresses {
			add(a, false)
		}
		for _, t := range []discoveryv1.AddressType{discoveryv1.AddressTypeIPv4, discoveryv1.AddressTypeIPv6} {
			eps := byType[t]
			for i := 0; i < len(eps); i += maxEndpointsPerSet {
				out = append(out, mirrorSlice{addressType: t, ports: ports, endpoints: eps[i:min(i+maxEndpointsPerSet, len(eps))]})
			}
		}
	}
	return out
}
