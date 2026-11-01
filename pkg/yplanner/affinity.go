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

// affinityIndex holds what the inter-pod affinity checks of one pod need
// from the whole cluster, gathered once per scheduling attempt: checking
// a node is then a lookup, not a walk over every pod on every node.
type affinityIndex struct {
	affinity []termIndex // the pod's required affinity terms
	anti     []termIndex // the pod's required anti-affinity terms
	// Placed pods with required anti-affinity of their own: few, usually none.
	antiPods []placedPod
}

// termIndex is where the pods a term points at are: the values of its
// topology key on their nodes.
type termIndex struct {
	term     corev1.PodAffinityTerm
	values   map[string]bool
	anywhere bool
}

type placedPod struct {
	pod  *corev1.Pod
	node *corev1.Node
}

func buildAffinityIndex(pod *corev1.Pod, state map[string]*nodeState) *affinityIndex {
	idx := &affinityIndex{}
	index := func(terms []corev1.PodAffinityTerm) []termIndex {
		out := make([]termIndex, 0, len(terms))
		for _, term := range terms {
			out = append(out, termIndex{term: term, values: map[string]bool{}})
		}
		return out
	}
	if a := pod.Spec.Affinity; a != nil && a.PodAffinity != nil {
		idx.affinity = index(a.PodAffinity.RequiredDuringSchedulingIgnoredDuringExecution)
	}
	if a := pod.Spec.Affinity; a != nil && a.PodAntiAffinity != nil {
		idx.anti = index(a.PodAntiAffinity.RequiredDuringSchedulingIgnoredDuringExecution)
	}
	for _, s := range state {
		for _, p := range s.pods {
			for i := range idx.affinity {
				t := &idx.affinity[i]
				if termMatches(t.term, pod, p) {
					t.anywhere = true
					if v, ok := s.node.Labels[t.term.TopologyKey]; ok {
						t.values[v] = true
					}
				}
			}
			for i := range idx.anti {
				t := &idx.anti[i]
				if termMatches(t.term, pod, p) {
					if v, ok := s.node.Labels[t.term.TopologyKey]; ok {
						t.values[v] = true
					}
				}
			}
			if a := p.Spec.Affinity; a != nil && a.PodAntiAffinity != nil && len(a.PodAntiAffinity.RequiredDuringSchedulingIgnoredDuringExecution) > 0 {
				idx.antiPods = append(idx.antiPods, placedPod{pod: p, node: s.node})
			}
		}
	}
	return idx
}

// interPodAffinity checks the required pod affinity and anti-affinity of
// the pod, and the anti-affinity of the pods already placed against it.
func interPodAffinity(pod *corev1.Pod, s *nodeState, idx *affinityIndex) string {
	for _, t := range idx.affinity {
		here, ok := s.node.Labels[t.term.TopologyKey]
		if !ok {
			return "node(s) didn't match pod affinity rules"
		}
		// The first pod of a group that wants to be together may go anywhere.
		if !t.values[here] && !(!t.anywhere && termMatches(t.term, pod, pod)) {
			return "node(s) didn't match pod affinity rules"
		}
	}
	for _, t := range idx.anti {
		if here, ok := s.node.Labels[t.term.TopologyKey]; ok && t.values[here] {
			return "node(s) didn't match pod anti-affinity rules"
		}
	}
	for _, placed := range idx.antiPods {
		for _, term := range placed.pod.Spec.Affinity.PodAntiAffinity.RequiredDuringSchedulingIgnoredDuringExecution {
			there, ok := placed.node.Labels[term.TopologyKey]
			if !ok {
				continue
			}
			if here, ok := s.node.Labels[term.TopologyKey]; ok && here == there && termMatches(term, placed.pod, pod) {
				return "node(s) didn't satisfy existing pods anti-affinity rules"
			}
		}
	}
	return ""
}
