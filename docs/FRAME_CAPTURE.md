# Frame pipeline capture

This diagnostic measures host work and frame boundaries without changing CPU,
VI, audio, PE, simulation or presentation policy. It is opt-in. Ordinary runs
allocate no capture buffers, start no capture thread and do no capture file IO.

Set `GEKKOAOT_PERF_CAPTURE` to a **new absolute output prefix** before launching.
The runner arms a bounded recorder; it starts only when `PREFIX.start` appears.
Review gameplay first. `GEKKOAOT_PERF_CAPTURE_SECONDS` defaults to 15 (1–120).
`PREFIX.stop` closes the window early. Exit also closes and flushes an active
window, including diagnostic stop-file exits which skip graphics teardown.

```sh
GEKKOAOT_ISO=/path/to/your/disc.rvz \
  GEKKOAOT_PERF_CAPTURE=/tmp/moh-capture GEKKOAOT_PERF_CAPTURE_SECONDS=20 \
  ./bin/gekkoaotctl run
# After reaching and visually reviewing the intended gameplay scene:
touch /tmp/moh-capture.start
python3 tools/frame_capture.py /tmp/moh-capture --analyze
```

The optional Linux controller takes an owned runner PID and a reviewed
`gameplay-proof.json` alongside its screenshot. It arms the window and records
per-thread and process CPU ticks. It refuses a PID without the matching capture
environment and a stop-file argument. It never sends input. Keep perf sampling
in a **separate, uninstrumented gameplay run**; permissions must already allow
it. No global perf policy is changed.

Outputs: histogram/counter JSON, raw event CSV, a Chrome/Perfetto trace, and
optionally CPU accounting and frame correlation JSON. JSON is published only
after all streams close. A full trace buffer increments an explicit drop count;
a truncated timeline must not be used to assert lost frames or full-window
presentation cadence. Histogram counts still include all accepted samples.

## What events mean

| Event | Observed boundary / identity |
| --- | --- |
| `aot_main_inclusive`, `aot_secondary_inclusive` | Native module dispatch call, guest entry PC. Includes nested host service calls. |
| `sdk_inclusive`, `native_os_inclusive` | Runtime SDK/OS dispatch body; does not count compiler-inlined services separately. |
| `idle_wait` | Actual host wait; id 1: GX event predicate; id 2: host deadline. |
| `clock_park_inclusive`, `poll_park_inclusive` | Existing policy function time, including quick rejection. Not proof all of that time slept. |
| `gx_queue_full`, `gx_queue_empty`, `gx_drain` | Producer capacity wait, consumer arrival wait, ordered drain. Drain id identifies `SyncReason`. |
| `parser_queue_age`, `render_queue_age` | Time from publication to consumer execution. Parser id identifies packet kind; value is packet size/count. |
| `render_work_inclusive`, `render_fence` | Worker callback time and an actual CPU wait for renderer work. |
| `frame_begin`, `frame_end` | Aurora recording packet ID; no implication that the packet reaches the display. |
| `xfb_publish` | Successful GX display copy publication; id is its Aurora packet ID, value is the XFB address. |
| `xfb_selected`, `scanout_duplicate` | VI-selected cached copy and explicit unchanged-scanout rejection. |
| `present_request` | Runtime scanout request, id is XFB address, value says whether a pending request was replaced. |
| `pe_command_parsed`, `pe_guest_delivery` | Existing PE command parse and delivery boundaries. **Neither proves GPU retirement.** Id is BP register, value is its payload. |
| `vi_field_observed` | Runtime observed a VI field boundary; value distinguishes odd/even. Not physical monitor refresh. |
| `gpu_submit`, `gpu_complete` | Actual WebGPU queue submission and successful asynchronous `OnSubmittedWorkDone` callback. Added completion observation runs only during capture, without waiting or modifying PE. |
| `gpu_staging_ready` | Existing successful staging remap callback; packet-associated, not display scanout. |
| `surface_acquire`, `surface_present`, `wsi_success` | WSI acquisition, present-call duration, and successful present. Failure never counts as success. |
| `pipeline_compile`, `pipeline_wait`, `pipeline_throttle` | Actual creation, required pipeline wait and existing background throttle. |
| `disc_read`, `disc_wait`, `dsp_work` | Instrumented host worker read/wait and DSP service bodies, where the corresponding implementation exists. |

Render queue items carry packet context. Cached XFB slots retain their producer
packet ID and restore it on the render thread; the parser's *current* packet
must not be used to name a previous scanout. Frame IDs, XFB addresses, VI counts
and PE token payloads are distinct identity domains. Multiple submissions or
XFB copies can belong to one game iteration. Never relabel XFB copies as FPS.

## Limits and interpretation

* WSI success is **not physical monitor scanout**. Presentation interval
  quantiles and present counts are reported under their actual names.
* Wall durations are inclusive and can overlap across nested calls and threads.
  Do not sum them as exclusive CPU utilization. Linux tick accounting gives
  actual per-thread/process CPU, with tick quantization and the arming margin.
* This first recorder observes completed scopes admitted inside the window.
  A wait already open at capture start or still open at stop can be absent.
  Entirely idle worker wall occupancy and exact sleep/wakeup scheduling latency
  consequently need OS scheduling traces; a missing scope is not zero wait.
* AOT, SDK, NativeOS, poll-policy, DSP and short parser/arrival events retain all
  histogram samples but sample timeline entries 1/256. Slow parser age/arrival
  events are retained. Other boundaries are unsampled. The timeline is not an
  instruction or state-materialization trace, and cannot resolve FP helpers.
* Counts of allocations, copies, CPU state materializations, inlined SDK calls,
  actual audio consumption/underruns and physical input/scanout latency are not
  provided by this recorder. They remain explicit validation gaps.
* Audio using the private harness's dummy backend does not validate audible
  correctness. Strict Native-Only coverage must be tested separately.
* Never derive a complete game loop from decompilation progress. MOH's public
  [GMFE69 research project](https://github.com/kyleckroeger/moh-frontline-decomp)
  describes a secondary game ELF separate from the boot DOL. Symbol/ELF
  identity must match before labeling profile addresses.

## Tests and rollout

`hardware.perf-capture` tests disabled/start/stop admission, frame context,
concurrent append/stop, buffer overflow and complete histogram counting.
`hardware.latency-histogram` verifies histogram bounds and concurrent writers.
`hardware.frame-capture-analysis` tests identity domains, partial windows,
repeated presents and exited-thread CPU accounting. Configure/build the pinned
Aurora bridge as usual to apply the anchor-checked, idempotent overlay.

The Phase 0 release gate is **not complete** merely because these tests pass.
The benchmark report must identify the reviewed scene, binary/module hashes,
flags, unmeasured categories, commercial-game validation and any unavailable
perf/scanout/audio instrumentation. Preserve correct architectural fallbacks.
