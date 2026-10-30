package nanny

import (
	"context"
	"encoding/json"
	"fmt"
	"net/netip"
	"sync"

	corev1 "k8s.io/api/core/v1"
	apierrors "k8s.io/apimachinery/pkg/api/errors"
	metav1 "k8s.io/apimachinery/pkg/apis/meta/v1"
	"k8s.io/apimachinery/pkg/labels"
	"k8s.io/apimachinery/pkg/types"
	"k8s.io/client-go/informers"
	"k8s.io/client-go/kubernetes"
	corelisters "k8s.io/client-go/listers/core/v1"
)

// NodeIPAM hands every node a pod CIDR of its own out of the cluster's.
type NodeIPAM struct {
	client  kubernetes.Interface
	nodes   corelisters.NodeLister
	cluster netip.Prefix
	bits    int
	loop    *loop

	mu   sync.Mutex
	used map[netip.Prefix]string
}

func NewNodeIPAM(client kubernetes.Interface, factory informers.SharedInformerFactory, clusterCIDR string, maskSize int) (*NodeIPAM, error) {
	cluster, err := netip.ParsePrefix(clusterCIDR)
	if err != nil || !cluster.Addr().Is4() || maskSize < cluster.Bits() || maskSize > 30 {
		return nil, fmt.Errorf("bad cluster CIDR %q or node mask /%d", clusterCIDR, maskSize)
	}
	n := &NodeIPAM{
		client: client, nodes: factory.Core().V1().Nodes().Lister(),
		cluster: cluster.Masked(), bits: maskSize, used: map[netip.Prefix]string{},
	}
	n.loop = newLoop("nodeipam", 1, n.sync)
	_, _ = factory.Core().V1().Nodes().Informer().AddEventHandler(n.loop.handlers())
	return n, nil
}

func (n *NodeIPAM) Run(ctx context.Context) { n.loop.run(ctx) }

func (n *NodeIPAM) sync(ctx context.Context, name string) error {
	n.mu.Lock()
	defer n.mu.Unlock()
	// The cache is the record: what nodes hold is taken, whatever this
	// process handed out before it restarted.
	nodes, err := n.nodes.List(labels.Everything())
	if err != nil {
		return err
	}
	n.used = map[netip.Prefix]string{}
	var node *corev1.Node
	for _, nd := range nodes {
		if p, err := netip.ParsePrefix(nd.Spec.PodCIDR); err == nil {
			n.used[p.Masked()] = nd.Name
		}
		if nd.Name == name {
			node = nd
		}
	}
	if node == nil || node.Spec.PodCIDR != "" {
		return nil
	}
	size := 1 << (n.bits - n.cluster.Bits())
	start := n.cluster.Addr().As4()
	base := uint32(start[0])<<24 | uint32(start[1])<<16 | uint32(start[2])<<8 | uint32(start[3])
	step := uint32(1) << (32 - n.bits)
	for i := range size {
		v := base + uint32(i)*step
		p := netip.PrefixFrom(netip.AddrFrom4([4]byte{byte(v >> 24), byte(v >> 16), byte(v >> 8), byte(v)}), n.bits)
		if _, taken := n.used[p]; taken {
			continue
		}
		patch, _ := json.Marshal(map[string]any{
			"metadata": map[string]any{"uid": node.UID},
			"spec":     map[string]any{"podCIDR": p.String(), "podCIDRs": []string{p.String()}},
		})
		_, err := n.client.CoreV1().Nodes().Patch(ctx, name, types.MergePatchType, patch, metav1.PatchOptions{})
		if apierrors.IsNotFound(err) {
			return nil
		}
		if err == nil {
			n.used[p] = name
		}
		return err
	}
	return fmt.Errorf("no pod CIDR left in %s for node %s", n.cluster, name)
}
