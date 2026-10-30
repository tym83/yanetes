package nanny

import (
	"context"
	"fmt"
	"log/slog"
	"sort"
	"time"

	"github.com/robfig/cron/v3"
	batchv1 "k8s.io/api/batch/v1"
	corev1 "k8s.io/api/core/v1"
	apiequality "k8s.io/apimachinery/pkg/api/equality"
	apierrors "k8s.io/apimachinery/pkg/api/errors"
	metav1 "k8s.io/apimachinery/pkg/apis/meta/v1"
	"k8s.io/apimachinery/pkg/labels"
	"k8s.io/client-go/informers"
	"k8s.io/client-go/kubernetes"
	batchlisters "k8s.io/client-go/listers/batch/v1"
	"k8s.io/client-go/tools/cache"
	"k8s.io/utils/ptr"
)

var kindCronJob = schemaKind{"batch", "v1", "CronJob"}

const cronScheduledAnnotation = batchv1.CronJobScheduledTimestampAnnotation

// CronJobs starts jobs on a schedule, as upstream's cronjob controller v2.
type CronJobs struct {
	client   kubernetes.Interface
	cronJobs batchlisters.CronJobLister
	jobs     batchlisters.JobLister
	loop     *loop
}

func NewCronJobs(client kubernetes.Interface, factory informers.SharedInformerFactory) *CronJobs {
	c := &CronJobs{
		client:   client,
		cronJobs: factory.Batch().V1().CronJobs().Lister(),
		jobs:     factory.Batch().V1().Jobs().Lister(),
	}
	c.loop = newLoop("cronjob", 5, c.sync)
	_, _ = factory.Batch().V1().CronJobs().Informer().AddEventHandler(c.loop.handlers())
	byJob := func(obj any) {
		job, ok := unwrap(obj).(*batchv1.Job)
		if !ok {
			return
		}
		if ref := metav1.GetControllerOf(job); ref != nil && ref.Kind == kindCronJob.kind {
			c.loop.add(job.Namespace + "/" + ref.Name)
		}
	}
	_, _ = factory.Batch().V1().Jobs().Informer().AddEventHandler(cache.ResourceEventHandlerFuncs{
		AddFunc: byJob, UpdateFunc: func(_, obj any) { byJob(obj) }, DeleteFunc: byJob,
	})
	return c
}

func (c *CronJobs) Run(ctx context.Context) { c.loop.run(ctx) }

func parseSchedule(cj *batchv1.CronJob) (cron.Schedule, *time.Location, error) {
	loc := time.Local
	if tz := ptr.Deref(cj.Spec.TimeZone, ""); tz != "" {
		l, err := time.LoadLocation(tz)
		if err != nil {
			return nil, nil, err
		}
		loc = l
	}
	sched, err := cron.ParseStandard(cj.Spec.Schedule)
	return sched, loc, err
}

// lastMissed is the latest schedule time in (since, now], and how many
// there were; upstream gives up counting past a hundred.
func lastMissed(sched cron.Schedule, since, now time.Time) (time.Time, int) {
	var last time.Time
	n := 0
	for t := sched.Next(since); !t.After(now); t = sched.Next(t) {
		last = t
		n++
		if n > 100 {
			// Too many: jump to the most recent without walking them all.
			for t2 := sched.Next(now.Add(-time.Minute * 60 * 24 * 7)); !t2.After(now); t2 = sched.Next(t2) {
				last = t2
			}
			break
		}
	}
	return last, n
}

func jobFinishedAt(job *batchv1.Job) (bool, batchv1.JobConditionType, time.Time) {
	for _, cond := range job.Status.Conditions {
		if (cond.Type == batchv1.JobComplete || cond.Type == batchv1.JobFailed) && cond.Status == corev1.ConditionTrue {
			at := cond.LastTransitionTime.Time
			if cond.Type == batchv1.JobComplete && job.Status.CompletionTime != nil {
				at = job.Status.CompletionTime.Time
			}
			return true, cond.Type, at
		}
	}
	return false, "", time.Time{}
}

