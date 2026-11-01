package nanny

import (
	"context"
	"fmt"
	"maps"
	"slices"
	"sort"
	"strings"

	corev1 "k8s.io/api/core/v1"
	discoveryv1 "k8s.io/api/discovery/v1"
	apiequality "k8s.io/apimachinery/pkg/api/equality"
	apierrors "k8s.io/apimachinery/pkg/api/errors"
	metav1 "k8s.io/apimachinery/pkg/apis/meta/v1"
	"k8s.io/apimachinery/pkg/labels"
	"k8s.io/apimachinery/pkg/util/intstr"
	"k8s.io/client-go/informers"
	"k8s.io/client-go/kubernetes"
	corelisters "k8s.io/client-go/listers/core/v1"
	discoverylisters "k8s.io/client-go/listers/discovery/v1"
	"k8s.io/client-go/tools/cache"
	"k8s.io/utils/ptr"
)

const (
	managedBy          = "endpointslice-controller.k8s.io"
	maxEndpointsPerSet = 100
)

// Endpoints writes the EndpointSlices of every service with a selector,
// and the Endpoints object older clients still read.
type Endpoints struct {
	client    kubernetes.Interface
	services  corelisters.ServiceLister
	pods      corelisters.PodLister
	slices    discoverylisters.EndpointSliceLister
	endpoints corelisters.EndpointsLister
	loop      *loop
}

func NewEndpoints(client kubernetes.Interface, factory informers.SharedInformerFactory) *Endpoints {
	e := &Endpoints{
		client:    client,
		services:  factory.Core().V1().Services().Lister(),
		pods:      factory.Core().V1().Pods().Lister(),
		slices:    factory.Discovery().V1().EndpointSlices().Lister(),
		endpoints: factory.Core().V1().Endpoints().Lister(),
	}
	e.loop = newLoop("endpoints", 10, e.sync)
	_, _ = factory.Core().V1().Services().Informer().AddEventHandler(e.loop.handlers())
	byPod := func(obj any) {
		pod, ok := unwrap(obj).(*corev1.Pod)
		if ok {
			e.wakeServices(pod.Namespace, pod.Labels)
		}
	}
	_, _ = factory.Core().V1().Pods().Informer().AddEventHandler(cache.ResourceEventHandlerFuncs{
		AddFunc: byPod,
		UpdateFunc: func(old, obj any) {
			byPod(obj)
			if o := old.(*corev1.Pod); !labels.Equals(o.Labels, obj.(*corev1.Pod).Labels) {
				byPod(o)
			}
		},
		DeleteFunc: byPod,
	})
	bySlice := func(obj any) {
		s, ok := unwrap(obj).(*discoveryv1.EndpointSlice)
		if ok && s.Labels[discoveryv1.LabelManagedBy] == managedBy {
			e.loop.add(s.Namespace + "/" + s.Labels[discoveryv1.LabelServiceName])
		}
	}
	_, _ = factory.Discovery().V1().EndpointSlices().Informer().AddEventHandler(cache.ResourceEventHandlerFuncs{
		UpdateFunc: func(_, obj any) { bySlice(obj) }, DeleteFunc: bySlice,
	})
	return e
}

func (e *Endpoints) Run(ctx context.Context) { e.loop.run(ctx) }

func (e *Endpoints) wakeServices(ns string, podLabels map[string]string) {
	services, _ := e.services.Services(ns).List(labels.Everything())
	for _, svc := range services {
		if svc.Spec.Selector != nil && labels.SelectorFromSet(svc.Spec.Selector).Matches(labels.Set(podLabels)) {
			e.loop.add(ns + "/" + svc.Name)
		}
	}
}

// resolvePort finds the port a service port reaches on a pod; a named
// target the pod lacks reaches nothing.
func resolvePort(pod *corev1.Pod, sp corev1.ServicePort) (int32, bool) {
	if sp.TargetPort.Type == intstr.Int {
		if sp.TargetPort.IntVal == 0 {
			return sp.Port, true
		}
		return sp.TargetPort.IntVal, true
	}
	for _, c := range pod.Spec.Containers {
		for _, p := range c.Ports {
			if p.Name == sp.TargetPort.StrVal && p.Protocol == sp.Protocol {
				return p.ContainerPort, true
			}
		}
	}
	return 0, false
}

type podEndpoint struct {
	pod   *corev1.Pod
	ports []discoveryv1.EndpointPort
	key   string
}

