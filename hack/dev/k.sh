#!/bin/bash
# kubectl against the workshop cluster, in the stand's namespace.
exec kubectl --kubeconfig "${KUBECONFIG_WORKSHOP:-$HOME/workshop.txt}" --context "${KUBE_CONTEXT:-admin@workshop}" -n "${NS:-tenant-yandexnetes}" "$@"
