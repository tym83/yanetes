#!/bin/sh
# What the builder needs besides the sources, made once on its disk
# (/work), in the go container. Kubernetes v1.37.1 is the reference.
#
#   /work/e2e/src/kubernetes  upstream source (protos, OpenAPI, e2e sources to read)
#   /work/e2e/kubernetes      e2e.test and ginkgo (kubernetes-test tarball)
#   /work/openapi             upstream OpenAPI v2/v3 documents yapi serves
#   /work/proto               upstream .proto files + hints for yapi's protobuf codec
#   /work/bin                 kubectl, kwok, built binaries
set -eu
V=v1.37.1
mkdir -p /work/e2e/src /work/bin /work/openapi /work/proto
[ -d /work/e2e/src/kubernetes ] || git clone --depth 1 --branch $V https://github.com/kubernetes/kubernetes /work/e2e/src/kubernetes
[ -x /work/e2e/kubernetes/test/bin/e2e.test ] || curl -fsSL https://dl.k8s.io/$V/kubernetes-test-linux-amd64.tar.gz | tar -xzf - -C /work/e2e
[ -x /work/bin/kubectl ] || { curl -fsSLo /work/bin/kubectl https://dl.k8s.io/$V/bin/linux/amd64/kubectl; chmod +x /work/bin/kubectl; }
K=/work/e2e/src/kubernetes
cp $K/api/openapi-spec/swagger.json /work/openapi/
cp -r $K/api/openapi-spec/v3 /work/openapi/
# Protos keep their import paths: k8s.io/api/..., k8s.io/apimachinery/..., ...
St=$K/staging/src/k8s.io
for m in api apimachinery apiextensions-apiserver kube-aggregator; do
  (cd $St && find $m -name generated.proto | while read f; do mkdir -p /work/proto/k8s.io/$(dirname $f); cp $f /work/proto/k8s.io/$f; done)
done
cd /work/src && go run ./hack/protohints /work/proto $St/api=k8s.io.api $St/apimachinery=k8s.io.apimachinery \
  $St/apiextensions-apiserver=k8s.io.apiextensions_apiserver $St/kube-aggregator=k8s.io.kube_aggregator
echo prepared
