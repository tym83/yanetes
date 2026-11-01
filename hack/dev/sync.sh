#!/bin/bash
# Copies the working tree into the builder at /work/src, where everything
# is built: nothing heavy runs on the laptop.
cd "$(dirname "$0")/../.." && COPYFILE_DISABLE=1 tar --exclude=.git --exclude=research -czf - . |
  "$(dirname "$0")/k.sh" exec -i builder -c cxx -- sh -c 'mkdir -p /work/src && tar -xzf - -C /work/src 2>/dev/null'
