#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Compare the C engine (firmware/src/engine.c, built for the host) with the TypeScript engine on golden vectors.
#   tools/golden.sh [REL_RMS]      (default: error rms <= 5 % of the reference rms; run inside `nix develop`)
# The chip-* patches (hard edges, no band limiting) are held to 10 % instead.
# The metric is rms, not max: waveforms with edges (saw, square) shift by a fraction of a sample between the float
# reference and the Q16 pitch grid, which gives short spikes at the edges but no audible difference.
set -e
cd "$(dirname "$0")/.."
TOL="${1:-0.05}"
(cd web && npx vite-node scripts/golden.ts)
mkdir -p build
gcc -O1 -Wall -Wextra -Wno-unused-parameter -Wno-unused-result -o build/host_test firmware/test/host_test.c -lm
gcc -O1 -Wall -Wextra -Wno-unused-parameter -Wno-unused-result -o build/proto_test firmware/test/proto_test.c -lm
fail=0
build/proto_test build/golden/303-acid.m1p build/golden/proto.syx $(cat build/golden/proto.args) build/golden/proto-slot.syx || fail=1
for p in build/golden/*.m1p; do
  n="${p%.m1p}"
  out="$(build/host_test "$p" "$n.ev" "$n.f32")" || { echo "FAIL $(basename "$n"): $out"; fail=1; continue; }
  err="$(echo "$out" | sed 's/.*rel_rms \([0-9.]*\).*/\1/')"
  tol="$TOL"
  case "$(basename "$n")" in chip-*) tol=0.10 ;; esac   # the chip oscillators are not band-limited: edges land on a different sample now and then
  if awk "BEGIN{exit !($err > $tol)}"; then echo "FAIL $(basename "$n"): $out"; fail=1; else echo "ok   $(basename "$n"): $out"; fi
done
exit $fail
