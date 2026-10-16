# Roadmap

What GekkoAOT does today is described in [Architecture](ARCHITECTURE.md).
The items below are goals, not release commitments.

## Compatibility

- Validate complete playthroughs, level changes and save/load across more
  games, not only the opening scenes.
- Fix the open crashes: Twilight Princess after the opening logos, Super Monkey
  Ball 2 after the title screen, Spyro: A Hero's Tail in its first cutscene and
  Wind Waker with NativeOS enabled.
- Movies: complete container and frontend handling, and add H4M/HVQM4.
- More JAudio variants, EFB effects, shadows and custom texture paths.
- Widescreen: CPU-side culling and frustum widening, so objects at the edges
  of a 16:9 view are not culled by the game.

## More direct native execution

- Recognize more complete SDK functions for GX, PAD, DVD, OS and audio.
- Replace more register and FIFO round trips with ordered native commands
  where every observable effect can be reproduced.
- Materialize less CPU state and dispatch less at proven boundaries.
- Extend direct memory access while keeping permissions, aliases, cache
  publication and exception restarts exact.
- Schedule at semantic events instead of frequent arbitrary returns to the
  runtime.

## Build time and efficiency

- Shorten cold compilation through shared analysis, safe incremental caches and
  better balanced compiler work, especially for games with many REL modules.
- Smaller REL modules: native entry points only where they can be reached.
- Profile real game, renderer, driver and audio hotspots and validate the game
  speed after each change.
- Less GPU synchronization and resource scanning, with sound invalidation and
  lifetime tracking.
- Broaden native DSP coverage while keeping the microcode fallback.

## Frame rate

- Decouple the presentation rate from the game's own rate without changing
  game speed, animation or input latency. An experimental geometry-replay
  mode exists and is disabled by default.

## Longer term

- Whole-program analysis of DOL, ELF and REL modules as one program.
- A canonical GekkoAOT IR feeding both a direct LLVM backend and
  reconstructed portable C++ (GekkoTranslate).
- GCINE, the native GameCube environment below the recompiled game logic.
- More host threads where the guest semantics allow it.
- ARM64 hosts, then WebAssembly/WebGPU.

A static recompiler with a growing set of native services is not yet an
automatic, complete PC port of every GameCube game.
