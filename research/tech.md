# Yandexnetes — tech research: Yandex open-source building blocks

Checked 2026-10-30. Repo dates are from the GitHub API (`pushed_at`, latest release). "Not found" means I searched and found no public source, not that the thing does not exist.

## TL;DR

- **etcd on YDB already exists upstream.** `ydb/apps/etcd_proxy` in the YDB repo serves etcd v3 gRPC (KV/Watch/Lease/Txn) on top of YDB tables plus a changefeed. It is marked "Zero version", and its own run script shows kube-apiserver running against it in kind. That gives us the storage layer for free.
- **Porto + CRI works.** `ten-nancy/portoshim` is a maintained CRI v1 shim (commits in Oct 2026). The last tagged release is from Feb 2024, and its deps pin k8s 1.25/1.26 libs.
- **userver can do the apiserver transport:** HTTPS, HTTP/2 over TLS with ALPN, streaming response bodies, gRPC (incl. streaming). HTTP/2 is only "Silver tier".
- **`ya make` is public** (`yandex/yatool`, Apache-2.0) but self-described as "bumpy road", with no releases. The `ya` wrapper downloads a prebuilt binary from Yandex S3 with TLS verification switched off.
- **No kine driver for YDB.**

---

## 1. YDB

| | |
|---|---|
| Repo | https://github.com/ydb-platform/ydb |
| License | Apache-2.0 |
| Alive? | Yes. Pushed 2026-10-30; latest release `26.2.1.14` on 2026-09-17 |

**etcd v3 API — native/official shim: YES (experimental).**
- `ydb/apps/etcd_proxy`: https://github.com/ydb-platform/ydb/tree/main/ydb/apps/etcd_proxy . It was introduced 2025-03-26 in PR "Etcd over ydb. Zero version." (#16101, https://github.com/ydb-platform/ydb/pull/16101) and has had about 25 commits since (last touched 2026-09-24). It is a separate binary, not built into `ydbd`.
- It implements etcd's `rpc.proto` / `kv.proto` / `auth.proto` (`proto/`) and has service files `etcd_grpc`, `etcd_watch`, `etcd_lease`, `etcd_impl` (https://github.com/ydb-platform/ydb/tree/main/ydb/apps/etcd_proxy/service).
- Schema (`service/create.sql`): tables `revision`, `commited`, `current`, `history`, `leases`, plus `ALTER TABLE current ADD CHANGEFEED changes WITH (FORMAT="JSON", MODE="NEW_AND_OLD_IMAGES", RETENTION_PERIOD = Interval("PT1H"))`. Watch reads that changefeed via the YDB Topic API (`etcd_watch.cpp`, `TopicClient->CreateReadSession`, partition 0 only).
- `readme.txt` lists these known gaps: no metrics, no logging, no retry policies; "Implement the watches for ranges. (Now the watches work only with a single key or a prefix.)"; changefeed "only single partition"; no IAM auth; compaction is unfinished; leases cause "TLI errors" (transaction locks invalidated) — https://github.com/ydb-platform/ydb/blob/main/ydb/apps/etcd_proxy/readme.txt
- `run_etcd.sh` is literally a kind recipe. It runs `./start.sh ram` for local YDB, then `etcd_proxy --init --import-from=127.0.0.1:2379 --import-prefix=/registry/` to copy the existing etcd contents, then serves on `:2479` with the kubeadm etcd certs. You then point kube-apiserver at 2479 — https://github.com/ydb-platform/ydb/blob/main/ydb/apps/etcd_proxy/run_etcd.sh
- Build it with `./ya make ydb/apps/etcd_proxy`. No published binary or image was found.

**kine driver for YDB: not found.** kine's drivers are `generic, memory, mysql, nats, pgsql, sqlite, t4, remote, split` (https://github.com/k3s-io/kine/tree/master/pkg/drivers). GitHub repo search for "kine ydb" / "ydb etcd" returned nothing. Note that YDB removed its legacy PostgreSQL wire support from `ydbd` in July 2026 (PR #45922, https://github.com/ydb-platform/ydb/pull/45922), so "kine pgsql → YDB" is not an option either.

**CDC / changefeeds: YES.** Changefeeds write row changes to a topic. Modes are `UPDATES`, `NEW_IMAGE`, `OLD_IMAGE`, `NEW_AND_OLD_IMAGES`. Delivery is exactly-once and ordered per key (partitioned by PK). Records appear only after commit. Optional virtual timestamps (coordinator step + txid) and "barriers" (resolved timestamps) are supported. Consumers read via the Topic API, the Kafka API, or Debezium-style JSON. Retention defaults to 24h (max 30 days) — https://ydb.tech/docs/en/concepts/cdc . This is enough to build a k8s watch with resourceVersion = virtual timestamp.

