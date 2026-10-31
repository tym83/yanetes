#!/usr/bin/env bash
# The upstream side of the comparison: a VM shaped like the one yapi runs
# on (4 vCPU, 8 GiB), plain Ubuntu; hack/bench/upstream.sh installs
# kube-apiserver, kube-controller-manager and kube-scheduler on it.
set -euo pipefail
NAME=${1:-yx-bench}
NS=${NS:-tenant-yandexnetes}
K=(kubectl --kubeconfig "${KUBECONFIG_WORKSHOP:-$HOME/workshop.txt}" --context "${KUBE_CONTEXT:-admin@workshop}" -n "$NS")
SSH_KEY=$(cat "${SSH_KEY_FILE:-$HOME/.ssh/id_ed25519.pub}")
"${K[@]}" apply -f - <<YAML
apiVersion: apps.cozystack.io/v1alpha1
kind: VMDisk
metadata:
  name: $NAME
spec:
  optical: false
  source:
    http:
      url: https://cloud-images.ubuntu.com/noble/current/noble-server-cloudimg-amd64.img
  storage: 30Gi
  storageClass: replicated
---
apiVersion: apps.cozystack.io/v1alpha1
kind: VMInstance
metadata:
  name: $NAME
spec:
  instanceType: ""
  instanceProfile: ubuntu
  runStrategy: Always
  resources:
    cpu: 4
    sockets: 1
    memory: 8Gi
  disks:
  - name: $NAME
  cloudInit: |
    #cloud-config
    hostname: $NAME
    ssh_authorized_keys:
      - $SSH_KEY
YAML
