# Changelog

All notable public changes to GekkoAOT are documented here.

GekkoAOT is still **pre-alpha**. Compatibility entries describe the furthest
state observed during development testing and do not imply full playability.

The project follows [Semantic Versioning](https://semver.org/) while it is pre-1.0.
Future release entries are maintained by Release Please from Conventional Commits.

## [Unreleased]

## [0.0.3](https://github.com/ant0-blase/GekkoAOT/compare/v0.0.2...v0.0.3) (2026-09-27)

### Fixes

- Fixed Windows portable builds failing before AOT compilation when the native module/cache path exceeded the legacy Win32 path length. Module and LLVM cache directory names now use compact deterministic keys while preserving full cache identity.

### Documentation

- Refreshed the project README and full-resolution runtime/gameplay screenshots.

### Release workflow

- Release publishing is explicit/manual so normal development commits do not create unintended version tags.


## [0.0.2](https://github.com/ant0-blase/GekkoAOT/compare/v0.0.1...v0.0.2) (2026-09-26)

### Native runtime and NativeOS

- Expanded **NativeOS** into a host-side implementation of the common GameCube OS SDK surface while keeping guest `OSThread` and `OSContext` structures ABI-visible and authoritative.
- Added native thread/scheduler services for thread lifecycle, ready/wait queues, yield/reschedule, suspend/resume, sleep/wakeup, priorities and scheduler control.
- Added native synchronization and callback-facing services for mutexes, condition variables, message queues and alarms.
- Added `OSContext` save/load/current-context handling plus complete Gekko floating-point state capture: FPR0-31, paired-single PS0-31 and FPSCR.
- Added native FPU-context helpers (`OSLoadFPUContext`, `OSSaveFPUContext`, `OSFillFPUContext`) and stack-pointer/switch helpers.
- Improved scheduler correctness with READY-priority coherency, deferred rescheduling while IRQ contexts are active and sanity checks before host-side context switches.
- Kept the tiny retail interrupt leaves (`OSDisableInterrupts`, `OSEnableInterrupts`, `OSRestoreInterrupts`) in guest AOT by default so their exact PPC register/CR/`mtmsr` semantics are preserved.
- Added MSR[EE] transition side-exits and interrupt-entry fences so pending external/decrementer interrupts are observed at architectural PPC boundaries.
- Added NativeOS hook-family diagnostics/bisection for scheduler, synchronization and interrupt paths.

### Native VFS, DVD and disc path

- Added/expanded **NativeVFS** backed by encounter/nod rather than a Python or emulator VFS layer.
- Native disc handling supports the pinned nod container families used by GekkoAOT, including ISO/GCM, RVZ, WIA, WBFS and GCZ.
- Added cached extraction of GameCube system files and metadata (`boot.bin`, `bi2.bin`, `fst.bin`, `main.dol`, disc ID and banner metadata).
- NativeVFS performs normalized FST path lookup and reads game files directly from the mounted GameCube partition.
- DI DMA completion is staged asynchronously instead of making DVD reads visible immediately, improving retail drive/IRQ timing behavior.
- Windows helper execution now uses direct Unicode Win32 process launching instead of routing native paths through `cmd.exe`.

### PowerPC AOT, CPU and memory

- Continued the DolRecomp-based **PowerPC -> LLVM AOT** pipeline with GekkoAOT-owned CPU/module ABI and per-game native module generation.
- Added native re-entry/resume boundaries, execution budgets, shared-poll handling and safer dispatcher exits for timing/interrupt-sensitive code.
- Added computed-CTR ABI fencing so dynamic `bctr`/`bctrl` targets receive synchronized PPC architectural state.
- Added context-capture and static direct-HLE state synchronization so host-native SDK calls can return to recompiled PPC code coherently.
- Restored more retail boot behavior around HID/BAT setup, exception vectors, instruction-cache transitions and locked-cache-visible state.
- Kept a 32 MiB host backing allocation for compatibility while exposing the retail 24 MiB MEM1 size to guest-visible software.
- Continued native/FakeVMEM address-space support and fast RAM-first guest-pointer paths.
- Native hardware services cover CP, PE, PI, MI, AI, DI, EXI, SI, VI and locked cache, with dedicated timing/IRQ/register regression tests.

### AuroraGX and GameCube graphics

- Continued the direct **AuroraGX** retail FIFO path instead of routing normal rendering through a Dolphin graphics backend.
- Added full `GXSetZTexture` plumbing through Aurora BP state, including Z-texture bias/control registers and shader-side depth output.
- Z-texture sampling handles the GameCube Z8, Z16 and Z24X8 source formats and the GX depth ADD/REPLACE operations.
- Expanded EFB depth-copy conversion support for `Z4`, `Z8`, `Z16`, `Z24X8`, `Z8M`, `Z8L`, raw `Z16R` and `Z16L`.
- Added/expanded GPU-backed EFB color/depth peek and readback infrastructure needed by titles that read PE results.
- Restored CP/XF Matrix Index A/B synchronization and masked PNMTXIDX/TEXMTXIDX to the GameCube 6-bit hardware field.
- Reworked XF transform/normal storage toward row-addressed Flipper behavior with partial-load handling.
- Added dynamic-mesh coherency by invalidating overlapping indexed-array GPU snapshots on guest cache-publication boundaries.
- Made first-use retail GX pipelines correctness-first so asynchronous host pipeline compilation does not silently lose the first draw.
- Added compatibility work around TEV alpha compare, destination-alpha handling, bind-group lifetimes, vertex-index capacity, zero-stride XF input and EFB-to-texture copies.
- Kept the modern XFB/presentation path, live viewport/aspect handling and standalone Alt+Enter fullscreen behavior.

### DSP, audio and video

- Expanded the native DSP path with GameCube DSP state/core services and host audio output.
- Added native **AX HLE** coverage for GameCube AX command lists, VPB voices, ADPCM/PCM decoding, SRC/resampling and mixer/output processing.
- Added early **JAudio HLE** coverage including AFC/PCM voice sources, resampling, mixing/filter state and common command handling.
- Kept a standalone DSP LLE fallback for unsupported/unknown microcode families without linking the Dolphin Core/System runtime.
- Added an optional native VP6/VP6F movie decode backend through FFmpeg/libavcodec when available.

### Input and host integration

- Added standalone SDL3 keyboard, gamepad and joystick input owned by the native host.
- Added configurable per-port button, stick, C-stick, trigger and deadzone mappings.
- Kept SI-facing controller state in the native runtime rather than depending on an emulator frontend.

### Performance and diagnostics

- Added a bounded native superchain to reduce unnecessary dispatcher transitions while preserving safe timing/event exits.
- Added invariant-TSC/portable realtime timing, lower-cost idle polling, O(1)-style NativeOS READY-thread caching and faster RAM/WGPIPE hot paths.
- Enabled stronger compiler/LTO/IPO optimization paths where supported while keeping debuggable compatibility builds available.
- Continued adaptive PGO/CrossGameDB tooling, GDB/perf helpers and hardware-focused regression tests.

### Packaging and reproducibility

- Release packages use a relocatable runtime/resource layout under `share/gekkoaot` with mutable state kept in the per-user GekkoAOT cache.
- Linux and Windows releases prebuild the DolRecomp/AuroraGX engine so end users do not need a manually patched upstream checkout.
- Improved Windows LLVM/MSVC packaging, DIA SDK rebinding, Ninja-built DolRecomp discovery and portable toolchain staging.
- Improved native Windows Unicode path/argument handling for disc images, SDK analysis and runtime helper processes.
- Linux packaging includes the portable runtime/toolchain path and AppImage generation.
- Pinned DolRecomp, Aurora and encounter/nod revisions remain reproducible and every source patch is checked before application.

### Compatibility

- **Harry Potter and the Sorcerer's Stone** (`GHLE69`): reaches gameplay / early 3D; visible graphics/rendering glitches remain.
- **Harry Potter and the Chamber of Secrets** (`GHSE69`): reaches gameplay / early 3D; visible graphics/rendering glitches remain.
- **Super Mario Sunshine** (`GMSE01`): reaches gameplay / early 3D; graphics/runtime correctness is still incomplete.
- **Mario Kart: Double Dash!!** (`GM4E01`): reaches gameplay / early 3D; some visible graphics/runtime glitches remain.
- **Nintendo Developer Demo**: current tested developer-demo path works without a known blocker.
- **Medal of Honor: Frontline** (`GMFE69`): boots/continues into early execution with multiple compatibility glitches.

No retail title is claimed as fully **Playable** yet.

### Internal

- Streamlined Windows/Linux entry-point and argument handling.
- Reworked CI/release automation for portable Linux/Windows artifacts and automatic Release Please publishing.

## [0.0.1] - 2026-09-19

Initial public **pre-alpha** release.

### Added

- Qt 6 frontend with game selection, launch/stop controls, graphics/input settings, logs and progress reporting.
- Native C++ `gekkoaotctl` build/run controller.
- GameCube disc/container path through encounter/nod, including the current ISO/GCM/RVZ/WIA/WBFS/GCZ family supported by the pinned nod release.
- DolRecomp LLVM AOT pipeline with pinned upstream revision and deterministic GekkoAOT compiler patch stack.
- Per-game native module generation, metadata tooling, module cache and secondary executable/REL build support.
- GekkoAOT-owned CPU state/module ABI, boot/runtime state and native address-space services.
- Native GameCube-facing hardware work for CP, PE, PI, MI, AI, DI, EXI, SI, VI and locked cache.
- Native AX-oriented DSP/audio path, early JAudio HLE work and standalone DSP fallback infrastructure.
- Native AuroraGX host bridge and retail FIFO command path.
- SDK/HLE structural recognition, static intercept manifests and direct native HLE lowering work.
- Adaptive LLVM PGO training/recovery tooling and CrossGameDB profiling support.
- GDB and `perf` compatibility/debug helpers.
- Declarative per-title game-pack format.
- Linux x86_64 and Windows x86_64 CI/release automation.

### Compatibility work included

- Native FakeVMEM aperture work for titles using the GameCube SDK VM allocation range.
- Raw EFB-to-texture copy serialization/tracing around Aurora's render-worker boundary.
- Shared SDL lifetime fix so embedded Aurora teardown does not terminate host-owned SDL subsystems.
- NativeGX teardown ordering and PGO profile recovery after shutdown-only failures.
- Compatibility-oriented AOT execution boundaries, shared polling handling, context-capture ABI handling, host-native lowering and static direct-HLE state synchronization.
- Computed-CTR Native ABI fence so dynamic `bctr`/`bctrl` targets receive coherent architectural PPC register state instead of stale pass-through values.

### Packaging and reproducibility

- Pinned DolRecomp, Aurora and encounter/nod revisions.
- `git apply --check` validation before every local upstream patch is applied.
- Installed runtime resources are relocatable under `share/gekkoaot`.
- Mutable source/build/cache state is kept outside packaged installations.
- `GEKKOAOT_STATE_DIR` can override the default state location.
- `version.txt` is the single release-version source used by CMake and release automation.

### Current compatibility snapshot

- Harry Potter and the Sorcerer's Stone (USA): reaches gameplay/early 3D with visible 3D glitches.
- Harry Potter and the Chamber of Secrets (USA): reaches menu; stuck on loading before gameplay.
- Nintendo Developer Demo: current tested demo path works without a known blocker.
- Super Mario Sunshine (USA): boots.
- Mario Kart: Double Dash!! (USA): reaches menu; stuck on loading before gameplay.
- Medal of Honor: Frontline (USA): boots/executes with several compatibility glitches.

### Known limitations

- Retail compatibility is still narrow and highly title-dependent.
- A boot/menu state does not imply playability.
- Graphics, timing, audio, input, SDK recognition and native service coverage remain incomplete.
- Some optional analysis/optimization paths use Python 3 (`adaptive_pgo.py` and `crossgame_db.py`); the normal native controller/runtime path is C/C++.
- DolRecomp and Aurora are patched at build time from clean pinned checkouts; GekkoAOT does not use runtime source-patch injection.
- Binary release archives are developer/pre-alpha builds and still require host build/runtime dependencies described in `docs/BUILDING.md`.
