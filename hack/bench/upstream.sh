#!/usr/bin/env bash
# Puts upstream's control plane on the bench VM, the way control-plane.sh
# puts ours on yx-1: kube-apiserver on etcd, kube-controller-manager and
# kube-scheduler, as systemd services. Binaries and certificates come from
# the builder over the pod network.
#
#   hack/bench/upstream.sh [vm-name]
set -euo pipefail
NODE=${1:-yx-bench}
NS=${NS:-tenant-yandexnetes}
K=(kubectl --kubeconfig "${KUBECONFIG_WORKSHOP:-$HOME/workshop.txt}" --context "${KUBE_CONTEXT:-admin@workshop}" -n "$NS")
b() { "${K[@]}" exec -i builder -c cxx -- sh -c "$1"; }
vm() {
  KUBECONFIG="${KUBECONFIG_WORKSHOP:-$HOME/workshop.txt}" virtctl --context "${KUBE_CONTEXT:-admin@workshop}" -n "$NS" \
    ssh "ubuntu@vmi/vm-instance-$1" --local-ssh-opts="-o StrictHostKeyChecking=no" \
    --local-ssh-opts="-o UserKnownHostsFile=/dev/null" --local-ssh-opts="-o LogLevel=ERROR" -c "$2"
}

IP=$("${K[@]}" get vmi "vm-instance-$NODE" -o jsonpath='{.status.interfaces[0].ipAddress}')
BUILDER=$("${K[@]}" get pod builder -o jsonpath='{.status.podIP}')

# Certificates, tokens and kubeconfigs, made once and kept in the builder.
b "set -e; d=/work/bench; mkdir -p \$d/pki; cd \$d/pki
  [ -f ca.key ] || openssl req -x509 -newkey rsa:2048 -nodes -keyout ca.key -out ca.crt -subj /CN=bench-ca -days 3650 2>/dev/null
  [ -f sa.key ] || openssl genrsa -out sa.key 2048 2>/dev/null
  openssl rsa -in sa.key -pubout -out sa.pub 2>/dev/null
  printf '[req]\ndistinguished_name=dn\n[dn]\n[ext]\nsubjectAltName=DNS:localhost,DNS:kubernetes,IP:127.0.0.1,IP:10.97.0.1,IP:$IP\n' > san.cnf
  openssl req -newkey rsa:2048 -nodes -keyout tls.key -subj /CN=kube-apiserver -out tls.csr 2>/dev/null
  openssl x509 -req -in tls.csr -CA ca.crt -CAkey ca.key -CAcreateserial -days 825 -extfile san.cnf -extensions ext -out tls.crt 2>/dev/null
  cd \$d
  [ -f tokens.csv ] || { echo \"\$(openssl rand -hex 24),admin,admin,system:masters\" > tokens.csv
    echo \"\$(openssl rand -hex 24),system:kube-controller-manager,kcm\" >> tokens.csv
    echo \"\$(openssl rand -hex 24),system:kube-scheduler,scheduler\" >> tokens.csv; }
  conf() { printf 'apiVersion: v1\nkind: Config\nclusters: [{name: bench, cluster: {server: \"%s\", insecure-skip-tls-verify: true}}]\nusers: [{name: u, user: {token: \"%s\"}}]\ncontexts: [{name: c, context: {cluster: bench, user: u}}]\ncurrent-context: c\n' \"\$1\" \"\$(grep \",\$2,\" tokens.csv | cut -d, -f1)\"; }
  conf https://$IP:6443 admin > admin.conf
  conf https://127.0.0.1:6443 admin > local-admin.conf
  conf https://127.0.0.1:6443 system:kube-controller-manager > kcm.conf
  conf https://127.0.0.1:6443 system:kube-scheduler > scheduler.conf
  tar -czf /work/out/www/bench/conf.tgz pki tokens.csv kcm.conf scheduler.conf local-admin.conf"

vm "$NODE" "set -e; sudo mkdir -p /opt/bench /etc/bench
  for f in kube-apiserver kube-controller-manager kube-scheduler; do sudo curl -fsS -o /opt/bench/\$f http://$BUILDER:8099/bench/\$f; sudo chmod +x /opt/bench/\$f; done
  curl -fsS http://$BUILDER:8099/bench/conf.tgz | sudo tar -xzf - -C /etc/bench
  unit() { sudo tee /etc/systemd/system/\$1.service >/dev/null; }
  unit kube-apiserver <<EOF
[Service]
ExecStart=/opt/bench/kube-apiserver --etcd-servers=http://etcd.$NS.svc.cozy.local:2379 --secure-port=6443 --advertise-address=$IP --tls-cert-file=/etc/bench/pki/tls.crt --tls-private-key-file=/etc/bench/pki/tls.key --client-ca-file=/etc/bench/pki/ca.crt --token-auth-file=/etc/bench/tokens.csv --service-account-key-file=/etc/bench/pki/sa.pub --service-account-signing-key-file=/etc/bench/pki/sa.key --service-account-issuer=https://kubernetes.default.svc --authorization-mode=RBAC --service-cluster-ip-range=10.97.0.0/16 --allow-privileged=true
Restart=always
LimitNOFILE=1048576
[Install]
WantedBy=multi-user.target
EOF
  unit kube-controller-manager <<EOF
[Service]
ExecStart=/opt/bench/kube-controller-manager --kubeconfig=/etc/bench/kcm.conf --authentication-kubeconfig=/etc/bench/kcm.conf --authorization-kubeconfig=/etc/bench/kcm.conf --use-service-account-credentials=true --service-account-private-key-file=/etc/bench/pki/sa.key --root-ca-file=/etc/bench/pki/ca.crt --leader-elect=false --allocate-node-cidrs=false
Restart=always
[Install]
WantedBy=multi-user.target
EOF
  unit kube-scheduler <<EOF
[Service]
ExecStart=/opt/bench/kube-scheduler --kubeconfig=/etc/bench/scheduler.conf --authentication-kubeconfig=/etc/bench/scheduler.conf --authorization-kubeconfig=/etc/bench/scheduler.conf --leader-elect=false
Restart=always
[Install]
WantedBy=multi-user.target
EOF
  sudo systemctl daemon-reload
  sudo systemctl enable kube-apiserver kube-controller-manager kube-scheduler
  sudo systemctl restart kube-apiserver && sleep 10 && sudo systemctl restart kube-controller-manager kube-scheduler
  sleep 5; systemctl is-active kube-apiserver kube-controller-manager kube-scheduler | tr '\n' ' '; echo
  curl -sk https://127.0.0.1:6443/version | head -3"
echo "upstream control plane on $NODE at https://$IP:6443; kubeconfig in the builder at /work/bench/admin.conf"
