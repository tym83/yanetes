package nanny

import (
	"context"
	"encoding/json"
	"fmt"
	"slices"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"

	batchv1 "k8s.io/api/batch/v1"
	corev1 "k8s.io/api/core/v1"
	apiequality "k8s.io/apimachinery/pkg/api/equality"
	apierrors "k8s.io/apimachinery/pkg/api/errors"
	metav1 "k8s.io/apimachinery/pkg/apis/meta/v1"
	"k8s.io/apimachinery/pkg/labels"
	"k8s.io/apimachinery/pkg/types"
	"k8s.io/client-go/informers"
	"k8s.io/client-go/kubernetes"
	batchlisters "k8s.io/client-go/listers/batch/v1"
	corelisters "k8s.io/client-go/listers/core/v1"
	"k8s.io/client-go/tools/cache"
	"k8s.io/utils/ptr"
)

var kindJob = schemaKind{"batch", "v1", "Job"}

const (
	jobTrackingFinalizer = batchv1.JobTrackingFinalizer
	jobIndexAnnotation   = batchv1.JobCompletionIndexAnnotation
	jobIndexEnv          = "JOB_COMPLETION_INDEX"
	jobBackoffBase       = 10 * time.Second
	jobBackoffMax        = 6 * time.Minute
)

// Jobs runs pods to completion: as many at once as the parallelism says,
// until enough succeed or too many fail. Every pod carries the tracking
// finalizer until its end is counted in the job's status, so a pod
// deleted early is still counted, as upstream does.
type Jobs struct {
	client kubernetes.Interface
	jobs   batchlisters.JobLister
	pods   corelisters.PodLister
	exp    *expectations
	loop   *loop
	// orphans drops the tracking finalizer of pods no job owns.
	orphans *loop

	mu       sync.Mutex
	failedAt map[types.UID]time.Time // the last failure each job saw, for backoff
	// written is the resourceVersion of each job's last status write: a
	// cache older than that would count pods whose finalizers are already
	// gone as never finished, and start them again.
	written map[types.UID]int64
}

func NewJobs(client kubernetes.Interface, factory informers.SharedInformerFactory) *Jobs {
	j := &Jobs{
		client:   client,
		jobs:     factory.Batch().V1().Jobs().Lister(),
		pods:     factory.Core().V1().Pods().Lister(),
		exp:      newExpectations(),
		failedAt: map[types.UID]time.Time{},
		written:  map[types.UID]int64{},
	}
	j.loop = newLoop("job", 10, j.sync)
	j.orphans = newLoop("job-orphans", 2, j.syncOrphan)
	_, _ = factory.Batch().V1().Jobs().Informer().AddEventHandler(j.loop.handlers())
	byPod := func(obj any, added, deleted bool) {
		pod, ok := unwrap(obj).(*corev1.Pod)
		if !ok {
			return
		}
		ref := metav1.GetControllerOf(pod)
		if ref != nil && ref.Kind == kindJob.kind {
			key := pod.Namespace + "/" + ref.Name
			switch {
			case added:
				j.exp.observe(key, 1, 0)
			case deleted:
				j.exp.observe(key, 0, 1)
			}
			j.loop.add(key)
		} else if !deleted {
			j.wakeMatching(pod)
		}
		if !deleted && slices.Contains(pod.Finalizers, jobTrackingFinalizer) {
			j.orphans.addObject(pod)
		}
	}
	_, _ = factory.Core().V1().Pods().Informer().AddEventHandler(cache.ResourceEventHandlerFuncs{
		AddFunc:    func(obj any) { byPod(obj, true, false) },
		UpdateFunc: func(_, obj any) { byPod(obj, false, false) },
		DeleteFunc: func(obj any) { byPod(obj, false, true) },
	})
	return j
}

func (j *Jobs) Run(ctx context.Context) {
	go j.orphans.run(ctx)
	j.loop.run(ctx)
}

func (j *Jobs) wakeMatching(pod *corev1.Pod) {
	jobs, _ := j.jobs.Jobs(pod.Namespace).List(labels.Everything())
	for _, job := range jobs {
		sel, err := metav1.LabelSelectorAsSelector(job.Spec.Selector)
		if err == nil && !sel.Empty() && sel.Matches(labels.Set(pod.Labels)) {
			j.loop.add(pod.Namespace + "/" + job.Name)
		}
	}
}

