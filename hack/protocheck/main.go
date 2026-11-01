// protocheck reads the same objects through a JSON client and a protobuf
// client and reports any difference between what the two decoded, then
// watches in protobuf: the server's protobuf answers must say exactly what
// its JSON ones say.
//
//	protocheck KUBECONFIG
package main

import (
	"context"
	"encoding/json"
	"fmt"
	"os"
	"strings"
	"time"

	corev1 "k8s.io/api/core/v1"
	apiequality "k8s.io/apimachinery/pkg/api/equality"
	metav1 "k8s.io/apimachinery/pkg/apis/meta/v1"
	"k8s.io/client-go/kubernetes"
	"k8s.io/client-go/tools/clientcmd"
)

func main() {
	cfg, err := clientcmd.BuildConfigFromFlags("", os.Args[1])
	if err != nil {
		panic(err)
	}
	jsonCfg, protoCfg := *cfg, *cfg
	jsonCfg.ContentType, jsonCfg.AcceptContentTypes = "application/json", "application/json"
	protoCfg.ContentType = "application/vnd.kubernetes.protobuf"
	protoCfg.AcceptContentTypes = "application/vnd.kubernetes.protobuf, application/json"
	j := kubernetes.NewForConfigOrDie(&jsonCfg)
	p := kubernetes.NewForConfigOrDie(&protoCfg)
	ctx := context.Background()
	bad := 0
	same := func(what string, a, b any) {
		if !apiequality.Semantic.DeepEqual(normalized(a), normalized(b)) {
			bad++
			fmt.Printf("DIFF %s:\n%s\n", what, firstDifference(a, b))
		}
	}
	check := func(what string, list func(kubernetes.Interface) (any, error)) {
		a, err1 := list(j)
		b, err2 := list(p)
		if err1 != nil || err2 != nil {
			bad++
			fmt.Printf("ERR %s: json %v, protobuf %v\n", what, err1, err2)
			return
		}
		same(what, a, b)
		fmt.Printf("ok %s\n", what)
	}
	opts := metav1.ListOptions{Limit: 200}
	check("pods", func(c kubernetes.Interface) (any, error) {
		l, err := c.CoreV1().Pods("").List(ctx, opts)
		if l != nil {
			l.ResourceVersion, l.Continue = "", ""
		}
		return l, err
	})
	check("nodes", func(c kubernetes.Interface) (any, error) {
		l, err := c.CoreV1().Nodes().List(ctx, opts)
		if l != nil {
			l.ResourceVersion, l.Continue, l.RemainingItemCount = "", "", nil
		}
		return l, err
	})
	check("services", func(c kubernetes.Interface) (any, error) { return c.CoreV1().Services("").List(ctx, opts) })
	check("configmaps", func(c kubernetes.Interface) (any, error) { return c.CoreV1().ConfigMaps("").List(ctx, opts) })
	check("secrets", func(c kubernetes.Interface) (any, error) { return c.CoreV1().Secrets("").List(ctx, opts) })
	check("serviceaccounts", func(c kubernetes.Interface) (any, error) { return c.CoreV1().ServiceAccounts("").List(ctx, opts) })
	check("endpointslices", func(c kubernetes.Interface) (any, error) { return c.DiscoveryV1().EndpointSlices("").List(ctx, opts) })
	check("deployments", func(c kubernetes.Interface) (any, error) { return c.AppsV1().Deployments("").List(ctx, opts) })
	check("leases", func(c kubernetes.Interface) (any, error) {
		return c.CoordinationV1().Leases("kube-system").List(ctx, opts)
	})
	check("clusterroles", func(c kubernetes.Interface) (any, error) { return c.RbacV1().ClusterRoles().List(ctx, opts) })
	check("events", func(c kubernetes.Interface) (any, error) { return c.CoreV1().Events("").List(ctx, opts) })
	check("namespaces", func(c kubernetes.Interface) (any, error) { return c.CoreV1().Namespaces().List(ctx, opts) })
	check("kube-system pods", func(c kubernetes.Interface) (any, error) { return c.CoreV1().Pods("kube-system").List(ctx, opts) })
	check("real nodes", func(c kubernetes.Interface) (any, error) {
		return c.CoreV1().Nodes().List(ctx, metav1.ListOptions{LabelSelector: "type!=kwok"})
	})
	check("statefulsets", func(c kubernetes.Interface) (any, error) { return c.AppsV1().StatefulSets("").List(ctx, opts) })
	check("daemonsets", func(c kubernetes.Interface) (any, error) { return c.AppsV1().DaemonSets("").List(ctx, opts) })
	check("jobs", func(c kubernetes.Interface) (any, error) { return c.BatchV1().Jobs("").List(ctx, opts) })
	check("cronjobs", func(c kubernetes.Interface) (any, error) { return c.BatchV1().CronJobs("").List(ctx, opts) })
	check("ingresses", func(c kubernetes.Interface) (any, error) { return c.NetworkingV1().Ingresses("").List(ctx, opts) })
	check("networkpolicies", func(c kubernetes.Interface) (any, error) { return c.NetworkingV1().NetworkPolicies("").List(ctx, opts) })
	check("pdbs", func(c kubernetes.Interface) (any, error) {
		return c.PolicyV1().PodDisruptionBudgets("").List(ctx, opts)
	})
	check("priorityclasses", func(c kubernetes.Interface) (any, error) { return c.SchedulingV1().PriorityClasses().List(ctx, opts) })
	check("rolebindings", func(c kubernetes.Interface) (any, error) { return c.RbacV1().RoleBindings("").List(ctx, opts) })
	check("vaps", func(c kubernetes.Interface) (any, error) {
		return c.AdmissionregistrationV1().ValidatingAdmissionPolicies().List(ctx, opts)
	})
	check("webhooks", func(c kubernetes.Interface) (any, error) {
		return c.AdmissionregistrationV1().ValidatingWebhookConfigurations().List(ctx, opts)
	})
	check("ipaddresses", func(c kubernetes.Interface) (any, error) { return c.NetworkingV1().IPAddresses().List(ctx, opts) })
	check("storageclasses", func(c kubernetes.Interface) (any, error) { return c.StorageV1().StorageClasses().List(ctx, opts) })
	// Errors come back as Status in protobuf too.
	_, err1 := j.CoreV1().Pods("default").Get(ctx, "no-such-pod", metav1.GetOptions{})
	_, err2 := p.CoreV1().Pods("default").Get(ctx, "no-such-pod", metav1.GetOptions{})
	if fmt.Sprint(err1) != fmt.Sprint(err2) {
		bad++
		fmt.Printf("DIFF not found: json %v, protobuf %v\n", err1, err2)
	} else {
		fmt.Printf("ok not found: %v\n", err2)
	}
	// A protobuf watch sees a change made through JSON, decoded the same.
	ns := fmt.Sprintf("protocheck-%d", time.Now().Unix())
	_, _ = j.CoreV1().Namespaces().Create(ctx, &corev1.Namespace{ObjectMeta: metav1.ObjectMeta{Name: ns}}, metav1.CreateOptions{})
	// Only ours: the namespace gets its kube-root-ca.crt meanwhile.
	w, err := p.CoreV1().ConfigMaps(ns).Watch(ctx, metav1.ListOptions{LabelSelector: "a=b"})
	if err != nil {
		panic(err)
	}
	created, err := j.CoreV1().ConfigMaps(ns).Create(ctx, &corev1.ConfigMap{
		ObjectMeta: metav1.ObjectMeta{GenerateName: "cm-", Labels: map[string]string{"a": "b"}},
		Data:       map[string]string{"k": "v"},
	}, metav1.CreateOptions{})
	if err != nil {
		panic(err)
	}
	select {
	case e := <-w.ResultChan():
		got, ok := e.Object.(*corev1.ConfigMap)
		if !ok {
			bad++
			fmt.Printf("DIFF watch: got %T %v\n", e.Object, e.Object)
		} else {
			same("watch event", created, got)
			fmt.Printf("ok watch %s %s\n", e.Type, got.Name)
		}
	case <-time.After(10 * time.Second):
		bad++
		fmt.Println("ERR watch: no event in 10s")
	}
	w.Stop()
	_ = j.CoreV1().Namespaces().Delete(ctx, ns, metav1.DeleteOptions{})
	fmt.Printf("%d problems\n", bad)
	if bad > 0 {
		os.Exit(1)
	}
}

