package nanny

import (
	"context"
	"encoding/json"
	"fmt"
	"log/slog"
	"slices"
	"sync"
	"time"

	apierrors "k8s.io/apimachinery/pkg/api/errors"
	"k8s.io/apimachinery/pkg/api/meta"
	metav1 "k8s.io/apimachinery/pkg/apis/meta/v1"
	"k8s.io/apimachinery/pkg/runtime/schema"
	"k8s.io/apimachinery/pkg/types"
	"k8s.io/client-go/discovery"
	"k8s.io/client-go/metadata"
	"k8s.io/client-go/metadata/metadatainformer"
	"k8s.io/client-go/restmapper"
	"k8s.io/client-go/tools/cache"
	"k8s.io/utils/ptr"
)

// GC deletes what its owners left behind and carries out foreground and
// orphan deletion, from metadata alone: it never reads a whole object.
type GC struct {
	meta      metadata.Interface
	discovery discovery.DiscoveryInterface
	mapper    *restmapper.DeferredDiscoveryRESTMapper
	factory   metadatainformer.SharedInformerFactory
	loop      *loop

	mu         sync.RWMutex
	nodes      map[types.UID]*gcNode
	dependents map[types.UID]map[types.UID]struct{}
	watched    map[schema.GroupVersionResource]bool
	synced     []cache.InformerSynced
}

type gcNode struct {
	gvr        schema.GroupVersionResource
	namespace  string
	name       string
	owners     []metav1.OwnerReference
	deleting   bool
	finalizers []string
}

const (
	finalizerForeground = metav1.FinalizerDeleteDependents
	finalizerOrphan     = metav1.FinalizerOrphanDependents
)

func NewGC(meta metadata.Interface, disco discovery.DiscoveryInterface) *GC {
	g := &GC{
		meta:       meta,
		discovery:  disco,
		mapper:     restmapper.NewDeferredDiscoveryRESTMapper(memoryCache(disco)),
		factory:    metadatainformer.NewSharedInformerFactory(meta, 0),
		nodes:      map[types.UID]*gcNode{},
		dependents: map[types.UID]map[types.UID]struct{}{},
		watched:    map[schema.GroupVersionResource]bool{},
	}
	g.loop = newLoop("garbagecollector", 10, g.sync)
	return g
}

func (g *GC) Run(ctx context.Context) {
	g.watchResources(ctx)
	go func() {
		ticker := time.NewTicker(30 * time.Second)
		defer ticker.Stop()
		for {
			select {
			case <-ctx.Done():
				return
			case <-ticker.C:
				g.mapper.Reset()
				g.watchResources(ctx)
			}
		}
	}()
	// Nothing is collected before every cache is full: an owner not yet
	// listed is not an owner that is gone.
	g.mu.RLock()
	synced := slices.Clone(g.synced)
	g.mu.RUnlock()
	if !cache.WaitForCacheSync(ctx.Done(), synced...) {
		return
	}
	slog.Info("garbage collector synced", "resources", len(synced))
	g.loop.run(ctx)
}

// watchResources starts an informer for every resource that can be listed,
// watched and deleted and does not have one yet.
func (g *GC) watchResources(ctx context.Context) {
	lists, err := g.discovery.ServerPreferredResources()
	if err != nil && len(lists) == 0 {
		slog.Warn("gc discovery failed", "err", err)
		return
	}
	for _, list := range lists {
		gv, err := schema.ParseGroupVersion(list.GroupVersion)
		if err != nil {
			continue
		}
		for _, r := range list.APIResources {
			if !slices.Contains(r.Verbs, "watch") || !slices.Contains(r.Verbs, "list") || !slices.Contains(r.Verbs, "delete") {
				continue
			}
			gvr := gv.WithResource(r.Name)
			g.mu.Lock()
			if g.watched[gvr] {
				g.mu.Unlock()
				continue
			}
			g.watched[gvr] = true
			informer := g.factory.ForResource(gvr).Informer()
			g.synced = append(g.synced, informer.HasSynced)
			g.mu.Unlock()
			_, _ = informer.AddEventHandler(cache.ResourceEventHandlerFuncs{
				AddFunc:    func(obj any) { g.observe(gvr, obj, false) },
				UpdateFunc: func(_, obj any) { g.observe(gvr, obj, false) },
				DeleteFunc: func(obj any) { g.observe(gvr, unwrap(obj), true) },
			})
		}
	}
	g.factory.Start(ctx.Done())
}

