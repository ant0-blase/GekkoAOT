# GekkoAOT architecture

This document describes the **current v0.0.1 implementation**, not the long-term roadmap.

## Runtime pipeline

```text
Disc image
  |
  v
encounter/nod
  |
  v
main.dol + executable metadata
  |
  v
pinned DolRecomp checkout
  + GekkoAOT compiler patch series
  |
  v
LLVM AOT / generated module
  |
  v
GekkoAOT native host
  +-- CPU/module ABI
  +-- SDK/HLE resolver
  +-- NativeOS
  +-- PI / MI / CP / PE / AI / DI / EXI / SI / VI / LC
  +-- DSP/audio services
  `-- GX host bridge -> patched pinned Aurora
```

## Ownership boundaries

### GekkoAOT

GekkoAOT owns:

- the host-facing CPU state and module ABI;
- boot/runtime memory and address-space behavior;
- GameCube-facing OS/HW/DSP service implementations;
- SDK/HLE structural recognition and interception policy;
- disc/build/cache orchestration;
- the renderer plugin/host bridge contract;
- per-game module generation and loading.

### DolRecomp

DolRecomp is the current PowerPC analysis/AOT compiler foundation. GekkoAOT pins a specific upstream revision and applies the auditable patch series under `patches/dolrecomp/` before building it.

The patch series adds or adjusts GekkoAOT-specific AOT behavior such as native re-entry, static intercept lowering, execution budgets, PGO integration, structural translation and compatibility boundaries.

### Aurora

Aurora is the GX rendering backend. GekkoAOT pins a specific upstream revision and applies the integration patches under `patches/aurora/` before building the bridge/backend used by the runtime.

GekkoAOT owns guest-memory/FIFO framing and hands renderer work to Aurora rather than embedding a full emulator chassis.

### encounter/nod

nod provides GameCube disc/container access. The v0.0.1 native host pins `v2.0.0-alpha.12` and uses the prebuilt/static package on supported desktop platforms when available.

## Patch policy

Upstream source caches are tool-owned and reproducible:

```text
checkout pinned revision
        |
        v
git reset --hard <pin>
        |
        v
git clean -ffd
        |
        v
git apply --check <patch>
        |
        v
git apply <patch>
```

There is no hidden requirement for a manually modified DolRecomp/Aurora checkout.

"No runtime patch injection" means GekkoAOT does not modify C/C++ source while a game is running. It **does** intentionally apply versioned source patches during the toolchain build stage.

## Python usage

The core controller, module tools and runtime are C/C++.

Python 3 is currently used by optional tooling:

- `tools/adaptive_pgo.py`;
- `tools/crossgame_db.py`.

These scripts participate in profiling/database workflows; documentation should not claim that the repository contains no Python.

## Installed resource model

Development checkout:

```text
<repo>/.gekkoaot/
```

Installed package:

```text
<prefix>/bin/...                 executable files
<prefix>/share/gekkoaot/...     read-only runtime resources
<user state>/...                mutable source/build/cache/profile data
```

Defaults:

```text
Linux:   ${XDG_CACHE_HOME:-~/.cache}/gekkoaot
Windows: %LOCALAPPDATA%\GekkoAOT
```

`GEKKOAOT_STATE_DIR` overrides the mutable state root.

This separation keeps GitHub release archives relocatable and prevents the runtime from trying to write into a system installation directory.

## GUI process model

The Qt frontend starts `gekkoaotctl` with `QProcess`. Helper executables are resolved next to the GUI/controller first. The controller then discovers its installed resource root relative to the executable.

On Unix, the controller process is placed in its own process group so **Stop** can terminate the active build/game process tree cleanly.

## Long-term direction

Higher-level PowerPC reconstruction, whole-program DOL+REL analysis and the planned GekkoTranslate/GCINE architecture are roadmap work rather than v0.0.1 guarantees.
See [ROADMAP.md](ROADMAP.md).
