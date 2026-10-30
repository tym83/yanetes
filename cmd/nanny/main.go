// nanny runs the controllers of Yandexnetes against yapi.
package main

import (
	"context"
	"flag"
	"log/slog"
	"os"
	"os/signal"
	"strings"
	"syscall"

	"k8s.io/client-go/informers"
	"k8s.io/client-go/kubernetes"
	"k8s.io/client-go/metadata"
	"k8s.io/client-go/tools/clientcmd"

	"github.com/tym83/yandexnetes/pkg/nanny"
)

func main() {
	kubeconfig := flag.String("kubeconfig", "", "path to a kubeconfig; in-cluster config when empty")
	rootCAFile := flag.String("root-ca-file", "", "CA bundle published to every namespace as kube-root-ca.crt")
	clusterCIDR := flag.String("cluster-cidr", "10.113.0.0/16", "pod addresses of the cluster")
	nodeMask := flag.Int("node-cidr-mask-size", 24, "size of the pod CIDR of each node")
	qps := flag.Float64("qps", 500, "requests per second to the API")
	flag.Parse()

	config, err := clientcmd.BuildConfigFromFlags("", *kubeconfig)
	if err != nil {
		fail("kubeconfig", err)
	}
	config.QPS, config.Burst = float32(*qps), int(*qps*2)
	config.UserAgent = "nanny"
	client, err := kubernetes.NewForConfig(config)
	if err != nil {
		fail("client", err)
	}
	meta, err := metadata.NewForConfig(config)
	if err != nil {
		fail("metadata client", err)
	}
	rootCA := ""
	if *rootCAFile != "" {
		b, err := os.ReadFile(*rootCAFile)
		if err != nil {
			fail("root CA", err)
		}
		rootCA = string(b)
	}

	ctx, stop := signal.NotifyContext(context.Background(), syscall.SIGINT, syscall.SIGTERM)
	defer stop()

	factory := informers.NewSharedInformerFactory(client, 0)
	ipam, err := nanny.NewNodeIPAM(client, factory, *clusterCIDR, *nodeMask)
	if err != nil {
		fail("node IPAM", err)
	}
	controllers := []interface{ Run(context.Context) }{
		nanny.NewNamespaces(client, meta, factory),
		nanny.NewServiceAccounts(client, factory, rootCA),
		nanny.NewReplicaSets(client, factory),
		nanny.NewDeployments(client, factory),
		nanny.NewReplicationControllers(client, factory),
		nanny.NewDaemonSets(client, factory),
		nanny.NewStatefulSets(client, factory),
		nanny.NewJobs(client, factory),
		nanny.NewCronJobs(client, factory),
		nanny.NewEndpoints(client, factory),
		ipam,
		nanny.NewNodeLifecycle(client, factory),
		nanny.NewPodGC(client, factory),
		nanny.NewGC(meta, client.Discovery()),
	}
	factory.Start(ctx.Done())
	for typ, ok := range factory.WaitForCacheSync(ctx.Done()) {
		if !ok {
			fail("cache sync", nil, "type", typ.String())
		}
	}
	slog.Info("nanny is watching the cluster", "controllers", len(controllers))
	for _, c := range controllers {
		go c.Run(ctx)
	}
	<-ctx.Done()
}

func fail(what string, err error, args ...any) {
	slog.Error(strings.TrimSpace("nanny: "+what), append([]any{"err", err}, args...)...)
	os.Exit(1)
}
