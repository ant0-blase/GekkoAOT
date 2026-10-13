# Architecture

This document describes the implementation prepared for GekkoAOT 0.0.7. Future work is listed separately in [Roadmap](ROADMAP.md).

## Compilation and modules

The controller reads GameCube disc containers through encounter/nod, extracts executable metadata, resolves recognized SDK entrypoints and invokes the pinned DolRecomp compiler. Main DOLs use LLVM AOT or Portable C AOT. Secondary DOL/ELF/REL images and executable apploader trailers have a separate discovery and compilation pipeline.

LLVM lowering uses statically known calls, branches and native ABI regions where the control-flow and register-state proofs allow them. Dynamic targets, computed branches, modified code and exception transitions retain dispatch or compatibility exits. Direct host execution still needs coherent GPR, FPR/paired-single, FPSCR, CR, LR, CTR, MSR and interrupt-visible state at those boundaries.

Modules carry capabilities and executable ranges. The loader checks module ABI compatibility and invalidates routing caches when executable identity or instruction-cache publication changes. A direct dispatch cache cannot make an unknown or changed executable safe.

The compiler, generated objects, final modules and SDK manifests are cached independently. Source fingerprints, ABI/capability changes, profiles and effective compilation options participate in cache selection. Compiler snapshots isolate running builds from later mutations of the managed dependency trees.

## Native SDK services

The resolver combines structural signatures, complete-body checks and semantic admission rules. A matching name alone does not justify replacing a function. Proven SDK services can bypass the original PPC body; uncertain variants retain their AOT implementation.

Services include selected OS/clock operations, PAD input, DVD/FST lookup, GX array bindings and matrix uploads. OS functions that capture a caller's context have explicit register-materialization requirements. Arbitrary GX, DVD and PAD functions are not universally replaced.

NativeOS preserves SDK priorities, queues, context switching, mutex/message behavior and lazy FPU ownership. Host synchronization assists the runtime, but the current implementation does not map every guest thread to an independently scheduled host thread. Existing launcher settings are retained; a new launcher configuration enables NativeOS and NativeHW.

## Memory

Guest RAM is backed by host memory. Known MEM1, locked-cache and resident paged-VM accesses can use direct host pointers when address, size, permissions and architectural controls are proven. Invalid translations still take the appropriate guest exception path.

An optional 4 GiB host virtual-address reservation supports a guest-address window. It does not imply that all 4 GiB are committed RAM or that every effective address can bypass MMU checks. Cached and uncached aliases, locked-cache bounds, page reference/change bits, DSI/ISI restart behavior and code-publication epochs remain observable contracts.

CPUState is still part of the module/runtime ABI. Native regions and state-free service boundaries reduce materialization where safe; complete removal of CPUState and general MMIO handling remains unfinished.

## Graphics

NativeGX submits ordered host batches to AuroraGX. Recognized GX array and matrix operations can carry typed native commands so the renderer updates state directly rather than decoding equivalent CP/XF packets again. Transport validation, queue ordering and the existing fallback preserve unsupported/custom streams.

The renderer also uses cached attribute plans, compact indexed-array uploads, typed vertex preparation, adjacent compatible draw coalescing, bind-group caching and tracked resource reuse. Draw merging is bounded and requires matching state; primitive winding, order and unsupported forms retain their original paths.

Texture and vertex resources are invalidated on relevant guest-write/cache-publication and EFB-copy boundaries. Uploaded GPU ranges must remain immutable while queued work can reference them. A cache hit cannot substitute stale guest bytes.

WGPIPE, CP/FIFO, BP/XF state, PE completion, EFB copies and VI/IRQ synchronization still exist for custom display lists and other compatibility paths. Earlier publication of completion-related commands reduces waiting without discarding their interrupt contract.

Widescreen separates perspective correction from orthographic UI treatment and transforms clipping where supported. CPU-side game culling, shadows, post-processing and unusual UI layouts can still need additional work. Automatic universal frustum widening is not complete.

## Disc, input, audio and video

NativeVFS reads host files or nod-backed disc partitions using FST lookup. Extraction preserves nested directories so embedded executable modules can be found. DI completion, cancellation and IRQ visibility remain asynchronous. Details are in [Native VFS](NATIVE_VFS.md).

SDL3 handles host keyboard/gamepad input. SI retains polling, transfer and response/IRQ semantics. EXI services persist memory cards to host files while maintaining the register/protocol contract.

AX and recognized JAudio workloads use native host decoding, mixing and audio output. Unknown DSP workloads retain a standalone DSP compatibility core. Proprietary DSP ROMs are not distributed; see [DSP/audio](../runtime/dsp/native/README.md).

An optional FFmpeg VP6/VP6F decoder service is available when its libraries are found. Decoder availability does not establish complete movie playback for every game's container/frontend. No universal H4M/HVQM4 support is claimed.

## Processes, resources and dependencies

The Qt application constructs the effective launch configuration and starts `gekkoaotctl`. The controller owns extraction, compilation and module selection, then starts the native runner and Aurora bridge. Normal execution does not require Python; optional analysis, patch-overlay and packaging workflows use it.

Installed resources live below `share/gekkoaot`; binaries live in `bin`. Mutable builds, profiles, SDK manifests, extracted executables and saves live in the state/user directories. `GEKKOAOT_RESOURCE_DIR`, `GEKKOAOT_STATE_DIR` and `GEKKOAOT_USER_DIR` provide overrides.

Upstream revisions are pinned in CMake. Reviewable patches and Aurora overlay scripts are applied to managed dependency checkouts. Their versioned names and ABI/cache identifiers are functional inputs and remain in the source distribution. They are distinct from local measurement reports, which are excluded from publication.