func (e *Endpoints) sync(ctx context.Context, key string) error {
	ns, name, _ := cache.SplitMetaNamespaceKey(key)
	svc, err := e.services.Services(ns).Get(name)
	if apierrors.IsNotFound(err) {
		// Slices go with their owner; Endpoints carry no owner and go here.
		err := e.client.CoreV1().Endpoints(ns).Delete(ctx, name, metav1.DeleteOptions{})
		if apierrors.IsNotFound(err) {
			return nil
		}
		return err
	}
	if err != nil {
		return err
	}
	if svc.Spec.Selector == nil || svc.Spec.Type == corev1.ServiceTypeExternalName || svc.DeletionTimestamp != nil {
		return nil
	}
	pods, err := e.pods.Pods(ns).List(labels.SelectorFromSet(svc.Spec.Selector))
	if err != nil {
		return err
	}
	var eps []podEndpoint
	for _, pod := range pods {
		if pod.Status.PodIP == "" || pod.Status.Phase == corev1.PodSucceeded || pod.Status.Phase == corev1.PodFailed {
			continue
		}
		var ports []discoveryv1.EndpointPort
		var parts []string
		for _, sp := range svc.Spec.Ports {
			port, ok := resolvePort(pod, sp)
			if !ok {
				continue
			}
			ports = append(ports, discoveryv1.EndpointPort{
				Name: ptr.To(sp.Name), Protocol: ptr.To(sp.Protocol), Port: ptr.To(port), AppProtocol: sp.AppProtocol,
			})
			parts = append(parts, fmt.Sprintf("%s/%s/%d", sp.Name, sp.Protocol, port))
		}
		eps = append(eps, podEndpoint{pod: pod, ports: ports, key: strings.Join(parts, ",")})
	}
	sort.Slice(eps, func(i, j int) bool { return eps[i].pod.Name < eps[j].pod.Name })
	return firstErr(e.syncSlices(ctx, svc, eps), e.syncEndpoints(ctx, svc, eps))
}

func endpointOf(svc *corev1.Service, pod *corev1.Pod) discoveryv1.Endpoint {
	ready := podReady(pod) && pod.DeletionTimestamp == nil
	serving := podReady(pod)
	if svc.Spec.PublishNotReadyAddresses {
		ready, serving = true, true
	}
	ep := discoveryv1.Endpoint{
		Addresses: []string{pod.Status.PodIP},
		Conditions: discoveryv1.EndpointConditions{
			Ready: ptr.To(ready), Serving: ptr.To(serving), Terminating: ptr.To(pod.DeletionTimestamp != nil),
		},
		TargetRef: &corev1.ObjectReference{Kind: "Pod", Namespace: pod.Namespace, Name: pod.Name, UID: pod.UID},
	}
	if pod.Spec.NodeName != "" {
		ep.NodeName = ptr.To(pod.Spec.NodeName)
	}
	if pod.Spec.Hostname != "" && pod.Spec.Subdomain == svc.Name {
		ep.Hostname = ptr.To(pod.Spec.Hostname)
	}
	return ep
}

func (e *Endpoints) syncSlices(ctx context.Context, svc *corev1.Service, eps []podEndpoint) error {
	// Desired slices: pods grouped by the ports they serve, a hundred a slice.
	type desired struct {
		ports     []discoveryv1.EndpointPort
		endpoints []discoveryv1.Endpoint
	}
	groups := map[string]*desired{}
	var order []string
	for _, ep := range eps {
		g := groups[ep.key]
		if g == nil {
			g = &desired{ports: ep.ports}
			groups[ep.key] = g
			order = append(order, ep.key)
		}
		g.endpoints = append(g.endpoints, endpointOf(svc, ep.pod))
	}
	var want []desired
	for _, k := range order {
		g := groups[k]
		for i := 0; i < len(g.endpoints); i += maxEndpointsPerSet {
			want = append(want, desired{ports: g.ports, endpoints: g.endpoints[i:min(i+maxEndpointsPerSet, len(g.endpoints))]})
		}
	}
	if len(want) == 0 {
		// A service without endpoints still gets a slice saying so.
		var ports []discoveryv1.EndpointPort
		for _, sp := range svc.Spec.Ports {
			if sp.TargetPort.Type == intstr.Int {
				port, _ := resolvePort(&corev1.Pod{}, sp)
				ports = append(ports, discoveryv1.EndpointPort{Name: ptr.To(sp.Name), Protocol: ptr.To(sp.Protocol), Port: ptr.To(port), AppProtocol: sp.AppProtocol})
			}
		}
		want = append(want, desired{ports: ports, endpoints: []discoveryv1.Endpoint{}})
	}
	sel := labels.SelectorFromSet(labels.Set{discoveryv1.LabelServiceName: svc.Name, discoveryv1.LabelManagedBy: managedBy})
	existing, err := e.slices.EndpointSlices(svc.Namespace).List(sel)
	if err != nil {
		return err
	}
	sort.Slice(existing, func(i, j int) bool { return existing[i].Name < existing[j].Name })
	sliceLabels := maps.Clone(svc.Labels)
	if sliceLabels == nil {
		sliceLabels = map[string]string{}
	}
	sliceLabels[discoveryv1.LabelServiceName] = svc.Name
	sliceLabels[discoveryv1.LabelManagedBy] = managedBy
	if svc.Spec.ClusterIP == corev1.ClusterIPNone {
		sliceLabels[corev1.IsHeadlessService] = ""
	}
	for i, w := range want {
		if i < len(existing) {
			s := existing[i]
			if apiequality.Semantic.DeepEqual(s.Endpoints, w.endpoints) && apiequality.Semantic.DeepEqual(s.Ports, w.ports) &&
				apiequality.Semantic.DeepEqual(s.Labels, sliceLabels) {
				continue
			}
			s = s.DeepCopy()
			s.Endpoints, s.Ports, s.Labels = w.endpoints, w.ports, sliceLabels
			if _, err := e.client.DiscoveryV1().EndpointSlices(svc.Namespace).Update(ctx, s, metav1.UpdateOptions{}); err != nil {
				return err
			}
			continue
		}
		s := &discoveryv1.EndpointSlice{
			ObjectMeta: metav1.ObjectMeta{
				GenerateName:    svc.Name + "-",
				Namespace:       svc.Namespace,
				Labels:          sliceLabels,
				OwnerReferences: []metav1.OwnerReference{*metav1.NewControllerRef(svc, kindService.gvk())},
			},
			AddressType: discoveryv1.AddressTypeIPv4,
			Endpoints:   w.endpoints,
			Ports:       w.ports,
		}
		if _, err := e.client.DiscoveryV1().EndpointSlices(svc.Namespace).Create(ctx, s, metav1.CreateOptions{}); err != nil {
			return err
		}
	}
	for _, s := range existing[min(len(want), len(existing)):] {
		err := e.client.DiscoveryV1().EndpointSlices(svc.Namespace).Delete(ctx, s.Name, metav1.DeleteOptions{})
		if err != nil && !apierrors.IsNotFound(err) {
			return err
		}
	}
	return nil
}

