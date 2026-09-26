- v151: guest-AOT MSR[EE] transition fence for NativeOS-off boot parity. DolRecomp now side-exits when an `mtmsr` changes EE in either direction (previously only 0->1), so the standalone dispatcher observes the exact PPC `OSDisableInterrupts`/`OSEnableInterrupts`/`OSRestoreInterrupts` state transitions even with `GEKKOAOT_NATIVE_OS=0`. This keeps the original retail interrupt leaves recompiling as PPC->host code, while restoring the dispatch/event boundary that direct NativeOS interception previously supplied implicitly. LLVM/module cache identity bumped (`msree151`) so stale code generated with the one-way fence is never reused.
- v150: correctness-first NativeOS interrupt-leaf policy. `OSDisableInterrupts`, `OSEnableInterrupts`, and `OSRestoreInterrupts` now stay guest-AOT by default instead of being replaced by direct NativeOS host leaves. These are only 5/5/9 PPC instructions, so DolRecomp already executes them as native host code while preserving the retail scratch-register, CR, `mtmsr`, instruction-boundary, and restartable-atomic-sequence semantics exactly. Higher-level NativeOS scheduler/context/synchronization hooks remain native. `GEKKOAOT_NATIVE_OS_INTERRUPT_LEAVES=1` keeps the host implementation available for conformance work, and the explicit interrupt bisect modes still force it. SDK resolver ABI bumped to 29 and the interrupt-leaf policy is part of the manifest cache key.
- v149: NativeOS interrupt-entry fence for statically lowered `OSDisableInterrupts`, `OSEnableInterrupts`, and `OSRestoreInterrupts`. A pending external or decrementer interrupt is now taken before the host leaf mutates MSR[EE], with SRR0 left at the intercepted SDK entry so RFI retries the call. This restores the asynchronous exception boundary that direct-HLE otherwise skipped. Runtime-only change; no SDK resolver/module ABI bump.
- **v146 NativeOS hook-family bisect:** adds `GEKKOAOT_NATIVE_OS_BISECT=interrupts|scheduler|sync|all` as a cumulative, cache-keyed diagnostic selector. This isolates glitches that occur only with NativeOS without changing Aurora/GX; resolver ABI is bumped to 26 so every mode gets its own manifest/module.
- v145: NativeOS context-capture ABI now materializes the complete Gekko FP/paired-single register file (FPR0-31, PS0-31 and FPSCR) for every scheduler/IRQ-capable NativeOS boundary, preventing stale volatile FP state from being snapshotted into OSContext host shadows; SDK resolver ABI bumped to 25 and module compatibility IDs bumped so old partial-FP AOT modules cannot be reused.
- v144 Native SDK graphics-context parity: restore NativeOS as the default per-game native SDK path; isolate complete FPR/FPSCR/paired-single state across OSSetCurrentContext interrupt/callback boundaries; add full-FP direct-token ABI materialization for OSContext/FPU hooks; add native OSLoad/Save/FillFPUContext plus stack-pointer helpers; structurally recover FPU wrappers inside proven OSContext clusters; and add IRQ-style FPU-context regression coverage.
- v143 universal Dolphin/IPL parity: execute Nintendo OS scheduler/context code in guest AOT by default, restore BS2/IPL HID/BAT/exception-vector/I-cache semantics, stage DI DMA until realistic GameCube drive completion instead of publishing reads immediately, keep Dolphin-compatible 32 MiB host backing while reporting retail 24 MiB MEM1, and remodel Aurora XF transform/normal memory as row-addressed Flipper banks with partial-load support.
- v142 GX per-vertex matrix-index correctness: mask PNMTXIDX and TEXMTXIDX to the Flipper 6-bit hardware field before Aurora shader lookup, matching Dolphin vertex-loader semantics and preventing recycled/skinned vertex bytes from selecting bogus transform matrices.
- v141 GX dynamic-mesh coherency: restore CP/XF Matrix Index A/B synchronization, invalidate overlapping indexed-array GPU snapshots on guest dcbst/dcbf/dcbi publication boundaries, and make first-use retail GX pipelines blocking so animated/skinned meshes cannot reuse stale vertices or lose a draw while the host pipeline compiles. No per-draw array hashing is enabled.
- v140 HPSS determinism fix: keep the v138 READY-priority coherency fix, remove the broad v137 spin-yield/forced runqueue reconciliation and the destructive v139 stale-context drop. Widescreen/Alt+Enter and all v136b hot-path optimizations remain enabled.
- v139 NativeOS context-switch guard: validate the selected READY OSContext before loading it (READY/suspend/priority, SRR0 alignment and no self-execution inside OSThread, saved r1 inside advertised stack); stale host-cache entries are dropped instead of loading corrupted CPU state. Adds an exact regression test for the HPSS thread+0x1b4 crash signature.
- v138 NativeOS ready-priority coherency: OSSetThreadPriority now updates the O(1) host READY priority mirror atomically with the guest OSThread fields, restoring immediate preemption after READY-thread priority changes (the HPSS boot regression introduced by v136) while retaining the v136 scheduler hot-path optimizations.
- v137 compatibility/presentation fix: interpolation now uses the same live 4:3/window-aspect viewport as direct VI scanout, standalone Aurora windows toggle fullscreen with Alt+Enter, and an adaptive AOT spin-yield guard temporarily shortens the native superchain plus reconciles NativeOS only for tight no-MMIO wait loops (restoring HPSS-style boot waits without giving up v136b normal-path throughput).
- v136 universal runtime hotpath: bounded-safe native superchain, invariant-TSC realtime clock with portable fallback, zero-hardware-cycle fast return, NativeOS O(1) best-ready cache with amortized guest audits, RAM-first guest pointers, single-load PI IRQ probing, true NativeOS idle parking, and stronger GCC/Clang/LTO optimization flags.
- v136b latency/throughput rebalance: keeps all v136 runtime hotpath wins, disables NativeOS host sleeping by default (still opt-in through `GEKKOAOT_NATIVE_IDLE_SLEEP_US`), raises the GUI-safe native chain horizon to 16384 cycles, and moves the fixed write-gather-pipe path ahead of generic VMEM/RAM/LC probes in `ExternalWrite`. This targets the small FPS regression seen after v136 while preserving the large CPU reduction.
- v135 runtime performance: coalesce host monotonic timing/presentation samples, cap native-idle clock polling at 16 kHz instead of one sample per 4096-cycle idle quantum, and cache NativeOS ready-thread priorities while validating the selected candidate against guest RAM.
- v133 targeted color diagnostic: keep Aurora 9c0bf66f and every modern GX feature; default only GXSetDstAlpha from dual-source blending to Aurora's existing alpha-prepass fallback, with GEKKOAOT_AURORA_DUAL_SOURCE=1 for an exact A/B test.
- v131c Aurora 9c0bf66f hotfix: accept the pre-3840 recording.hpp resolve_pass_into default GX_TF_RGBA8 declaration so modern XFB zero-copy injection configures on the bisect pin.
- v131 near-head renderer bisect: restore the modern Aurora/GX bridge and v128 features, pin Aurora to 9c0bf66f (one commit before 3840bf9 EFB-alpha handling), and keep v129 computed-CTR ABI fencing; this narrows the color regression without reverting the modern 3D renderer.
# Changelog

## [Unreleased]

- v130b: keep the pre-lighting Aurora 7f2801cd rollback coherent by disabling the incompatible Z-texture patch instead of partially applying it. This avoids mismatched shader/uniform layouts while isolating the 2D/3D/color regression.

- v130 renderer rollback: restored the pre-lighting v115 Aurora/GX path (Aurora 7f2801cd) while retaining the v129 AOT/runtime fixes; removes the post-v115 renderer/API changes from the active path to recover 2D/3D/color correctness.
# Changelog

All notable public changes to GekkoAOT are documented here.

The project follows [Semantic Versioning](https://semver.org/) while it is pre-1.0.
Future entries are maintained by Release Please from Conventional Commits.

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