// syncOrphan drops the tracking finalizer of a pod whose job is gone or
// that its job let go of.
func (j *Jobs) syncOrphan(ctx context.Context, key string) error {
	ns, name, _ := cache.SplitMetaNamespaceKey(key)
	pod, err := j.pods.Pods(ns).Get(name)
	if err != nil || !slices.Contains(pod.Finalizers, jobTrackingFinalizer) {
		return nil
	}
	if ref := metav1.GetControllerOf(pod); ref != nil && ref.Kind == kindJob.kind {
		job, err := j.jobs.Jobs(ns).Get(ref.Name)
		if err == nil && job.UID == ref.UID {
			return nil
		}
		if err == nil || !apierrors.IsNotFound(err) {
			return err
		}
		// The cache may lag behind a job just created: ask the API.
		job, err = j.client.BatchV1().Jobs(ns).Get(ctx, ref.Name, metav1.GetOptions{})
		if err == nil && job.UID == ref.UID {
			return nil
		}
	}
	return dropTrackingFinalizer(ctx, j.client, pod)
}

func dropTrackingFinalizer(ctx context.Context, client kubernetes.Interface, pod *corev1.Pod) error {
	left := slices.DeleteFunc(slices.Clone(pod.Finalizers), func(f string) bool { return f == jobTrackingFinalizer })
	body, _ := json.Marshal(map[string]any{"metadata": map[string]any{"finalizers": left, "uid": pod.UID}})
	_, err := client.CoreV1().Pods(pod.Namespace).Patch(ctx, pod.Name, types.MergePatchType, body, metav1.PatchOptions{})
	if apierrors.IsNotFound(err) || apierrors.IsConflict(err) {
		return nil
	}
	return err
}

func jobCondition(job *batchv1.Job, t batchv1.JobConditionType) *batchv1.JobCondition {
	for i := range job.Status.Conditions {
		if job.Status.Conditions[i].Type == t && job.Status.Conditions[i].Status == corev1.ConditionTrue {
			return &job.Status.Conditions[i]
		}
	}
	return nil
}

func jobFinished(job *batchv1.Job) bool {
	return jobCondition(job, batchv1.JobComplete) != nil || jobCondition(job, batchv1.JobFailed) != nil
}

func setJobCondition(conds []batchv1.JobCondition, t batchv1.JobConditionType, status corev1.ConditionStatus, reason, message string, now metav1.Time) []batchv1.JobCondition {
	for i, c := range conds {
		if c.Type != t {
			continue
		}
		if c.Status != status || c.Reason != reason || c.Message != message {
			if c.Status != status {
				conds[i].LastTransitionTime = now
			}
			conds[i].Status, conds[i].Reason, conds[i].Message, conds[i].LastProbeTime = status, reason, message, now
		}
		return conds
	}
	if status != corev1.ConditionTrue && t != batchv1.JobSuspended {
		return conds
	}
	return append(conds, batchv1.JobCondition{Type: t, Status: status, Reason: reason, Message: message, LastProbeTime: now, LastTransitionTime: now})
}

func podIndex(pod *corev1.Pod) int {
	v, ok := pod.Annotations[jobIndexAnnotation]
	if !ok {
		v = pod.Labels[jobIndexAnnotation]
	}
	i, err := strconv.Atoi(v)
	if err != nil {
		return -1
	}
	return i
}

// formatIndexes writes a set of indexes the way completedIndexes does: "0-2,5".
func formatIndexes(set map[int]bool) string {
	var idx []int
	for i := range set {
		idx = append(idx, i)
	}
	sort.Ints(idx)
	var parts []string
	for i := 0; i < len(idx); {
		k := i
		for k+1 < len(idx) && idx[k+1] == idx[k]+1 {
			k++
		}
		if k == i {
			parts = append(parts, strconv.Itoa(idx[i]))
		} else {
			parts = append(parts, fmt.Sprintf("%d-%d", idx[i], idx[k]))
		}
		i = k + 1
	}
	return strings.Join(parts, ",")
}

func parseIndexes(s string) map[int]bool {
	set := map[int]bool{}
	for _, part := range strings.Split(s, ",") {
		if part == "" {
			continue
		}
		lo, hi, found := strings.Cut(part, "-")
		a, _ := strconv.Atoi(lo)
		b := a
		if found {
			b, _ = strconv.Atoi(hi)
		}
		for i := a; i <= b; i++ {
			set[i] = true
		}
	}
	return set
}

