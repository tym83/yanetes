#!/usr/bin/env bash
# Puts the control plane on a node, the way kubeadm puts it on the first
# one: yapi, nanny and yplanner as systemd services of a worker VM, where
# they reach pod and service addresses (webhooks, aggregated APIs, proxies)
# that the builder pod outside the cluster network cannot.
#
#   hack/workshop/control-plane.sh <node> [restart]
#
# The binaries come from the builder: yapi with the shared libraries ldd
# names (the VM runs the same Ubuntu as the build image), nanny and
# yplanner static. The serving certificate is reissued for the node's
# address; secrets go from the builder to the VM through a pipe, never
# through git. With "restart" only the binaries are replaced.
set -euo pipefail

NODE=${1:?node name required}
MODE=${2:-install}
NS=${NS:-tenant-yandexnetes}
K=(kubectl --kubeconfig "${KUBECONFIG_WORKSHOP:-$HOME/workshop.txt}" --context "${KUBE_CONTEXT:-admin@workshop}" -n "$NS")
b() { "${K[@]}" exec -i builder -c "$1" -- sh -c "$2"; }
vm() {
  KUBECONFIG="${KUBECONFIG_WORKSHOP:-$HOME/workshop.txt}" virtctl --context "${KUBE_CONTEXT:-admin@workshop}" -n "$NS" \
    ssh "ubuntu@vmi/vm-instance-$1" --local-ssh-opts="-o StrictHostKeyChecking=no" \
    --local-ssh-opts="-o UserKnownHostsFile=/dev/null" --local-ssh-opts="-o LogLevel=ERROR" -c "$2"
}

IP=$("${K[@]}" get vmi "vm-instance-$NODE" -o jsonpath='{.status.interfaces[0].ipAddress}')
API="https://$IP:6443"
echo "control plane on $NODE at $API"

