// yplanner places the pods of Yandexnetes on nodes.
package main

import (
	"context"
	"flag"
	"log/slog"
	"os"
	"os/signal"
	"syscall"

	"k8s.io/client-go/informers"
	"k8s.io/client-go/kubernetes"
	"k8s.io/client-go/tools/clientcmd"

	"github.com/tym83/yandexnetes/pkg/yplanner"
)

func main() {
	kubeconfig := flag.String("kubeconfig", "", "path to a kubeconfig; in-cluster config when empty")
	name := flag.String("scheduler-name", "default-scheduler", "the schedulerName of the pods this places")
	qps := flag.Float64("qps", 500, "requests per second to the API")
	flag.Parse()

	config, err := clientcmd.BuildConfigFromFlags("", *kubeconfig)
	if err != nil {
		slog.Error("yplanner: kubeconfig", "err", err)
		os.Exit(1)
	}
	config.QPS, config.Burst = float32(*qps), int(*qps*2)
	config.UserAgent = "yplanner"
	client, err := kubernetes.NewForConfig(config)
	if err != nil {
		slog.Error("yplanner: client", "err", err)
		os.Exit(1)
	}
	ctx, stop := signal.NotifyContext(context.Background(), syscall.SIGINT, syscall.SIGTERM)
	defer stop()

	factory := informers.NewSharedInformerFactory(client, 0)
	planner := yplanner.New(client, factory, *name)
	factory.Start(ctx.Done())
	factory.WaitForCacheSync(ctx.Done())
	planner.Run(ctx)
}