// onlyReplaceFailed: a terminating pod is not replaced, nor counted as
// failed, until it is Failed.
func onlyReplaceFailed(job *batchv1.Job) bool {
	return job.Spec.PodFailurePolicy != nil ||
		ptr.Deref(job.Spec.PodReplacementPolicy, batchv1.TerminatingOrFailed) == batchv1.Failed
}

func podFailed(job *batchv1.Job, pod *corev1.Pod) bool {
	if pod.Status.Phase == corev1.PodFailed {
		return true
	}
	if onlyReplaceFailed(job) {
		return false
	}
	return pod.DeletionTimestamp != nil && pod.Status.Phase != corev1.PodSucceeded
}

// failurePolicyAction says what the job's pod failure policy makes of a
// failed pod: "" when no rule matches.
func failurePolicyAction(job *batchv1.Job, pod *corev1.Pod) (batchv1.PodFailurePolicyAction, string) {
	if job.Spec.PodFailurePolicy == nil {
		return "", ""
	}
	for i, rule := range job.Spec.PodFailurePolicy.Rules {
		if req := rule.OnExitCodes; req != nil {
			statuses := append(append([]corev1.ContainerStatus(nil), pod.Status.InitContainerStatuses...), pod.Status.ContainerStatuses...)
			for _, cs := range statuses {
				if req.ContainerName != nil && *req.ContainerName != cs.Name {
					continue
				}
				t := cs.State.Terminated
				if t == nil || t.ExitCode == 0 {
					continue
				}
				in := slices.Contains(req.Values, t.ExitCode)
				if (req.Operator == batchv1.PodFailurePolicyOnExitCodesOpIn) == in {
					return rule.Action, fmt.Sprintf("Container %s for pod %s/%s failed with exit code %d matching %s rule at index %d",
						cs.Name, pod.Namespace, pod.Name, t.ExitCode, rule.Action, i)
				}
			}
		}
		for _, pat := range rule.OnPodConditions {
			for _, c := range pod.Status.Conditions {
				if string(c.Type) == string(pat.Type) && c.Status == pat.Status {
					return rule.Action, fmt.Sprintf("Pod %s/%s has condition %s matching %s rule at index %d",
						pod.Namespace, pod.Name, c.Type, rule.Action, i)
				}
			}
		}
	}
	return "", ""
}

func (j *Jobs) sync(ctx context.Context, key string) error {
	ns, name, _ := cache.SplitMetaNamespaceKey(key)
	job, err := j.jobs.Jobs(ns).Get(name)
	if apierrors.IsNotFound(err) {
		j.exp.forget(key)
		return nil
	}
	if err != nil {
		return err
	}
	j.mu.Lock()
	wrote := j.written[job.UID]
	j.mu.Unlock()
	if rv, err := strconv.ParseInt(job.ResourceVersion, 10, 64); err == nil && rv < wrote {
		return later(100 * time.Millisecond)
	}
	selector, err := metav1.LabelSelectorAsSelector(job.Spec.Selector)
	if err != nil || selector.Empty() {
		return nil
	}
	all, err := j.pods.Pods(ns).List(labels.Everything())
	if err != nil {
		return err
	}
	pods, err := claimPods(ctx, j.client, job, kindJob, selector, all)
	if err != nil {
		return err
	}
	if jobFinished(job) {
		// Everything is counted: nothing more to hold on to.
		for _, p := range pods {
			if slices.Contains(p.Finalizers, jobTrackingFinalizer) {
				if err := dropTrackingFinalizer(ctx, j.client, p); err != nil {
					return err
				}
			}
		}
		return nil
	}
	return j.reconcile(ctx, key, job, pods)
}

