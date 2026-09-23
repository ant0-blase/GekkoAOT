# Roadmap

This roadmap intentionally separates current v0.0.1 functionality from future design goals.

## Near term: compatibility foundation

- expand title-by-title boot coverage without game-specific hard-coded addresses in generic runtime code;
- stabilize NativeOS scheduling/context boundaries;
- improve GX FIFO/EFB/texture correctness on retail workloads;
- expand DSP/audio timing and ucode-family coverage;
- improve SDK/HLE structural recognition confidence and diagnostics;
- make REL/secondary executable handling routine rather than exceptional;
- add repeatable compatibility smoke tests that do not distribute copyrighted game content.

## Runtime efficiency

- reduce unnecessary host dispatcher transitions;
- convert proven SDK/HLE calls into direct native service calls;
- improve static direct-call lowering and context synchronization;
- use PGO/CrossGameDB data only where it remains architecture-neutral and reproducible;
- preserve correctness before pursuing unsafe host-side shortcuts.

## GekkoTranslate

Long-term research target:

```text
PowerPC DOL / ELF / REL
        |
        v
 disassembly + CFG/data-flow analysis
        |
        v
 whole-program GekkoAOT IR
        |
        +---------------------+
        |                     |
        v                     v
 direct LLVM backend     reconstructed C++
        |                     |
        +----------+----------+
                   |
                   v
          native host machine code
```

The reconstructed C++ path is intended to be an independently generated representation of executable behavior, not recovered proprietary source code.

## GCINE

GCINE is the name reserved for the longer-term native GameCube compatibility environment: OS/service behavior, hardware-visible contracts, memory/timing interfaces and other compatibility services required by recompiled titles.

## Additional targets

After x86_64 compatibility is more mature:

- ARM64 desktop;
- WebAssembly/WebGPU experiments;
- improved keyboard/mouse/controller abstraction;
- universal widescreen/FPS work only where game timing/camera semantics can be changed safely.