**Single-node deployment** — https://ydb.tech/docs/en/quickstart
- Docker image `ydbplatform/local-ydb:latest`, ports 2135 (gRPC TLS), 2136 (gRPC), 8765 (UI), 9092 (Kafka proxy). Apple Silicon needs `YDB_USE_IN_MEMORY_PDISKS=true`. The database path is `/local`.
- Binary install: `curl https://install.ydb.tech | bash`, then `./start.sh ram` or `./start.sh disk` (the disk mode creates an 80 GB `ydb.data` file). The database path is `/Root/test`.
- Resources (https://ydb.tech/docs/en/devops/concepts/system-requirements): x86-64 **with AVX2**, Linux kernel ≥4.19 (6.6 recommended), libc ≥2.30. Disk minimum 80 GB, 800+ GB recommended. **No RAM or CPU minimum is stated.** On >32-core hosts, run each dynamic node in a 10–32-core cpuset. Building YDB from source needs ≥80 GB free (https://github.com/ydb-platform/ydb/blob/main/BUILD.md).
- YDB Kubernetes operator: https://github.com/ydb-platform/ydb-kubernetes-operator (Apache-2.0, release 0.8.0 on 2026-03-13; slow cadence).

## 2. Porto

| | |
|---|---|
| Original | https://github.com/yandex/porto — **archived**, last push 2023-01-20 |
| Live fork | https://github.com/ten-nancy/porto — LGPL (LICENSE: "subject of the GNU LESSER GENERAL PUBLIC LICENSE"); pushed 2026-10-28; latest release `v5.3.41` on 2024-08-27 (commits continue, tags lag) |

- Porto history: started mid-2014 on cgroups, and by 2015 most Yandex apps ran in Porto containers (Habr, Platform Engineering CTO, 2023: https://habr.com/ru/post/761946).
- **CRI: YES — Portoshim** https://github.com/ten-nancy/portoshim (Apache-2.0, © 2023 YANDEX LLC). Pushed 2026-10-27, with recent commits on integration tests and privileged containers. The last release is `v1.0.11` on 2024-02-05.
  - It serves the CRI socket at `/run/portoshim.sock`. The README tells you to start kubelet with `--container-runtime=remote --container-runtime-endpoint=unix:///run/portoshim.sock` (the first flag was removed in k8s 1.27, so it is outdated) — https://github.com/ten-nancy/portoshim#kubernetes-over-porto
  - It implements **both CRI `runtime/v1` and `v1alpha2`** (`cmd/portoshim/cri_api.go`). Kubelet ≥1.26 requires v1, which is present.
  - `go.mod` pins `k8s.io/cri-api v0.23.5`, `k8s.io/kubernetes v1.26.1`, `client-go v0.25.3`. **No explicit "supported kubelet versions" statement was found.** The CRI v1 surface makes recent kubelets plausible, but we must test this ourselves (newer CRI fields, e.g. 1.29+ image/stats RPCs, will be missing).
  - Fun fact: test fixtures use container names like `ISS-AGENT--alexperevalov-portoshim-stage-48/box/...`, which is a public trace of ISS (`cmd/portoshim/runtime_mapper_test.go`).

## 3. userver

| | |
|---|---|
| Repo | https://github.com/userver-framework/userver |
| License | Apache-2.0 |
| Alive? | Yes. Pushed 2026-10-30; release `v3.2` on 2026-09-09 |

Doc page: https://github.com/userver-framework/userver/blob/develop/scripts/docs/en/userver/http_server.md
- **HTTPS:** yes ("Golden Tier").
- **HTTP/2:** yes, enabled by `connection: http-version: '2'`, with `http2-session` tuning ("**Silver Tier**" quality). The changelog says "Added full HTTP/2 over TLS support with ALPN negotiation" (h2 + http/1.1 fallback in `universal/src/crypto/ssl_ctx.cpp`). client-go negotiates h2 via ALPN, so this matters.
- **Streaming / chunked:** yes — "Streaming API" via `HandleStreamRequest(..., ResponseBodyStream&)` and the `response-body-stream: true` config. That is enough for `?watch=1` chunked JSON.
- **WebSocket:** yes (Golden). This helps with `exec`/`attach`/`port-forward` if we go WebSocket (k8s ≥1.30 prefers WS over SPDY for these).
- **gRPC:** yes, `userver::grpc`, "Platinum Tier", with server and client and unary/client/server/bidi streams (https://github.com/userver-framework/userver/blob/develop/scripts/docs/en/userver/grpc/grpc.md). It is useful for talking to etcd_proxy or YDB.
- **JSON/YAML:** `formats::json`, `formats::yaml`, and chaotic codegen from JSON Schema/OpenAPI (`chaotic/`, `chaotic-openapi/`). That means k8s OpenAPI → C++ types is feasible. Protobuf comes via the gRPC module (k8s protobuf content-type would need generated messages).
- **A built-in YDB driver exists** (`ydb/` dir, `cmake/SetupYdbCppSDK.cmake`). There are also PostgreSQL, Mongo, Redis, Kafka, ClickHouse, and others.
- **Build:** CMake ≥3.14, **C++20** default (`cmake/UserverSetupEnvironment.cmake`), GCC ≥11.2 / Clang ≥16 (changelog). Deps come via CPM, Conan, or distro packages (Ubuntu 22.04/24.04, Debian, Fedora, Alpine, Arch, macOS in CI).
- **Docker images:** `ghcr.io/userver-framework/ubuntu-22.04-userver-base` (deps only), `...-userver` (userver preinstalled), `...-userver-pg`. Ubuntu 24.04 variants are published too (`scripts/docker/ubuntu-24.04-userver.dockerfile`). The devcontainer image is ~6 GB. Source: https://github.com/userver-framework/userver/blob/develop/scripts/docs/en/userver/build/build.md
- Joke material: both userver and YDB ship `CLAUDE.md` / `AGENTS.md` in the repo root.

## 4. `ya` / `ya make`

- **Public:** https://github.com/yandex/yatool — Apache-2.0 (© 2023 YANDEX LLC), pushed 2026-10-30, **no releases or tags**. The README says: "Warning - bumpy road ahead … At this moment `ya` does not have a stable release and might not provide flawless and integrated experience for external users." Docs come in RU and EN.
- **Shipped in YDB:** the repo root has `ya`, `ya.bat`, `ya.conf`, and `.arcadia.root`. YDB builds only with `./ya make ydb/apps/ydbd --build relwithdebinfo`, which uses the YDB remote build cache (https://github.com/ydb-platform/ydb/blob/main/BUILD.md).
- The `ya` script is a Python shim ("Please, keep this script in sync with arcadia/ya"). It downloads a prebuilt `ya-bin` per platform from `https://devtools-registry.s3.yandex.net/<id>` and checks it by md5. Its `_tune_ssl()` sets `ssl._create_default_https_context = ssl._create_unverified_context`, so **TLS verification is disabled** for the download (https://github.com/ydb-platform/ydb/blob/main/ya).
- The YDB repo layout mirrors Arcadia: `contrib/ library/ util/ devtools/ build/ yql/ yt/ vendor/`.

## 5. Other Yandex OSS

| Project | Repo | License | Status (2026-10) | Use for us |
|---|---|---|---|---|
| CatBoost | https://github.com/catboost/catboost | Apache-2.0 | alive, v1.2.10 (2026-02-19), pushed 2026-10-30 | ML scheduler scoring ("YetiRank" for pods) |
| Yandex Tank | https://github.com/yandex/yandex-tank | Apache-2.0 | pushed 2026-10-27; GitHub "latest release" tag is ancient (2020) | load-test apiserver |
| Pandora | https://github.com/yandex/pandora | MPL-2.0 | v0.6.2 (2025-06-23), pushed 2026-10-04 | Go load generator (Tank's gun) |
| Gixy | https://github.com/yandex/gixy | MPL-2.0 | upstream stale: last push 2024-07, v0.1.21 (2024-01) | — |
| Gixy fork | https://github.com/dvershinin/gixy | MPL-2.0 | active, v0.2.59 (2026-10-25) | nginx ingress config lint |
| Odyssey | https://github.com/yandex/odyssey | BSD-3-Clause | alive, v1.5.2 (2026-09-13) | PG pooler addon |
| ClickHouse | https://github.com/ClickHouse/ClickHouse | Apache-2.0 | very alive, v26.10.2 (2026-10-24). Open-sourced by Yandex June 2016, spun out as ClickHouse Inc. 2021-09-20 (https://yandex.com/company/press_center/press_releases/2021/2021-09-20) | audit log / events store |
| YTsaurus | https://github.com/ytsaurus/ytsaurus | Apache-2.0 | alive, 25.4.2 (2026-09-30) | operator: https://github.com/ytsaurus/ytsaurus-k8s-operator (Apache-2.0, v0.36.0, 2026-08-20) |
| Diplodoc ("Diplodock") | https://github.com/diplodoc-platform/cli | MIT | alive, v5.64.1 (2026-10-29). `yandex/diplodock` does not exist (404) | docs site in YFM |
| YDB operator | https://github.com/ydb-platform/ydb-kubernetes-operator | Apache-2.0 | 0.8.0 (2026-03-13) | run YDB in-cluster |
| Yandex Cloud CSI | https://github.com/yandex-cloud/yc-csi-driver | Apache-2.0 | pushed 2026-10-07 | only for YC disks |
| k8s-csi-s3 (GeeseFS) | https://github.com/yandex-cloud/k8s-csi-s3 | Apache-2.0 | v0.43.7 (2026-05-05) | S3 PVs; GeeseFS: https://github.com/yandex-cloud/geesefs |
| Crossplane provider YC | https://github.com/yandex-cloud/crossplane-provider-yc | — | pushed 2026-10-26 | — |
| Terraform provider | https://github.com/yandex-cloud/terraform-provider-yandex | MPL-2.0 | pushed 2026-10-29 | — |
| Perforator | https://github.com/yandex/perforator | Apache-2.0 (LICENSE header; GitHub can't classify, mixed) | v0.1.0 (2026-02-24), pushed 2026-10-30 | eBPF cluster-wide continuous profiler, DaemonSet |
| YaFSDP | https://github.com/yandex/YaFSDP | Apache-2.0 | pushed 2026-10-26 | GPU flavour |
| temporal-over-ydb | https://github.com/yandex/temporal-over-ydb | MIT | pushed 2026-10-23 | precedent for "X over YDB" |
| pgmigrate | https://github.com/yandex/pgmigrate | PostgreSQL | pushed 2026-09-19 | — |
| WAL-G (Yandex-born) | https://github.com/wal-g/wal-g | Apache-2.0 | pushed 2026-10-28 | — |
| Gravity UI | https://github.com/gravity-ui/uikit | MIT | pushed 2026-10-29 | dashboard UI (YDB/YTsaurus UIs use it) |
| YDB embedded UI | https://github.com/ydb-platform/ydb-embedded-ui | Apache-2.0 | pushed 2026-10-29 | — |
| YaLM-100B | https://github.com/yandex/YaLM-100B | Apache-2.0 | dead (2023-07) | — |

## 6. Internal Yandex infra — public facts and scale numbers

| System | What it is | Publicly claimed numbers | Source |
|---|---|---|---|
| **RTC** (Runtime Cloud) | Internal cloud, part of "Deploy Platform"; started early 2010s when services "were counted in the hundreds" | "несколько десятков тысяч сервисов, аллоцирующих **более 5 миллионов ядер** на **более чем 100 тысяч серверов**"; **>100,000 service-spec changes per day**; in 2023 ~100k cores still waiting to migrate to YP | Lipin (CTO Yandex Platform Engineering), Habr, 2023-10-10: https://habr.com/ru/post/761946 |
| **Nanny** | Service management for RTC, since **2013**; resource isolation became core | — | same |
| **ISS** | **Not found** in public articles. Only trace: `ISS-AGENT--...` container names in portoshim tests | — | https://github.com/ten-nancy/portoshim/blob/master/cmd/portoshim/runtime_mapper_test.go |
| **YP** (Yandex Planner) | Cluster resource manager, built 2018, architecture "resembles Kubernetes"; one independent install per DC; stores state in YT dynamic tables / Cypress; users enter via Yandex Deploy | **>75% of company compute, >50,000 hosts, >2,000,000 cores**, largest cluster **just over 30,000 hosts** (vs k8s "до пяти тысяч"), peak nominal utilization 97%, measured CPU util 90.4%; server ≈ $10k; 1 util point on 10k machines ≈ $1M; 90→95% ≈ ≥$25M; defrag gives +5–7 points, "tens of millions of dollars" saved | Fomin, Habr, 2022-06-07: https://habr.com/ru/companies/yandex/articles/564510/ ; also "десятки тысяч нод и миллионы подов" in https://habr.com/ru/post/761946 |
| **Porto** | Container runtime on cgroups, mid-2014; most apps inside by 2015 | — | https://habr.com/ru/post/761946 |
| **Arcadia** | Company monorepo (started as search repo) | 2020: **6M commits, ~2M files, 2 TB with history**. 2021: "**порядка шести терабайт**", "**~7,000 changes every day**". 2023: trunk **10 million files**, repo **>2 TiB**, **>30,000 commits/week**. 2023 (ShiftMag): "roughly **10TB** repos", "millions of code files" (numbers disagree across talks — use whichever is funnier) | https://habr.com/ru/companies/yandex/articles/482926/ ; https://habr.com/ru/post/544842 ; https://habr.com/ru/post/761946 ; https://shiftmag.dev/mono-repo-infrastructure-yandex-1011/ |
| **Arc** | In-house VCS (2019), Git-like UX, FUSE working copy over distributed object storage; replaced SVN after a ~1.5-year Mercurial attempt; git-svn conversion failed on the full repo | 2020: 20% of monorepo devs on Arc; CI robots generated **2× the load of all Yandex developers combined**; expected "hundreds of times" growth | https://habr.com/ru/companies/yandex/articles/482926/ ; https://habr.com/ru/post/761946 |
| **ya make** | Build system since 2013, "used at Yandex for more than 10 years" | — | https://github.com/yandex/yatool ; https://habr.com/ru/post/761946 |
| **Sandbox** | Cluster for arbitrary automation tasks; runtime for CI and releases; since **2008** | **~200,000 cores, almost 1 PB RAM, ~50 PB disk, ~1M tasks/day** producing >2M resources, 5 DCs, ~5,000 servers, 15 control servers; state DB ~0.5 TB / ~400M objects / 20 shards × 5 replicas; team = **5 devs + 1 PM, no sysadmins** | Kardamanov, Habr, 2021-03-05: https://habr.com/ru/post/544842 |
| **Logbroker** | Multi-DC data collection/delivery bus, originally an API over Apache Kafka, exactly-once, survives DC loss | 2014: **~60 TB/day inbound**, outbound 3–4× inbound, up to **10,000 partitions**; 88% of chunks delivered <1 s, 99% <5 s; data grew ~2.5×/year (note: YDB Topics are its open-source successor — inference, not sourced) | Ozeritsky, Habr, 2014-10-09: https://habr.com/ru/company/yandex/blog/239823 |
| **Solomon** | Internal monitoring/TSDB. Public trace: YQL `solomon` provider in YDB repo (https://github.com/ydb-platform/ydb/tree/main/ydb/library/yql/providers/solomon). External product lineage: Yandex Monitoring / Monium | **Unverified:** a search snippet attributes "700 млн точек в секунду на запись" to a Yandex Monitoring job ad, but I could not open the primary source — don't use it without checking | — |
| **Juggler** | Alert aggregation / event monitoring. **No public article found.** Public trace: YTsaurus Odin ships a `juggler_client.py` | — | https://github.com/ytsaurus/ytsaurus/blob/main/yt/odin/lib/yt_odin/odinserver/juggler_client.py |
| **Startrek / Tracker** | Internal issue tracker, later productised as Yandex Tracker; built because existing solutions "не справлялись с необходимой нагрузкой"; used internally by ~7,000 people at external launch; reportedly even used for ordering kitchen cookies | — | CNews: https://zoom.cnews.ru/news/item/368221 ; Habr author page: https://habr.com/ru/users/lotrek/articles . Public code trace: `ytsaurus-cms/internal/startrek/` https://github.com/ytsaurus/ytsaurus-cms/tree/main/internal/startrek |
| **ABC** (service catalog), **Staff** (employee directory) | **Not found** in public articles or talks | — | — |
| **YQL** | SQL over YT/YDB | "около **800 петабайт** данных и **600 000** SQL-запросов" daily (Habr author profile blurb — weak source) | https://habr.com/en/users/aozeritsky/articles |
| **Yandex Metrica on YDB** | — | ">100 billion events/day", ">1.6M RPS" into YDB (HighLoad 2022 talk) | https://presentations.ydb.tech/2022/ru/highload_foundation_million_rps_metrika/presentation.pdf |
| Search indexing | — | ">10 petabytes per day" processed to index search; 90+ services | https://shiftmag.dev/mono-repo-infrastructure-yandex-1011/ |

### Joke-ready one-liners (all sourced above)
- YP: one cluster >30,000 hosts, against k8s's 5,000.
- RTC: 5M+ cores, 100k+ servers, 100k+ spec changes/day.
- Arcadia: 10M files in trunk, 30k+ commits/week; GitHub median repo = 1 MB vs Arcadia 2 TB (the talk's own comparison).
- Sandbox: 200k cores, 1 PB RAM, run by 5 devs and a PM, no sysadmins.
- Arc: CI robots alone produced 2× the load of every Yandex developer combined.
- `ya`: verifies md5, but not TLS.