func (e *Endpoints) syncEndpoints(ctx context.Context, svc *corev1.Service, eps []podEndpoint) error {
	subsets := map[string]*corev1.EndpointSubset{}
	var order []string
	for _, ep := range eps {
		if len(ep.ports) == 0 && len(svc.Spec.Ports) > 0 {
			continue
		}
		if ep.pod.DeletionTimestamp != nil && !svc.Spec.PublishNotReadyAddresses {
			continue
		}
		s := subsets[ep.key]
		if s == nil {
			s = &corev1.EndpointSubset{}
			for _, p := range ep.ports {
				s.Ports = append(s.Ports, corev1.EndpointPort{Name: *p.Name, Port: *p.Port, Protocol: *p.Protocol, AppProtocol: p.AppProtocol})
			}
			subsets[ep.key] = s
			order = append(order, ep.key)
		}
		addr := corev1.EndpointAddress{
			IP:        ep.pod.Status.PodIP,
			NodeName:  ptr.To(ep.pod.Spec.NodeName),
			TargetRef: &corev1.ObjectReference{Kind: "Pod", Namespace: ep.pod.Namespace, Name: ep.pod.Name, UID: ep.pod.UID},
		}
		if ep.pod.Spec.NodeName == "" {
			addr.NodeName = nil
		}
		if ep.pod.Spec.Hostname != "" && ep.pod.Spec.Subdomain == svc.Name {
			addr.Hostname = ep.pod.Spec.Hostname
		}
		if podReady(ep.pod) || svc.Spec.PublishNotReadyAddresses {
			s.Addresses = append(s.Addresses, addr)
		} else {
			s.NotReadyAddresses = append(s.NotReadyAddresses, addr)
		}
	}
	out := []corev1.EndpointSubset{}
	for _, k := range order {
		out = append(out, *subsets[k])
	}
	epLabels := maps.Clone(svc.Labels)
	if epLabels == nil {
		epLabels = map[string]string{}
	}
	if svc.Spec.ClusterIP == corev1.ClusterIPNone {
		epLabels[corev1.IsHeadlessService] = ""
	}
	current, err := e.endpoints.Endpoints(svc.Namespace).Get(svc.Name)
	if apierrors.IsNotFound(err) {
		ep := &corev1.Endpoints{ObjectMeta: metav1.ObjectMeta{Name: svc.Name, Namespace: svc.Namespace, Labels: epLabels}, Subsets: out}
		_, err = e.client.CoreV1().Endpoints(svc.Namespace).Create(ctx, ep, metav1.CreateOptions{})
		if apierrors.IsAlreadyExists(err) {
			return later(0)
		}
		return err
	}
	if err != nil {
		return err
	}
	if apiequality.Semantic.DeepEqual(current.Subsets, out) && apiequality.Semantic.DeepEqual(current.Labels, epLabels) {
		return nil
	}
	if len(current.Subsets) == 0 && len(out) == 0 && len(current.Labels) == len(epLabels) && slices.Equal(slices.Sorted(maps.Keys(current.Labels)), slices.Sorted(maps.Keys(epLabels))) {
		return nil
	}
	current = current.DeepCopy()
	current.Subsets, current.Labels = out, epLabels
	_, err = e.client.CoreV1().Endpoints(svc.Namespace).Update(ctx, current, metav1.UpdateOptions{})
	return err
}