func (g *GC) observe(gvr schema.GroupVersionResource, obj any, deleted bool) {
	o, ok := obj.(metav1.Object)
	if !ok {
		return
	}
	uid := o.GetUID()
	g.mu.Lock()
	if old, ok := g.nodes[uid]; ok {
		for _, ref := range old.owners {
			delete(g.dependents[ref.UID], uid)
		}
	}
	var wake []types.UID
	if deleted {
		delete(g.nodes, uid)
	} else {
		n := &gcNode{
			gvr: gvr, namespace: o.GetNamespace(), name: o.GetName(),
			owners: o.GetOwnerReferences(), deleting: o.GetDeletionTimestamp() != nil,
			finalizers: o.GetFinalizers(),
		}
		g.nodes[uid] = n
		for _, ref := range n.owners {
			if g.dependents[ref.UID] == nil {
				g.dependents[ref.UID] = map[types.UID]struct{}{}
			}
			g.dependents[ref.UID][uid] = struct{}{}
		}
	}
	// Owners waiting on their dependents, and dependents of what went away.
	if old := o.GetOwnerReferences(); len(old) > 0 {
		for _, ref := range old {
			wake = append(wake, ref.UID)
		}
	}
	if deleted || o.GetDeletionTimestamp() != nil {
		for dep := range g.dependents[uid] {
			wake = append(wake, dep)
		}
	}
	g.mu.Unlock()
	if !deleted && (len(o.GetOwnerReferences()) > 0 || o.GetDeletionTimestamp() != nil) {
		g.loop.add(string(uid))
	}
	for _, w := range wake {
		g.loop.add(string(w))
	}
}

func (g *GC) node(uid types.UID) *gcNode {
	g.mu.RLock()
	defer g.mu.RUnlock()
	return g.nodes[uid]
}

func (g *GC) dependentsOf(uid types.UID) []types.UID {
	g.mu.RLock()
	defer g.mu.RUnlock()
	out := make([]types.UID, 0, len(g.dependents[uid]))
	for d := range g.dependents[uid] {
		if _, ok := g.nodes[d]; ok {
			out = append(out, d)
		}
	}
	return out
}

func (g *GC) sync(ctx context.Context, key string) error {
	uid := types.UID(key)
	n := g.node(uid)
	if n == nil {
		return nil
	}
	if n.deleting && slices.Contains(n.finalizers, finalizerOrphan) {
		return g.orphan(ctx, uid, n)
	}
	if n.deleting && slices.Contains(n.finalizers, finalizerForeground) {
		return g.foreground(ctx, uid, n)
	}
	if len(n.owners) == 0 || n.deleting {
		return nil
	}
	var absent, present []metav1.OwnerReference
	waiting := 0
	for _, ref := range n.owners {
		state, err := g.ownerState(ctx, n.namespace, ref)
		if err != nil {
			return err
		}
		switch state {
		case ownerAbsent:
			absent = append(absent, ref)
		case ownerWaiting:
			waiting++
			present = append(present, ref)
		default:
			present = append(present, ref)
		}
	}
	switch {
	case len(present) == 0:
		return g.delete(ctx, n, metav1.DeletePropagationBackground)
	case waiting == len(present) && len(absent) == 0:
		// Every owner waits for its dependents in foreground: go first.
		return g.delete(ctx, n, metav1.DeletePropagationForeground)
	case len(absent) > 0:
		return g.dropOwners(ctx, uid, n, absent)
	}
	return nil
}

type ownerStatus int

const (
	ownerPresent ownerStatus = iota
	ownerAbsent
	ownerWaiting
)

// ownerState checks the owner live when the cache does not have it: a
// cache that has not seen it yet must not get a dependent deleted.
func (g *GC) ownerState(ctx context.Context, namespace string, ref metav1.OwnerReference) (ownerStatus, error) {
	state := func(deleting bool, finalizers []string) ownerStatus {
		if deleting && slices.Contains(finalizers, finalizerForeground) {
			return ownerWaiting
		}
		return ownerPresent
	}
	if n := g.node(ref.UID); n != nil {
		return state(n.deleting, n.finalizers), nil
	}
	gv, err := schema.ParseGroupVersion(ref.APIVersion)
	if err != nil {
		return ownerAbsent, nil
	}
	mapping, err := g.mapper.RESTMapping(gv.WithKind(ref.Kind).GroupKind(), gv.Version)
	if err != nil {
		if meta.IsNoMatchError(err) {
			return ownerAbsent, nil
		}
		return ownerPresent, err
	}
	res := g.meta.Resource(mapping.Resource)
	var got *metav1.PartialObjectMetadata
	if mapping.Scope.Name() == meta.RESTScopeNameNamespace {
		got, err = res.Namespace(namespace).Get(ctx, ref.Name, metav1.GetOptions{})
	} else {
		got, err = res.Get(ctx, ref.Name, metav1.GetOptions{})
	}
	if apierrors.IsNotFound(err) {
		return ownerAbsent, nil
	}
	if err != nil {
		return ownerPresent, err
	}
	if got.UID != ref.UID {
		return ownerAbsent, nil
	}
	return state(got.DeletionTimestamp != nil, got.Finalizers), nil
}

