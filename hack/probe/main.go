// probe reproduces what conformance tests do against the server, one
// scenario at a time, and prints what came back.
package main

import (
	"context"
	"fmt"
	"os"
	"time"

	corev1 "k8s.io/api/core/v1"
	metav1 "k8s.io/apimachinery/pkg/apis/meta/v1"
	"k8s.io/apimachinery/pkg/types"
	"k8s.io/client-go/kubernetes"
	"k8s.io/client-go/tools/clientcmd"
)

func main() {
	cfg, err := clientcmd.BuildConfigFromFlags("", os.Args[1])
	if err != nil {
		panic(err)
	}
	cfg.ContentType = "application/vnd.kubernetes.protobuf"
	c := kubernetes.NewForConfigOrDie(cfg)
	ctx := context.Background()
	ns := "probe-watch"
	_, _ = c.CoreV1().Namespaces().Create(ctx, &corev1.Namespace{ObjectMeta: metav1.ObjectMeta{Name: ns}}, metav1.CreateOptions{})
	cms := c.CoreV1().ConfigMaps(ns)
	_, err = cms.Create(ctx, &corev1.ConfigMap{ObjectMeta: metav1.ObjectMeta{Name: "w", Labels: map[string]string{"special": "x"}}}, metav1.CreateOptions{})
	fmt.Println("create:", err)
	list, err := cms.List(ctx, metav1.ListOptions{LabelSelector: "special=x"})
	fmt.Println("list:", err, list.ResourceVersion, len(list.Items))
	w, err := cms.Watch(ctx, metav1.ListOptions{ResourceVersion: list.ResourceVersion, LabelSelector: "special=x"})
	fmt.Println("watch:", err)
	_, err = cms.Patch(ctx, "w", types.MergePatchType, []byte(`{"metadata":{"annotations":{"patched":"true"}}}`), metav1.PatchOptions{})
	fmt.Println("patch:", err)
	select {
	case evt, ok := <-w.ResultChan():
		fmt.Printf("event ok=%v type=%v obj=%T %v\n", ok, evt.Type, evt.Object, evt.Object)
	case <-time.After(10 * time.Second):
		fmt.Println("no event in 10s")
	}
	_ = c.CoreV1().Namespaces().Delete(ctx, ns, metav1.DeleteOptions{})
}
