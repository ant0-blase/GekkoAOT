# Changelog

All notable public changes to GekkoAOT are documented here.

GekkoAOT is still **pre-alpha**. Compatibility entries describe the furthest
state observed during development testing and do not imply full playability.

The project follows [Semantic Versioning](https://semver.org/) while it is pre-1.0.
Releases are prepared explicitly and published through the manual Release workflow.

## [Unreleased]

## [0.0.9] (2026-11-04)

### Highlights

- The Enhanced Renderer: ambient occlusion, contact shadows, per-fragment GX
  lighting, sun shadow cascades, indirect light, reflections, particle lights,
  depth of field and generated materials, added to any game whose 3D scene
  can be captured.
- AI texture reconstruction with Real-ESRGAN on the GPU or ESPCN on the CPU.
- Remastered and Remake graphics presets.
- Three compilation modes: Full, Balanced and Fast.
- Lower CPU use across the GX and main threads.
- GoldenEye: Rogue Agent no longer crashes at random, and Medal of Honor:
  Rising Sun gets past its first loading screen.

### Graphics

- Scene capture: opaque, depth-tested 3D draws write view normals, depth with
  a camera identity (up to 15 projections per frame) and their color
  contribution to extra targets. The game's EFB, copies and CPU reads are
  unchanged.
- Ambient occlusion, contact shadows and bilateral filtering that never mix
  pixels of different cameras, so a weapon model does not shade the world.
- Per-fragment evaluation of the game's own GX lights on draws with normals.
- Environment light from the game's directional light, or a configured sun on
  baked scenes, with the world's vertical axis locked from scene evidence.
- Sun cascades: every eligible draw is replayed into a shadow atlas with its
  real geometry, skinning and alpha test, filtered with 4×4 PCF. The game's
  own blob shadows fade while cascades are active.
- One-bounce indirect light and screen-space reflections, the latter only on
  surfaces with GX reflection evidence.
- Particle lights selected on the GPU from additive and alpha-over particles,
  with radiance estimated from the TEV program.
- Depth of field, generated normal, roughness and height maps, parallax
  occlusion, surface micro-detail and specular highlights.
- Texture reconstruction at 2× or 4×: Real-ESRGAN compact on a dedicated GPU
  compute queue, ESPCN on the CPU, and rule-based paths for fonts, thin strips,
  pixel art and limited palettes. Results are cached on disk.
- Presets: Remastered — Natural, Vivid, Cinematic and Performance; Remake —
  AI textures (Remastered Cinematic with 4× Real-ESRGAN, surface detail,
  specular and FXAA).
- FXAA, alone or with 4× MSAA.
- Live field-of-view override, with CPU culling widened through proven native
  camera providers.
- Shaders: known pipelines compile at startup, unknown ones on worker
  threads; a draw is never skipped while its pipeline compiles.
- VSync uses Mailbox when available: no tearing and one to two refreshes less
  latency than FIFO.
- Experimental frame-rate unlock: the newest frame's geometry is replayed at
  the display rate with interpolated (Smooth) or predicted (Instant)
  transforms. It falls back to direct presentation when a replay no longer
  fits in a refresh. Off by default.

### Compilation

- Full, Balanced and Fast modes in the launcher. Fast starts Medal of Honor:
  Frontline in 68 s from a cold cache on an 8-core laptop.
- FPSCR status bits are not computed and reservations are not cleared on
  stores when a per-game scan proves no instruction observes them
  (microbenchmarks: 1.96× and 3.97×).
- Functions get a direct start entry; the resume switch is used only to
  re-enter mid-function. Hot functions used to run about 10 KB of register
  moves on every call.
- SDK services are bound to named, ABI-checked native entry points.
- Secondary executables get hardware lowering and ThinLTO. With ThinLTO,
  per-chunk machine code is no longer generated twice.
- REL pointers keep their live-address provenance through arithmetic.
- Job hashing is linear in the number of chunks; Super Mario Sunshine's
  analysis went from 96 s to 39 s.
- REL branches that the synthetic module layout puts out of REL24/REL14
  range keep their real target instead of failing the build (titles with
  45-110 MiB of REL modules).
- Several games can compile at the same time: concurrent builds no longer
  reset each other's source checkout, and the graphics bridge is no longer
  relinked by every controller start.
- The controller and the side-module builder start their tools directly
  instead of through a shell.
- Games whose entry point is a plain function start with the IPL's stack and
  small-data registers.

### Performance

- Harry Potter and the Chamber of Secrets, Burrow cutscene: main, GX and
  render threads together from 118 % to 54 % of one core.
- Star Fox Adventures, ThornTail Hollow at 1920×1200 with every effect: from
  4 to 37 fps. Logic-op shadow masks no longer split the render pass per
  triangle.
- Precomputed draw runs per display-list plan, stable texture IDs per
  source, and per-page write tracking (Linux) so a guest write invalidates
  only the textures it touches.
- The module caches its memory owner and views; the runtime is called only
  on a miss (was about 11 % of process CPU in Chamber of Secrets).
- Cache-flush loops no longer take a queue lock per 32-byte line.
- Renderer PGO with GCC on Linux, trained by any game and shared.
- EA's MPEG-2 IDCT runs natively in Medal of Honor: Frontline and 007: Agent
  Under Fire.
- Medal of Honor: Frontline's particle loop runs natively (code hash
  checked; `GEKKOAOT_MOH_NATIVE_PARTICLES=0` disables it).

### Compatibility

- Non-IEEE floating-point mode now flushes results only. Loads and stores are
  exact, so integers and pointers copied through floating-point registers are
  no longer zeroed. This fixes the random crashes of GoldenEye: Rogue Agent
  (Havok contact IDs) and affects every game whose SDK enables the mode.