func (g *GC) client(n *gcNode) metadata.ResourceInterface {
	if n.namespace == "" {
		return g.meta.Resource(n.gvr)
	}
	return g.meta.Resource(n.gvr).Namespace(n.namespace)
}

func (g *GC) delete(ctx context.Context, n *gcNode, policy metav1.DeletionPropagation) error {
	err := g.client(n).Delete(ctx, n.name, metav1.DeleteOptions{PropagationPolicy: &policy})
	if apierrors.IsNotFound(err) {
		return nil
	}
	return err
}

func (g *GC) patch(ctx context.Context, n *gcNode, uid types.UID, patch any) error {
	body, err := json.Marshal(patch)
	if err != nil {
		return err
	}
	_, err = g.client(n).Patch(ctx, n.name, types.MergePatchType, body, metav1.PatchOptions{})
	if apierrors.IsNotFound(err) {
		return nil
	}
	return err
}

// dropOwners removes owner references that point at nothing.
func (g *GC) dropOwners(ctx context.Context, uid types.UID, n *gcNode, drop []metav1.OwnerReference) error {
	kept := slices.DeleteFunc(slices.Clone(n.owners), func(ref metav1.OwnerReference) bool {
		return slices.ContainsFunc(drop, func(d metav1.OwnerReference) bool { return d.UID == ref.UID })
	})
	return g.patch(ctx, n, uid, map[string]any{"metadata": map[string]any{"ownerReferences": kept, "uid": uid}})
}

func (g *GC) removeFinalizer(ctx context.Context, uid types.UID, n *gcNode, finalizer string) error {
	left := slices.DeleteFunc(slices.Clone(n.finalizers), func(f string) bool { return f == finalizer })
	return g.patch(ctx, n, uid, map[string]any{"metadata": map[string]any{"finalizers": left, "uid": uid}})
}

func (g *GC) orphan(ctx context.Context, uid types.UID, n *gcNode) error {
	for _, dep := range g.dependentsOf(uid) {
		d := g.node(dep)
		if d == nil {
			continue
		}
		if err := g.dropOwners(ctx, dep, d, []metav1.OwnerReference{{UID: uid}}); err != nil {
			return fmt.Errorf("orphaning %s/%s: %w", d.namespace, d.name, err)
		}
	}
	return g.removeFinalizer(ctx, uid, n, finalizerOrphan)
}

func (g *GC) foreground(ctx context.Context, uid types.UID, n *gcNode) error {
	// An object that waits for its own dependents stops blocking owners
	// that wait for it, as upstream's collector does: otherwise a cycle of
	// owners waits forever.
	if len(g.dependentsOf(uid)) > 0 {
		refs := slices.Clone(n.owners)
		changed := false
		for i, ref := range refs {
			if ref.BlockOwnerDeletion == nil || !*ref.BlockOwnerDeletion {
				continue
			}
			state, err := g.ownerState(ctx, n.namespace, ref)
			if err != nil {
				return err
			}
			if state == ownerWaiting {
				refs[i].BlockOwnerDeletion = ptr.To(false)
				changed = true
			}
		}
		if changed {
			return g.patch(ctx, n, uid, map[string]any{"metadata": map[string]any{"ownerReferences": refs, "uid": uid}})
		}
	}
	blocking := 0
	for _, dep := range g.dependentsOf(uid) {
		d := g.node(dep)
		if d == nil {
			continue
		}
		for _, ref := range d.owners {
			if ref.UID == uid && ref.BlockOwnerDeletion != nil && *ref.BlockOwnerDeletion {
				blocking++
			}
		}
		if !d.deleting {
			// A dependent with another owner that stays keeps living; it
			// only lets go of the owner being deleted, as upstream's
			// collector does.
			staying := false
			for _, ref := range d.owners {
				if ref.UID == uid {
					continue
				}
				state, err := g.ownerState(ctx, d.namespace, ref)
				if err != nil {
					return err
				}
				staying = staying || state == ownerPresent
			}
			if staying {
				if err := g.dropOwners(ctx, dep, d, []metav1.OwnerReference{{UID: uid}}); err != nil {
					return err
				}
				continue
			}
			policy := metav1.DeletePropagationBackground
			if len(g.dependentsOf(dep)) > 0 {
				policy = metav1.DeletePropagationForeground
			}
			if err := g.delete(ctx, d, policy); err != nil {
				return err
			}
		}
	}
	if blocking > 0 {
		return later(time.Second)
	}
	return g.removeFinalizer(ctx, uid, n, finalizerForeground)
}
