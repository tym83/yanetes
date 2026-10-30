#!/usr/bin/env bash
# Starts a Yandexnetes worker as a VM in the tenant:
#
#   hack/workshop/node.sh <name>
#
# The node's token goes into yapi's token file (yapi rereads it on
# restart), the CA and the tokens into the VM's cloud-init; nothing secret
# lands in git.
set -euo pipefail

NAME=${1:?node name required}
NS=${NS:-tenant-yandexnetes}
DIR=$(cd "$(dirname "$0")" && pwd)
K=(kubectl --kubeconfig "${KUBECONFIG_WORKSHOP:-$HOME/workshop.txt}" --context "${KUBE_CONTEXT:-admin@workshop}" -n "$NS")
b() { "${K[@]}" exec builder -c cxx -- sh -c "$1"; }

API=${API:-https://$("${K[@]}" get pod builder -o jsonpath='{.status.podIP}'):6443}

token_for() { # user groups
  local t
  t=$(b "grep ',$1,' /work/run/tokens.csv | head -1 | cut -d, -f1")
  if [ -z "$t" ]; then
    t=$(openssl rand -hex 24)
    b "echo '$t,$1,,\"$2\"' >> /work/run/tokens.csv"
    echo "added a token for $1; restart yapi to load it" >&2
  fi
  echo "$t"
}
TOKEN=$(token_for "system:node:$NAME" system:nodes)
PROXY_TOKEN=$(token_for system:kube-proxy system:node-proxier)
CA=$(b "base64 -w0 /work/run/pki/ca.crt")
SSH_KEY=$(cat "${SSH_KEY_FILE:-$HOME/.ssh/id_ed25519.pub}")

cloud_init=$(sed -e "s|@NAME@|$NAME|g" -e "s|@API@|$API|g" -e "s|@TOKEN@|$TOKEN|g" \
  -e "s|@PROXY_TOKEN@|$PROXY_TOKEN|g" -e "s|@CA@|$CA|g" -e "s|@SSH_KEY@|$SSH_KEY|g" "$DIR/node-cloud-init.yaml")

"${K[@]}" apply -f - <<EOF
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
    cpu: ${CPU:-4}
    sockets: 1
    memory: ${MEMORY:-8Gi}
  disks:
  - name: $NAME
  cloudInit: |
$(printf '%s\n' "$cloud_init" | sed 's/^/    /')
EOF