func (j *Jobs) reconcile(ctx context.Context, key string, job *batchv1.Job, pods []*corev1.Pod) error {
	var err error
	now := metav1.Now()
	status := *job.Status.DeepCopy()
	indexed := ptr.Deref(job.Spec.CompletionMode, batchv1.NonIndexedCompletion) == batchv1.IndexedCompletion
	if status.UncountedTerminatedPods == nil {
		status.UncountedTerminatedPods = &batchv1.UncountedTerminatedPods{}
	}
	uncounted := status.UncountedTerminatedPods
	completed := parseIndexes(status.CompletedIndexes)
	// Per-index backoff: an index whose pods failed too often, or that a
	// FailIndex rule matched, is given up; the rest go on.
	perIndex := indexed && job.Spec.BackoffLimitPerIndex != nil
	failedIdx := parseIndexes(ptr.Deref(status.FailedIndexes, ""))
	indexFailures := map[int]int{}
	for _, p := range pods {
		if i := podIndex(p); i >= 0 && p.Status.Phase == corev1.PodFailed {
			indexFailures[i] = max(indexFailures[i], indexFailureCount(p)+1)
		}
	}

	var activePods []*corev1.Pod
	var terminating, ready int32
	var newlyFinished, released []*corev1.Pod
	failJob, failMessage := false, ""
	for _, p := range pods {
		succeeded := p.Status.Phase == corev1.PodSucceeded
		failed := !succeeded && podFailed(job, p)
		if p.DeletionTimestamp != nil && !succeeded && p.Status.Phase != corev1.PodFailed {
			terminating++
		}
		if !succeeded && !failed {
			if p.DeletionTimestamp == nil {
				activePods = append(activePods, p)
				if podReady(p) {
					ready++
				}
			}
			continue
		}
		// A finished pod is counted once: while it holds the finalizer and
		// is not yet among the uncounted.
		if !slices.Contains(p.Finalizers, jobTrackingFinalizer) ||
			slices.Contains(uncounted.Succeeded, p.UID) || slices.Contains(uncounted.Failed, p.UID) {
			continue
		}
		if failed {
			action, msg := failurePolicyAction(job, p)
			switch action {
			case batchv1.PodFailurePolicyActionIgnore:
				// Neither counted nor kept: just released.
				released = append(released, p)
				continue
			case batchv1.PodFailurePolicyActionFailJob:
				failJob, failMessage = true, msg
			}
			if perIndex {
				if i := podIndex(p); i >= 0 && (action == batchv1.PodFailurePolicyActionFailIndex || indexFailureCount(p)+1 > int(*job.Spec.BackoffLimitPerIndex)) {
					failedIdx[i] = true
				}
			}
			uncounted.Failed = append(uncounted.Failed, p.UID)
		} else {
			if indexed {
				i := podIndex(p)
				if i < 0 || i >= int(ptr.Deref(job.Spec.Completions, 0)) || completed[i] {
					released = append(released, p)
					continue
				}
				completed[i] = true
			}
			uncounted.Succeeded = append(uncounted.Succeeded, p.UID)
		}
		newlyFinished = append(newlyFinished, p)
	}

	// First the uncounted list is written, then the finalizers go, then the
	// counts move: a crash in between counts nothing twice and nothing never.
	if len(newlyFinished) > 0 {
		if job, err = j.writeStatus(ctx, job, &status); err != nil {
			return err
		}
	}
	if len(uncounted.Succeeded)+len(uncounted.Failed) > 0 {
		byUID := map[types.UID]*corev1.Pod{}
		for _, p := range pods {
			byUID[p.UID] = p
		}
		for _, p := range newlyFinished {
			byUID[p.UID] = p
		}
		for _, list := range [][]types.UID{uncounted.Succeeded, uncounted.Failed} {
			for _, uid := range list {
				if p := byUID[uid]; p != nil && slices.Contains(p.Finalizers, jobTrackingFinalizer) {
					if err := dropTrackingFinalizer(ctx, j.client, p); err != nil {
						return err
					}
				}
			}
		}
		if n := len(uncounted.Failed); n > 0 {
			j.mu.Lock()
			j.failedAt[job.UID] = time.Now()
			j.mu.Unlock()
		}
		status.Succeeded += int32(len(uncounted.Succeeded))
		status.Failed += int32(len(uncounted.Failed))
		uncounted.Succeeded, uncounted.Failed = nil, nil
	}
	// Ignored failures and surplus successes count for nothing.
	for _, p := range released {
		if err := dropTrackingFinalizer(ctx, j.client, p); err != nil {
			return err
		}
	}
	if indexed {
		status.CompletedIndexes = formatIndexes(completed)
	}
	if perIndex {
		status.FailedIndexes = ptr.To(formatIndexes(failedIdx))
	}

	suspended := ptr.Deref(job.Spec.Suspend, false)
	if status.StartTime == nil && !suspended {
		status.StartTime = &now
	}
	completions := job.Spec.Completions
	parallelism := ptr.Deref(job.Spec.Parallelism, 1)

	// The terminal-to-be conditions.
	failureTarget := jobCondition(job, batchv1.JobFailureTarget)
	successTarget := jobCondition(job, batchv1.JobSuccessCriteriaMet)
	var failReason, failMsg string
	switch {
	case failureTarget != nil:
		failReason, failMsg = failureTarget.Reason, failureTarget.Message
	case successTarget != nil:
	case failJob:
		failReason, failMsg = "PodFailurePolicy", failMessage
	case perIndex && job.Spec.MaxFailedIndexes != nil && len(failedIdx) > int(*job.Spec.MaxFailedIndexes):
		failReason, failMsg = batchv1.JobReasonMaxFailedIndexesExceeded, "Job has exceeded the specified maximal number of failed indexes"
	case perIndex && len(failedIdx) > 0 && len(failedIdx)+len(completed) >= int(ptr.Deref(completions, 0)):
		failReason, failMsg = batchv1.JobReasonFailedIndexes, "Job has failed indexes"
	case !perIndex && status.Failed > ptr.Deref(job.Spec.BackoffLimit, 6):
		failReason, failMsg = "BackoffLimitExceeded", "Job has reached the specified backoff limit"
	case job.Spec.ActiveDeadlineSeconds != nil && status.StartTime != nil && !suspended &&
		now.Sub(status.StartTime.Time) >= time.Duration(*job.Spec.ActiveDeadlineSeconds)*time.Second:
		failReason, failMsg = "DeadlineExceeded", "Job was active longer than specified deadline"
	}
	succeededEnough := false
	successReason, successMsg := "CompletionsReached", "Reached expected number of succeeded pods"
	if successTarget != nil {
		successReason, successMsg = successTarget.Reason, successTarget.Message
	}
	if failReason == "" {
		rule := -1
		if indexed && job.Spec.SuccessPolicy != nil && successTarget == nil {
			rule = successPolicyMatch(job.Spec.SuccessPolicy, completed)
		}
		switch {
		case successTarget != nil:
			succeededEnough = true
		case rule >= 0:
			succeededEnough = true
			successReason, successMsg = batchv1.JobReasonSuccessPolicy, fmt.Sprintf("Matched rules at index %d", rule)
		case indexed:
			succeededEnough = completions != nil && len(completed) >= int(*completions)
		case completions != nil:
			succeededEnough = status.Succeeded >= *completions
		default:
			succeededEnough = status.Succeeded > 0 && len(activePods) == 0
		}
	}

	var errs error
	switch {
	case failReason != "" || succeededEnough:
		if failReason != "" {
			status.Conditions = setJobCondition(status.Conditions, batchv1.JobFailureTarget, corev1.ConditionTrue, failReason, failMsg, now)
		} else {
			status.Conditions = setJobCondition(status.Conditions, batchv1.JobSuccessCriteriaMet, corev1.ConditionTrue, successReason, successMsg, now)
		}
		errs = j.deletePods(ctx, key, activePods)
		if errs == nil && len(activePods) == 0 && terminating == 0 {
			if failReason != "" {
				status.Conditions = setJobCondition(status.Conditions, batchv1.JobFailed, corev1.ConditionTrue, failReason, failMsg, now)
			} else {
				status.Conditions = setJobCondition(status.Conditions, batchv1.JobComplete, corev1.ConditionTrue, successReason, successMsg, now)
				status.CompletionTime = &now
			}
		}
		activePods = nil
	case suspended:
		errs = j.deletePods(ctx, key, activePods)
		activePods = nil
		if jobCondition(job, batchv1.JobSuspended) == nil {
			status.Conditions = setJobCondition(status.Conditions, batchv1.JobSuspended, corev1.ConditionTrue, "JobSuspended", "Job suspended", now)
		}
	default:
		if jobCondition(job, batchv1.JobSuspended) != nil {
			status.Conditions = setJobCondition(status.Conditions, batchv1.JobSuspended, corev1.ConditionFalse, "JobResumed", "Job resumed", now)
			status.StartTime = &now
		}
		if j.exp.satisfied(key) {
			activePods, errs = j.manage(ctx, key, job, &status, activePods, terminating, completed, failedIdx, indexFailures, indexed, parallelism)
		}
	}

	status.Active = int32(len(activePods))
	status.Ready = &ready
	status.Terminating = &terminating
	if _, err := j.writeStatus(ctx, job, &status); err != nil {
		return err
	}
	if errs != nil {
		return errs
	}
	if job.Spec.ActiveDeadlineSeconds != nil && status.StartTime != nil && !jobFinished(&batchv1.Job{Status: status}) {
		left := time.Until(status.StartTime.Add(time.Duration(*job.Spec.ActiveDeadlineSeconds) * time.Second))
		if left > 0 {
			return later(left + 100*time.Millisecond)
		}
	}
	return nil
}

