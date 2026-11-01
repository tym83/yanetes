# Яnetes

[Русская версия](README.ru.md)

Kubernetes without Kubernetes. The parts of a cluster that think — the API
server, the controllers, the scheduler — are written from scratch on top of
the open source stack of one large search company: an API server in C++ on
[userver](https://github.com/userver-framework/userver) that keeps the
cluster in [YDB](https://github.com/ydb-platform/ydb) instead of etcd. The
nodes are upstream: kubelet, kube-proxy, containerd, flannel and CoreDNS
talk to it as they would to kube-apiserver and do not notice.

It is satire carried out for real. The joke is a company that rewrites
everything itself; the punchline is that the result is measured with
upstream's own tests and benchmarks.

| Part | What it is | Where |
|---|---|---|
| **yapi** | API server: C++20, userver, YDB as the only storage | `yapi/` |
| **nanny** | Controllers (deployments, statefulsets, jobs, GC, quotas, …), Go | `pkg/nanny/`, `cmd/nanny/` |
| **yplanner** | Scheduler with affinity, host ports and preemption, Go | `pkg/yplanner/`, `cmd/yplanner/` |
| Node side | Upstream kubelet, kube-proxy, containerd, flannel, CoreDNS | `hack/workshop/`, `deploy/addons/` |

## Where it stands

- **Conformance:** passes upstream conformance for Kubernetes v1.37.1,
  462 of 462 tests in one run, with RBAC on.
- **Scale:** runs 30 001 [KWOK](https://kwok.sigs.k8s.io/) nodes on a
  control plane of 4 vCPU. Against kube-apiserver + etcd on the same
  hardware:

| Scenario | kube-apiserver + etcd | Яnetes |
|---|---|---|
| Register 30 001 nodes | 14.0 s | 5.9 s |
| All nodes Ready | 78 s | 53 s |
| Heartbeat age, p99 | 32 s | 3 s |
| Create 10 000 pods | 14.2 s | 7.7 s |
| 10 000 pods to Running | 266 s | 69 s |
| List 30 001 nodes as JSON | 1.5 s | 1.0 s |

Not there yet: objects carry no `managedFields`, and lists in protobuf
are still being measured.

## Building and running

Everything heavy is built and run in a Kubernetes namespace: a builder pod,
a YDB pod and a few virtual machines. [`docs/HANDOFF.md`](docs/HANDOFF.md)
describes the architecture, the stand, how to build, deploy and test, the
benchmark method and the lessons learned.

```sh
hack/dev/build.sh                            # build yapi, nanny, yplanner
hack/workshop/control-plane.sh yx-1          # put the control plane on a VM
hack/workshop/node.sh yx-2                   # start and join a worker
NODES=8 sh /work/e2e.sh r1                   # conformance, inside the builder
```

## Not affiliated

Яnetes is an independent satirical project. It is not affiliated with,
endorsed by or sponsored by Yandex LLC or any of its affiliates. It uses
userver and YDB as any user of those open source projects would. Names of
other companies and products belong to their owners.

## License

[Apache License 2.0](LICENSE).
