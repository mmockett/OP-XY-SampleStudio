#!/bin/sh
# Builds and runs the harness in a 32-bit Linux container (the device's pointer size), then renders.
#   tools/screens/run.sh <open-xy checkout with Sample Studio> [out folder]
set -e
here=$(cd "$(dirname "$0")" && pwd)
sdk=$(cd "$1" && pwd)
out=${2:-$here/out}
mkdir -p "$out/frames"
docker build -q --platform linux/386 -t studio-screens-386 "$here" >/dev/null
docker run --rm --platform linux/386 -v "$sdk":/sdk:ro -v "$here":/h:ro -v "$out/frames":/out studio-screens-386 sh -c '
  cp -r /sdk/sdk/platform/opxy-1.2.1 /work/p && cp /h/stock_host.hpp /work/p/stock.hpp && cp /h/barrier_host.hpp /work/p/barrier.hpp &&
  g++ -std=gnu++11 -O1 -w -fno-exceptions -fno-rtti -I/work/p -I/sdk/sdk/include -o /work/harness /h/harness.cpp &&
  /work/harness /out'