// successPolicyMatch returns the first rule of an indexed job's success
// policy the completed indexes satisfy, or -1.
func successPolicyMatch(policy *batchv1.SuccessPolicy, completed map[int]bool) int {
	for i, r := range policy.Rules {
		count := 0
		satisfied := true
		if r.SucceededIndexes != nil {
			for _, part := range strings.Split(*r.SucceededIndexes, ",") {
				lo, hi, found := strings.Cut(strings.TrimSpace(part), "-")
				first, err1 := strconv.Atoi(lo)
				last := first
				var err2 error
				if found {
					last, err2 = strconv.Atoi(hi)
				}
				if err1 != nil || err2 != nil {
					satisfied = false
					break
				}
				for k := first; k <= last; k++ {
					if completed[k] {
						count++
					} else if r.SucceededCount == nil {
						satisfied = false
					}
				}
			}
		} else {
			count = len(completed)
		}
		if r.SucceededCount != nil && count < int(*r.SucceededCount) {
			satisfied = false
		}
		if satisfied {
			return i
		}
	}
	return -1
}

// manage starts or stops pods to keep the number running the job asks for.
func (j *Jobs) manage(ctx context.Context, key string, job *batchv1.Job, status *batchv1.JobStatus, activePods []*corev1.Pod,
	terminating int32, completed, failedIdx map[int]bool, indexFailures map[int]int, indexed bool, parallelism int32) ([]*corev1.Pod, error) {
	want := parallelism
	if indexed {
		left := int32(0)
		for i := 0; i < int(ptr.Deref(job.Spec.Completions, 0)); i++ {
			if !completed[i] && !failedIdx[i] {
				left++
			}
		}
		want = min(want, left)
	} else if c := job.Spec.Completions; c != nil {
		want = min(want, max(0, *c-status.Succeeded))
	} else if status.Succeeded > 0 {
		want = 0
	}
	running := int32(len(activePods))
	if onlyReplaceFailed(job) {
		running += terminating
	}
	if running > want {
		// Surplus: the newest go first.
		victims := rankForDeletion(activePods, int(running-want))
		err := j.deletePods(ctx, key, victims)
		var kept []*corev1.Pod
		for _, p := range activePods {
			if !slices.Contains(victims, p) {
				kept = append(kept, p)
			}
		}
		return kept, err
	}
	diff := int(want - running)
	if diff == 0 {
		return activePods, nil
	}
	if wait := j.backoff(job, status); wait > 0 {
		return activePods, later(wait)
	}
	var indexes []int
	if indexed {
		taken := map[int]bool{}
		for _, p := range activePods {
			taken[podIndex(p)] = true
		}
		for i := 0; i < int(ptr.Deref(job.Spec.Completions, 0)) && len(indexes) < diff; i++ {
			if !completed[i] && !taken[i] && !failedIdx[i] {
				indexes = append(indexes, i)
			}
		}
		diff = len(indexes)
	}
	diff = min(diff, burstLimit)
	j.exp.expect(key, diff, 0)
	errs := make(chan error, diff)
	for n := range diff {
		template := job.Spec.Template.DeepCopy()
		template.Finalizers = append(template.Finalizers, jobTrackingFinalizer)
		generate := job.Name + "-"
		if indexed {
			addIndex(job, template, indexes[n])
			generate = fmt.Sprintf("%s-%d-", job.Name, indexes[n])
			if job.Spec.BackoffLimitPerIndex != nil {
				template.Annotations[jobIndexFailureCountAnnotation] = strconv.Itoa(indexFailures[indexes[n]])
			}
		}
		go func() { errs <- createPod(ctx, j.client, job, kindJob, template, generate) }()
	}
	var first error
	for range diff {
		if err := <-errs; err != nil {
			j.exp.observe(key, 1, 0)
			first = firstErr(first, err)
		}
	}
	if apierrors.HasStatusCause(first, corev1.NamespaceTerminatingCause) {
		first = nil
	}
	return activePods, first
}

