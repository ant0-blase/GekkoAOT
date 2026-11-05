#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
CTL="${GEKKOAOT_CTL:-$ROOT/bin/gekkoaotctl}"
if [[ ! -x "$CTL" ]]; then
  echo "error: $CTL not found; build GekkoAOT first" >&2
  exit 1
fi
command -v gdb >/dev/null || { echo 'error: gdb is required' >&2; exit 1; }

export GEKKOAOT_COMPAT_DIAGNOSTICS=1
export GEKKOAOT_COMPAT_BREAK_ON_FAULT=1

# Use a command file so host-side crashes are useful even when they are not
# GekkoAOT compatibility traps. After the first SIGTRAP/SIGABRT/SIGSEGV stop,
# GDB automatically prints the native stack and all thread stacks, then stays
# interactive for deeper inspection.
GDB_CMDS="$(mktemp -t gekkoaot-gdb.XXXXXX)"
trap 'rm -f "$GDB_CMDS"' EXIT
cat >"$GDB_CMDS" <<'GDB'
set pagination off
set print thread-events off
set confirm off
handle SIGTRAP stop print nopass
handle SIGABRT stop print pass
handle SIGSEGV stop print pass
run
echo \n=== GekkoAOT automatic crash backtrace ===\n
bt
bt full
info threads
thread apply all bt
echo === end automatic crash backtrace ===\n
GDB
export GEKKOAOT_RUN_PREFIX="gdb -q -x '$GDB_CMDS' --args"

cat >&2 <<'MSG'
[GekkoAOT compat] GDB will stop on the first native compatibility fault or host crash.
SIGTRAP, SIGABRT and SIGSEGV automatically dump bt/bt full/all-thread stacks.
GDB remains interactive after the dump. Useful follow-ups:
  frame 1
  p/x cpu_
  info registers
Continue with: c
MSG

"$CTL" run