# The bundle: binaries, libraries, schemas, the static config.
b cxx "set -e; o=/work/out/cp; mkdir -p \$o/bin \$o/lib \$o/etc
  strip -o \$o/bin/yapi /work/build/yapi/yapi
  ldd /work/build/yapi/yapi | awk '/=> \\//{print \$3}' | grep -v -E 'libc.so|libm.so|libpthread|libdl.so|librt.so|libresolv' | xargs -I{} cp -L {} \$o/lib/
  cp /work/src/yapi/configs/static_config.yaml \$o/etc/
  cp -r /work/proto /work/openapi \$o/; mkdir -p \$o/rbac && cp /work/src/yapi/bootstrap/rbac/*.yaml \$o/rbac/"
b go "cp /work/bin/nanny /work/bin/yplanner /work/out/cp/bin/"
# Each release unpacks into a directory of its own and "current" moves to
# it at once: files rewritten in place under a running process crash it
# (SIGBUS), and a transfer cut short must not leave a half-written library.
# The node fetches the bundle from the builder over the pod network: a
# stream through this machine's link is the slow, breakable part.
REL=$(date -u +%Y%m%d%H%M%S)
BUILDER=$("${K[@]}" get pod builder -o jsonpath='{.status.podIP}')
b cxx "set -e; mkdir -p /work/out/www; tar -C /work/out/cp -czf /work/out/www/cp-$REL.tgz .
  curl -fs -o /dev/null http://127.0.0.1:8099/ || (setsid nohup python3 -m http.server 8099 --directory /work/out/www >/dev/null 2>&1 < /dev/null &)
  sleep 1"
vm "$NODE" "set -e; sudo mkdir -p /opt/yandexnetes/releases/$REL
  curl -fsS http://$BUILDER:8099/cp-$REL.tgz | sudo tar -xzf - -C /opt/yandexnetes/releases/$REL
  sudo test -x /opt/yandexnetes/releases/$REL/bin/yapi
  sudo ln -sfn /opt/yandexnetes/releases/$REL /opt/yandexnetes/current.new && sudo mv -T /opt/yandexnetes/current.new /opt/yandexnetes/current"

if [ "$MODE" != restart ]; then
  # A serving certificate that names the node too, from the same CA.
  b cxx "set -e; cd /work/run/pki
    cat > san.cnf <<C
[req]
distinguished_name=dn
[dn]
[ext]
subjectAltName=DNS:localhost,DNS:$NODE,DNS:kubernetes,DNS:kubernetes.default,DNS:kubernetes.default.svc,DNS:kubernetes.default.svc.cluster.local,IP:127.0.0.1,IP:10.112.0.1,IP:$IP
keyUsage=digitalSignature,keyEncipherment
extendedKeyUsage=serverAuth
C
    openssl req -new -key tls.key -subj /CN=yapi -out tls.csr
    openssl x509 -req -in tls.csr -CA ca.crt -CAkey ca.key -CAcreateserial -days 825 -extfile san.cnf -extensions ext -out tls.crt 2>/dev/null
    cat tls.crt ca.crt > tls-chain.crt
    # The aggregator's own CA and the certificate it shows the API servers
    # behind it, which trust it for the identity headers.
    if [ ! -f front-proxy-ca.key ]; then
      openssl req -x509 -newkey rsa:2048 -nodes -keyout front-proxy-ca.key -out front-proxy-ca.crt -subj /CN=front-proxy-ca -days 3650 2>/dev/null
    fi
    printf '[ext]\nextendedKeyUsage=clientAuth\n' > front-proxy.cnf
    openssl req -newkey rsa:2048 -nodes -keyout front-proxy-client.key -subj /CN=front-proxy-client -out front-proxy-client.csr 2>/dev/null
    openssl x509 -req -in front-proxy-client.csr -CA front-proxy-ca.crt -CAkey front-proxy-ca.key -CAcreateserial -days 825 \
      -extfile front-proxy.cnf -extensions ext -out front-proxy-client.crt 2>/dev/null"
  b cxx "tar -C /work/run -czf - pki/ca.crt pki/tls.key pki/tls-chain.crt pki/sa.key pki/front-proxy-ca.crt pki/front-proxy-client.crt pki/front-proxy-client.key tokens.csv admin.conf" |
    vm "$NODE" "sudo mkdir -p /etc/yandexnetes/cp && sudo tar -xzf - -C /etc/yandexnetes/cp && sudo chmod -R go-rwx /etc/yandexnetes/cp"
  vm "$NODE" "sudo tee /etc/yandexnetes/cp/config_vars.yaml >/dev/null <<EOF
port: 6443
tls-cert: /etc/yandexnetes/cp/pki/tls-chain.crt
tls-key: /etc/yandexnetes/cp/pki/tls.key
tokens-file: /etc/yandexnetes/cp/tokens.csv
openapi-dir: /opt/yandexnetes/current/openapi
proto-dir: /opt/yandexnetes/current/proto
rbac-dir: /opt/yandexnetes/current/rbac
ydb-endpoint: ydb.$NS.svc.cozy.local:2136
ydb-database: /local
worker-threads: 4
log-level: warning
service-account-key-file: /etc/yandexnetes/cp/pki/sa.key
advertise-address: $IP
service-cidr: 10.112.0.0/16
client-ca-file: /etc/yandexnetes/cp/pki/ca.crt
requestheader-client-ca-file: /etc/yandexnetes/cp/pki/front-proxy-ca.crt
proxy-client-cert-file: /etc/yandexnetes/cp/pki/front-proxy-client.crt
proxy-client-key-file: /etc/yandexnetes/cp/pki/front-proxy-client.key
EOF
sudo sed -E 's#server: \"?https://[^\",} ]*\"?#server: \"https://127.0.0.1:6443\"#' /etc/yandexnetes/cp/admin.conf | sudo tee /etc/yandexnetes/cp/local.conf >/dev/null
unit() { sudo tee /etc/systemd/system/\$1.service >/dev/null; }
unit yapi <<EOF
[Unit]
After=network-online.target
[Service]
Environment=LD_LIBRARY_PATH=/opt/yandexnetes/current/lib
ExecStart=/opt/yandexnetes/current/bin/yapi --config /opt/yandexnetes/current/etc/static_config.yaml --config_vars /etc/yandexnetes/cp/config_vars.yaml
Restart=always
RestartSec=3
LimitNOFILE=1048576
[Install]
WantedBy=multi-user.target
EOF
unit nanny <<EOF
[Unit]
After=yapi.service
[Service]
ExecStart=/opt/yandexnetes/current/bin/nanny --kubeconfig /etc/yandexnetes/cp/local.conf --root-ca-file /etc/yandexnetes/cp/pki/ca.crt
Restart=always
RestartSec=3
[Install]
WantedBy=multi-user.target
EOF
unit yplanner <<EOF
[Unit]
After=yapi.service
[Service]
ExecStart=/opt/yandexnetes/current/bin/yplanner --kubeconfig /etc/yandexnetes/cp/local.conf
Restart=always
RestartSec=3
[Install]
WantedBy=multi-user.target
EOF
sudo systemctl daemon-reload"
  # The builder's copies stop: one control plane writes the kubernetes endpoints.
  b cxx "pkill -x yapi || true"
  b go "pkill -x nanny || true; pkill -x yplanner || true"
  # A reinstall over running services must restart them onto the new
  # release and configuration; enable --now leaves running ones alone.
  vm "$NODE" "sudo systemctl enable yapi nanny yplanner && sudo systemctl restart yapi && sleep 3 && sudo systemctl restart nanny yplanner"
  # Kubelets, kube-proxies and flannels, and the builder's own kubeconfig, follow.
  for vmi in $("${K[@]}" get vmi -o jsonpath='{.items[*].metadata.name}'); do
    n=${vmi#vm-instance-}
    vm "$n" "sudo sed -i 's|server: \"https://[^\"]*\"|server: \"$API\"|' /etc/yandexnetes/kubelet.conf /etc/yandexnetes/kube-proxy.conf && sudo systemctl restart kubelet kube-proxy flanneld"
  done
  b cxx "sed -i -E 's#server: \"?https://[^\",} ]*\"?#server: \"$API\"#' /work/run/admin.conf"
else
  vm "$NODE" "sudo systemctl restart yapi && sleep 3 && sudo systemctl restart nanny yplanner"
fi
vm "$NODE" "sleep 3; systemctl is-active yapi nanny yplanner | tr '\n' ' '; echo; curl -sk https://127.0.0.1:6443/version"
