#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
CTL="${GEKKOAOT_CTL:-$ROOT/bin/gekkoaotctl}"
OUT="${1:-$ROOT/perf.data}"
if [[ ! -x "$CTL" ]]; then
  echo "error: $CTL not found; build GekkoAOT first" >&2
  exit 1
fi
command -v perf >/dev/null || { echo 'error: perf is required' >&2; exit 1; }

# Do NOT enable GEKKOAOT_COMPAT_DIAGNOSTICS here: the single-dispatch debug
# horizon intentionally changes hot-path behavior. perf should observe normal
# runtime scheduling and AOT chaining.
unset GEKKOAOT_COMPAT_DIAGNOSTICS || true
unset GEKKOAOT_COMPAT_BREAK_ON_FAULT || true
export GEKKOAOT_RUN_PREFIX="perf record -F ${GEKKOAOT_PERF_FREQ:-999} -e cycles:u --call-graph dwarf,16384 -o '$OUT' --"

"$CTL" run || rc=$?
rc="${rc:-0}"
if [[ -s "$OUT" ]]; then
  echo >&2
  echo "[GekkoAOT compat] perf capture: $OUT" >&2
  echo "Report: perf report -i '$OUT'" >&2
  echo "Text:   perf report --stdio -i '$OUT' --sort=overhead,symbol --percent-limit=0.1 | head -n 180" >&2
fi
exit "$rc"
