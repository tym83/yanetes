package nanny

import (
	"k8s.io/apimachinery/pkg/runtime/schema"
	"k8s.io/client-go/discovery"
	"k8s.io/client-go/discovery/cached/memory"
)

type schemaKind struct{ group, version, kind string }

func (k schemaKind) gvk() schema.GroupVersionKind {
	return schema.GroupVersionKind{Group: k.group, Version: k.version, Kind: k.kind}
}

var (
	kindReplicaSet = schemaKind{"apps", "v1", "ReplicaSet"}
	kindDeployment = schemaKind{"apps", "v1", "Deployment"}
	kindService    = schemaKind{"", "v1", "Service"}
)

func memoryCache(d discovery.DiscoveryInterface) discovery.CachedDiscoveryInterface {
	return memory.NewMemCacheClient(d)
}
