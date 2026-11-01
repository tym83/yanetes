// Package nanny holds the controllers of Яnetes: what upstream's
// kube-controller-manager does, written again against yapi.
package nanny

import (
	"context"
	"errors"
	"fmt"
	"log/slog"
	"time"

	"k8s.io/apimachinery/pkg/util/wait"
	"k8s.io/client-go/tools/cache"
	"k8s.io/client-go/util/workqueue"
)

// errLater asks for another look after a while without counting as a
// failure: a namespace still has content, a rollout waits for pods.
type errLater struct{ after time.Duration }

func (e errLater) Error() string { return fmt.Sprintf("again in %s", e.after) }

func later(d time.Duration) error { return errLater{d} }

// loop is one controller: a queue of keys and what to do with each.
type loop struct {
	name    string
	queue   workqueue.TypedRateLimitingInterface[string]
	sync    func(ctx context.Context, key string) error
	workers int
}

func newLoop(name string, workers int, sync func(ctx context.Context, key string) error) *loop {
	return &loop{
		name: name,
		queue: workqueue.NewTypedRateLimitingQueueWithConfig(
			workqueue.DefaultTypedControllerRateLimiter[string](),
			workqueue.TypedRateLimitingQueueConfig[string]{Name: name},
		),
		sync:    sync,
		workers: workers,
	}
}

func (l *loop) add(key string) { l.queue.Add(key) }

// addObject queues the namespace/name key of an informer object.
func (l *loop) addObject(obj any) {
	key, err := cache.DeletionHandlingMetaNamespaceKeyFunc(obj)
	if err == nil {
		l.queue.Add(key)
	}
}

func (l *loop) run(ctx context.Context) {
	defer l.queue.ShutDown()
	slog.Info("controller started", "controller", l.name, "workers", l.workers)
	for range l.workers {
		go wait.UntilWithContext(ctx, l.work, time.Second)
	}
	<-ctx.Done()
}

func (l *loop) work(ctx context.Context) {
	for {
		key, quit := l.queue.Get()
		if quit {
			return
		}
		err := l.sync(ctx, key)
		var again errLater
		switch {
		case err == nil:
			l.queue.Forget(key)
		case errors.As(err, &again):
			l.queue.Forget(key)
			l.queue.AddAfter(key, again.after)
		default:
			slog.Warn("sync failed", "controller", l.name, "key", key, "err", err)
			l.queue.AddRateLimited(key)
		}
		l.queue.Done(key)
	}
}

// handlers queues the object itself on every change.
func (l *loop) handlers() cache.ResourceEventHandlerFuncs {
	return cache.ResourceEventHandlerFuncs{
		AddFunc:    l.addObject,
		UpdateFunc: func(_, obj any) { l.addObject(obj) },
		DeleteFunc: l.addObject,
	}
}

// unwrap returns the object of a delete event whose final state was missed.
func unwrap(obj any) any {
	if t, ok := obj.(cache.DeletedFinalStateUnknown); ok {
		return t.Obj
	}
	return obj
}
