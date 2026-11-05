# Architecture

This document describes how GekkoAOT 0.0.9 turns a GameCube disc into a running
native program. Planned work is listed in the [roadmap](ROADMAP.md).

```text
 disc image ──► nod ──► boot.bin · fst.bin · main.dol · REL/ELF modules
                                   │
                                   ▼
                      DolRecomp + LLVM (ahead of time)
                                   │
                                   ▼
                        per-game native module (.so/.dll)
                                   │
                                   ▼
          ┌──────────────── GekkoAOT runtime ────────────────┐
          │  NativeOS      hardware services      NativeVFS  │
          │  (scheduler,   (CP PE PI MI AI DI     (disc and  │
          │   threads,      EXI SI VI, DSP)        files)    │
          │   sync, time)                                     │
          └───────────────────────┬──────────────────────────┘
                                  ▼
                       NativeGX ──► AuroraGX ──► Vulkan / D3D12 / OpenGL
```

The Qt launcher builds the launch configuration and starts `gekkoaotctl`. The
controller extracts the disc, compiles what is missing, then starts the native
runner with the game module and the AuroraGX renderer plugin. Python is only
used by optional development tools.

## Compilation

The main DOL, its REL modules, secondary DOL/ELF executables and executable
apploader payloads each go through the compiler.

```text
PowerPC code
   │  control-flow and register analysis
   ▼
DolRecomp LLVM backend
   │  static calls and tail calls, native re-entry points,
   │  execution budgets, SDK intercept lowering, PGO
   ▼
LLVM objects ──► per-game module with the GekkoAOT ABI
```

Known calls become ordinary host calls. A dispatcher remains only for targets
that are truly dynamic: function pointers, computed branches, code written at
run time and exception returns. Each function has a direct start entry; the
resume switch over region leaders is taken only when execution re-enters
mid-function.

REL modules are relocated by the game at run time. GekkoAOT compiles them at
canonical addresses inside a fixed native code window, packs them densely (757
modules for Twilight Princess) and translates addresses between the canonical
and live layouts. Pointer provenance is tracked through arithmetic and merges,
so a live address is never rebased twice. Imports from modules absent from the
disc stay unresolved, as `OSLink` leaves them. RELs stored inside archive files
are extracted and compiled too.

When several executables share an address range (a game with alternative main
ELFs), the runtime identifies the image in memory by chunk hashes and their
neighbors. A chunk the game patches at run time, such as the SDK VM library's
exception handlers, is still attributed to its image.

### Tiers

| Tier | IR | Code generation | Link |
| --- | --- | --- | --- |
| Full | -O3 | -O2 | ThinLTO, PGO when a profile exists |
| Balanced | none | -O2 | per object |
| Fast | none | FastISel | per object |

With ThinLTO the linker generates code from bitcode, so per-chunk machine
objects are not emitted. Secondary executables (side ELFs) get the same
hardware lowering, proofs and ThinLTO as the main DOL, with a non-LTO retry if
the link fails.

### Proof-gated code generation

Some PowerPC state is expensive to maintain and rarely observed. The
controller scans every executable section of a game before compiling and
removes that work only when no instruction could observe it:

- **FPSCR status bits** are not computed when the game never reads them
  (`mcrfs`, record forms, `mffs` other than context saves). Scalar
  single-precision operations run about twice as fast in microbenchmarks.
- **Reservations** are not cleared on every store when the game has no
  `lwarx`. Guest stores run about four times as fast in microbenchmarks.

### Floating point

The compiler keeps Gekko floating-point semantics: rounding modes, FPSCR,
NaN propagation and paired-single quantization. Native fast paths are taken
only when a proof or a run-time check shows they give the same result.

Non-IEEE mode (`FPSCR[NI]`, which the SDK enables in `OSInit`) maps to the
host's flush-to-zero for results only. Loads and stores stay exact: `lfs` and
`stfs` copy any 32-bit pattern unchanged, as on hardware, because compilers
move integers through floating-point registers. `stfs` takes a cold exact
path only when the host conversion flushed a value to zero.

### Caches

Compiled objects, modules, SDK manifests and profiles are cached separately and
keyed on compiler version, ABI, options and profile inputs. A compiler
snapshot isolates running builds from later toolchain changes.

## Guest state at native boundaries

