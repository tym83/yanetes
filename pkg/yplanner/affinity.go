package yplanner

import (
	corev1 "k8s.io/api/core/v1"
	metav1 "k8s.io/apimachinery/pkg/apis/meta/v1"
	"k8s.io/apimachinery/pkg/labels"
)

// termMatches tells whether a pod is one an affinity term of owner points at.
func termMatches(term corev1.PodAffinityTerm, owner, pod *corev1.Pod) bool {
	if pod.DeletionTimestamp != nil {
		return false
	}
	namespaces := term.Namespaces
	if len(namespaces) == 0 && term.NamespaceSelector == nil {
		namespaces = []string{owner.Namespace}
	}
	inNamespace := false
	for _, ns := range namespaces {
		inNamespace = inNamespace || ns == pod.Namespace
	}
	// An empty namespace selector means every namespace; a non-empty one
	// would need namespace labels the planner does not watch, so it counts
	// as every namespace too.
	if term.NamespaceSelector != nil {
		inNamespace = true
	}
	if !inNamespace {
		return false
	}
	sel, err := metav1.LabelSelectorAsSelector(term.LabelSelector)
	if err != nil {
		return false
	}
	return sel.Matches(labels.Set(pod.Labels))
}

// interPodAffinity checks the required pod affinity and anti-affinity of
// the pod, and the anti-affinity of the pods already placed against it.
func interPodAffinity(pod *corev1.Pod, s *nodeState, all map[string]*nodeState) string {
	value := func(n *corev1.Node, key string) (string, bool) {
		v, ok := n.Labels[key]
		return v, ok
	}
	if a := pod.Spec.Affinity; a != nil && a.PodAffinity != nil {
		for _, term := range a.PodAffinity.RequiredDuringSchedulingIgnoredDuringExecution {
			here, ok := value(s.node, term.TopologyKey)
			if !ok {
				return "node(s) didn't match pod affinity rules"
			}
			found, anywhere := false, false
			for _, other := range all {
				for _, p := range other.pods {
					if !termMatches(term, pod, p) {
						continue
					}
					anywhere = true
					if v, ok := value(other.node, term.TopologyKey); ok && v == here {
						found = true
					}
				}
			}
			// The first pod of a group that wants to be together may go anywhere.
			if !found && !(!anywhere && termMatches(term, pod, pod)) {
				return "node(s) didn't match pod affinity rules"
			}
		}
	}
	if a := pod.Spec.Affinity; a != nil && a.PodAntiAffinity != nil {
		for _, term := range a.PodAntiAffinity.RequiredDuringSchedulingIgnoredDuringExecution {
			here, ok := value(s.node, term.TopologyKey)
			if !ok {
				continue
			}
			for _, other := range all {
				if v, ok := value(other.node, term.TopologyKey); !ok || v != here {
					continue
				}
				for _, p := range other.pods {
					if termMatches(term, pod, p) {
						return "node(s) didn't match pod anti-affinity rules"
					}
				}
			}
		}
	}
	for _, other := range all {
		for _, p := range other.pods {
			a := p.Spec.Affinity
			if a == nil || a.PodAntiAffinity == nil {
				continue
			}
			for _, term := range a.PodAntiAffinity.RequiredDuringSchedulingIgnoredDuringExecution {
				there, ok := value(other.node, term.TopologyKey)
				if !ok {
					continue
				}
				if here, ok := value(s.node, term.TopologyKey); ok && here == there && termMatches(term, p, pod) {
					return "node(s) didn't satisfy existing pods anti-affinity rules"
				}
			}
		}
	}
	return ""
}
