#!/bin/sh
# The failures of a conformance run, one line each: sh fails.sh RUN [MAX]
cd /work/e2e/${1:-r1}
awk '/^• \[FAILED\]/{f=1;n=NR;name=""} f&&NR==n+1{name=$0} f&&/^  \[FAILED\] [^i]/{sub(/^ +/,"",name); print substr(name,1,90) " || " substr($0,1,230); f=0}' run.log | sort | uniq -c | sort -k2 | head -${2:-100}
