package yplanner

import (
	"context"
	"encoding/json"
	"fmt"
	"log/slog"
	"maps"
	"sort"

	corev1 "k8s.io/api/core/v1"
	apierrors "k8s.io/apimachinery/pkg/api/errors"
	metav1 "k8s.io/apimachinery/pkg/apis/meta/v1"
	"k8s.io/apimachinery/pkg/types"
	resourcehelper "k8s.io/component-helpers/resource"
	corev1helpers "k8s.io/component-helpers/scheduling/corev1"
	"k8s.io/component-helpers/scheduling/corev1/nodeaffinity"
)

// candidate is a node a pod fits on once its victims are gone.
type candidate struct {
	state   *nodeState
	victims []*corev1.Pod
}

// preempt makes room for a pod that fits nowhere by evicting pods of lower
// priority from one node, as upstream's DefaultPreemption does: on each
// node the least it can take, then the node where that costs least. It
// says whether it evicted anything; the pod is retried as they go.
func (p *Planner) preempt(ctx context.Context, pod *corev1.Pod, state map[string]*nodeState) bool {
	if pod.Spec.PreemptionPolicy != nil && *pod.Spec.PreemptionPolicy == corev1.PreemptNever {
		return false
	}
	// A pod already waiting on its victims does not take more.
	if pod.Status.NominatedNodeName != "" {
		for _, other := range state[pod.Status.NominatedNodeName].podsOrNil() {
			if other.DeletionTimestamp != nil && corev1helpers.PodPriority(other) < corev1helpers.PodPriority(pod) {
				return true
			}
		}
	}
	required := nodeaffinity.GetRequiredNodeAffinity(pod)
	requests := resourcehelper.PodRequests(pod, resourcehelper.PodResourcesOptions{})
	priority := corev1helpers.PodPriority(pod)
	base := buildAffinityIndex(pod, state)
	// Victims change what affinity sees only when affinity is in play.
	affinityMatters := len(base.affinity) > 0 || len(base.anti) > 0 || len(base.antiPods) > 0
	var candidates []candidate
	for name, s := range state {
		var lower []*corev1.Pod
		for _, other := range s.pods {
			if other.DeletionTimestamp == nil && other.Spec.NodeName != "" && corev1helpers.PodPriority(other) < priority {
				lower = append(lower, other)
			}
		}
		if len(lower) == 0 {
			continue
		}
		fits := func(gone []*corev1.Pod) bool {
			reduced := s.without(gone)
			idx := base
			if affinityMatters {
				all := maps.Clone(state)
				all[name] = reduced
				idx = buildAffinityIndex(pod, all)
			}
			return p.misfit(pod, reduced, required, requests, idx) == ""
		}
		if !fits(lower) {
			continue
		}
		// Give back the most important first while the pod still fits.
		sort.SliceStable(lower, func(i, j int) bool {
			return corev1helpers.PodPriority(lower[i]) > corev1helpers.PodPriority(lower[j])
		})
		victims := lower
		for i := 0; i < len(victims); {
			rest := append(append([]*corev1.Pod{}, victims[:i]...), victims[i+1:]...)
			if fits(rest) {
				victims = rest
			} else {
				i++
			}
		}
		candidates = append(candidates, candidate{state: s, victims: victims})
	}
	if len(candidates) == 0 {
		return false
	}
	// Least harm: the lowest top priority evicted, then the lowest sum, then the fewest.
	cost := func(c candidate) (int32, int64) {
		var top int32 = -1 << 31
		var sum int64
		for _, v := range c.victims {
			pr := corev1helpers.PodPriority(v)
			top = max(top, pr)
			sum += int64(pr)
		}
		return top, sum
	}
	sort.SliceStable(candidates, func(i, j int) bool {
		ti, si := cost(candidates[i])
		tj, sj := cost(candidates[j])
		if ti != tj {
			return ti < tj
		}
		if si != sj {
			return si < sj
		}
		return len(candidates[i].victims) < len(candidates[j].victims)
	})
	chosen := candidates[0]
	node := chosen.state.node.Name
	nominate := fmt.Sprintf(`{"status":{"nominatedNodeName":%q}}`, node)
	if _, err := p.client.CoreV1().Pods(pod.Namespace).Patch(ctx, pod.Name, types.MergePatchType, []byte(nominate), metav1.PatchOptions{}, "status"); err != nil && !apierrors.IsNotFound(err) {
		slog.Warn("nominated node not written", "pod", pod.Namespace+"/"+pod.Name, "err", err)
	}
	for _, v := range chosen.victims {
		p.evict(ctx, v, pod, node)
	}
	return true
}

func (p *Planner) evict(ctx context.Context, victim, preemptor *corev1.Pod, node string) {
	cond := corev1.PodCondition{
		Type: corev1.DisruptionTarget, Status: corev1.ConditionTrue, Reason: corev1.PodReasonPreemptionByScheduler,
		Message: fmt.Sprintf("%s: preempting to accommodate a higher priority pod", p.name), LastTransitionTime: metav1.Now(),
	}
	// Conditions merge by type: the others stay as they are.
	patch, _ := json.Marshal(map[string]any{"status": map[string]any{"conditions": []corev1.PodCondition{cond}}})
	if _, err := p.client.CoreV1().Pods(victim.Namespace).Patch(ctx, victim.Name, types.StrategicMergePatchType, patch, metav1.PatchOptions{}, "status"); err != nil && !apierrors.IsNotFound(err) {
		slog.Warn("disruption condition not written", "pod", victim.Namespace+"/"+victim.Name, "err", err)
	}
	uid := victim.UID
	err := p.client.CoreV1().Pods(victim.Namespace).Delete(ctx, victim.Name, metav1.DeleteOptions{Preconditions: &metav1.Preconditions{UID: &uid}})
	if err != nil && !apierrors.IsNotFound(err) {
		slog.Warn("victim not deleted", "pod", victim.Namespace+"/"+victim.Name, "err", err)
		return
	}
	p.recorder.Eventf(victim, corev1.EventTypeNormal, "Preempted", "Preempted by pod %v on node %v", preemptor.UID, node)
	p.forget(uid)
}

// without is the node as it would be with some of its pods gone.
func (s *nodeState) without(gone []*corev1.Pod) *nodeState {
	skip := map[types.UID]bool{}
	for _, g := range gone {
		skip[g.UID] = true
	}
	out := &nodeState{node: s.node, requested: corev1.ResourceList{}, hostPorts: map[string]int{}}
	for _, pod := range s.pods {
		if !skip[pod.UID] {
			out.add(pod)
		}
	}
	return out
}

func (s *nodeState) podsOrNil() []*corev1.Pod {
	if s == nil {
		return nil
	}
	return s.pods
}