Native code cannot enter a host service without first writing back the
architectural PowerPC state the service may observe: GPRs, CR, LR, CTR, XER,
SRR0/SRR1, GQRs, FPRs, paired singles, FPSCR, MSR[EE] and the cycle count.
Each such boundary is an explicit ABI transition. State is fenced before
computed `bctr`/`bctrl` targets so stale host temporaries cannot leak across
dynamic calls. Enabling MSR[EE] or returning from an exception ends the
current native chain, so a pending interrupt is delivered at the correct
instruction.

## SDK recognition and NativeOS

At build time the SDK resolver identifies SDK functions by structure:
normalized signatures, complete-body checks, tiny-leaf ABI proofs and
call-graph adjacency. An uncertain match stays as compiled guest code. The
result is a per-game manifest; each admitted service is bound to a named,
ABI-checked native entry point that the module negotiates at load time.

Recognized services include clock reads, `PADRead`, DVD and FST functions,
memory and string functions, cache maintenance, `GXSetArray`, GX matrix
uploads, several GX pixel-state leaves, memory card, audio and serial
services, and the OS scheduler.

NativeOS implements the GameCube OS in the host: thread creation and
lifecycle, ready and wait queues, priorities, suspend/resume, sleep/wakeup,
mutexes, condition variables, message queues, alarms, `OSContext` save and
load, and lazy FPU ownership. It keeps the guest `OSThread` and `OSContext`
structures and the SDK's active-thread list consistent, because retail code
reads them directly. Tiny leaves such as `OSDisableInterrupts` stay as
compiled guest code, which already runs natively.

## Memory

| Region | Host backing |
| --- | --- |
| MEM1 | 24 MiB reported to the game, 32 MiB allocated |
| Virtual memory (`0x7E000000`) | 32 MiB, flat or paged through the guest MMU |
| Locked cache (`0xE0000000`) | 16 KiB, shared with locked-cache DMA |
| MMIO and the write-gather pipe | Device handlers |

Proven MEM1, locked-cache and resident paged-VM accesses use direct host
pointers. Bounds, permissions, cached/uncached aliases, page reference and
change bits and DSI/ISI restarts remain exact. An optional 4 GiB host address
reservation supports a `base + address` window; it reserves address space
only. The module caches its memory owner, address base and views and calls the
runtime only on a miss.

The main module's code is moved onto transparent huge pages on Linux. REL
modules stay file-backed, so only the code that runs is loaded.

## Hardware services

The runtime implements the devices a game talks to: the command processor
(CP), pixel engine (PE), processor interface (PI), memory interface (MI),
audio interface (AI), disc interface (DI), external interface (EXI: memory
cards, RTC, SRAM, IPL ROM), serial interface (SI: controllers) and video
interface (VI). Interrupts are routed through the guest-visible PI state.

When no IPL ROM is supplied, EXI provides a replacement boot-ROM font drawn
for GekkoAOT, so games that call `OSInitFont` start normally.

### Native device services

Recognized SDK calls skip register-level emulation entirely:

| Domain | Native path |
| --- | --- |
| Locked cache | Host pointers and synchronous SDK copies |
| MI | Direct RAM lowering; SDK handler registration and interrupt masks kept |
| EXI | Persistent SRAM, RTC with persisted bias, direct ROM and font access, UART to the host console |
| Memory card | Shared RAW image, cached directory and block chains, a persistence worker with atomic replacement |
| AI | Host PCM playback on the bound RAM span, integrated with AX and JAudio |
| SI | SDL3 snapshots written to `PADStatus`; SDK sampling profiles set the host capture rate |
| DI | SDK read queue to a persistent I/O worker, then RAM publication and callbacks |

SDK callbacks still run the game's compiled code. Queue state, structures,
errors and completion order are preserved. Unrecognized code keeps the
register adapters.

`gekkoaot-native-run --native-only` (or `GEKKOAOT_NATIVE_ONLY=1`) rejects any
untranslated LC, MI, EXI, AI, SI or DI access and prints the subsystem, address,
PC and LR. It is a diagnostic for coverage work; GX, VI and DSP are outside
its scope.

### Time and idle

CPU execution and hardware time are separate. Compiled code runs as fast as
the host allows, while the time base, decrementer, VI, DSP and audio follow
real time.

Many games wait in loops: polling a clock, a hardware register or a memory
flag, or servicing a stream of short timers while every thread is idle.
GekkoAOT recognizes these patterns and sleeps until the next event that could
end the wait. Clock polls are analyzed in a sandbox to find the exact time the
loop would exit.

