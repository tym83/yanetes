// probe creates a ValidatingWebhookConfiguration through a protobuf client
// and prints what the server kept, to check the protobuf decoder.
package main

import (
	"context"
	"fmt"
	"os"

	admissionv1 "k8s.io/api/admissionregistration/v1"
	metav1 "k8s.io/apimachinery/pkg/apis/meta/v1"
	"k8s.io/client-go/kubernetes"
	"k8s.io/client-go/tools/clientcmd"
	"k8s.io/utils/ptr"
)

func main() {
	cfg, err := clientcmd.BuildConfigFromFlags("", os.Args[1])
	if err != nil {
		panic(err)
	}
	cfg.ContentType = "application/vnd.kubernetes.protobuf"
	c := kubernetes.NewForConfigOrDie(cfg)
	none := admissionv1.SideEffectClassNone
	v := &admissionv1.ValidatingWebhookConfiguration{
		ObjectMeta: metav1.ObjectMeta{Name: "probe-proto"},
		Webhooks: []admissionv1.ValidatingWebhook{{
			Name: "probe.example.com", SideEffects: &none, AdmissionReviewVersions: []string{"v1"},
			ClientConfig: admissionv1.WebhookClientConfig{URL: ptr.To("https://example.invalid/x")},
		}},
	}
	got, err := c.AdmissionregistrationV1().ValidatingWebhookConfigurations().Create(context.Background(), v, metav1.CreateOptions{})
	fmt.Println("err:", err)
	if got != nil {
		fmt.Println("webhooks kept:", len(got.Webhooks))
		got.Webhooks[0].Rules = []admissionv1.RuleWithOperations{{Operations: []admissionv1.OperationType{admissionv1.Update}}}
		upd, err := c.AdmissionregistrationV1().ValidatingWebhookConfigurations().Update(context.Background(), got, metav1.UpdateOptions{})
		fmt.Println("update err:", err)
		if upd != nil {
			fmt.Println("after update:", len(upd.Webhooks))
		}
	}
	_ = c.AdmissionregistrationV1().ValidatingWebhookConfigurations().Delete(context.Background(), "probe-proto", metav1.DeleteOptions{})
}