func (c *CronJobs) sync(ctx context.Context, key string) error {
	ns, name, _ := cache.SplitMetaNamespaceKey(key)
	cj, err := c.cronJobs.CronJobs(ns).Get(name)
	if apierrors.IsNotFound(err) {
		return nil
	}
	if err != nil {
		return err
	}
	if cj.DeletionTimestamp != nil {
		return nil
	}
	all, err := c.jobs.Jobs(ns).List(labels.Everything())
	if err != nil {
		return err
	}
	var owned []*batchv1.Job
	for _, job := range all {
		if ref := metav1.GetControllerOf(job); ref != nil && ref.UID == cj.UID {
			owned = append(owned, job)
		}
	}
	status := *cj.Status.DeepCopy()

	// Active jobs are the owned ones not finished; finished ones leave the list.
	status.Active = nil
	var running []*batchv1.Job
	for _, job := range owned {
		done, kind, at := jobFinishedAt(job)
		if !done {
			if job.DeletionTimestamp == nil {
				running = append(running, job)
				status.Active = append(status.Active, corev1.ObjectReference{
					Kind: "Job", Namespace: job.Namespace, Name: job.Name, UID: job.UID,
					APIVersion: "batch/v1",
				})
			}
			continue
		}
		if kind == batchv1.JobComplete && (status.LastSuccessfulTime == nil || at.After(status.LastSuccessfulTime.Time)) {
			status.LastSuccessfulTime = &metav1.Time{Time: at}
		}
	}

	sched, loc, err := parseSchedule(cj)
	if err != nil {
		slog.Warn("cronjob schedule does not parse", "cronjob", key, "err", err)
		return c.writeStatus(ctx, cj, status)
	}
	now := time.Now().In(loc)
	since := cj.CreationTimestamp.Time
	if status.LastScheduleTime != nil {
		since = status.LastScheduleTime.Time
	}
	if d := cj.Spec.StartingDeadlineSeconds; d != nil {
		if earliest := now.Add(-time.Duration(*d) * time.Second); earliest.After(since) {
			since = earliest
		}
	}
	var createErr error
	if !ptr.Deref(cj.Spec.Suspend, false) {
		scheduled, _ := lastMissed(sched, since.In(loc), now)
		if !scheduled.IsZero() {
			tooLate := cj.Spec.StartingDeadlineSeconds != nil &&
				now.Sub(scheduled) > time.Duration(*cj.Spec.StartingDeadlineSeconds)*time.Second
			switch {
			case tooLate:
			case cj.Spec.ConcurrencyPolicy == batchv1.ForbidConcurrent && len(running) > 0:
				// Wait for the running one; the same time is tried again later.
				scheduled = time.Time{}
			default:
				if cj.Spec.ConcurrencyPolicy == batchv1.ReplaceConcurrent {
					background := metav1.DeletePropagationBackground
					for _, job := range running {
						err := c.client.BatchV1().Jobs(ns).Delete(ctx, job.Name, metav1.DeleteOptions{PropagationPolicy: &background})
						if err != nil && !apierrors.IsNotFound(err) {
							return err
						}
					}
					status.Active = nil
				}
				job, err := c.createJob(ctx, cj, scheduled, loc)
				if err != nil {
					createErr = err
					scheduled = time.Time{}
				} else if job != nil {
					status.Active = append(status.Active, corev1.ObjectReference{
						Kind: "Job", Namespace: job.Namespace, Name: job.Name, UID: job.UID,
						APIVersion: "batch/v1",
					})
				}
			}
			if !scheduled.IsZero() {
				status.LastScheduleTime = &metav1.Time{Time: scheduled}
			}
		}
	}
	if err := c.cleanup(ctx, cj, owned); err != nil {
		return err
	}
	if err := c.writeStatus(ctx, cj, status); err != nil {
		return err
	}
	if createErr != nil {
		return createErr
	}
	if ptr.Deref(cj.Spec.Suspend, false) {
		return nil
	}
	next := sched.Next(now)
	if cj.Spec.ConcurrencyPolicy == batchv1.ForbidConcurrent && len(running) > 0 {
		// Job events bring it back when the running one ends.
		return nil
	}
	return later(time.Until(next) + 100*time.Millisecond)
}