const jobIndexFailureCountAnnotation = "batch.kubernetes.io/job-index-failure-count"

// indexFailureCount is how many pods of the pod's index failed before it.
func indexFailureCount(p *corev1.Pod) int {
	n, _ := strconv.Atoi(p.Annotations[jobIndexFailureCountAnnotation])
	return n
}

// addIndex marks a pod of an Indexed job with its index, as upstream does:
// annotation, label, hostname and the JOB_COMPLETION_INDEX variable.
func addIndex(job *batchv1.Job, t *corev1.PodTemplateSpec, index int) {
	v := strconv.Itoa(index)
	if t.Annotations == nil {
		t.Annotations = map[string]string{}
	}
	t.Annotations[jobIndexAnnotation] = v
	if t.Labels == nil {
		t.Labels = map[string]string{}
	}
	t.Labels[jobIndexAnnotation] = v
	if t.Spec.Hostname == "" {
		t.Spec.Hostname = fmt.Sprintf("%s-%d", job.Name, index)
	}
	env := corev1.EnvVar{Name: jobIndexEnv, ValueFrom: &corev1.EnvVarSource{FieldRef: &corev1.ObjectFieldSelector{
		APIVersion: "v1", FieldPath: fmt.Sprintf("metadata.annotations['%s']", jobIndexAnnotation),
	}}}
	for _, list := range [][]corev1.Container{t.Spec.InitContainers, t.Spec.Containers} {
		for i := range list {
			if !slices.ContainsFunc(list[i].Env, func(e corev1.EnvVar) bool { return e.Name == jobIndexEnv }) {
				list[i].Env = append(list[i].Env, env)
			}
		}
	}
}