- Medal of Honor: Rising Sun stopped after its first loading screen: the SDK
  VM library patches its exception handlers, and the runtime could no longer
  tell which of the game's two main executables was in memory.
- GoldenEye: Rogue Agent: effects missing in the top-left quarter, where the
  game saves, clears and restores its bloom work area.
- Tales of Symphonia: a dark or light band at the depth of the far blur plane
  with sun and sky light.
- Star Fox Adventures: black screen when more than 32 disc reads were queued
  at once, and a callback stack-alignment failure.
- Rayman 3: sun shadows invisible behind a full-screen glow pass.
- Killer7 and Shrek 2: textures that were missing or wrong now display.
- Mario Kart: Double Dash!!: widescreen presentation fixed.
- Real-ESRGAN jobs no longer fail on the GPU and fall back to the CPU.
- Movie frames in GoldenEye: Rogue Agent no longer get horizontal lines from
  texture reconstruction.

### Launcher

- Compilation mode setting.
- Graphics dialog with every Enhanced Renderer, material and texture option,
  applied live; settings migrate once to the new defaults.
- Cheat code and texture pack managers for desktop and TV mode.
- Memory card manager shows save icons.

### Diagnostics

- `GEKKOAOT_ENHANCED_DIAGNOSTICS` and `GEKKOAOT_ENHANCED_VIEW` debug views
  (depth, coverage, normals, cameras, environment, cascades, indirect,
  reflections).
- `GEKKOAOT_GX_PHASE_PROFILE`, `GEKKOAOT_NATIVE_SAMPLE` and
  `GEKKOAOT_HUGE_TEXT_PERF_MAP` for profiling.
- `GEKKOAOT_CRASH_DUMP` writes guest memory when a run ends.
- The compatibility laboratory (`tools/compat_lab.py`) compiles and observes
  a whole disc collection in private headless sessions.

### Documentation

- New pages: [Graphics](docs/GRAPHICS.md), [Launcher](docs/LAUNCHER.md) and
  [Testing](docs/TESTING.md). Disc access, native services and the GX IR are
  now part of [Architecture](docs/ARCHITECTURE.md).
- [Environment variables](docs/ENVIRONMENT.md): every `GEKKOAOT_*` variable,
  generated from the sources and checked by the test suite.

### Clean-room

- The repository stores no game or SDK code. SDK recognition uses
  non-reversible fingerprints: hashes of normalized function bodies and
  prefixes, 8-bit hashes per instruction and counters
  (`tools/sdk_groundtruth_fingerprint.py`); exact structural proofs store a
  mask template and a 64-bit hash (`runtime/sdk/word_proof.h`). Resolution
  results are unchanged on 129 tested game executables.
- Code translated from game executables and its translator were removed.
- Tests that execute SDK bodies read them from a local directory
  (`GEKKOAOT_SDK_FIXTURES`) and are reported as skipped without it.
- `policy.clean-room` guards these rules; see [Clean-room policy](docs/CLEAN_ROOM.md).
- The release workflow grants write access only to the job that publishes,
  and the controller starts tools without a shell.

### Upgrading

- Every game is compiled again on its first launch after updating, once per
  compilation mode.

### Known issues

- Fast mode code is about three times slower on the main thread than Full.
- GoldenEye: Rogue Agent's VP6 intro movie shows vertical stripes.
- Terminator 3: The Redemption: the Enhanced Renderer leaves the player
  character much darker than the scene.
- The frame-rate unlock replays only the newest vertex data, so CPU-animated
  meshes stay at the game's rate.
- Effects use the visible scene only, and pre-rendered backgrounds receive
  them only on 3D objects.
- The issues listed for 0.0.8 that are not mentioned above remain open.

## [0.0.8] (2026-10-16)

### Highlights

- A new launcher, with a desktop library and a controller-driven TV mode for
  the living room and the Steam Deck.
- Discord Rich Presence.
- An in-game performance HUD.
- Reworked widescreen: 3D scenes are widened while menus, HUDs and screen
  effects keep their original proportions.
- Lower CPU and memory use. Idle detection brings several games from 13 % to
  3–7 % of the machine, and Tales of Symphonia now uses 430 MB of RAM instead
  of 1.8 GB.
- Crash fixes for Animaniacs, Ghost Recon 2, Harry Potter and the Sorcerer's
  Stone and Medal of Honor: Frontline.

### Launcher

- Redesigned desktop window: a game library with banners read from the disc,
  search, folder import, play time and last-played tracking, and playback
  controls in the title bar.
- TV mode: a full-screen interface with Library, Recent and Settings that works
  with a controller, keyboard or mouse. Switching between TV mode and the
  desktop keeps the running game.
- Compilation queue: games can be compiled in the background; the
  Compilations panel shows their progress.
- Graphics settings with presets (Original, Crisp, Balanced, Cinematic, Custom)
  and a preview: graphics API, aspect ratio, anti-aliasing, texture filtering,
  sharpening, bloom, tone mapping, local contrast and color controls.
- Controller settings for each port: keyboard, gamepad or joystick, remapping,
  dead zones and axis inversion.
