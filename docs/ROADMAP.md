# Roadmap

The current implementation is described in [Architecture](ARCHITECTURE.md). These items remain development goals, not release guarantees.

## Compatibility

- Validate complete gameplay, level changes and save/load behavior across a wider title corpus.
- Improve movie frontends/container handling, JAudio families, EFB effects, shadows and custom texture paths.
- Make widescreen UI placement, clipping and CPU-side frustum/culling corrections more reliable without distorting 2D content.
- Keep regressions reproducible without distributing commercial game or SDK assets.

## More direct native execution

- Extend complete-body SDK recognition for GX, PAD, DVD, OS and audio services.
- Replace additional register/FIFO round trips with ordered native commands when all observable effects can be reproduced.
- Reduce CPUState materialization and generic dispatch at proven boundaries.
- Broaden direct memory lowering while retaining permissions, aliases, cache publication and exception restart semantics.
- Improve scheduling at semantic events rather than adding frequent arbitrary runtime returns.

## Build and runtime efficiency

- Reduce cold AOT build time through shared analysis, safe incremental caches and balanced compiler work.
- Target measured game, renderer, driver and audio hotspots while validating real game speed.
- Reduce redundant GPU synchronization and resource scanning with sound invalidation/lifetime tracking.
- Broaden native DSP handling while retaining unsupported microcode compatibility.

## Longer-term targets

Whole-program DOL/ELF/REL analysis, further host-thread scheduling, reconstructed intermediate representations and additional host architectures remain research areas. A static recompiler plus increasingly native services does not yet provide an automatic complete PC port of every GameCube game.
