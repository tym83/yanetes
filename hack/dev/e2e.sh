#!/bin/sh
# Runs upstream conformance against the stand, in the builder's go
# container (copy to /work/e2e.sh):
#
#   NODES=8 sh /work/e2e.sh RUN [FOCUS] [SKIP]
#
# Defaults: every [Conformance] test except [Serial] and [Disruptive],
# 8 in parallel. The serial part: NODES=1 with focus
# '\[Conformance\].*\[Serial\]|\[Serial\].*\[Conformance\]' and skip 'NONE'.
# Results in /work/e2e/RUN/run.log; summary with sh /work/e2e/fails.sh RUN.
cd /work/e2e
export PATH=/work/bin:$PATH
R=/work/e2e/$1; mkdir -p $R
FOCUS=${2:-'\[Conformance\]'}
SKIP=${3:-'\[Serial\]|\[Disruptive\]'}
setsid nohup kubernetes/test/bin/ginkgo --nodes=${NODES:-8} --timeout=4h --output-interceptor-mode=none --output-dir=$R --junit-report=junit.xml --no-color \
  kubernetes/test/bin/e2e.test -- --kubeconfig=/work/run/admin.conf --provider=skeleton \
  --ginkgo.focus="$FOCUS" --ginkgo.skip="$SKIP" --report-dir=$R > $R/run.log 2>&1 < /dev/null &
echo started $R
