# CrossGameDB

`community-v1.json` stores normalized PPC signatures and aggregate hotness evidence,
not game code, executable replacements or copied profile counters.

## Compiler integration

For ordinary LLVM builds, the controller automatically matches the current DOL
against community and local databases. Only identical **normalized signatures**
are accepted; instruction immediates, registers and relocation addresses are
normalized, so this does not assert byte-identical functions. Fuzzy matches are
excluded by the compiler consumer. Evidence containing the current disc ID is
also excluded, to avoid feeding a game's own observations back as cross-title PGO.

Selected translated functions receive LLVM's `hot` attribute before the standard
O3 pipeline. This may guide inlining and code layout. Guest instructions,
state residency, RAM behavior, FP semantics and compilation chunk size remain
unchanged. There is no lookup or Python execution in the running game.

A game's own PGO is authoritative: generation and profile-use builds ignore
cross-title hints. This keeps a database update from changing the CFG between
collection and use. Unmatched functions use standard O3. Neither coverage of
every function nor a speedup for every game is guaranteed.

Decisions are cached in `.gekkoaot/cache/crossgame-priors/<disc-id>/`, keyed by
DOL/database/tool content and partition size; nonempty manifests also enter
LLVM-object and module cache keys. Disable with `GEKKOAOT_CROSSGAME_ENABLE=0`.
Existing binaries require recompilation to gain new compiler hints.

PGO collections update the local database and merge it into the community file.
Keep entries architecture-neutral; do not add copyrighted game/SDK payloads.

Validation:

```sh
python3 tests/hardware/crossgame_probe.py
python3 tests/hardware/pgo_probe.py
```

Matching tests check relocation, own-title exclusion, unknown code and fuzzy
exclusion. The LLVM fixture checks that O3 consumes the hint, executes identical
results, and emits unchanged IR in generation/profile-use mode.
