# Testing

## Unit and hardware tests

CTest covers the controller, module ABI, NativeOS, device services, disc I/O,
GX IR, widescreen policy, renderer settings, texture models, the launcher and
the release tree:

```sh
ctest --test-dir build --output-on-failure
```

GPU tests are opt-in (`GEKKOAOT_RUN_GPU_TESTS=1`); see
[Graphics](GRAPHICS.md#diagnostics). Microbenchmarks for code-generation
changes live in `tests/bench/`.

## Compatibility laboratory

`tools/compat_lab.py` inventories a disc collection, compiles each game
through `gekkoaotctl` and observes it in a private headless Gamescope session.
It never sends input to the desktop and never opens the user's memory cards;
every run gets a fresh user directory. Extracted disc trees are repacked into a
private GCM.

```sh
python3 tools/compat_lab.py --profile full --inventory-only
python3 tools/compat_lab.py --profile quick --incremental
python3 tools/compat_lab.py --profile graphics --incremental --native-samples
python3 tools/compat_lab.py --profile full --compile-only --jobs 2
python3 tools/compat_lab.py --release --seconds 90
```

The disc collection is `--corpus`, else `GEKKOAOT_GAME_CORPUS`, else
`games/` in the repository. `--results`, `--only`, `--seconds`, `--compile-timeout`,
`--jobs`, `--controller` and `--runner` customize a campaign. Runs are serial
so GPU measurements do not overlap. CMake targets `compat-inventory`,
`compat-quick`, `compat-graphics`, `compat-full`, `compat-release`,
`compat-playability` and `compat-widescreen` wrap the common profiles; they
are excluded from the default build.

### Caching

The controller owns compilation caches. The lab requires the controller's exact
launch selection and validates the DOL, SDK identity and secondary module
list. `--incremental` reuses an observation only when the source, the
runner and controller hashes, the profile and the environment all match; any
change to shared runtime, compiler, renderer or patch sources invalidates the
whole selected set. `--observe-cached` is a diagnostic mode, forbidden in
release runs.

### Statuses

| Status | Meaning |
| --- | --- |
| `COMPILE_FAILED` | Compilation failed or timed out. |
| `BOOT_FAILED` | The process started but lost its worker. |
| `BOOT_OK` | A DOL loaded and the runtime progressed. |
| `MENU_OK` | A reviewed menu fixture matched. |
| `GAMEPLAY_VERIFIED` | A human-reviewed gameplay scene was imported. |
| `CRASH`, `GRAPHICS_BROKEN`, `AUDIO_BROKEN` | Failures; they take precedence over progress. |
| `NOT_TESTED` | Not selected, compile-only, or no execution infrastructure. |

Screenshots and scripted input never certify gameplay on their own. Reviews
are imported with `--review review.json`; the importer checks that each
capture belongs to the unchanged run. Outputs are `results.json`,
`results.csv` and a filterable `index.html`, plus per-run logs, captures,
register snapshots on failures and process samples.

### Fixtures

`--fixtures fixtures.json` supplies scripted input and reference screenshots
per disc (keys are corpus-relative paths):

```json
{
  "Example (USA)/Example (USA).rvz": {
    "scene": "menu",
    "reviewed_by": "reviewer name",
    "input": [{"at_seconds": 3, "keys": ["Return"], "hold_seconds": 0.1}],
    "screenshots": [{
      "capture_index": -1,
      "baseline": "baselines/example-menu.png",
      "sha256": "SHA-256 of the reviewed baseline",
      "max_mean_error": 0.01
    }]
  }
}
```

The graphical worker needs Gamescope, Xwayland, xdotool, gamescopectl and a
Vulkan driver. Other hosts support inventory and compilation.

### Gameplay and widescreen campaigns

`compat-playability` (`--profile full --gameplay --enhanced --seconds 180
--input-seconds 70`) attempts entry into every game with exploratory Start/A
input and the full Enhanced Renderer, and exports the renderer coverage
report. Acceptance needs a reviewed gameplay scene, measured contribution on
a presented graph and a visual Enhanced review.

`compat-widescreen` runs every entry in 4:3 and 16:9 and keeps both outcomes.
A 16:9 launch never certifies widescreen; matching scenes must be reviewed for
proportions, FOV, culling and composition.

## Frame pipeline capture

An opt-in recorder measures host work and frame boundaries without changing
any timing policy. Ordinary runs allocate nothing for it.

```sh
GEKKOAOT_ISO=/path/to/disc.rvz \
  GEKKOAOT_PERF_CAPTURE=/tmp/capture GEKKOAOT_PERF_CAPTURE_SECONDS=20 \
  ./bin/gekkoaotctl run
# once the intended scene is on screen:
touch /tmp/capture.start
python3 tools/frame_capture.py /tmp/capture --analyze
```

`PREFIX.stop` ends the window early. Outputs are histogram and counter JSON, a
raw event CSV and a Chrome/Perfetto trace.

| Event | Boundary |
| --- | --- |
| `aot_main_inclusive`, `aot_secondary_inclusive` | Native module dispatch, including nested services |
| `sdk_inclusive`, `native_os_inclusive` | Runtime SDK and OS dispatch bodies |
| `idle_wait` | Host wait on a GX event (id 1) or a deadline (id 2) |
| `clock_park_inclusive`, `poll_park_inclusive` | Poll-parking policy, including quick rejections |
| `gx_queue_full`, `gx_queue_empty`, `gx_drain` | Producer and consumer waits, ordered drains |
| `parser_queue_age`, `render_queue_age` | Publication-to-execution latency |
| `frame_begin`, `frame_end`, `xfb_publish` | Aurora recording packets and display copies |
| `xfb_selected`, `scanout_duplicate`, `present_request` | VI scanout selection |
| `pe_command_parsed`, `pe_guest_delivery` | PE parse and delivery (not GPU retirement) |
| `gpu_submit`, `gpu_complete` | WebGPU submission and completion callbacks |
| `surface_acquire`, `surface_present`, `wsi_success` | Window-system presentation |
| `pipeline_compile`, `pipeline_wait` | Shader pipeline creation and waits |
| `disc_read`, `disc_wait`, `dsp_work` | Disc and DSP worker bodies |

Durations are inclusive and can overlap across threads; do not sum them as CPU
time. WSI success is not physical scanout. Frame IDs, XFB addresses, VI counts
and PE tokens are separate identity domains, and XFB copies are not frames per
second.

## Profiling

- `GEKKOAOT_NATIVE_SAMPLE=<dir>` with `tools/analyze_native_samples.py`:
  signal-based sampling without perf privileges.
- `GEKKOAOT_GX_PHASE_PROFILE=1`: exclusive cycles per GX phase.
- `GEKKOAOT_HUGE_TEXT_PERF_MAP=1`: maps module code for `perf report`.
- `GEKKOAOT_CRASH_DUMP=<file>`: writes guest MEM1 when the run ends.

Keep captures under `.gekkoaot/diagnostics/` or another ignored directory.