// createJob starts the job of one schedule time; one that exists already
// was started by an earlier pass.
func (c *CronJobs) createJob(ctx context.Context, cj *batchv1.CronJob, scheduled time.Time, loc *time.Location) (*batchv1.Job, error) {
	tmpl := cj.Spec.JobTemplate
	annotations := map[string]string{}
	for k, v := range tmpl.Annotations {
		annotations[k] = v
	}
	annotations[cronScheduledAnnotation] = scheduled.In(loc).Format(time.RFC3339)
	jobLabels := map[string]string{}
	for k, v := range tmpl.Labels {
		jobLabels[k] = v
	}
	job := &batchv1.Job{
		ObjectMeta: metav1.ObjectMeta{
			Name:            fmt.Sprintf("%s-%d", cj.Name, scheduled.Unix()/60),
			Namespace:       cj.Namespace,
			Labels:          jobLabels,
			Annotations:     annotations,
			OwnerReferences: []metav1.OwnerReference{*metav1.NewControllerRef(cj, kindCronJob.gvk())},
		},
		Spec: *tmpl.Spec.DeepCopy(),
	}
	got, err := c.client.BatchV1().Jobs(cj.Namespace).Create(ctx, job, metav1.CreateOptions{})
	if apierrors.IsAlreadyExists(err) {
		return nil, nil
	}
	if apierrors.HasStatusCause(err, corev1.NamespaceTerminatingCause) {
		return nil, nil
	}
	return got, err
}

// cleanup keeps only as many finished jobs as the history limits allow.
func (c *CronJobs) cleanup(ctx context.Context, cj *batchv1.CronJob, owned []*batchv1.Job) error {
	type finished struct {
		job *batchv1.Job
		at  time.Time
	}
	var ok, failed []finished
	for _, job := range owned {
		if job.DeletionTimestamp != nil {
			continue
		}
		done, kind, at := jobFinishedAt(job)
		if !done {
			continue
		}
		if kind == batchv1.JobComplete {
			ok = append(ok, finished{job, at})
		} else {
			failed = append(failed, finished{job, at})
		}
	}
	background := metav1.DeletePropagationBackground
	trim := func(list []finished, limit *int32) error {
		if limit == nil {
			return nil
		}
		sort.Slice(list, func(i, j int) bool { return list[i].at.Before(list[j].at) })
		for i := 0; i < len(list)-int(*limit); i++ {
			err := c.client.BatchV1().Jobs(cj.Namespace).Delete(ctx, list[i].job.Name, metav1.DeleteOptions{PropagationPolicy: &background})
			if err != nil && !apierrors.IsNotFound(err) {
				return err
			}
		}
		return nil
	}
	return firstErr(trim(ok, cj.Spec.SuccessfulJobsHistoryLimit), trim(failed, cj.Spec.FailedJobsHistoryLimit))
}

func (c *CronJobs) writeStatus(ctx context.Context, cj *batchv1.CronJob, status batchv1.CronJobStatus) error {
	if apiequality.Semantic.DeepEqual(cj.Status, status) {
		return nil
	}
	cj = cj.DeepCopy()
	cj.Status = status
	_, err := c.client.BatchV1().CronJobs(cj.Namespace).UpdateStatus(ctx, cj, metav1.UpdateOptions{})
	if apierrors.IsNotFound(err) {
		return nil
	}
	return err
}