## Disc access

`gekkoaot-native-disc` opens ISO, GCM, RVZ, WIA, WBFS and GCZ images through
[nod](https://github.com/encounter/nod). The controller caches system files,
executables and metadata under `<state>/cache/discs/<game>/`; the FST tree is
preserved, so REL modules in nested directories and inside archives are found.

NativeVFS resolves paths through the FST against host files or the mounted
partition. Reads use host I/O without simulated seek or rotation delays.
Requests are asynchronous: bytes are published before completion is reported,
and a cancelled request cannot overwrite a reused buffer.
`GEKKOAOT_VFS_GC_TIMING=1` adds console disc timing when NativeHW DI is off.

## Graphics

```text
game GX code
   ├─► recognized SDK calls ──► typed native commands ─┐
   └─► write-gather pipe ──► CP FIFO ──► CP/XF/BP ─────┤
                                                       ▼
                                          NativeGX IR ──► AuroraGX ──► GPU
```

Recognized array, matrix and pixel-state calls are sent as typed commands in
the same ordered queue as the FIFO. FIFO register writes are lowered to the
same IR: vertex formats, projection, viewport, lighting and texture-generator
blocks and every BP domain. State-only display lists are preflighted, cached
by exact contents and replayed as owned plans; primitive display lists get
typed framing plans. Unsupported shapes keep the original decoder.

AuroraGX caches vertex attribute plans, indexed-array windows, bind groups and
texture versions, merges compatible adjacent draws into precomputed runs and
reuses GPU snapshots. Texture memory is write-protected per page (Linux), so
a guest write invalidates only the textures it touches. EFB copies invalidate
only overlapping sources.

The renderer supports EFB color and depth peeks and pokes, EFB-to-texture and
display copies, Z textures (Z8, Z16, Z24X8 with ADD and REPLACE) and the EFB
depth copy formats, TEV and destination alpha, logic operations, bounding
boxes and the 6-bit matrix index registers.

Widescreen, the Enhanced Renderer, texture reconstruction and the frame-rate
unlock are described in [Graphics](GRAPHICS.md).

### Presentation

After scan-out, optional post-processing applies sharpening, bloom, tone
mapping, local contrast, vignette, grain and color adjustments. The
performance HUD is drawn in the same pass. With VSync on, presentation uses
Mailbox where available, so the newest frame is shown at the next refresh
without tearing.

## Audio

```text
game audio code
   ├─► AX       VPB voices, ADPCM/PCM, resampling, mixer and sends
   ├─► JAudio   AFC/PCM voices, filters, command lists
   └─► DSP LLE  unknown microcode
```

AX and recognized JAudio workloads are decoded and mixed natively. Unknown
microcode runs on a standalone DSP core. DVD audio streaming (DTK ADPCM) is
mixed into the host output. Proprietary DSP ROMs are not distributed; see
[DSP and audio](../runtime/dsp/native/README.md).

Movies play through the game's own decoders, compiled like the rest of the
game. EA's MPEG-2 IDCT has a native replacement in the two games that share its
build (Medal of Honor: Frontline, 007: Agent Under Fire).

## Threads

| Thread | Work |
| --- | --- |
| Main | Compiled game code, NativeOS, MMIO and interrupt delivery |
| GX worker | FIFO and native command decoding, AuroraGX submission |
| Render | Command encoding, Enhanced passes and presentation |
| Compute | Neural texture reconstruction |
| Audio | DSP, mixing and host audio |
| I/O | Disc reads, shader compilation, caches |

## Files and resources

Installed resources live under `share/gekkoaot` and binaries under `bin`.
Builds, profiles, SDK manifests, extracted executables, texture caches and
saves live in the state and user directories, normally `.gekkoaot/` in a
source tree. `GEKKOAOT_RESOURCE_DIR`, `GEKKOAOT_STATE_DIR` and
`GEKKOAOT_USER_DIR` override them. Extracted game files and saves are never
distributed.

Upstream components are pinned:

| Component | Revision |
| --- | --- |
| DolRecomp | `71ce7f97419b1bb1ba9a9596c41507f6629e0fb0` |
| Aurora | `9c0bf66f1ed3276b60ad1cd746e2fb48818a6298` |
| nod | `v2.0.0-alpha.12` |

GekkoAOT's changes to them are kept as patches and overlay scripts in the
repository and checked before they are applied.
