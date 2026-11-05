# Performance

The complete list of variables is generated in [Environment variables](ENVIRONMENT.md).

GekkoAOT combines native AOT code with native replacements for the services a
game uses. CPU use depends on the game, the scene, the compiled module and the
host, so this page describes the mechanisms and how to measure them rather
than promising a fixed figure.

## Reference measurements

Measured on Linux with an Intel Core Ultra 7 258V (8 cores) and its integrated
Arc 140V GPU, 16:9, 75–120 seconds per game. CPU is the share of the whole
machine used by the game process.

| Game | Before | After | Change | Since |
| --- | --- | --- | --- | --- |
| GoldenEye: Rogue Agent | 13.2 % | 3.3 % | Idle threads no longer service a timer storm | 0.0.8 |
| Medal of Honor: Rising Sun | 13.3 % | 5.1 % | Same | 0.0.8 |
| Disney/Pixar Cars | 13 % | 7 % | Yield, fixed-point and register polls parked | 0.0.8 |
| Tales of Symphonia | 1.8 GB RAM | 430 MB RAM | REL code no longer copied into huge pages | 0.0.8 |
| Driven | 12.8 % | 3.4 % | Clock polls parked | 0.0.7 |

Most other games in the test set use 2–8 % of the machine.

Measurements added in 0.0.9, as a share of **one core** unless stated:

| Scene | Before | After | Change |
| --- | --- | --- | --- |
| Harry Potter and the Chamber of Secrets, Burrow cutscene: main + GX + render threads | 118 % | 54 % | Draw runs, texture write watch, owner cache, translated skinning loops, trained PGO for the module and the renderer |
| Star Fox Adventures, ThornTail Hollow, 1920×1200, full Enhanced settings | 4 fps | 37 fps | Logic-op shadow masks no longer split the render pass per triangle |
| Star Fox Adventures, same scene: GPU busy | 100 % | 71 % | Single depth load per AO tap |
| Medal of Honor: Frontline, menus: GX thread | 8.4 % | 3.2 % | HD-texture request memo, screen-fill verdict cache |
| Medal of Honor: Frontline, menus: main thread | 37 % | 33 % | Cache flushes no longer take a queue lock per line |

Code-generation microbenchmarks (`tests/bench/`): single-precision arithmetic
1.96× faster without FPSCR status bits, guest stores 3.97× faster without
reservation clearing. Each applies only to games whose code is proven not to
observe the removed state; 75 of 96 tested executables qualify for the first.

## How CPU time is saved

**Native code paths**

- Direct static calls and tail calls avoid the dispatcher when the target and
  register state are known.
- Recognized clock, PAD, DVD/FST, memory, cache and GX services replace their
  compiled bodies.
- MEM1, locked-cache and resident paged-VM accesses use host pointers when
  bounds and permissions are proven.
- Quantized loads and stores and scalar/paired-single math use native
  operations. NaNs, denormals, exceptions and unsupported FPSCR modes keep
  exact fallback paths.

**Waiting without spinning**

- Polls of memory, counters, hardware registers and the clock sleep until the
  next event that could end them. Clock polls are run in a sandbox to compute
  the exact exit time.
- Yield loops and loops that make no progress are parked.
- When every thread is idle, short repeating timers are coalesced so the host
  sleeps instead of delivering tens of thousands of interrupts per second.
- Device, interrupt and scheduler events always wake a parked loop.

**Graphics**

- Recognized GX array and matrix calls become ordered typed commands, so the
  renderer does not parse the equivalent FIFO packets.
- Compatible adjacent draws share preparation and submission.
- Attribute plans, indexed arrays, bind groups and texture versions are
  cached. Guest writes and EFB operations invalidate what they touch.
- Completion-related commands (draw done, tokens) are published early so the
  game does not wait on work that is still batched.

**Memory**

- On Linux the main module's code is moved onto transparent huge pages,
  which reduces instruction-TLB misses.
- REL modules stay file-backed: only the code that runs is loaded, and the
  pages can be shared and reclaimed.

These are generic mechanisms, not per-game patches. The CP FIFO, MMIO,
CPUState and DSP fallbacks remain for everything that is not proven.

## Options

Most options default to the faster path; set one to `0` to compare against its
fallback. Compile-time options need a rebuild of the game module; runtime
options can be compared with the same module.

