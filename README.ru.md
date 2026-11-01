# Яnetes

[English version](README.md)

Kubernetes без Kubernetes. Те части кластера, которые думают, — API-сервер,
контроллеры, планировщик — написаны с нуля на открытом стеке одной большой
поисковой компании: API-сервер на C++ и
[userver](https://github.com/userver-framework/userver) хранит кластер в
[YDB](https://github.com/ydb-platform/ydb) вместо etcd. Узлы — апстримные:
kubelet, kube-proxy, containerd, flannel и CoreDNS говорят с ним так же, как
с kube-apiserver, и разницы не замечают.

Это сатира, доведённая до дела. Шутка — компания, которая всё переписывает
сама; соль в том, что результат измерен собственными тестами и бенчмарками
апстрима.

| Часть | Что это | Где |
|---|---|---|
| **yapi** | API-сервер: C++20, userver, YDB как единственное хранилище | `yapi/` |
| **nanny** | Контроллеры (deployments, statefulsets, jobs, GC, квоты, …), Go | `pkg/nanny/`, `cmd/nanny/` |
| **yplanner** | Планировщик с affinity, host ports и вытеснением, Go | `pkg/yplanner/`, `cmd/yplanner/` |
| Узлы | Апстримные kubelet, kube-proxy, containerd, flannel, CoreDNS | `hack/workshop/`, `deploy/addons/` |

## Где мы сейчас

- **Conformance:** проходит апстримный conformance Kubernetes v1.37.1 —
  462 из 462 тестов за один прогон, с включённым RBAC.
- **Масштаб:** держит 30 001 узел [KWOK](https://kwok.sigs.k8s.io/) на
  control plane из 4 vCPU. Против kube-apiserver + etcd на том же железе:

| Сценарий | kube-apiserver + etcd | Яnetes |
|---|---|---|
| Регистрация 30 001 узла | 14,0 с | 5,9 с |
| Все узлы Ready | 78 с | 53 с |
| Возраст heartbeat, p99 | 32 с | 3 с |
| Создание 10 000 подов | 14,2 с | 7,7 с |
| 10 000 подов до Running | 266 с | 69 с |
| Список 30 001 узла в JSON | 1,5 с | 1,0 с |

Чего пока нет: у объектов нет `managedFields`, списки в protobuf ещё
перемеряются.

## Сборка и запуск

Всё тяжёлое собирается и работает в namespace Kubernetes: под-сборщик, под
с YDB и несколько виртуальных машин. В [`docs/HANDOFF.md`](docs/HANDOFF.md)
(на английском) описаны архитектура, стенд, сборка, деплой и тесты, методика
бенчмарка и полученные уроки.

```sh
hack/dev/build.sh                            # собрать yapi, nanny, yplanner
hack/workshop/control-plane.sh yx-1          # поставить control plane на VM
hack/workshop/node.sh yx-2                   # поднять и подключить воркер
NODES=8 sh /work/e2e.sh r1                   # conformance, внутри сборщика
```

## Не аффилирован

Яnetes — независимый сатирический проект. Он не связан с ООО «Яндекс» и его
аффилированными лицами, не одобрен и не спонсируется ими. userver и YDB
используются так же, как их использует любой пользователь этих открытых
проектов. Названия других компаний и продуктов принадлежат их владельцам.

## Лицензия

[Apache License 2.0](LICENSE).
