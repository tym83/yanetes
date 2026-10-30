package nanny

import (
	"context"
	"time"

	apierrors "k8s.io/apimachinery/pkg/api/errors"
	metav1 "k8s.io/apimachinery/pkg/apis/meta/v1"
	"k8s.io/client-go/informers"
	"k8s.io/client-go/kubernetes"
	batchlisters "k8s.io/client-go/listers/batch/v1"
	"k8s.io/client-go/tools/cache"
)

// TTLAfterFinished deletes jobs ttlSecondsAfterFinished after they finish.
type TTLAfterFinished struct {
	client kubernetes.Interface
	jobs   batchlisters.JobLister
	loop   *loop
}

func NewTTLAfterFinished(client kubernetes.Interface, factory informers.SharedInformerFactory) *TTLAfterFinished {
	t := &TTLAfterFinished{client: client, jobs: factory.Batch().V1().Jobs().Lister()}
	t.loop = newLoop("ttl-after-finished", 4, t.sync)
	_, _ = factory.Batch().V1().Jobs().Informer().AddEventHandler(cache.ResourceEventHandlerFuncs{
		AddFunc:    t.loop.addObject,
		UpdateFunc: func(_, obj any) { t.loop.addObject(obj) },
	})
	return t
}

func (t *TTLAfterFinished) Run(ctx context.Context) { t.loop.run(ctx) }

func (t *TTLAfterFinished) sync(ctx context.Context, key string) error {
	ns, name, _ := cache.SplitMetaNamespaceKey(key)
	job, err := t.jobs.Jobs(ns).Get(name)
	if err != nil || job.DeletionTimestamp != nil || job.Spec.TTLSecondsAfterFinished == nil {
		return nil
	}
	done, _, at := jobFinishedAt(job)
	if !done {
		return nil
	}
	if left := time.Until(at.Add(time.Duration(*job.Spec.TTLSecondsAfterFinished) * time.Second)); left > 0 {
		return later(left)
	}
	background := metav1.DeletePropagationBackground
	err = t.client.BatchV1().Jobs(ns).Delete(ctx, name, metav1.DeleteOptions{
		PropagationPolicy: &background,
		Preconditions:     &metav1.Preconditions{UID: &job.UID},
	})
	if apierrors.IsNotFound(err) || apierrors.IsConflict(err) {
		return nil
	}
	return err
}
