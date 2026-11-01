package nanny

import (
	"testing"

	appsv1 "k8s.io/api/apps/v1"
	metav1 "k8s.io/apimachinery/pkg/apis/meta/v1"
	"k8s.io/apimachinery/pkg/util/intstr"
	"k8s.io/utils/ptr"
)

// The case upstream's conformance test checks: a rollout stuck at 8 old and
// 5 new pods of 10 (surge 3) scaled to 30 becomes 20 and 13.
func TestProportionSpreadsByLastSize(t *testing.T) {
	dep := &appsv1.Deployment{Spec: appsv1.DeploymentSpec{
		Replicas: ptr.To[int32](30),
		Strategy: appsv1.DeploymentStrategy{
			Type: appsv1.RollingUpdateDeploymentStrategyType,
			RollingUpdate: &appsv1.RollingUpdateDeployment{
				MaxSurge:       ptr.To(intstr.FromInt32(3)),
				MaxUnavailable: ptr.To(intstr.FromInt32(2)),
			},
		},
	}}
	set := func(n int32) *appsv1.ReplicaSet {
		return &appsv1.ReplicaSet{
			ObjectMeta: metav1.ObjectMeta{Annotations: map[string]string{annMaxReplicas: "13"}},
			Spec:       appsv1.ReplicaSetSpec{Replicas: ptr.To(n)},
		}
	}
	toAdd := int32(33 - 13)
	first := proportion(set(8), dep, toAdd, 0)
	second := proportion(set(5), dep, toAdd, first)
	if 8+first != 20 || 5+second != 13 {
		t.Fatalf("got %d and %d, want 20 and 13", 8+first, 5+second)
	}
}
