# Optimizations and diagnostics

GekkoAOT combines native AOT execution with proven host-service replacements. Performance depends on the title, scene, compilation policy and host. This release makes no universal CPU-percentage or compilation-time guarantee.

## Native execution paths

- Direct static calls and tailcalls reduce generic dispatch where target and state requirements are known.
- Recognized clock, PAD, DVD/FST and GX services can replace their original AOT bodies.
- MEM1, locked-cache and resident paged-VM paths avoid repeated generic address translation where bounds and permissions are proven.
- Quantized memory and scalar/paired-single math use native host operations for admitted cases; NaNs, denormals, exceptions and unsupported controls retain exact compatibility paths.
- Proven RAM/clock polling can sleep until a useful deadline. Device, interrupt and scheduler events remain wake boundaries.
- Ordered typed GX array/matrix commands bypass redundant renderer parsing. Compatible adjacent draws share preparation/submission work.
- Attribute plans, indexed-array preparation, bind groups and resource versions are cached. Guest writes and EFB operations invalidate affected resources.
- GPU upload and snapshot lifetimes prevent pending draws from observing overwritten data.

These are generic services and compiler/renderer transformations, not hard-coded replacements for a particular game's logic. CP/FIFO, MMIO, CPUState, MMU/cache and DSP fallbacks are still needed.

## Independent controls

The launcher selects native paths for new configurations and respects explicit environment overrides. Existing saved settings can differ. A compile-time flag generally requires a compatible rebuild; runtime flags can be compared using the same compiled module.

| Control | Scope | Purpose |
| --- | --- | --- |
| `GEKKOAOT_NATIVE_OS` | Compile/runtime | Native OS service admission; GUI default is enabled for new settings |
| `GEKKOAOT_NATIVE_HW` / `GEKKOAOT_NATIVE_HW_<DEVICE>` | Runtime | Native hardware policy and individual device controls |
| `GEKKOAOT_NATIVE_SDK_CLOCK_LOWERING` | Compile | Direct recognized SDK clock calls |
| `GEKKOAOT_NATIVE_HARDWARE_LOWERING` | Compile | Proven direct hardware services |
| `GEKKOAOT_NATIVE_WGPIPE_TRY_LOWERING` | Compile | Native write-gather submission for admitted stores |
| `GEKKOAOT_NATIVE_ADDRESS_LOWERING` | Compile | Proven direct guest-memory lowering |
| `GEKKOAOT_NATIVE_FLAT_VMEM` | Compile/runtime | Direct flat memory view |
| `GEKKOAOT_NATIVE_PAGED_VMEM` | Compile | Resident paged-VM lowering |
| `GEKKOAOT_NATIVE_LOCKED_CACHE` | Compile/runtime | Direct locked-cache view |
| `GEKKOAOT_NATIVE_SCALAR_MATH` | Compile | Exact admitted native scalar math |
| `GEKKOAOT_PSQ_RESOLVED` | Compile | Resolve a paired access once before quantization |
| `GEKKOAOT_PSQ_WGPIPE` | Compile | Quantized stores into native write gather |
| `GEKKOAOT_PSQ_EXTEND_FAST` | Compile | Native/exact quantized float expansion |
| `GEKKOAOT_NATIVE_GX_ARRAY_SERVICE` | Compile/runtime | Recognized `GXSetArray` host service |
| `GEKKOAOT_NATIVE_GX_DIRECT_MATRICES` | Runtime | Typed recognized GX matrix uploads |
| `GEKKOAOT_NATIVE_GX_TYPED_ARRAYS` | Runtime | Typed SDK array commands |
| `GEKKOAOT_NATIVE_GX_ATTRIBUTE_PLAN` | Runtime | Cached ordered vertex-attribute traversal |
| `GEKKOAOT_NATIVE_GX_DRAW_RUNS` | Runtime | Coalesce adjacent compatible draws |
| `GEKKOAOT_NATIVE_GX_BIND_GROUP_CACHE` | Runtime | Reuse compatible GPU bindings |
| `GEKKOAOT_NATIVE_GX_TEXTURE_VERSION_REUSE` | Runtime | Reuse tracked texture-source versions |
| `GEKKOAOT_NATIVE_READ_POLL` / `GEKKOAOT_NATIVE_COUNTER_POLL` | Runtime | Proven polling-loop waits |
| `GEKKOAOT_CLOCK_SPIN_PARK` | Runtime | Bounded analysis and parking of repeated clock waits |
| `GEKKOAOT_GX_EARLY_PE_PUBLISH` | Runtime | Publish completion-related graphics work promptly |
| `GEKKOAOT_HUGE_TEXT` | Linux runtime | Optional transparent huge-page backing for suitable AOT text |

Set an independently supported control to `0` to compare its fallback. Compiler options, exact defaults and supported combinations are defined by `tools/gekkoaotctl.cpp`, `src/BuildController.cpp` and the owning runtime service.

Experimental state-SSA and LLVM hot/cold-splitting options are not general performance guarantees. Artificial guest-rate ceilings remain opt-in and can reduce game speed.

## Compilation and caches

Generated objects, modules, profiles and dependency snapshots are stored under the state directory, normally `.gekkoaot/` for source-tree development or the platform user cache for installed builds. `GEKKOAOT_STATE_DIR` overrides it.

Warm launches reuse matching artifacts. Changes to compiler sources, module ABI, native admission, lowering settings or profile input can require translation or relinking. Cached objects do not justify using an incompatible final module. A release-version change can also select a new module key.

LLVM retains its optimized IR pipeline. Floating-point code does not use blanket fast-math. Worker batching and adaptive secondary-module scheduling reduce orchestration costs without changing the guest workload. Full cold compilation of large titles can still exceed a few minutes.

## Profile-guided optimization

**Train PGO** builds an instrumented LLVM module, runs representative execution and flushes counters on graceful shutdown. The controller merges profiles with `llvm-profdata`, validates them and builds the profile-use module. Later launches can automatically use a compatible merged profile.

Profiles are stored at `<state>/cache/pgo/<game-id>/merged.profdata`. `GEKKOAOT_PGO_AUTO_USE=0` disables automatic selection. Training is cumulative by default; `GEKKOAOT_PGO_ACCUMULATE=0` starts fresh.

PGO and ThinLTO are separate from runtime compatibility. After a CFG/codegen change, retraining may be required. Do not infer a performance improvement from profile availability alone.

## Reproducible measurement

1. Capture the effective launcher arguments/environment, exact compiler/module and graphics settings.
2. Use the same save, scene, pacing, native options and thread affinity for both samples.
3. Distinguish cold compilation, warm compilation, load time and steady gameplay.
4. Measure the whole runner and individual threads; state whether CPU percentages are normalized to one core or the entire machine.
5. Compare frame pacing and rendered output along with CPU/RAM use. A slower simulation can falsely appear cheaper.
6. Disable one path at a time and retain correctness evidence for its fallback.

Keep captures and full reports under `.gekkoaot/diagnostics/` or another ignored directory. Do not commit perf data, raw profiles, benchmark CSVs, save files or extracted game content. Development scripts remain available under `tools/`; their generated measurements are not release resources.
