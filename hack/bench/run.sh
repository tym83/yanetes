#!/bin/sh
# One scenario, run the same way against any API server, in the builder:
#
#   sh run.sh KUBECONFIG TAG
#
# 30 001 KWOK nodes, their heartbeats, lists, and 10 000 pods placed and
# running. Results go to /work/bench/TAG.txt; KWOK runs under
# /work/bench/TAG/ with the same configuration as for every server.
set -u
CONF=$1
TAG=$2
OUT=/work/bench/$TAG.txt
DIR=/work/bench/$TAG
export PATH=/work/bin:$PATH
K="kubectl --kubeconfig $CONF --request-timeout=300s"
mkdir -p "$DIR"
: > "$OUT"
say() { echo "$*" | tee -a "$OUT"; }
now() { date +%s; }

say "== $TAG $(date -u +%FT%TZ)"
say "$(perf -kubeconfig "$CONF" nodes 30001)"

cp /work/perf/all.yaml "$DIR/kwok.yaml"
(cd "$DIR" && setsid nohup kwok --kubeconfig "$CONF" -c kwok.yaml \
  --manage-nodes-with-annotation-selector=kwok.x-k8s.io/node=fake > kwok.log 2>&1 < /dev/null &)
start=$(now)
while :; do
  ready=$($K get nodes -l type=kwok --no-headers 2>/dev/null | awk '$2=="Ready"' | wc -l)
  [ "$ready" -ge 30001 ] && break
  [ $(( $(now) - start )) -gt 1800 ] && break
  sleep 10
done
say "nodes Ready: $ready of 30001 after $(( $(now) - start ))s"

sleep 180
t=$(now)
say "$($K -n kube-node-lease get leases -o jsonpath='{range .items[*]}{.spec.renewTime}{"\n"}{end}' |
  while read -r r; do echo $(( t - $(date -d "$r" +%s) )); done | sort -n |
  awk '{a[NR]=$1} END {print "lease age s: p50", a[int(NR*0.5)], "p90", a[int(NR*0.9)], "p99", a[int(NR*0.99)], "max", a[NR]}')"
say "$($K get nodes -l type=kwok --no-headers | awk '{print $2}' | sort | uniq -c | tr '\n' ' ')"

for i in 1 2; do
  s=$(date +%s%N)
  n=$($K get nodes --no-headers 2>/dev/null | wc -l)
  say "kubectl get nodes: $n in $(( ($(date +%s%N) - s) / 1000000 ))ms"
done
say "$(perf -kubeconfig "$CONF" -workers 4 lists 8)"

start=$(now)
say "$(perf -kubeconfig "$CONF" pods 10000 bench)"
while :; do
  running=$($K -n bench get pods --no-headers 2>/dev/null | awk '$3=="Running"' | wc -l)
  [ "$running" -ge 10000 ] && break
  [ $(( $(now) - start )) -gt 1800 ] && break
  sleep 5
done
say "pods Running: $running of 10000 after $(( $(now) - start ))s"
say "== done $(date -u +%FT%TZ)"
