// perf loads an API server the way a big cluster does and prints what it
// took: creations per second and latency percentiles. The same run against
// yapi on YDB and against kube-apiserver on etcd is the comparison.
//
//	perf -kubeconfig k nodes 30001        # KWOK-managed fake nodes
//	perf -kubeconfig k pods 100000 ns     # pods spread over those nodes
//	perf -kubeconfig k lists 200          # full lists of nodes and pods
package main

import (
	"context"
	"flag"
	"fmt"
	"os"
	"sort"
	"strconv"
	"sync"
	"sync/atomic"
	"time"

	corev1 "k8s.io/api/core/v1"
	apierrors "k8s.io/apimachinery/pkg/api/errors"
	"k8s.io/apimachinery/pkg/api/resource"
	metav1 "k8s.io/apimachinery/pkg/apis/meta/v1"
	"k8s.io/client-go/kubernetes"
	"k8s.io/client-go/tools/clientcmd"
)

const kwokNode = "kwok.x-k8s.io/node"

func main() {
	kubeconfig := flag.String("kubeconfig", os.Getenv("KUBECONFIG"), "kubeconfig of the server under test")
	workers := flag.Int("workers", 200, "requests in flight")
	flag.Parse()
	if flag.NArg() < 2 {
		fmt.Fprintln(os.Stderr, "usage: perf [flags] nodes|pods|lists COUNT [NAMESPACE]")
		os.Exit(2)
	}
	cfg, err := clientcmd.BuildConfigFromFlags("", *kubeconfig)
	if err != nil {
		panic(err)
	}
	cfg.QPS, cfg.Burst = -1, 0 // no client-side throttling: the server is what we measure
	cfg.ContentType = "application/vnd.kubernetes.protobuf"
	client := kubernetes.NewForConfigOrDie(cfg)
	count, err := strconv.Atoi(flag.Arg(1))
	if err != nil {
		panic(err)
	}
	ctx := context.Background()
	switch flag.Arg(0) {
	case "nodes":
		run("create node", count, *workers, func(i int) error {
			_, err := client.CoreV1().Nodes().Create(ctx, fakeNode(i), metav1.CreateOptions{})
			return ignoreExists(err)
		})
	case "pods":
		ns := flag.Arg(2)
		if ns == "" {
			ns = "perf"
		}
		_, _ = client.CoreV1().Namespaces().Create(ctx, &corev1.Namespace{ObjectMeta: metav1.ObjectMeta{Name: ns}}, metav1.CreateOptions{})
		run("create pod", count, *workers, func(i int) error {
			_, err := client.CoreV1().Pods(ns).Create(ctx, fakePod(i), metav1.CreateOptions{})
			return ignoreExists(err)
		})
	case "lists":
		run("list nodes+pods", count, min(*workers, 8), func(int) error {
			if _, err := client.CoreV1().Nodes().List(ctx, metav1.ListOptions{}); err != nil {
				return err
			}
			_, err := client.CoreV1().Pods("").List(ctx, metav1.ListOptions{ResourceVersion: "0"})
			return err
		})
	default:
		fmt.Fprintln(os.Stderr, "unknown scenario", flag.Arg(0))
		os.Exit(2)
	}
}

func ignoreExists(err error) error {
	if apierrors.IsAlreadyExists(err) {
		return nil
	}
	return err
}

func fakeNode(i int) *corev1.Node {
	capacity := corev1.ResourceList{
		corev1.ResourceCPU:    resource.MustParse("32"),
		corev1.ResourceMemory: resource.MustParse("256Gi"),
		corev1.ResourcePods:   resource.MustParse("110"),
	}
	return &corev1.Node{
		ObjectMeta: metav1.ObjectMeta{
			Name:        fmt.Sprintf("kwok-%05d", i),
			Labels:      map[string]string{"type": "kwok", "kubernetes.io/hostname": fmt.Sprintf("kwok-%05d", i)},
			Annotations: map[string]string{kwokNode: "fake", "node.alpha.kubernetes.io/ttl": "0"},
		},
		Spec: corev1.NodeSpec{Taints: []corev1.Taint{{Key: kwokNode, Value: "fake", Effect: corev1.TaintEffectNoSchedule}}},
		Status: corev1.NodeStatus{
			Capacity: capacity, Allocatable: capacity,
			NodeInfo: corev1.NodeSystemInfo{KubeletVersion: "fake", OperatingSystem: "linux", Architecture: "amd64"},
		},
	}
}

func fakePod(i int) *corev1.Pod {
	return &corev1.Pod{
		ObjectMeta: metav1.ObjectMeta{Name: fmt.Sprintf("perf-%07d", i), Labels: map[string]string{"app": "perf"}},
		Spec: corev1.PodSpec{
			Containers: []corev1.Container{{
				Name: "app", Image: "fake",
				Resources: corev1.ResourceRequirements{Requests: corev1.ResourceList{
					corev1.ResourceCPU: resource.MustParse("100m"), corev1.ResourceMemory: resource.MustParse("64Mi"),
				}},
			}},
			NodeSelector: map[string]string{"type": "kwok"},
			Tolerations:  []corev1.Toleration{{Key: kwokNode, Operator: corev1.TolerationOpExists, Effect: corev1.TaintEffectNoSchedule}},
		},
	}
}

// run does count requests with workers in flight and reports throughput
// and latency percentiles.
func run(what string, count, workers int, do func(int) error) {
	latencies := make([]time.Duration, count)
	var next, failed atomic.Int64
	var firstErr atomic.Value
	start := time.Now()
	var wg sync.WaitGroup
	for range workers {
		wg.Add(1)
		go func() {
			defer wg.Done()
			for {
				i := int(next.Add(1) - 1)
				if i >= count {
					return
				}
				t := time.Now()
				if err := do(i); err != nil {
					failed.Add(1)
					firstErr.CompareAndSwap(nil, err)
				}
				latencies[i] = time.Since(t)
			}
		}()
	}
	wg.Wait()
	elapsed := time.Since(start)
	sort.Slice(latencies, func(a, b int) bool { return latencies[a] < latencies[b] })
	pct := func(p float64) time.Duration { return latencies[min(len(latencies)-1, int(float64(len(latencies))*p))] }
	fmt.Printf("%s: %d in %s, %.0f/s, p50 %s, p90 %s, p99 %s, max %s, failed %d\n",
		what, count, elapsed.Round(time.Millisecond), float64(count)/elapsed.Seconds(),
		pct(0.50).Round(time.Millisecond), pct(0.90).Round(time.Millisecond), pct(0.99).Round(time.Millisecond),
		latencies[len(latencies)-1].Round(time.Millisecond), failed.Load())
	if err, ok := firstErr.Load().(error); ok {
		fmt.Printf("  first error: %v\n", err)
	}
}
