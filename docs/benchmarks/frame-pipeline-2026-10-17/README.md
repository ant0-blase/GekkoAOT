# Frame pipeline evidence, 2026-10-17

Status: **Phase 0 in progress, not a 60 FPS fix or full Native-Only coverage**.
Raw CSV, traces, reviewed screenshots, private saves and commands remain in
`.gekkoaot/diagnostics/phase0-20261017/`. No game bytes or saves are distributed.
Only aggregate telemetry and executable hashes are checked in.

## MOH Frontline, GMFE69 revision 0

Reviewed workload: first-person D-Day shoreline facing the beach/bunkers, rifle
and compass HUD, after four seconds of forward movement. Sky-only, water-only,
loading and introductory scenes were excluded. The initial saturated diagnostic
capture was discarded; `moh-baseline2` is complete (159,705 stored events,
zero dropped events, 20.040 seconds). Startup is excluded by the manual gate.

| Actual boundary | Count in the baseline window |
| --- | ---: |
| Successful GX display-copy publication | 1,671 |
| Parsed PE finish commands / delivery | 836 / 836 |
| Observed VI field boundaries | 1,201 |
| Host present requests | 1,202 |
| Successful WSI present | 831 (41.47/s) |
| GPU queue submissions / successful retirement observations | 1,672 / 1,670 |
| Explicit cached scanout duplicates rejected | 370 |
| Parser queue-full waits | 14, totaling 37.81 ms |

WSI interval median / p95 / p99: **16.795 / 33.960 / 34.625 ms**. These are
successful host surface presents, not physical scanout. XFB copies average
83.4/s; counting them as displayed FPS would double-count this workload.
The unpresented-copy count is a bounded-window correlation result, not proof
of lost draws: auxiliary copies and boundary frames must be accounted for.
PE parsing/delivery is explicitly separate from actual GPU queue retirement.

Runner process CPU was 17.77 CPU seconds in the 20.067-second tick observation:
88.55% of one core. Main thread: 12.78 s; GX parser: 3.55 s; render: 1.41 s.
These are CPU times; inclusive trace wall durations are not added to them.
Queue-full blocking here is under 0.2% of the capture window. There were no
captured pipeline compilations in this warm gameplay interval. This evidence
does not support choosing pipeline warmup or a globally full parser queue as
the sole explanation for this scene's remaining pacing issue.

The two frame-rate counters were not measuring the same event. Other scenes
and the reported D-Day trenches still require separate controlled captures.

## Profiling limits

Hardware `cycles:u` and software `cpu-clock:u` perf events were both denied by
the host's perf access policy. Passwordless sudo was unavailable; no system
policy was changed. Consequently this report **does not contain an
uninstrumented host instruction-sampling profile**, and cannot confirm a
`ppc_fmuls`/`secondary_dispatch` host-time breakdown.

`moh-entry-symbols.json` maps *sampled module entry scopes* against the local
secondary `Moh2RelGC.elf`, SHA-1
`eb75ebc0073db35e75eba06742b151591d36c004`, matching the identity documented by
[the GMFE69 research project](https://github.com/kyleckroeger/moh-frontline-decomp).
Those inclusive entry scopes are not leaf-function or PMU samples. No game
source, addresses or special cases are used to change runtime behavior.

Private tests use dummy audio and a headless compositor. They cannot certify
physical display/input latency, audible correctness, audio consumption drift
or underruns. Allocation/copy/materialization counters and fully exclusive
per-thread wall accounting remain unimplemented. These gaps keep the full
Phase 0 acceptance gate open.

## Reproduction

Use the same runner/bridge/module/SDK hashes and options in `conditions.json`,
the same copied save and camera/position, a fresh capture prefix and a reviewed
in-game screenshot. See [capture instructions](../../FRAME_CAPTURE.md). Keep
builds/tests quiet during captures. Run cold and warm trials separately.
No FPS or simulation-speed optimization is claimed by this evidence PR.
