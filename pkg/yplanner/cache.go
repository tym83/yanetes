package yplanner

import (
	"sort"
	"sync"

	corev1 "k8s.io/api/core/v1"
	"k8s.io/apimachinery/pkg/types"
	resourcehelper "k8s.io/component-helpers/resource"
)

// clusterCache is the planner's picture of the nodes and what their pods
// take, kept up to date from informer events, as upstream's scheduler
// cache is: a pod is placed against it without listing the cluster.
type clusterCache struct {
	mu    sync.Mutex
	nodes map[string]*nodeState
	names []string // node names, sorted
	// Where each counted pod is, and the pods of nodes not seen yet.
	podNode map[types.UID]string
	pending map[string]map[types.UID]*corev1.Pod
}

func newClusterCache() *clusterCache {
	return &clusterCache{
		nodes:   map[string]*nodeState{},
		podNode: map[types.UID]string{},
		pending: map[string]map[types.UID]*corev1.Pod{},
	}
}

func (c *clusterCache) upsertNode(n *corev1.Node) {
	c.mu.Lock()
	defer c.mu.Unlock()
	if s, ok := c.nodes[n.Name]; ok {
		s.node = n
		return
	}
	s := &nodeState{node: n, requested: corev1.ResourceList{}, hostPorts: map[string]int{}}
	c.nodes[n.Name] = s
	i := sort.SearchStrings(c.names, n.Name)
	c.names = append(c.names, "")
	copy(c.names[i+1:], c.names[i:])
	c.names[i] = n.Name
	for _, pod := range c.pending[n.Name] {
		s.add(pod)
	}
	delete(c.pending, n.Name)
}

func (c *clusterCache) deleteNode(name string) {
	c.mu.Lock()
	defer c.mu.Unlock()
	s, ok := c.nodes[name]
	if !ok {
		return
	}
	// Its pods wait for it to come back, or for their own deletion.
	if len(s.pods) > 0 {
		waiting := map[types.UID]*corev1.Pod{}
		for _, pod := range s.pods {
			waiting[pod.UID] = pod
		}
		c.pending[name] = waiting
	}
	delete(c.nodes, name)
	if i := sort.SearchStrings(c.names, name); i < len(c.names) && c.names[i] == name {
		c.names = append(c.names[:i], c.names[i+1:]...)
	}
}

// counts says whether a pod takes room: bound and not finished.
func counts(pod *corev1.Pod) bool {
	return pod.Spec.NodeName != "" && pod.Status.Phase != corev1.PodSucceeded && pod.Status.Phase != corev1.PodFailed
}

// place counts a pod on a node; a pod counted elsewhere moves.
func (c *clusterCache) place(pod *corev1.Pod, node string) {
	c.mu.Lock()
	defer c.mu.Unlock()
	c.placeLocked(pod, node)
}

func (c *clusterCache) placeLocked(pod *corev1.Pod, node string) {
	c.removeLocked(pod.UID)
	c.podNode[pod.UID] = node
	if s, ok := c.nodes[node]; ok {
		s.add(pod)
		return
	}
	if c.pending[node] == nil {
		c.pending[node] = map[types.UID]*corev1.Pod{}
	}
	c.pending[node][pod.UID] = pod
}

// update follows a pod as the informer sees it.
func (c *clusterCache) update(pod *corev1.Pod) {
	if counts(pod) {
		c.place(pod, pod.Spec.NodeName)
		return
	}
	c.remove(pod.UID)
}

func (c *clusterCache) remove(uid types.UID) {
	c.mu.Lock()
	defer c.mu.Unlock()
	c.removeLocked(uid)
}

func (c *clusterCache) removeLocked(uid types.UID) {
	node, ok := c.podNode[uid]
	if !ok {
		return
	}
	delete(c.podNode, uid)
	if s, ok := c.nodes[node]; ok {
		s.remove(uid)
	}
	if waiting := c.pending[node]; waiting != nil {
		delete(waiting, uid)
	}
}

// add counts a pod in on the node.
func (s *nodeState) add(pod *corev1.Pod) {
	for name, q := range resourcehelper.PodRequests(pod, resourcehelper.PodResourcesOptions{}) {
		cur := s.requested[name]
		cur.Add(q)
		s.requested[name] = cur
	}
	s.podCount++
	s.pods = append(s.pods, pod)
	for _, key := range hostPortsOf(pod) {
		s.hostPorts[key]++
	}
}

// remove counts a pod out again.
func (s *nodeState) remove(uid types.UID) {
	for i, pod := range s.pods {
		if pod.UID != uid {
			continue
		}
		for name, q := range resourcehelper.PodRequests(pod, resourcehelper.PodResourcesOptions{}) {
			cur := s.requested[name]
			cur.Sub(q)
			s.requested[name] = cur
		}
		s.podCount--
		for _, key := range hostPortsOf(pod) {
			if s.hostPorts[key]--; s.hostPorts[key] <= 0 {
				delete(s.hostPorts, key)
			}
		}
		s.pods = append(s.pods[:i], s.pods[i+1:]...)
		return
	}
}

func hostPortsOf(pod *corev1.Pod) []string {
	var out []string
	for _, c := range pod.Spec.Containers {
		for _, port := range c.Ports {
			if port.HostPort > 0 {
				out = append(out, portKey(port.HostIP, port.Protocol, port.HostPort))
			}
		}
	}
	return out
}
