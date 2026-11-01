#!/bin/bash
# Syncs and builds everything in the builder: yapi with its unit tests,
# then nanny, yplanner and the perf tool. Deploy with
# hack/workshop/control-plane.sh yx-1 restart.
set -euo pipefail
D=$(dirname "$0")
"$D/sync.sh"
"$D/k.sh" exec builder -c cxx -- sh -c 'cd /work/build/yapi && cmake --build . -j16 2>&1 | grep -E "error:" ; ./yapi_unittest --gtest_brief=1 | tail -1'
"$D/k.sh" exec builder -c go -- sh -c 'cd /work/src && go build -o /work/bin/nanny ./cmd/nanny && go build -o /work/bin/yplanner ./cmd/yplanner && go build -o /work/bin/perf ./hack/perf && echo go ok'
