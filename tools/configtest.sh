#!/bin/sh
# Run the directed differential suite under a spread of configurations,
# so config coverage is a standing regression rather than a one-time
# bring-up check. Every program must pass -d in every configuration.
set -e
cd "$(dirname "$0")/.."

run_suite() {
  desc="$1"
  shift
  printf '== configtest: %s\n' "$desc"
  for t in tests/*.hex; do
    ./rvsim -d "$@" "$t" >/dev/null 2>/tmp/rvsim-configtest.err || {
      echo "FAIL: $t ($desc)"
      tail -4 /tmp/rvsim-configtest.err
      exit 1
    }
  done
  if [ -f cdemo/demo.bin ]; then
    ./rvsim -d "$@" cdemo/demo.bin >/dev/null 2>/tmp/rvsim-configtest.err || {
      echo "FAIL: cdemo/demo.bin ($desc)"
      tail -4 /tmp/rvsim-configtest.err
      exit 1
    }
  fi
}

run_suite "defaults"
run_suite "width 1" -O width=1 -O wbPorts=1 -O aluCount=1
run_suite "width 4" -O width=4 -O wbPorts=4 -O aluCount=4
run_suite "rob 16" -O robSize=16
run_suite "rob 128" -O robSize=128
run_suite "single mshr" -O l1d.mshrs=1 -O l2.mshrs=1
run_suite "conservative" -O memOrder=conservative
run_suite "bypass" -O memOrder=bypass
run_suite "flat memory" -O flatMemory=1
run_suite "tiny caches" -O l1i.size=4k -O l1d.size=4k -O l2.size=32k
run_suite "gshare" -O ghrBits=8
run_suite "dep predictor" -O depPredictor=1
echo "configtest: all configurations pass"
