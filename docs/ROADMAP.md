# Roadmap

What GekkoAOT does today is described in [Architecture](ARCHITECTURE.md).
The items below are goals, not release commitments.

## Compatibility

- Validate complete playthroughs, level changes and save/load across more
  games, not only the opening scenes.
- Fix the open crashes: Twilight Princess after the opening logos, Super Monkey
  Ball 2 after the title screen, Spyro: A Hero's Tail in its first cutscene and
  Wind Waker with NativeOS enabled.
- Movies: VP6 artifacts and decoder performance.
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

- Start games within a minute on a cold cache: compile the entry path first,
  run, and replace it with the full build in the background.
- Shorten cold compilation through shared analysis, safe incremental caches and
  better balanced compiler work, especially for games with many REL modules.
- Smaller REL modules: native entry points only where they can be reached.
- Profile real game, renderer, driver and audio hotspots and validate the game
  speed after each change.
- Less GPU synchronization and resource scanning, with sound invalidation and
  lifetime tracking.
- Broaden native DSP coverage while keeping the microcode fallback.

## Frame rate

- Make the geometry-replay unlock the default once it holds 60 fps and more
  without visible pairing errors across the test set; replay CPU-built vertex
  animation too.

## Graphics

- Temporal anti-aliasing.
- Water detection and planar reflections where the scene proves a plane.
- Silhouette smoothing across whole models, with an edge registry shared
  between draws.
- Mesh and texture replacement packs keyed by content hash.

## Longer term

- Whole-program analysis of DOL, ELF and REL modules as one program.
- A canonical GekkoAOT IR feeding both a direct LLVM backend and
  reconstructed portable C++ (GekkoTranslate).
- GCINE, the native GameCube environment below the recompiled game logic.
- More host threads where the guest semantics allow it.
- ARM64 hosts, then WebAssembly/WebGPU.

A static recompiler with a growing set of native services is not yet an
automatic, complete PC port of every GameCube game.