// backoff is how long the job still waits after its last failure: ten
// seconds doubled per failure, six minutes at most.
func (j *Jobs) backoff(job *batchv1.Job, status *batchv1.JobStatus) time.Duration {
	if status.Failed == 0 {
		return 0
	}
	j.mu.Lock()
	at, ok := j.failedAt[job.UID]
	j.mu.Unlock()
	if !ok {
		return 0
	}
	d := jobBackoffBase
	for i := int32(1); i < status.Failed && d < jobBackoffMax; i++ {
		d *= 2
	}
	return time.Until(at.Add(min(d, jobBackoffMax)))
}

func (j *Jobs) deletePods(ctx context.Context, key string, pods []*corev1.Pod) error {
	if len(pods) == 0 {
		return nil
	}
	j.exp.expect(key, 0, len(pods))
	var first error
	for _, p := range pods {
		err := j.client.CoreV1().Pods(p.Namespace).Delete(ctx, p.Name, metav1.DeleteOptions{Preconditions: &metav1.Preconditions{UID: &p.UID}})
		if err != nil {
			j.exp.observe(key, 0, 1)
			if !apierrors.IsNotFound(err) {
				first = firstErr(first, err)
			}
		}
	}
	return first
}

// writeStatus stores the status unless it is what the job has, and
// returns the job as stored, for the next write of the same sync.
func (j *Jobs) writeStatus(ctx context.Context, job *batchv1.Job, status *batchv1.JobStatus) (*batchv1.Job, error) {
	cur := *job.Status.DeepCopy()
	if cur.UncountedTerminatedPods == nil {
		cur.UncountedTerminatedPods = &batchv1.UncountedTerminatedPods{}
	}
	if apiequality.Semantic.DeepEqual(&cur, status) {
		return job, nil
	}
	job = job.DeepCopy()
	job.Status = *status.DeepCopy()
	got, err := j.client.BatchV1().Jobs(job.Namespace).UpdateStatus(ctx, job, metav1.UpdateOptions{})
	if apierrors.IsNotFound(err) {
		return job, nil
	}
	if err != nil {
		return job, err
	}
	if rv, err := strconv.ParseInt(got.ResourceVersion, 10, 64); err == nil {
		j.mu.Lock()
		j.written[got.UID] = rv
		j.mu.Unlock()
	}
	return got, nil
}