| Option | Default | Scope | Purpose |
| --- | --- | --- | --- |
| `GEKKOAOT_NATIVE_OS` | 1 (launcher) | Compile and runtime | Native OS services |
| `GEKKOAOT_NATIVE_OS_ACTIVE_QUEUE` | 1 | Runtime | Keep the SDK active-thread list exact |
| `GEKKOAOT_NATIVE_HW`, `GEKKOAOT_NATIVE_HW_<DEVICE>` | 1 | Runtime | Native hardware policy and per-device control |
| `GEKKOAOT_NATIVE_SDK_CLOCK_LOWERING` | 1 | Compile | Direct recognized SDK clock calls |
| `GEKKOAOT_NATIVE_HARDWARE_LOWERING` | 1 | Compile | Proven direct hardware services |
| `GEKKOAOT_NATIVE_WGPIPE_TRY_LOWERING` | 1 | Compile | Native write-gather submission |
| `GEKKOAOT_NATIVE_ADDRESS_LOWERING` | 1 | Compile | Proven direct guest-memory access |
| `GEKKOAOT_NATIVE_FLAT_VMEM` | 1 | Compile and runtime | Direct flat virtual-memory view |
| `GEKKOAOT_NATIVE_PAGED_VMEM` | 1 | Compile | Resident paged-VM access |
| `GEKKOAOT_NATIVE_LOCKED_CACHE` | 1 | Compile and runtime | Direct locked-cache view |
| `GEKKOAOT_NATIVE_SCALAR_MATH` | 1 | Compile | Exact native scalar math |
| `GEKKOAOT_PSQ_RESOLVED` | 1 | Compile | Resolve a paired access once |
| `GEKKOAOT_PSQ_WGPIPE` | 1 | Compile | Quantized stores into the write-gather pipe |
| `GEKKOAOT_PSQ_EXTEND_FAST` | 1 | Compile | Native quantized float expansion |
| `GEKKOAOT_NATIVE_GX_ARRAY_SERVICE` | 1 | Compile and runtime | Native `GXSetArray` |
| `GEKKOAOT_NATIVE_GX_DIRECT_MATRICES` | 1 | Runtime | Typed GX matrix uploads |
| `GEKKOAOT_NATIVE_GX_TYPED_ARRAYS` | 1 | Runtime | Typed SDK array commands |
| `GEKKOAOT_NATIVE_GX_ATTRIBUTE_PLAN` | 1 | Runtime | Cached vertex-attribute traversal |
| `GEKKOAOT_NATIVE_GX_DRAW_RUNS` | 1 | Runtime | Merge adjacent compatible draws |
| `GEKKOAOT_NATIVE_GX_BIND_GROUP_CACHE` | 1 | Runtime | Reuse GPU bindings |
| `GEKKOAOT_NATIVE_GX_TEXTURE_VERSION_REUSE` | 1 | Runtime | Reuse texture-source versions |
| `GEKKOAOT_GX_EARLY_PE_PUBLISH` | 1 | Runtime | Publish completion commands early |
| `GEKKOAOT_NATIVE_READ_POLL`, `GEKKOAOT_NATIVE_COUNTER_POLL` | 1 | Runtime | Park memory and counter polls |
| `GEKKOAOT_CLOCK_SPIN_PARK` | 1 | Runtime | Park clock polls |
| `GEKKOAOT_YIELD_SPIN_PARK` | 1 | Runtime | Park yield loops |
| `GEKKOAOT_FIXED_POINT_PARK` | 1 | Runtime | Park loops that make no progress |
| `GEKKOAOT_MMIO_POLL_PARK` | 1 | Runtime | Park hardware register polls |
| `GEKKOAOT_AOT_POLL_PARK` | 1 | Runtime | Park compiled poll loops |
| `GEKKOAOT_IDLE_DEC_QUANTUM_US` | 100 | Runtime | Timer coalescing window while all threads are idle |
| `GEKKOAOT_CP_POLL_DRAIN` | 1 | Runtime | Serve a deferred GPU drain when a thread polls the CP |
| `GEKKOAOT_CP_POLL_PARK` | 0 | Runtime | Also park CP status polls (experimental) |
| `GEKKOAOT_HUGE_TEXT` | 1 | Linux runtime | Huge pages for the main module's code |
| `GEKKOAOT_HUGE_TEXT_SECONDARY` | 0 | Linux runtime | Also copy REL module code into huge pages |
| `GEKKOAOT_GX_MATRIX_CHAIN` | 0 | Runtime | Keep native chains running across GX matrix calls (experimental) |
| `GEKKOAOT_GUEST_RATE_CEILING_PCT` | 0 (off) | Runtime | Cap the guest instruction rate; can slow games down |
| `GEKKOAOT_FPSCR_STATUS_ELISION` | 1 | Compile | Skip FPSCR status bits when no code reads them |
| `GEKKOAOT_NO_RESERVATIONS` | 1 | Compile | Skip reservation clearing when the game has no `lwarx` |
| `GEKKOAOT_SECONDARY_THINLTO` | 1 | Compile | ThinLTO for secondary executables |
| `GEKKOAOT_LLVM_IR_LEVEL`, `GEKKOAOT_LLVM_CODEGEN_LEVEL` | 3, 2 | Compile | Compiler tier; the launcher's compilation modes set them |
| `GEKKOAOT_HOT_CHAIN` | 0 | Compile | Keep hot functions chained across region leaders (experimental) |
| `GEKKOAOT_NATIVE_GX_DRAW_RUNS` | 1 | Runtime | Precomputed draw runs per display-list plan |
| `GEKKOAOT_NATIVE_GX_TEXTURE_WRITE_WATCH` | 1 | Runtime (Linux) | Invalidate only textures whose pages were written |
| `GEKKOAOT_NATIVE_GX_TEXTURE_SOURCE_IDS` | 1 | Runtime | Stable texture object IDs per source |
| `GEKKOAOT_NATIVE_GX_COPY_RANGE_FORGET` | 1 | Runtime | EFB copies invalidate only overlapping sources |
| `GEKKOAOT_GX_PGO` | 1 | Build | Use renderer PGO counters when present |
| `GEKKOAOT_MOH_NATIVE_PARTICLES` | 1 | Runtime | Native particle loop for Medal of Honor: Frontline's engine (code hash checked) |
| `GEKKOAOT_CPU_TIMER_GUEST_WORK` | 0 | Runtime | Advance the time base by retired cycles instead of real time (experimental; can desynchronize audio and video) |

