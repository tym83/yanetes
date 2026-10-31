package nanny

import (
	"context"
	"encoding/json"
	"hash/fnv"
	"maps"
	"reflect"
	"sort"
	"strconv"

	appsv1 "k8s.io/api/apps/v1"
	corev1 "k8s.io/api/core/v1"
	apierrors "k8s.io/apimachinery/pkg/api/errors"
	metav1 "k8s.io/apimachinery/pkg/apis/meta/v1"
	"k8s.io/apimachinery/pkg/labels"
	"k8s.io/apimachinery/pkg/runtime"
	"k8s.io/apimachinery/pkg/util/rand"
	"k8s.io/client-go/kubernetes"
	appslisters "k8s.io/client-go/listers/apps/v1"
)

// Revisions of daemon sets and stateful sets, kept as upstream keeps them:
// a ControllerRevision holds a patch that replaces the template, so a
// rollback is applying it, and its name and hash label come from that data.

const labelRevisionHash = appsv1.ControllerRevisionHashLabelKey

// revisionData is the patch a revision stores for a template.
func revisionData(template *corev1.PodTemplateSpec) ([]byte, error) {
	raw, err := json.Marshal(template)
	if err != nil {
		return nil, err
	}
	var t map[string]any
	if err := json.Unmarshal(raw, &t); err != nil {
		return nil, err
	}
	t["$patch"] = "replace"
	return json.Marshal(map[string]any{"spec": map[string]any{"template": t}})
}

// templateOf reads the template back out of a revision.
func templateOf(rev *appsv1.ControllerRevision) (*corev1.PodTemplateSpec, error) {
	var data struct {
		Spec struct {
			Template corev1.PodTemplateSpec `json:"template"`
		} `json:"spec"`
	}
	if err := json.Unmarshal(rev.Data.Raw, &data); err != nil {
		return nil, err
	}
	return &data.Spec.Template, nil
}

// sameData compares revision data as JSON values: the server may give
// the keys back in another order.
func sameData(a, b []byte) bool {
	var x, y any
	if json.Unmarshal(a, &x) != nil || json.Unmarshal(b, &y) != nil {
		return false
	}
	return reflect.DeepEqual(x, y)
}

func revisionHash(data []byte, collisions *int32) string {
	h := fnv.New32a()
	h.Write(data)
	if collisions != nil {
		h.Write([]byte(strconv.Itoa(int(*collisions))))
	}
	return rand.SafeEncodeString(strconv.FormatUint(uint64(h.Sum32()), 10))
}

type revisions struct {
	client kubernetes.Interface
	lister appslisters.ControllerRevisionLister
}

// owned lists the revisions the set controls, adopting orphans that match
// its selector, oldest revision first.
func (r *revisions) owned(ctx context.Context, owner metav1.Object, kind schemaKind, selector labels.Selector) ([]*appsv1.ControllerRevision, error) {
	all, err := r.lister.ControllerRevisions(owner.GetNamespace()).List(labels.Everything())
	if err != nil {
		return nil, err
	}
	var out []*appsv1.ControllerRevision
	for _, rev := range all {
		ref := metav1.GetControllerOf(rev)
		switch {
		case ref != nil && ref.UID == owner.GetUID():
			out = append(out, rev)
		case ref == nil && rev.DeletionTimestamp == nil && owner.GetDeletionTimestamp() == nil && selector.Matches(labels.Set(rev.Labels)):
			rev = rev.DeepCopy()
			rev.OwnerReferences = append(rev.OwnerReferences, *metav1.NewControllerRef(owner, kind.gvk()))
			got, err := r.client.AppsV1().ControllerRevisions(rev.Namespace).Update(ctx, rev, metav1.UpdateOptions{})
			if err == nil {
				out = append(out, got)
			} else if !apierrors.IsNotFound(err) && !apierrors.IsConflict(err) {
				return nil, err
			}
		}
	}
	sort.SliceStable(out, func(i, j int) bool { return out[i].Revision < out[j].Revision })
	return out, nil
}

// ensure finds or makes the revision of the template and makes it the
// newest. A taken name with other data counts a collision: the caller
// stores the count and tries again.
func (r *revisions) ensure(ctx context.Context, owner metav1.Object, kind schemaKind, template *corev1.PodTemplateSpec,
	existing []*appsv1.ControllerRevision, collisions *int32) (*appsv1.ControllerRevision, bool, error) {
	data, err := revisionData(template)
	if err != nil {
		return nil, false, err
	}
	next := int64(1)
	if n := len(existing); n > 0 {
		next = existing[n-1].Revision + 1
	}
	for _, rev := range existing {
		// One being deleted is no revision to come back to.
		if rev.DeletionTimestamp != nil || !sameData(rev.Data.Raw, data) {
			continue
		}
		if rev.Revision == next-1 {
			return rev, false, nil
		}
		// An old template come back is the newest revision again.
		rev = rev.DeepCopy()
		rev.Revision = next
		got, err := r.client.AppsV1().ControllerRevisions(rev.Namespace).Update(ctx, rev, metav1.UpdateOptions{})
		return got, false, err
	}
	hash := revisionHash(data, collisions)
	revLabels := maps.Clone(template.Labels)
	if revLabels == nil {
		revLabels = map[string]string{}
	}
	revLabels[labelRevisionHash] = hash
	rev := &appsv1.ControllerRevision{
		ObjectMeta: metav1.ObjectMeta{
			Name:            owner.GetName() + "-" + hash,
			Namespace:       owner.GetNamespace(),
			Labels:          revLabels,
			OwnerReferences: []metav1.OwnerReference{*metav1.NewControllerRef(owner, kind.gvk())},
		},
		Data:     runtime.RawExtension{Raw: data},
		Revision: next,
	}
	got, err := r.client.AppsV1().ControllerRevisions(owner.GetNamespace()).Create(ctx, rev, metav1.CreateOptions{})
	if apierrors.IsAlreadyExists(err) {
		taken, getErr := r.client.AppsV1().ControllerRevisions(owner.GetNamespace()).Get(ctx, rev.Name, metav1.GetOptions{})
		if getErr == nil && metav1.IsControlledBy(taken, owner) && sameData(taken.Data.Raw, data) {
			return taken, false, nil
		}
		return nil, true, nil
	}
	return got, false, err
}

// trim deletes the oldest revisions past the history limit, sparing those
// still in use.
func (r *revisions) trim(ctx context.Context, revs []*appsv1.ControllerRevision, limit int32, live map[string]bool) error {
	var spare []*appsv1.ControllerRevision
	for _, rev := range revs {
		if !live[rev.Name] {
			spare = append(spare, rev)
		}
	}
	for i := 0; i < len(spare)-int(limit); i++ {
		err := r.client.AppsV1().ControllerRevisions(spare[i].Namespace).Delete(ctx, spare[i].Name, metav1.DeleteOptions{})
		if err != nil && !apierrors.IsNotFound(err) {
			return err
		}
	}
	return nil
}
