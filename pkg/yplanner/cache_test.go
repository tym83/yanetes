package yplanner

import (
	"testing"

	corev1 "k8s.io/api/core/v1"
	"k8s.io/apimachinery/pkg/api/resource"
	metav1 "k8s.io/apimachinery/pkg/apis/meta/v1"
	k8stypes "k8s.io/apimachinery/pkg/types"
)

func cachedPod(uid, node, cpu string) *corev1.Pod {
	return &corev1.Pod{
		ObjectMeta: metav1.ObjectMeta{UID: k8stypes.UID("uid-" + uid), Name: uid},
		Spec: corev1.PodSpec{NodeName: node, Containers: []corev1.Container{{
			Name:      "c",
			Resources: corev1.ResourceRequirements{Requests: corev1.ResourceList{corev1.ResourceCPU: resource.MustParse(cpu)}},
			Ports:     []corev1.ContainerPort{{HostPort: 80, Protocol: corev1.ProtocolTCP}},
		}}},
	}
}

func TestCacheFollowsPods(t *testing.T) {
	c := newClusterCache()
	// The pod is seen before its node: it is counted once the node comes.
	c.update(cachedPod("a", "n1", "500m"))
	c.upsertNode(&corev1.Node{ObjectMeta: metav1.ObjectMeta{Name: "n1"}})
	c.upsertNode(&corev1.Node{ObjectMeta: metav1.ObjectMeta{Name: "n0"}})
	if got := c.names; len(got) != 2 || got[0] != "n0" || got[1] != "n1" {
		t.Fatalf("names %v", got)
	}
	n1 := c.nodes["n1"]
	if cpu := n1.requested[corev1.ResourceCPU]; cpu.MilliValue() != 500 || n1.podCount != 1 || n1.hostPorts[portKey("", corev1.ProtocolTCP, 80)] != 1 {
		t.Fatalf("n1 after add: cpu %v pods %d ports %v", cpu.MilliValue(), n1.podCount, n1.hostPorts)
	}
	// Moving counts it out of the old node.
	c.update(cachedPod("a", "n0", "500m"))
	if n1.podCount != 0 || len(n1.hostPorts) != 0 || c.nodes["n0"].podCount != 1 {
		t.Fatalf("after move: n1 %d n0 %d", n1.podCount, c.nodes["n0"].podCount)
	}
	// A finished pod takes no room.
	done := cachedPod("a", "n0", "500m")
	done.Status.Phase = corev1.PodSucceeded
	c.update(done)
	if cpu := c.nodes["n0"].requested[corev1.ResourceCPU]; cpu.MilliValue() != 0 || c.nodes["n0"].podCount != 0 {
		t.Fatalf("after finish: cpu %v", cpu.MilliValue())
	}
}