Exact defaults and supported combinations are defined in
`tools/gekkoaotctl.cpp`, `src/BuildController.cpp` and the runtime service
that owns each option.

## Compilation and caches

Objects, modules, profiles and toolchain snapshots are stored in the state
directory, normally `.gekkoaot/` in a source tree or the platform cache
directory for installed builds. `GEKKOAOT_STATE_DIR` overrides it.

Later launches reuse matching artifacts. Changes to the compiler, module ABI,
native options or profiles can require a recompilation or relink, and a new
release can select a new module key. A cold compilation of a large game can
take several minutes; games with many REL modules take longer.

LLVM keeps its optimized IR pipeline. Floating point never uses blanket
fast-math.

## Compilation tiers

The launcher's **Full**, **Balanced** and **Fast** modes trade first-compile
time for code quality (see [Launcher](LAUNCHER.md#library-and-compilation)).
Measured cold, click to game start, on the reference machine in Fast mode:
Medal of Honor: Frontline 68 s, Tales of Symphonia 455 s, GoldenEye: Rogue
Agent 445 s. Full mode links the same games with ThinLTO in roughly 10 to 25
minutes. Fast code is about three times slower on the main thread.

## Profile-guided optimization

**Train PGO** builds an instrumented LLVM module, runs the game and writes the
counters on a clean exit. The controller merges them with `llvm-profdata`,
validates them and builds the optimized module. Later launches use a
compatible profile automatically.

Profiles are stored in `<state>/cache/pgo/<game-id>/merged.profdata`.
`GEKKOAOT_PGO_AUTO_USE=0` disables automatic use, and
`GEKKOAOT_PGO_ACCUMULATE=0` starts a new profile instead of adding to the old
one. After a code-generation change, retrain the profile.

Profiles are keyed by the module's IR, so changing the SDK manifest produces
a module without PGO until it is retrained. The renderer has its own GCC PGO on Linux: counters collected by
any game are shared in `<state>/cache/gx-pgo` and used automatically.

## Measuring

1. Record the launcher arguments and environment, the compiled module and the
   graphics settings.
2. Use the same save, scene, inputs and native options for both runs.
3. Separate cold compilation, warm launch, loading and steady gameplay.
4. Report whether a CPU percentage is per core or for the whole machine, and
   measure the process and its threads.
5. Check the frame rate and the picture as well: a slower game can look
   cheaper.
6. Change one option at a time.

The performance HUD shows the frame rate, game speed, process and main-thread
CPU and GPU load directly in the game.

Keep captures and reports under `.gekkoaot/diagnostics/` or another ignored
directory. Do not commit profiling data, raw profiles, saves or extracted game
content.
