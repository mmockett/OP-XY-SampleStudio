#!/bin/sh
# Builds and runs the harness in a 32-bit Linux container (the device's pointer size), then lists
# the frames it wrote; render.py draws them.
#   tools/screens/run.sh <open-xy checkout> [out folder]
set -e
here=$(cd "$(dirname "$0")" && pwd)
ext=$(cd "$here/../../sample_studio" && pwd)
sdk=$(cd "$1" && pwd)
out=${2:-$here/out}
mkdir -p "$out/frames"
docker build -q --platform linux/386 -t studio-screens-386 "$here" >/dev/null
docker run --rm --platform linux/386 -v "$sdk/sdk/include":/include:ro -v "$ext":/ext:ro -v "$here":/h:ro \
  -v "$out/frames":/out studio-screens-386 sh -c '
  g++ -std=gnu++11 -O1 -w -fno-exceptions -fno-rtti -I/include -I/ext -I/h -o /work/harness /h/harness.cpp &&
  /work/harness /out'
