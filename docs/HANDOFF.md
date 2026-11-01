# Яnetes — handoff

Everything someone picking this project up needs: what it is, how it is
built and run, where it stands, what is next, and the rules of the house.
Written 2026-11-01. The repository is the source of truth; when this file
and the code disagree, the code wins and this file is stale.

## 1. What this is

**Яnetes** (ASCII `yanetes`, Go module `github.com/tym83/yanetes`) is "Kubernetes without Kubernetes": a
Kubernetes whose thinking parts are written from scratch on Yandex's open
source stack, while the node side is upstream.

| Layer | What | Where |
|---|---|---|
| API server | **yapi**, C++20 on [userver](https://github.com/userver-framework/userver) 3.3-rc | `yapi/` |
| Storage | **YDB** directly (no etcd): tables `objects`, `revisions`, `events`, `compactions` | `yapi/src/store.*` |
| Controllers | **nanny**, Go, client-go | `pkg/nanny/`, `cmd/nanny/` |
| Scheduler | **yplanner**, Go | `pkg/yplanner/`, `cmd/yplanner/` |
| Node side | upstream kubelet, kube-proxy, containerd, flannel, CoreDNS | `hack/workshop/node-cloud-init.yaml`, `deploy/addons/` |

It is satire with a straight face: the joke (a company that rewrites
everything itself) is carried out for real, measured with upstream's own
tests. The research behind it is in `research/` (stack facts, lore); the
original design draft is `docs/design.ru.md` (Russian, partly outdated:
satire overlays in its table are not built yet).

A running journal of the work, in Russian, is kept in `SESSION_LOG.md` at
the repository root (git-ignored, local to the owner's machine).

Status in one line: **passes upstream conformance 462/462 in one run**
(Kubernetes v1.37.1, RBAC on) and runs **30 001 KWOK nodes** on a 4-vCPU
control plane, beating kube-apiserver + etcd on most scenarios on the same
hardware (§6).

## 2. Architecture

### yapi (C++)

One userver service. Components (`yapi/src/main.cpp` registers them):

- `yapi-store` (`store.*`): YDB access. Writes go through a per-resource
  **batcher** (group commit: many writes per transaction, `kMaxBatch`
  256). Each resource has its own revision counter (`revisions` table) —
  unlike etcd's global one; several fixes exist because of that (see
  `Compact` bumping idle resources, §7). The event log (`events`) is
  compacted every 150 s, etcd-like. Lists read YDB 1000 rows at a time in
  one snapshot transaction.
- `yapi-watch` (`watch.*`): the **watch cache**. Per resource: every
  object in memory plus a window of the last 100 000 events, each carrying
  the previous version of the object (`WatchEvent::prev`). Serves watches,
  lists (once caught up with the store) and admission reads
  (`Store::ListInformed/GetInformed`).
- `yapi-auth` (`auth.*`): static tokens file, ServiceAccount JWTs (RS256,
  TokenRequest/TokenReview), OIDC discovery.
- `yapi-rbac` (`rbac.*`): RBAC authorizer with aggregation; reconciles
  upstream's default policy at start from `yapi/bootstrap/rbac/*.yaml`
  (dumps of upstream's `bootstrappolicy/testdata`, plus `components.yaml`
  for flannel and CoreDNS). `system:masters` bypasses; group
  `system:nodes` gets ClusterRole `system:node`. Impersonation is in
  `api.cpp` (`Api::Impersonated`).
- `yapi-services` (`services.*`): ClusterIP allocation through
  `IPAddress` objects (as upstream since 1.33), NodePorts.
- `yapi-crds` (`crds.*`, `schema.*`, `openapi.*`): CustomResourceDefinitions —
  structural schemas, pruning, defaulting, versions and conversion, scale
  and status subresources, printer columns, published OpenAPI v2/v3.
- `yapi-webhooks` (`webhooks.*`): admission and conversion webhooks.
  Calls go to a ready endpoint of the service (`routing.*`), not its
  ClusterIP.
- `yapi-policies` (`policies.*`, `cel.*`): ValidatingAdmissionPolicy and
  MutatingAdmissionPolicy on an own CEL implementation (parser, type
  checker with cel-go's error wording, evaluator; tests `cel_test.cpp`).
- `yapi-aggregator` (`aggregator.*`): APIService proxying with a
  front-proxy client certificate, discovery of remote groups, availability
  conditions; publishes `kube-system/extension-apiserver-authentication`.
- `yapi-protobuf` (`proto.*`): decodes protobuf request bodies into JSON
  and encodes answers back (objects, lists, Status, watch frames) when the
  client asks for `application/vnd.kubernetes.protobuf` first, using
  upstream `generated.proto` files and hints (`hack/protohints`). Encoded
  objects are cached per uid, resourceVersion and type; lists are put
  together from item bytes. `hack/protocheck KUBECONFIG` compares what a
  JSON client and a protobuf client decode for ~30 types.
- The HTTP handler `Api` (`api.*`, ~2.5k lines): routing, discovery
  (incl. aggregated v2), CRUD, patch (merge, JSON, strategic, server-side
  apply), watch, field validation, tables, proxy/exec/attach/port-forward/
  logs to kubelets (`kubelet.*`), subresources, admission chain
  (`admission.*`, `validation.*`, `objects.*`), conversions between
  served shapes (`conversion.*`).

### nanny (Go)

All controllers kube-controller-manager would run: namespace (ordered
deletion), ServiceAccount and root CA, garbage collector (metadata
informers, foreground/orphan, cycle breaking), ReplicaSet,
ReplicationController, Deployment (incl. proportional scaling),
StatefulSet, DaemonSet, ControllerRevision history, Job (incl. indexed,
success policy, per-index backoff, pod failure policy), CronJob,
EndpointSlice and Endpoints and mirroring, node IPAM, node lifecycle
(taints, NoExecute eviction), pod GC, ResourceQuota, disruption (PDB),
TTL-after-finished. One file per controller in `pkg/nanny/`.

### yplanner (Go)

Filters (taints, node affinity/selector, resources, host ports, pod
affinity/anti-affinity), least-allocated scoring, preemption
(`preempt.go`), an informer-fed cache of what each node holds
(`cache.go`), node sampling like upstream's `percentageOfNodesToScore`,
asynchronous binding.

## 3. The stand

Everything heavy runs in a tenant of the "workshop" Cozystack cluster
(Hetzner bare metal, 3 × 96-core AMD EPYC Genoa, 128 GiB). Nothing heavy
on a laptop.

- Admin kubeconfig: `~/workshop.txt`, context `admin@workshop`, namespace
  `tenant-yanetes`. `hack/dev/k.sh` wraps that kubectl.
- Tenant manifest `hack/workshop/tenant.yaml` (quota 100 CPU / 128 GiB).
- Pod **builder** (`hack/workshop/builder.yaml`): container `cxx`
  (userver image, builds yapi) and `go` (builds Go, runs e2e and KWOK).
  Disk `/work`: `src` (synced sources), `build/yapi` (CMake/Ninja
  Release, ccache), `bin` (kubectl, kwok, nanny, yplanner, perf), `e2e`
  (tests), `proto`, `openapi`, `run` (PKI, tokens, kubeconfig of the stand:
  `/work/run/admin.conf`), `perf` (KWOK config), `bench` (comparison).
  `hack/dev/prepare-builder.sh` documents how `proto`, `openapi` and `e2e`
  were made.
- Pod **ydb** (`hack/workshop/ydb.yaml`): local-ydb, 8 CPU / 16 GiB,
  100 GiB replicated (DRBD) volume. Log level lowered to 3 in
  `/ydb_data/cluster/kikimr_configs/config.yaml` (§7).
- VMs (KubeVirt via Cozystack `VMInstance`, Ubuntu 24.04, 4 vCPU / 8 GiB):
  - **yx-1**: control plane (yapi, nanny, yplanner as systemd units from
    `/opt/yanetes/current`), also a node with taint
    `node-role.kubernetes.io/control-plane:NoSchedule`.
  - **yx-2**, **yx-3**: workers (`hack/workshop/node.sh NAME`).
  - **yx-bench**: upstream kube-apiserver/controller-manager/scheduler for
    comparisons (`hack/bench/upstream.sh`), with pod **etcd**
    (`hack/bench/etcd.yaml`, same limits and volume as ydb).
- SSH into a VM: `virtctl --context admin@workshop -n tenant-yanetes ssh ubuntu@vmi/vm-instance-yx-1` (with `KUBECONFIG=~/workshop.txt`).
- The stand running on 2026-11-01 predates the rename: its namespace is
  `tenant-yandexnetes` and its VMs keep `/opt/yandexnetes` and
  `/etc/yandexnetes`. Pass `NS=tenant-yandexnetes` to the scripts to reach
  it. `control-plane.sh` and `node.sh` lay out the new `/opt/yanetes` and
  `/etc/yanetes` from scratch (certificates come from the builder), so a
  full install moves a VM over; the old directories can then go.

## 4. Build, deploy, test

```sh
hack/dev/build.sh                          # sync + build yapi (+unit tests), nanny, yplanner, perf
hack/workshop/control-plane.sh yx-1 restart  # atomic release on yx-1, restart services
hack/workshop/control-plane.sh yx-1          # full install: certs, config, units (needed after config changes)
```

The deploy bundles binaries, the libraries ldd names, `/work/proto`,
`/work/openapi` and the RBAC bootstrap into a release directory that the
VM fetches from the builder over the pod network (`:8099`) and switches
to with a symlink — copying libraries over a running process caused
SIGBUS once.

Unit tests: `yapi_unittest` (gtest: CEL, patch, selectors, objects),
`go test ./...` (deployment proportions, scheduler cache).

Conformance (in the builder, go container; copy `hack/dev/e2e.sh` and
`hack/dev/fails.sh` to `/work/e2e.sh`, `/work/e2e/fails.sh`):

```sh
NODES=8 sh /work/e2e.sh r20                                    # 433 parallel tests
NODES=1 sh /work/e2e.sh s20 '\[Conformance\].*\[Serial\]|\[Serial\].*\[Conformance\]' 'NONE'   # 29 serial incl. disruptive
sh /work/e2e/fails.sh r20                                      # failures, one per line
```

Never restart yapi while a run is going. Clean leftovers of killed runs
before a new run (namespaces, CRDs, webhook configurations, APIServices,
IngressClasses, fake nodes): stale admission webhooks broke unrelated
tests more than once.

## 5. Conformance history and state

- Last clean full run: r15 + s8 on 2026-10-31, **462/462**, commit
  `ac63964`, 3 nodes, RBAC on.
- Since then: performance work (watch cache, list paging, admission from
  cache, scheduler rewrite). **Conformance has not been re-run after
  `8145a53`** — do it before anything is published.
- Tests known to be flaky under load, not reproduced alone: Node lifecycle
  (`should run through the lifecycle of a node`, hangs at Update under
  heavy parallel load and leaves its fake node, which then fails other
  tests' "all nodes ready" check), DaemonSet rollback (rare).
- Not conformance, failing by design: VAP `can restrict access by-node`
  (needs upstream's graph node authorizer), gRPC probe TLS mode (alpha
  gate).

## 6. Performance

Tools: `hack/perf` (load generator: nodes, pods, lists),
`/work/perf/all.yaml` (KWOK configuration: `nodeLeaseParallelism: 256`
plus upstream KWOK stages), `hack/bench/run.sh KUBECONFIG TAG` (one
scenario for any server; results in `/work/bench/TAG.txt`).

Same hardware (control plane on 4 vCPU; storage pod 8 CPU on a replicated
volume), same KWOK, 30 001 nodes:

| Scenario | kube-apiserver + etcd | Яnetes |
|---|---|---|
| Register 30 001 nodes | 14.0 s, 2148/s, p99 365 ms | 5.9 s, 5054/s, p99 66 ms |
| All nodes Ready | 78 s | 53 s |
| Heartbeat age p99 (limit 40 s) | 32 s | 3 s |
| Create 10 000 pods | 14.2 s, 703/s, p99 1.07 s | 7.7 s, 1294/s, p99 289 ms (admission from cache) |
| 10 000 pods to Running | 266 s | 69 s |
| `GET nodes` JSON, 30 001 nodes | 1.5 s (106 MB, with managedFields) | 1.0 s (59 MB) |
| List nodes+pods, client asks protobuf | 3.0 s | 4.0 s before protobuf answers; to be re-measured |
| `kubectl get nodes` (table) | 12.4 s | 11.0 s |
| Steady CPU | ≈ 4.35 cores | ≈ 4.4 cores |

## 7. Lessons that cost time

- YDB returns at most 1000 rows per query and the client default timeout
  is 1 s: anything listing more must page (see `Store::List`).
- local-ydb at log level NOTICE wrote a 24 GB log onto its data volume;
  together with DRBD I/O the PDisk stalled ("blob storage not discovered").
  The env var `YDB_DEFAULT_LOG_LEVEL` is ignored on an existing cluster.
- Per-resource revisions: a resource nobody writes stays at the compacted
  revision; compaction now moves it on by one (etcd's global revision
  always moves).
- Update must not create objects except for kinds whose upstream strategy
  allows it (leases, endpoints, events, limitranges, services,
  runtimeclasses, RBAC) — a controller updating a stale copy has to hear
  NotFound.
- Protobuf decoding needs hints protobuf does not carry: pointer and
  non-omitempty scalars (zero values), inlined structs, renamed fields
  (`hack/protohints`). Regenerate with staging paths, not `go list`.
- Webhooks: dial endpoints, not ClusterIPs (kube-proxy lags on new
  services). userver's async DNS ignores `connect_to`; use getaddrinfo.
- KWOK's default lease parallelism (4) cannot keep 30 000 leases fresh;
  set 256 (real kubelets renew in parallel anyway).
- `kubectl auth can-i --as` needs impersonation support or it silently
  answers for the caller.

## 8. What is next, in order

1. **Conformance after the performance work and protobuf answers**: full
   run r16 + s9 was started 2026-11-01 (`/work/e2e/full6.out`). Commits
   after it (`4517a8d`, encoded-object cache) still need a run.
2. **Re-measure** with `hack/bench/run.sh` against both servers (§6); the
   upstream side is still installed on yx-bench, etcd pod still running.
3. **Scale**: give yx-1 16–32 vCPU (tenant quota is 100 CPU), aim at
   100 000+ KWOK nodes. Watch memory of the watch cache and the encoded
   cache, nanny CPU (node lifecycle took 1.7 cores at 30k nodes).
4. `managedFields` (server-side apply field ownership) — absent today.
5. Rename to Яnetes / `yanetes` (§9), README (English) + `README.ru.md`,
   publish `github.com/tym83/yanetes`. Show the README to the owner before
   publishing. Until then the code lives in the private repository
   `github.com/tym83/yanetes`, branch `feat/skeleton`.
6. Satire overlays from `docs/design.ru.md` (selectively; not the "Plus"
   subscription — dropped by the owner).
7. Article (Russian) for Habr with the numbers.
8. Porto via portoshim (risky: pinned to k8s 1.26 libraries).

## 9. Rules of the house

- Name: **Яnetes** (Cyrillic Я + "netes"), ASCII `yanetes` wherever
  only ASCII goes (module path, paths, namespaces). No Yandex logos or branding; the README says
  it is not affiliated with Yandex LLC and is satire. Reason: trademark
  risk.
- Languages: code, comments, commits, PRs, documentation in **English**
  (documentation also gets a `.ru.md` copy); the article in **Russian**.
- Git: public repository github.com/tym83/yanetes; work on feature
  branches, never on main. Every commit `--signoff` and exactly one trailer
  `Assisted-by: LLM`; no other AI attribution anywhere (no
  Co-Authored-By, no "generated with" in code, docs or PRs). Semantic
  commit messages that say why, not what. The repo will be the owner's
  personal one (`tym83/*`): PRs ready (not draft), merge after green CI.
- Code style: match the surrounding code — comments explain why, in full
  sentences; no comment noise.
- Heavy work only in the workshop cluster. Always pass explicit
  `--kubeconfig`/`--context` to kubectl.
- Do not delete data you did not create; leftovers of test runs on the
  stand are fair game.
