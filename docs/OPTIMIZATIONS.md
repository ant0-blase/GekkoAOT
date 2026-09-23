# Optimizations

GekkoAOT v0.0.1 uses DolRecomp's LLVM AOT backend and keeps generated per-game objects, modules and profiles below the GekkoAOT **state directory**.

For source-tree development the state directory defaults to:

```text
./.gekkoaot/
```

For installed builds it defaults to the platform user cache directory documented in [BUILDING.md](BUILDING.md). `GEKKOAOT_STATE_DIR` overrides the location in both cases.

The native module build uses strict floating-point settings (`-ffp-contract=off`, `-fno-fast-math`) where supported so PowerPC floating-point behavior is not traded away for unsafe host optimizations.

## Profile-guided optimization

PGO is exposed by the controller/GUI for the LLVM AOT backend. **Train PGO** builds an instrumented LLVM module, launches representative execution and uses a graceful stop path so compiler-rt can flush `.profraw` counters. The controller then merges and validates the profile with `llvm-profdata`, builds the profile-use module and stores the cumulative profile below:

```text
<state>/cache/pgo/<game-id>/merged.profdata
```

Later **Play** launches automatically reuse that merged profile unless `GEKKOAOT_PGO_AUTO_USE=0` is set. Repeated training sessions are cumulative by default; the previous profile receives weight 1 and the new session weight 4. `GEKKOAOT_PGO_ACCUMULATE=0` starts a fresh profile.

`tools/adaptive_pgo.py` and `tools/crossgame_db.py` are optional analysis/orchestration helpers used by current profiling/database workflows. They are not a requirement for the normal native host runtime itself.

PGO is a compile-time optimization and does not require hard-coded game addresses. Each title contributes its own execution profile to the same DolRecomp LLVM pipeline. The PGO-use build also requests ThinLTO when Clang and emitted LLVM bitcode are available.

## AOT execution quantum

The native AOT scheduler uses a larger compile-time execution quantum than the historical 256-cycle RecompCore default. Normal builds default to 2048 guest cycles / 8192 guard steps; PGO builds default to 4096 / 16384.

The values can be tuned with:

- `GEKKOAOT_AOT_GUARD_CYCLES`
- `GEKKOAOT_AOT_GUARD_STEPS`
- `GEKKOAOT_PGO_GUARD_CYCLES`
- `GEKKOAOT_PGO_GUARD_STEPS`

Timing state is still settled at real runtime boundaries; changing the quantum only reduces unnecessary returns to the host dispatcher.
