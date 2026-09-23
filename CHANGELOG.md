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