- Memory card manager: browse, import, export and convert saves (RAW, GCP, MCI,
  GCI, GCS and SAV), create cards, and keep a backup before every edit. See
  [Memory cards](docs/LAUNCHER.md#memory-cards).
- Discord Rich Presence through the official Discord Social SDK, optional at
  build time. It shows the game and the session time, never file names or
  paths. See [Discord Rich Presence](docs/LAUNCHER.md#discord-rich-presence).
- New typefaces (Google Sans Flex and Doto), icons and a reduced-motion option.

### Performance HUD

- Shows the frame rate, frame time and a frame-time graph, the game's own
  frame rate, the video interface rate, game speed, CPU use (process and main
  thread) and GPU load. Each item can be turned on or off.
- GPU load is read from the kernel DRM interface on Linux, from sysfs on AMD,
  through NVML on NVIDIA and through PDH on Windows.
- The displayed frame rate is now the real presentation rate: duplicate
  presents of interlaced fields are no longer counted, so 30 fps games report
  30 fps.

### Widescreen

- Perspective 3D is widened to the window; orthographic menus and HUDs stay at
  4:3 in the center. Fades, letterbox bars and full-screen effects cover the
  whole width.
- HUD elements that contain live 3D stay inside their frames (the Game Boy
  Horror in Luigi's Mansion).
- Copies of the screen used for bloom, blur and region restores keep their
  original width. This fixes the bloom seams in Shadow the Hedgehog and the
  black box in Lemony Snicket's A Series of Unfortunate Events.
- Frames built on a pre-rendered background switch to 4:3 automatically
  (Resident Evil rooms).
- Menus drawn as perspective cards are clipped to 4:3 (Pikmin file select).
- Fixed the letterbox bars in Metroid Prime, 512-pixel-wide frames in Finding
  Nemo and the mission fade-ins in Call of Duty: Finest Hour.

### Performance

- Idle threads sleep until the next event instead of servicing a stream of
  timer interrupts. Medal of Honor: Rising Sun went from 13 % to 5 % of the
  machine and GoldenEye: Rogue Agent from 13 % to 3 %.
- More busy-wait loops are parked: clock polls, yield loops, loops that make
  no progress and hardware register polls. Cars went from 13 % to 7 %.
- REL modules are no longer copied into huge pages at load time. Only the main
  executable is, which removes about 1.4 GB of resident memory in Tales of
  Symphonia at no measurable CPU cost.

### Compatibility

- Animaniacs: The Great Edgar Hunt no longer crashes when a level loads.
  NativeOS now removes finished threads from the SDK's active-thread list, so
  the game can safely reuse their memory.
- Tom Clancy's Ghost Recon 2 reaches its menus without an IPL ROM. GekkoAOT
  provides its own boot-ROM font when none is supplied.
- Killer7: aiming works in the opening chapter. EFB color and depth peeks now
  read the frame that was just displayed.
- Medal of Honor: Frontline: choosing "Abort Mission" no longer freezes.
- Harry Potter and the Sorcerer's Stone: fixed a crash when the game reuses the
  memory of a finished thread.
- Tony Hawk's Underground: a GPU drain deferred by a FIFO breakpoint is now
  served when the game polls the command processor, which removes one cause of
  its opening freeze. The game can still freeze; see the known issues.
- Resident Evil 4: REL modules stored inside archive files are found and
  compiled. REL imports from modules that are not on the current disc are left
  unresolved, as on the console.
- The Legend of Zelda: Twilight Princess compiles: its 757 REL modules now fit
  the native code window. It boots to the opening logos.

### Diagnostics

- Logs for guest exceptions, guest call stacks on every fault, the live REL
  memory map, memory watch points and a stall watchdog that also reports
  interrupt, DSP and audio state.
- New options to compare behaviors: `GEKKOAOT_NATIVE_OS_ACTIVE_QUEUE`,
  `GEKKOAOT_HUGE_TEXT_SECONDARY`, `GEKKOAOT_ROM_FONT` and
  `GEKKOAOT_CP_POLL_DRAIN`. See [Performance](docs/OPTIMIZATIONS.md).

### Upgrading

- Compiled games are stored per release, so each game is compiled again on its
  first launch after updating.

### Known issues

- The Legend of Zelda: Twilight Princess crashes after the opening logos.
- The Legend of Zelda: The Wind Waker halts a few seconds after boot with
  NativeOS enabled. With NativeOS disabled it reaches its title screen.
- Super Monkey Ball 2 crashes after Start is pressed on the title screen.
- Spyro: A Hero's Tail freezes during its first in-engine cutscene.
- Tony Hawk's Underground can freeze after the Neversoft logo.
- GoldenEye: Rogue Agent reaches gameplay but is still unstable.
- Killer7 renders some textures incorrectly.
- Finding Nemo runs at about 16 fps.
- Resident Evil 4 has not been verified in game with the new REL extraction.
- The experimental frame-rate unlock is disabled by default and not ready for
  use.

See [Compatibility](docs/COMPATIBILITY.md) for every tested game.

## [0.0.7] (2026-10-13)

This entry covers the local implementation since 0.0.6, including the compiler/runtime, graphics, audio, compatibility and launcher changes accumulated after 0.0.6.

### Compatibility coverage and boot progress

- Expanded the documented runtime coverage to 23 titles/developer samples, with gameplay, menus, opening video and boot activity recorded separately. These are development observations, not complete-game certifications.
- Documented **Harry Potter and the Prisoner of Azkaban** reaching the Hogwarts Express gameplay corridor, alongside the existing **Chamber of Secrets** courtyard and **Sorcerer's Stone** gameplay observations.
- Documented **GoldenEye: Rogue Agent** Fort Knox gameplay and **Call of Duty: Finest Hour** Stalingrad gameplay; pending-interrupt and graphics-completion paths received additional fixes.
- Included **Disney/Pixar Cars** racing gameplay, **Animaniacs: The Great Edgar Hunt** menu/card navigation, **Finding Nemo** menu execution and **Driven** card/menu execution.
- Recorded **Disney's Hide & Sneak** earlier first-level LLVM gameplay and later restored REL discovery, plus **Disney's Magical Mirror Starring Mickey Mouse** boot/rendering activity after nested-directory extraction fixes.
- Included user-observed gameplay for **Luigi's Mansion**, **Tales of Symphonia**, **Ghost Recon 2**, **Star Fox Adventures**, **Terminator 3** and **Resident Evil**. Their remaining rendering, audio, movie or transition issues are listed with the observed state.
- Retained **Mario Kart: Double Dash!!**, **Super Mario Sunshine**, **MOH Frontline** and the developer demo observations, and added the limited **MOH Rising Sun** level-selection and **Tony Hawk's Underground** opening-video/card-navigation results.
- Kept additional corpus/compiled targets separate from verified runtime starts. See [Compatibility](docs/COMPATIBILITY.md) for the complete table and remaining work.

### PowerPC AOT and native call boundaries

- Expanded static direct-call/tailcall lowering and compile-time native-intercept filtering; known host services avoid repeated generic dispatch/admission work.
- Added optional owner-specific hardware, address-window, flat-VM and locked-cache bindings while retaining compatibility with older modules.
- Improved direct secondary-image routing using module-owned caches, immutable page indices and dense instruction-owner tables, with fallback for gaps, aliases and sparse/oversized images.
- Hoisted stable secondary-chain policy checks out of repeated dispatch iterations and kept cold legacy routing separate from native superchains.
- Added pending-IRQ epoch barriers when enabling MSR[EE] and when returning through RFI, so primary/secondary chains yield at interrupt-visible boundaries.
- Preserved poll-park exits and saturated-event-counter handling rather than redispatching a parked loop or treating saturation as a safe chain.
- Added static host-call and cold FP-unavailable exits, SPR re-entry boundaries and additional alternate-entry support.
- Added LLVM state alias metadata and pure owner-service annotations to reduce redundant loads/calls without applying blanket fast-math.
- Preserved both LLVM and Portable C AOT, including direct/inlined Portable C memory helpers and exact compatibility paths.

### Memory, VM, cache publication and exceptions

- Added a guarded 4 GiB host guest-address window, with checked `base + EA` lowering for admitted RAM accesses and shared cached/uncached backing.
- Added direct flat-VM access with per-access admission so changes to paging controls can revoke a previously valid view.
- Added resident paged-VM lowering through an owner-specific translation mirror, with MSR/segment, permission, reference/change-bit and page-boundary checks.
- Added direct locked-cache scalar/paired accesses to the same storage used by SDK DMA, retaining exact extents and fallback at invalid boundaries.
- Improved DSI restart/fault propagation, including quantized helpers and unrecoverable-vector state handling, rather than losing a memory exception across a native helper.
- Added guest-write/cache-publication tracking for renderer resources and executable routing; texture/array reuse is invalidated by relevant writes and EFB copies.
- Added native memory/string operations over validated contiguous spans and page-aware guest reads, with conservative decline when paged operands are not resident.
- Added Linux transparent huge-page backing for suitable AOT text while retaining fixed function addresses, executable protections and an unchanged mapping on failure.

### Scalar, paired-single and quantized floating point

- Specialized dynamic-GQR float cases instead of expanding every quantization type into each generated body; other types retain helper paths.
- Resolved paired memory spans once before conversion where a direct host span can safely cover both elements.
- Added exact/native f32 expansion for quantized float loads, including dedicated handling of denormals and NaNs.
- Added direct paired/quantized WGPIPE submission and dynamic-address gather attempts before publishing unnecessary CPU state.
- Expanded admitted native scalar arithmetic and division, respecting FPSCR controls, non-IEEE behavior and exceptional-result fallback.
- Added scalar/paired FMA and paired arithmetic specializations with explicit rounding, denormal and non-IEEE handling.
- Coalesced proven paired-FP regions without crossing alternate entries or observable boundaries, preserving the first FP-unavailable exception PC.
- Retained independent controls for regression comparisons; state-SSA/full register-residency experiments are not represented as universal defaults.

### Native SDK, OS and timing services

- Enabled NativeHW and all nine host-service components by default in the standalone runtime as well as fresh launcher configurations. Explicit umbrella/device opt-outs remain available, and launcher diagnostics now report the NativeHW selection.
- Added a structural SDK service corpus and generators; recognition metadata is separated from complete-body semantic admission.
- Expanded native `PADRead`, `DVDFastOpen`, memory/string/cache helpers, SDK clock leaves, `GXSetArray` and normal/position/texture matrix services.
- Added complete-body checks for GX SDK variants and explicit context-capture requirements for scheduler/context services. Unsupported or modified bodies retain compiled PPC execution.
- Improved NativeOS lazy FPU ownership and context save/load/current-context boundaries, preserving architectural FP state for the guest SDK handler.
- Added deadline-driven idle handling across device, decrementer and OS events, with bounded RAM/counter poll proofs and clock-spin parking.
- Kept interrupt-disabled finite waits and pending IRQ latches distinct from actual guest interrupt delivery.
- Reduced hot-path environment scans and repeated priority/queue/service bookkeeping where the effective policy is immutable for a session.
- Kept arbitrary guest-rate limiting opt-in; a lower CPU percentage obtained by slowing simulation is not treated as an optimization.

### WGPIPE, FIFO, CP and PE ordering

- Added native write-gather append paths for admitted store widths and typed SDK packet submission, retaining partial gathers and guest FIFO mirrors.
- Batched CP burst consumption and pointer advancement; complete safe stream ranges can be submitted without repeated per-command copies.
- Added direct producer batch slots and ordered host-command metadata without separate per-SDK-call allocation or publication.
- Added direct BP/CP/XF state ingress after validated framing, avoiding a second renderer opcode/header decode for supported commands.
- Queued ordered PE token/finish notifications until acknowledgment instead of overwriting an enabled unacknowledged event.
- Deferred breakpoint-release progress to the proper hardware boundary so stop/rearm sequences are not consumed prematurely inside a register write.
- Published queued work promptly at guest idle and completion-related BP boundaries, reducing `GXDrawDone` waits.
- Retained custom display lists, forensic/raw recording, backlogs, breakpoints and unsupported paths as compatibility fallbacks.

### NativeGX batching and GPU resource costs

- Added compatible adjacent-draw coalescing, including bounded primitive expansion with preserved order and winding.
- Added ordered typed array/matrix commands so proven SDK operations update native Aurora state without CP/XF packet parsing.
- Added cached attribute plans, native vertex programs and packed gathering for supported short indexed draws.
- Added compact INDEX16 upload windows with exact shader index bias, preserving sparse indices, NBT3 and guest mutations.
- Added snapshot atlases, array tiles, persistent GPU snapshots and native resource tables with immutable pending-draw ranges.
- Added encoder-scoped bind-group caching with invalidation at pass/custom-encoder boundaries and exact dynamic offsets.
- Added texture-source/version reuse with BP invalidation, EFB-copy and guest-publication checks, reducing repeated decode/hash work.
- Added persistent staging/storage pools and lazy staging; optional upload-arena/snapshot-recycling experiments remain independently selectable.
- Enabled stronger closed-plugin code generation and IPO/LTO where supported, while keeping portable release CPU policies.
- Reset GPU caches, Dawn handles and pending state correctly across renderer teardown/restart.

### EFB, textures, shadows and widescreen

- Added GPU fragment bounding-box accumulation/readback and BP integration, with guest coordinates, scissor and fragment visibility handling for paths used by the Mickey titles.
- Improved color/depth EFB peeks/pokes, copy-format conversion and batched poke handling.
- Preserved the appropriate EFB attachments across bounded display-copy recordings, including no-clear and color-preserving depth-clear cases, without unbounded host frames.
- Improved texture-source invalidation, repeated BP decoding after XF aliases, missing texture-coordinate defaults and manual SU sampling scale used by movie Y/U/V passes.
- Added BP scissor-offset decoding and consistent viewport/scissor adjustment while preserving EFB copy coordinates.
- Distinguished perspective scenes, orthographic UI, screen fills and observed texture/shadow producer passes for widescreen projection/scissor handling.
- Corrected clipping of aspect-corrected UI, including Luigi's Game Boy Horror; the Tales of Symphonia map/save result was also checked with the user.
- Retained 3D camera/frustum candidates across subsequent HUD projections and limited redundant per-frame camera work. Universal CPU-side culling and every shadow pass are still unfinished.
- Added configurable host post-processing after final XFB scanout: sharpen, bloom, vignette, grain, tone and local contrast, without changing guest EFB/RAM data.

### DVD, VFS and streamed disc audio

- Preserved nested FST directories and migrated old extraction-policy caches so embedded REL/executable discovery can find their files.
- Added native FST leaf lookup and host-backed DVDFileInfo population for admitted SDK bodies.
- Added persistent file/nod handle reuse for immutable disc reads while retaining live overlay reopening.
- Improved asynchronous DI completion, cancellation, drive-cover/stop behavior and shutdown quiescence of pending host I/O.
- Added streamed DVD/DTK ADPCM decoding and mixing into host audio, with current/next track handling, stream status/position, volume and sample-rate conversion.
- Kept drive bookkeeping and IRQ/register behavior in DI rather than requiring every disc provider to duplicate it.

### DSP, AX and JAudio

- Expanded JAudio standard/light protocol recognition, synchronization masks, deferred acknowledgments and frame/subframe voice processing.
- Added main-RAM PCM16 voice sources and direct resident VPB access, preserving the PPC-owned tail during writeback.
- Corrected AFC sample caching, predictor history and non-aligned loop handling.
- Matched signed 16-bit mixer wrap, gain/ramp scaling and front stereo DAC output; avoided adding rear/wet effects twice.
- Improved DSP reset/bootstrap/task-mail transitions and separated protocol deadlines from interrupt acknowledgment.
- Added deferred ARAM DMA busy-clear/completion IRQ timing and relevant idle wake deadlines.
- Improved host audio buffering and frame mixing, retaining standalone DSP fallback for unknown microcode families.
- Improved optional VP6/VP6F conversion through libswscale when available. Full movie frontends and H4M/HVQM4 decoding are not claimed complete.

### Input, memory cards and launcher

- Added native EXI host-card storage, initialization/checksums, command/DMA handling and coalesced dirty-range persistence.
- Improved SI automatic polling, explicit transfers and idle input deadlines so keyboard/gamepad state remains responsive.
- Corrected legacy shoulder/trigger mappings and keyboard/controller stick composition; reduced unnecessary device-health polling while keeping input sample cadence.
- Enabled NativeOS for new launcher settings and retained existing user preferences, launcher cards and explicit native environment overrides.
- Made selected host-FPS, MSAA, anisotropy and post-processing settings authoritative rather than silently forcing different native-profile values.
- Exposed and defaulted additional proven native lowering/rendering paths while keeping independent fallback controls.

### Compilation, caching and regression coverage

- Added compiler worker batching and adaptive REL scheduling under a shared CPU-slot budget.
- Made ThinLTO bitcode emission on-demand, reduced unnecessary fallback audits and bounded expensive coalescing/vectorizer searches.
- Preserved source timestamps across deterministic dependency patch composition so unchanged sources do not trigger needless compiler/renderer rebuilds.
- Separated translated-object and final module/glue identities, included effective native options/capabilities, and added source/toolchain fingerprints for secondary caches.
- Added pipeline serialization and compiler snapshots to avoid conflicting builds and stale dependency/configuration reuse.
- Improved per-object build progress, cached-object reporting and failure retry behavior.
- Registered the runner-dependent FP policy integration test only for native-host builds, with an explicit helper dependency; tools-only builds remain independently testable.
- Expanded native hardware, SDK admission, VM/fault, FP, vertex/resource, PE/FIFO ordering, restart and build/cache tests. Linux release preparation passed 79 CTest cases plus five publication-boundary tests.

### Release maintenance

- Launch the Windows REL-to-DOL pre-scan directly through CreateProcessW, preserve quoted shell fallback commands, and test real helper execution with spaces, brackets, argument quotes and exit-code propagation.
- Publish rebuilt release assets through a draft, refreshing the version tag and release notes only after successful platform builds.
- Run CTest in both CI and release jobs; keep large PE test fixtures off the Windows stack and accept CMake tool-cache type variations when validating reusable builds.
- Fixed MSVC test portability (Windows file timestamps and empty argument packs) and Clang narrowing in the generated Aurora texture cache key.
- Added retained gameplay screenshots for Luigi’s Mansion, Chamber of Secrets, GoldenEye and Call of Duty to the README gallery.
- Updated the full README, preserving its screenshots, technical explanations and long-term architecture sections, and expanded the compatibility documentation.
- Consolidated old versioned development/audit notes into maintained topic documents; raw audit CSVs and profiling captures stay local.
- Removed inactive Release Please metadata and code backups; version 0.0.7 remains an explicit manual-release preparation.
- Added source/archive/package exclusion rules and publication-content checks to CI and the manual release workflow.

### Remaining limits

- Full-game compatibility, later transitions, sound/movie families and visual correctness remain title- and scene-dependent.
- Complete SDK replacement, CPUState/dispatcher removal, custom FIFO/MMIO elimination, universal frustum/widescreen correction and full H4M/movie support remain unfinished.
- Known Luigi flashlight/rectangle, Sunshine water, Ghost Recon movie/widescreen and effect-dependent performance issues remain follow-up targets.
- No universal CPU-use target or two-to-three-minute cold compilation ceiling has been established.

## [0.0.6] (2026-09-27)

### Fixes

- Fixed portable Linux/AppImage secondary rebuilds reusing stale CMake paths from a previous `/tmp/.mount_*` instance; the small side-module CMake tree is regenerated while expensive LLVM objects stay cached.
- Fixed portable secondary AOT linking for large executables by archiving generated LLVM chunks before linking the side module. This avoids Zig/LLD exhausting the process file-descriptor quota on titles with thousands of generated objects (for example MOH Frontline's 5,527-chunk secondary ELF).

### Performance

- Reduced steady-state GX/CP overhead with inline WGPIPE detection, chunked write-gather copies, a dedicated 32-byte CP notification fast path, a synchronous single-burst drain fast path, and cheaper FIFO diagnostic-ring bookkeeping.

## [0.0.5] (2026-09-27)

### Fixes

- Fixed a Windows native-runner crash with exit code `0xC00000FD` (`STATUS_STACK_OVERFLOW`) during NativeGX startup. The 1 MiB TLUT backing store is now heap-backed and reset in place, and the Windows runner reserves a 16 MiB main-thread stack for additional runtime/renderer headroom.

## [0.0.4] (2026-09-27)

### Compatibility

- **Medal of Honor: Frontline** (`GMFE69`): font/text rendering now works in the currently tested path, movie playback no longer remains on a black screen, and level loading now progresses into the level.
- **Star Fox Adventures**: compatibility continues to advance through the generic runtime/graphics fixes in this release; support remains incomplete.
- No retail title is claimed as fully playable yet.

### AuroraGX / graphics

- Added host-side GameCube **TLUT/TMEM palette handling** for CI4/CI8/CI14X2 paths, including partial palette uploads, raw BP address decoding and regression coverage. This restores palette-backed rendering paths used by fonts and other indexed textures.
- Added a descriptor-aware Aurora texture cache patch so cached textures are invalidated when source address, dimensions, format, mip count or palette interpretation changes instead of relying only on object/version IDs.
- Corrected the raw retail GX viewport origin from `340` to `342` and added an independent FIFO regression test for a 640x448 retail viewport.
- Expanded GX state diagnostics around projection/viewport, texture/TLUT state and draw-time matrix state while continuing matrix/mesh/camera correctness work.
- Improved CP FIFO breakpoint/resume behavior and BP-mask/display-copy regression coverage.

### Disc, DSP and video

- Expanded native disc/runtime coverage for FST lookup, exact and unaligned reads, end-of-image behavior, deferred DI DMA visibility, cancellation and cascaded transfers.
- Reworked native DSP reset/init/task lifecycle behavior so GameCube SDK reset, bootstrap and task-loader transitions are kept separate and covered by a dedicated regression test.
- Improved the movie/video presentation path used by **Medal of Honor: Frontline**, removing the previous persistent black output in the tested sequence.
- Native **VP6/VP6F** support is still incomplete. GekkoAOT has an optional FFmpeg/libavcodec codec backend, but the full native game movie frontend/integration still needs work; visible video artifacts can remain.

### Known remaining issues

- **Medal of Honor: Frontline** still shows rendering artifacts affecting camera/projection, matrices/transforms and meshes.
- Video rendering/decoding can still show artifacts until the native VP6 path and its frontend integration are completed.
- **Star Fox Adventures** remains an active compatibility target rather than a playable-status claim.

## [0.0.3] (2026-09-27)

### Fixes

- Fixed Windows portable builds failing before AOT compilation when the native module/cache path exceeded the legacy Win32 path length. Module and LLVM cache directory names now use compact deterministic keys while preserving full cache identity.
- Fixed Windows module metadata generation by launching the metadata helper directly instead of routing its arguments through `cmd.exe`.
- Fixed portable module configuration by providing Zig `ar`/`ranlib` wrappers and explicit CMake archive-tool configuration.
- Added a pinned portable SDL3 fallback for keyboard/gamepad input, including headless Linux release builds and direct Win32 keyboard-state fallback for the Aurora window.
- Reduced the default standalone AuroraGX window to `960x540` logical pixels while keeping environment overrides.

### Documentation

- Refreshed the project README and full-resolution runtime/gameplay screenshots.
- Added SDL3 portable-build licensing information.

### Release workflow

- Release publishing is explicit/manual so normal development commits do not create unintended version tags.

## [0.0.2] (2026-09-26)

### Native runtime and NativeOS

- Expanded **NativeOS** into a host-side implementation of the common GameCube OS SDK surface while keeping guest `OSThread` and `OSContext` structures ABI-visible and authoritative.
- Added native thread/scheduler services for thread lifecycle, ready/wait queues, yield/reschedule, suspend/resume, sleep/wakeup, priorities and scheduler control.
- Added native synchronization and callback-facing services for mutexes, condition variables, message queues and alarms.
- Added `OSContext` save/load/current-context handling plus complete Gekko floating-point state capture: FPR0-31, paired-single PS0-31 and FPSCR.
- Added native FPU-context helpers (`OSLoadFPUContext`, `OSSaveFPUContext`, `OSFillFPUContext`) and stack-pointer/switch helpers.
- Improved scheduler correctness with READY-priority coherency, deferred rescheduling while IRQ contexts are active and sanity checks before host-side context switches.
- Kept the tiny retail interrupt leaves (`OSDisableInterrupts`, `OSEnableInterrupts`, `OSRestoreInterrupts`) in guest AOT by default so their exact PPC register/CR/`mtmsr` semantics are preserved.
- Added MSR[EE] transition side-exits and interrupt-entry fences so pending external/decrementer interrupts are observed at architectural PPC boundaries.
- Added NativeOS hook-family diagnostics/bisection for scheduler, synchronization and interrupt paths.

### Native VFS, DVD and disc path

- Added/expanded **NativeVFS** backed by encounter/nod rather than a Python or emulator VFS layer.
- Native disc handling supports the pinned nod container families used by GekkoAOT, including ISO/GCM, RVZ, WIA, WBFS and GCZ.
- Added cached extraction of GameCube system files and metadata (`boot.bin`, `bi2.bin`, `fst.bin`, `main.dol`, disc ID and banner metadata).
- NativeVFS performs normalized FST path lookup and reads game files directly from the mounted GameCube partition.
- DI DMA completion is staged asynchronously instead of making DVD reads visible immediately, improving retail drive/IRQ timing behavior.
- Windows helper execution now uses direct Unicode Win32 process launching instead of routing native paths through `cmd.exe`.

### PowerPC AOT, CPU and memory

- Continued the DolRecomp-based **PowerPC -> LLVM AOT** pipeline with GekkoAOT-owned CPU/module ABI and per-game native module generation.
- Added native re-entry/resume boundaries, execution budgets, shared-poll handling and safer dispatcher exits for timing/interrupt-sensitive code.
- Added computed-CTR ABI fencing so dynamic `bctr`/`bctrl` targets receive synchronized PPC architectural state.
- Added context-capture and static direct-HLE state synchronization so host-native SDK calls can return to recompiled PPC code coherently.
- Restored more retail boot behavior around HID/BAT setup, exception vectors, instruction-cache transitions and locked-cache-visible state.
- Kept a 32 MiB host backing allocation for compatibility while exposing the retail 24 MiB MEM1 size to guest-visible software.
- Continued native/FakeVMEM address-space support and fast RAM-first guest-pointer paths.
- Native hardware services cover CP, PE, PI, MI, AI, DI, EXI, SI, VI and locked cache, with dedicated timing/IRQ/register regression tests.

### AuroraGX and GameCube graphics

- Continued the direct **AuroraGX** retail FIFO path instead of routing normal rendering through a Dolphin graphics backend.
- Added full `GXSetZTexture` plumbing through Aurora BP state, including Z-texture bias/control registers and shader-side depth output.
- Z-texture sampling handles the GameCube Z8, Z16 and Z24X8 source formats and the GX depth ADD/REPLACE operations.
- Expanded EFB depth-copy conversion support for `Z4`, `Z8`, `Z16`, `Z24X8`, `Z8M`, `Z8L`, raw `Z16R` and `Z16L`.
- Added/expanded GPU-backed EFB color/depth peek and readback infrastructure needed by titles that read PE results.
- Restored CP/XF Matrix Index A/B synchronization and masked PNMTXIDX/TEXMTXIDX to the GameCube 6-bit hardware field.
- Reworked XF transform/normal storage toward row-addressed Flipper behavior with partial-load handling.
- Added dynamic-mesh coherency by invalidating overlapping indexed-array GPU snapshots on guest cache-publication boundaries.
- Made first-use retail GX pipelines correctness-first so asynchronous host pipeline compilation does not silently lose the first draw.
- Added compatibility work around TEV alpha compare, destination-alpha handling, bind-group lifetimes, vertex-index capacity, zero-stride XF input and EFB-to-texture copies.
- Kept the modern XFB/presentation path, live viewport/aspect handling and standalone Alt+Enter fullscreen behavior.

### DSP, audio and video

- Expanded the native DSP path with GameCube DSP state/core services and host audio output.
- Added native **AX HLE** coverage for GameCube AX command lists, VPB voices, ADPCM/PCM decoding, SRC/resampling and mixer/output processing.
- Added early **JAudio HLE** coverage including AFC/PCM voice sources, resampling, mixing/filter state and common command handling.
- Kept a standalone DSP LLE fallback for unsupported/unknown microcode families without linking the Dolphin Core/System runtime.
- Added an optional native VP6/VP6F movie decode backend through FFmpeg/libavcodec when available.

### Input and host integration

- Added standalone SDL3 keyboard, gamepad and joystick input owned by the native host.
- Added configurable per-port button, stick, C-stick, trigger and deadzone mappings.
- Kept SI-facing controller state in the native runtime rather than depending on an emulator frontend.

### Performance and diagnostics

- Added a bounded native superchain to reduce unnecessary dispatcher transitions while preserving safe timing/event exits.
- Added invariant-TSC/portable realtime timing, lower-cost idle polling, O(1)-style NativeOS READY-thread caching and faster RAM/WGPIPE hot paths.
- Enabled stronger compiler/LTO/IPO optimization paths where supported while keeping debuggable compatibility builds available.
- Continued adaptive PGO/CrossGameDB tooling, GDB/perf helpers and hardware-focused regression tests.

### Packaging and reproducibility

- Release packages use a relocatable runtime/resource layout under `share/gekkoaot` with mutable state kept in the per-user GekkoAOT cache.
- Linux and Windows releases prebuild the DolRecomp/AuroraGX engine so end users do not need a manually patched upstream checkout.
- Improved Windows LLVM/MSVC packaging, DIA SDK rebinding, Ninja-built DolRecomp discovery and portable toolchain staging.
- Improved native Windows Unicode path/argument handling for disc images, SDK analysis and runtime helper processes.
- Linux packaging includes the portable runtime/toolchain path and AppImage generation.
- Pinned DolRecomp, Aurora and encounter/nod revisions remain reproducible and every source patch is checked before application.

### Compatibility

- **Harry Potter and the Sorcerer's Stone** (`GHLE69`): reaches gameplay / early 3D; visible graphics/rendering glitches remain.
- **Harry Potter and the Chamber of Secrets** (`GHSE69`): reaches gameplay / early 3D; visible graphics/rendering glitches remain.
- **Super Mario Sunshine** (`GMSE01`): reaches gameplay / early 3D; graphics/runtime correctness is still incomplete.
- **Mario Kart: Double Dash!!** (`GM4E01`): reaches gameplay / early 3D; some visible graphics/runtime glitches remain.
- **Nintendo Developer Demo**: current tested developer-demo path works without a known blocker.
- **Medal of Honor: Frontline** (`GMFE69`): boots/continues into early execution with multiple compatibility glitches.

No retail title is claimed as fully **Playable** yet.

### Internal

- Streamlined Windows/Linux entry-point and argument handling.
- Reworked CI/release automation for portable Linux/Windows artifacts and automatic Release Please publishing.

## [0.0.1] - 2026-09-19

Initial public **pre-alpha** release.

### Added

- Qt 6 frontend with game selection, launch/stop controls, graphics/input settings, logs and progress reporting.
- Native C++ `gekkoaotctl` build/run controller.
- GameCube disc/container path through encounter/nod, including the current ISO/GCM/RVZ/WIA/WBFS/GCZ family supported by the pinned nod release.
- DolRecomp LLVM AOT pipeline with pinned upstream revision and deterministic GekkoAOT compiler patch stack.
- Per-game native module generation, metadata tooling, module cache and secondary executable/REL build support.
- GekkoAOT-owned CPU state/module ABI, boot/runtime state and native address-space services.
- Native GameCube-facing hardware work for CP, PE, PI, MI, AI, DI, EXI, SI, VI and locked cache.
- Native AX-oriented DSP/audio path, early JAudio HLE work and standalone DSP fallback infrastructure.
- Native AuroraGX host bridge and retail FIFO command path.
- SDK/HLE structural recognition, static intercept manifests and direct native HLE lowering work.
- Adaptive LLVM PGO training/recovery tooling and CrossGameDB profiling support.
- GDB and `perf` compatibility/debug helpers.
- Declarative per-title game-pack format.
- Linux x86_64 and Windows x86_64 CI/release automation.

### Compatibility work included

- Native FakeVMEM aperture work for titles using the GameCube SDK VM allocation range.
- Raw EFB-to-texture copy serialization/tracing around Aurora's render-worker boundary.
- Shared SDL lifetime fix so embedded Aurora teardown does not terminate host-owned SDL subsystems.
- NativeGX teardown ordering and PGO profile recovery after shutdown-only failures.
- Compatibility-oriented AOT execution boundaries, shared polling handling, context-capture ABI handling, host-native lowering and static direct-HLE state synchronization.
- Computed-CTR Native ABI fence so dynamic `bctr`/`bctrl` targets receive coherent architectural PPC register state instead of stale pass-through values.

### Packaging and reproducibility

- Pinned DolRecomp, Aurora and encounter/nod revisions.
- `git apply --check` validation before every local upstream patch is applied.
- Installed runtime resources are relocatable under `share/gekkoaot`.
- Mutable source/build/cache state is kept outside packaged installations.
- `GEKKOAOT_STATE_DIR` can override the default state location.
- `version.txt` is the single release-version source used by CMake and release automation.

### Current compatibility snapshot

- Harry Potter and the Sorcerer's Stone (USA): reaches gameplay/early 3D with visible 3D glitches.
- Harry Potter and the Chamber of Secrets (USA): reaches menu; stuck on loading before gameplay.
- Nintendo Developer Demo: current tested demo path works without a known blocker.
- Super Mario Sunshine (USA): boots.
- Mario Kart: Double Dash!! (USA): reaches menu; stuck on loading before gameplay.
- Medal of Honor: Frontline (USA): boots/executes with several compatibility glitches.

### Known limitations

- Retail compatibility is still narrow and highly title-dependent.
- A boot/menu state does not imply playability.
- Graphics, timing, audio, input, SDK recognition and native service coverage remain incomplete.
- Some optional analysis/optimization paths use Python 3 (`adaptive_pgo.py` and `crossgame_db.py`); the normal native controller/runtime path is C/C++.
- DolRecomp and Aurora are patched at build time from clean pinned checkouts; GekkoAOT does not use runtime source-patch injection.
- Binary release archives are developer/pre-alpha builds and still require host build/runtime dependencies described in `docs/BUILDING.md`.
