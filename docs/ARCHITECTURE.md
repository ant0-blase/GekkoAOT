# Architecture

This document describes how GekkoAOT 0.0.8 turns a GameCube disc into a running
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

GekkoAOT reads disc images (ISO, GCM, RVZ, WIA, WBFS, GCZ) through
[nod](https://github.com/encounter/nod) and caches the system files and
executables per title. The main DOL, its REL modules, secondary DOL/ELF
executables and executable apploader payloads each go through the compiler.

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
run time and exception returns.

REL modules are relocated by the game at run time. GekkoAOT compiles them at
canonical addresses inside a fixed native code window, packs them densely (757
modules for Twilight Princess) and translates addresses between the canonical
and live layouts. Imports from modules that are absent from the disc stay
unresolved, as `OSLink` leaves them. RELs stored inside archive files are
extracted and compiled too.

Compiled objects, modules, SDK manifests and profiles are cached separately and
keyed on compiler sources, ABI, options and profile inputs. A compiler
snapshot isolates running builds from later changes to the toolchain.

## Guest state at native boundaries

Native code cannot enter a host service without first writing back the
architectural PowerPC state the service may observe: GPRs, CR, LR, CTR, XER,
SRR0/SRR1, GQRs, FPRs, paired singles, FPSCR, MSR[EE] and the cycle count.
GekkoAOT treats each such boundary as an explicit ABI transition, and fences
state before computed `bctr`/`bctrl` targets so stale host temporaries cannot
leak across dynamic calls.

Enabling MSR[EE] or returning from an exception ends the current native chain
so a pending interrupt is delivered at the correct instruction.

## SDK recognition and NativeOS

At build time, the SDK resolver identifies SDK functions in the executable by
structure: normalized signatures, complete-body checks, tiny-leaf ABI proofs
and call-graph adjacency. A match must be proven; an uncertain one stays as
compiled guest code. The result is a per-game manifest that the compiler turns
into direct calls.

Recognized services include clock reads, `PADRead`, `DVDFastOpen`, memory and
string functions, cache maintenance, `GXSetArray` and matrix uploads, and the
OS scheduler.

NativeOS implements the GameCube OS in the host: thread creation and
lifecycle, ready and wait queues, priorities, suspend/resume, sleep/wakeup,
mutexes, condition variables, message queues, alarms, `OSContext` save and
load, and lazy FPU ownership. It keeps the guest `OSThread` and `OSContext`
structures, and the SDK's active-thread list, consistent with what the game
expects, because retail code reads them directly.

NativeOS is not all-or-nothing. Tiny leaves such as `OSDisableInterrupts`,
`OSEnableInterrupts` and `OSRestoreInterrupts` stay as compiled guest code,
which already runs natively and keeps their exact semantics.

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
only.

The main module's code is moved onto transparent huge pages on Linux to reduce
instruction-TLB misses. REL modules stay file-backed, so only the code that
runs is loaded.

## Hardware services

The runtime implements the GameCube devices the game talks to: the command
processor (CP), pixel engine (PE), processor interface (PI), memory interface
(MI), audio interface (AI), disc interface (DI), external interface (EXI,
memory cards, RTC, SRAM, IPL ROM), serial interface (SI, controllers) and video
interface (VI). Interrupts are routed through the guest-visible PI state.

When no IPL ROM is supplied, the EXI service provides a replacement boot-ROM
font drawn for GekkoAOT, so games that call `OSInitFont` start normally.

### Time and idle

CPU execution and hardware time are separate. Compiled code runs as fast as
the host allows, while the time base, decrementer, VI, DSP and audio follow
real time.

Many games wait in loops: polling a clock, a hardware register or a memory
flag, or servicing a stream of short timers while every thread is idle.
GekkoAOT recognizes these patterns and sleeps until the next event that could
end the wait, instead of executing the loop. Clock polls are analyzed in a
sandbox to find the exact time the loop would exit.

## Disc access

NativeVFS answers DVD requests from the nod-backed disc or extracted files
through FST lookup. DI transfers complete asynchronously, so the game observes
the same completion and interrupt order as on the console. See
[Native VFS](NATIVE_VFS.md).

## Graphics

```text
game GX code
   ├─► recognized SDK calls ──► typed native commands ─┐
   └─► write-gather pipe ──► CP FIFO ──► CP/XF/BP ─────┤
                                                       ▼
                                                   AuroraGX ──► GPU
```

Recognized array and matrix calls are sent to AuroraGX as typed commands in
the same ordered queue as the FIFO, so the renderer does not decode the
equivalent CP/XF packets again. Custom display lists and unrecognized streams
use the FIFO path.

AuroraGX caches vertex attribute plans, indexed-array windows, bind groups and
texture versions, merges compatible adjacent draws and reuses GPU snapshots.
Guest writes, cache flushes and EFB copies invalidate the affected resources.

The renderer supports EFB color and depth peeks and pokes, EFB-to-texture and
display copies, Z textures (Z8, Z16, Z24X8 with ADD and REPLACE) and the EFB
depth copy formats, TEV and destination alpha, bounding boxes and the 6-bit
matrix index registers.

### Widescreen

The EFB stays 4:3; presentation is widened. Each draw is classified:

- perspective 3D is widened horizontally (hor+);
- orthographic UI is kept at 4:3 and centered;
- full-width fades, letterbox bars and screen fills are stretched to the
  window;
- copies of the screen used for bloom or blur, and region save/restore
  passes, keep their original width;
- small HUD viewports that hold live 3D are moved with the UI;
- frames built on a pre-rendered background are presented in 4:3.

No per-game patch is involved.

### Presentation

After scan-out, optional post-processing applies sharpening, bloom, tone
mapping, local contrast, vignette, grain and color adjustments. The
performance HUD is drawn by the renderer plugin in the same pass and reads
frame times, game and VI rates, game speed, CPU use and GPU load published by
the runtime.

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

An optional FFmpeg-based VP6/VP6F decoder is used when available. H4M/HVQM4
movies are not supported yet.

## Threads

| Thread | Work |
| --- | --- |
| Main | Compiled game code, NativeOS, MMIO and interrupt delivery |
| GX worker | FIFO and native command decoding, AuroraGX submission |
| Render | Command encoding and presentation |
| Audio | DSP, mixing and host audio |
| I/O | Disc reads, shader compilation, caches |

## Files and resources

Installed resources live under `share/gekkoaot` and binaries under `bin`.
Builds, profiles, SDK manifests, extracted executables and saves live in the
state and user directories, normally `.gekkoaot/` in a source tree.
`GEKKOAOT_RESOURCE_DIR`, `GEKKOAOT_STATE_DIR` and `GEKKOAOT_USER_DIR` override
them.

Upstream components are pinned:

| Component | Revision |
| --- | --- |
| DolRecomp | `71ce7f97419b1bb1ba9a9596c41507f6629e0fb0` |
| Aurora | `9c0bf66f1ed3276b60ad1cd746e2fb48818a6298` |
| nod | `v2.0.0-alpha.12` |

GekkoAOT's changes to them are kept as patches and overlay scripts in the
repository and checked before they are applied.