// firstDifference shows where the two decodings part, as indented JSON.
func firstDifference(a, b any) string {
	x, _ := json.MarshalIndent(a, "", " ")
	y, _ := json.MarshalIndent(b, "", " ")
	xs, ys := strings.Split(string(x), "\n"), strings.Split(string(y), "\n")
	for i := 0; i < len(xs) && i < len(ys); i++ {
		if xs[i] != ys[i] {
			from := max(0, i-6)
			return "json:\n" + strings.Join(xs[from:min(len(xs), i+3)], "\n") + "\nprotobuf:\n" + strings.Join(ys[from:min(len(ys), i+3)], "\n")
		}
	}
	return fmt.Sprintf("lengths %d and %d", len(xs), len(ys))
}

// normalized is an object as plain JSON data, without the kind and
// apiVersion of list items: JSON answers carry them, protobuf ones cannot.
func normalized(v any) any {
	raw, _ := json.Marshal(v)
	var out map[string]any
	_ = json.Unmarshal(raw, &out)
	// Lists read twice differ in revision and paging on a live cluster.
	delete(out, "metadata")
	// Protobuf has no empty list apart from a missing one.
	if items, ok := out["items"].([]any); ok && len(items) == 0 {
		out["items"] = nil
	}
	if items, ok := out["items"].([]any); ok {
		for _, item := range items {
			if m, ok := item.(map[string]any); ok {
				delete(m, "kind")
				delete(m, "apiVersion")
			}
		}
	}
	return out
}
