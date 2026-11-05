# Graphics

This page covers what GekkoAOT adds on top of the game's own rendering: the
Enhanced Renderer, texture reconstruction, presets, widescreen and the
frame-rate unlock. The GX path itself is described in
[Architecture](ARCHITECTURE.md#graphics).

Every feature here is universal. No rule is keyed on a game ID, an address or
a texture name: effects apply where the GX command stream proves they are
valid, and the original image is kept everywhere else.

## Presets

| Preset | Contents |
| --- | --- |
| Original | No scene or image effect. |
| Balanced, Cinematic, Crisp | AO, contact shadows and per-fragment GX lighting, with post-processing tuned for each look. |
| Remastered — Natural, Vivid, Cinematic | Enhanced Renderer: AO, contact shadows, fragment lighting, environment light, sun cascades, one-bounce GI, SSR, generated materials, plus post-processing. |
| Remastered — Performance | The Remastered set without parallax occlusion. |
| Remake — AI textures | Remastered Cinematic, 4× Real-ESRGAN texture reconstruction, surface detail, specular highlights and FXAA. |
| Custom | Any combination. |

Presets set scene and image effects together. They never change the display
resolution, aspect policy, field of view, HUD or frame pacing. Every setting
also exists in the per-game `config.ini` and as a `GEKKOAOT_*` environment
variable; the launcher applies changes live.

## Enhanced Renderer

### Scene capture

Opaque, depth-tested perspective draws write extra attachments while the game
renders: a view normal, the view depth with a camera identity, and the opaque
color contribution. The guest EFB, its copies, CPU peeks and RAM copies keep
their original contents and semantics. The effects read only these
attachments and compose into a separate presentation texture.

- **Camera identity.** Each pixel belongs to one of at most 15 exact
  projection, viewport and depth-range combinations, packed with 19 bits of
  depth into three exact 11/11/10-bit values. AO, contact tracing and
  bilateral filters never mix pixels of different cameras, so a weapon drawn
  with its own projection does not shade the world behind it. MSAA samples
  are read individually.
- **Contribution tracking.** Alpha-over attenuates the receiver below it,
  additive and subtractive draws keep it, multiplication modulates it, and an
  opaque interface draw removes it. Composition adds the light correction to
  the original color, so translucent layers and the game's own lighting stay
  intact.
- **Depth independent of guest Z.** A depth-only clear before a weapon pass
  keeps the world capture. Depth prepasses followed by `EQUAL`/`LEQUAL` color
  draws are recognized.
- **Copies and post chains.** A screen-space redraw of an EFB copy, a game's
  own blur or bloom chain working in a corner of the EFB, and the small
  clears those chains issue all leave the scene's receivers in place. A copy
  with a composed twin carries the effects into the redraw instead, so they
  are never applied twice. `GEKKOAOT_ENHANCED_KEEP_RECEIVERS=0` disables
  these rules.

Orthographic draws, translucent fragments, render-to-texture producers and
inset viewports are not receivers. Unsupported blend equations and exhausted
captures fall back to the original image for the pixels concerned.

### Effects

| Effect | Implementation |
| --- | --- |
| Ambient occlusion | View-space horizon sampling at half resolution on large outputs, separable bilateral blur, depth-aware upsample. Radii scale with each camera's near plane, so a game's coordinate units do not change the look. |
| Contact shadows | Screen-space rays toward the light, rejecting the receiver's own texel and tangent plane. |
| Fragment lighting | Draws that supply GX normals evaluate the game's own GX light and channel equations per fragment instead of per vertex. No light is added. |
| Environment light | Relights receivers by a relative factor of sky and sun terms. Average exposure never changes, and a surface keeps at least 45 % of its original light. |
| Sun cascades | Every eligible draw of a displayed pass is replayed into a depth atlas from the sun, with its real geometry, skinning, alpha test and cull mode. Practical log splits, texel snapping, 4×4 PCF, receiver-plane bias and blended transitions. The game's own blob shadows fade while cascades are active. |
| Indirect light | One diffuse bounce gathered from visible receivers in the normal hemisphere. |
| Reflections | Screen-space ray march with Schlick Fresnel, only on surfaces with GX reflection evidence (a sampled specular channel or a normal-based environment texture generator). |
| Particle light | Additive and alpha-over particles become local lights. Sources are selected on the GPU (4 or 8 brightest), with radiance estimated from the TEV program and texture statistics. |
| Depth of field | Fixed focal plane relative to the lens, applied after composition; foreground and interface stay sharp. |
| Surface detail | Generated normal, roughness and height maps, micro-detail, GGX sun highlights and grazing sky reflection on the receiver's share of each pixel. |
| Parallax occlusion | Apparent depth from the generated height map (up to 12 or 24 steps). |

All effects use only what is on screen: off-screen or occluded surfaces
neither bounce nor reflect.

### Sun and vertical axis

In automatic mode the sun is the game's own directional GX light, read in view
space every pass. A light that turns with the camera is rejected. Games with
baked lighting get a sun at a configured azimuth and elevation in the world.
The world's vertical axis is locked from long-term evidence of upright
geometry. Only passes that reach the screen feed this tracking; a pass copied
into a texture keeps the visible scene's state. Discontinuities are logged as
`GEKKOAOT_ENV_EVENT` lines when diagnostics are enabled.

### Field of view

The FOV override scales both perspective focal coefficients and keeps the
game's lens offsets and depth coefficients. Interface and render-to-texture
projections are excluded. CPU-side culling is widened through native camera
providers, proved from code and data flow: four-plane angular cameras and
half-angle tangent cameras. Other engines keep the game's own culling, so
objects at the edge of a wider view can still disappear.

### Shader compilation

Pipeline descriptions are stored per game. Known descriptions compile at
startup; unknown ones compile on worker threads. A draw waits for its exact
pipeline when it first appears and is never skipped or drawn with an
approximate shader.

## Texture reconstruction

**Graphics → Remaster** selects the scale (original, 2× or 4×), the model and
the quality. Values map to `texture_scale`, `texture_backend` and
`texture_quality`.

| Backend | Model |
| --- | --- |
| 0 | Edge-directed rules, no neural network |
| 1 | ESPCN, 21 284 (2×) and 24 752 (4×) parameters, CPU |
| 2 | Real-ESRGAN compact (`srvgg`), 1 213 296 parameters, GPU compute |

- Real-ESRGAN runs on a dedicated compute device and executor thread, in
  256-texel tiles with a 34-texel halo. The game keeps rendering while tiles
  run. A CPU fallback exists and is logged.
- With backend 2, textures between 16 and 512 texels per side go through the
  network. Thin strips and font coverage masks use contour reconstruction, and
  limited-palette HUD art keeps its authored colors.
- Movie frames, continuously changing textures and EFB copies use an immediate
  edge-directed GPU filter instead of a model job.
- Results are cached as DDS files keyed on content, palette, dimensions, wrap,
  mips, model and settings. Balanced keeps authored mip chains; High rebuilds
  them from the reconstruction. Disk and GPU budgets apply.
- A user texture pack has priority. Inference needs no Python or external
  library at run time. Model provenance and licenses are in `assets/models/`.

## Widescreen

The EFB stays 4:3; presentation is widened and each draw is classified:

| Content | Treatment |
| --- | --- |
| Perspective 3D | Widened horizontally (hor+) |
| Orthographic menus and HUDs | Kept at 4:3, centered |
| Fades, letterbox bars, screen fills | Stretched to the full width |
| Screen copies for bloom or blur, region restores | Original width |
| Small HUD viewports with live 3D | Moved with the UI |
| Frames on a pre-rendered background | Presented in 4:3 |

## Frame-rate unlock

The unlock replays the newest frame's real geometry at the display rate with
interpolated or extrapolated transforms. No image is synthesized, game speed
is unchanged, and the game still runs at its own rate.

- Every GX uniform block is keyed by draw signature and order. Projection,
  position and normal matrices are blended; texture matrices are not. Rigid
  transforms use polar decomposition and quaternion interpolation.
- Draws are paired by order and camera-compensated distance; unpaired 3D
  blocks follow the camera only. Render-to-texture producers are kept exact.
- *Smooth* interpolates (one game frame of latency). *Instant* predicts (no
  added latency).
- Replays are timed against measured GPU cost. When a frame and its replay no
  longer fit in a refresh, the unlock switches to direct presentation until
  they fit again, with hysteresis.
- Frames that read bounding boxes, overflow storage or need encoder tasks are
  not replayed. Games that alternate frame contents (two copies per frame) are
  shown at their own rate.

The unlock is off by default.

## Anti-aliasing and post-processing

MSAA (up to 4×), FXAA and anisotropic filtering are available. Post-processing
runs on the final scanout: sharpening, bloom, filmic tone mapping, local
contrast, vignette, grain, exposure, temperature and color controls.

## Diagnostics

| Variable | Output |
| --- | --- |
| `GEKKOAOT_ENHANCED_DIAGNOSTICS=1` (`all` for every pass) | Draw classification, per-pass reasons and composition, copy provenance |
| `GEKKOAOT_ENHANCED_VIEW` | Debug views: `depth`, `coverage`, `normals`, `cameras`, `environment`, `environment-split`, `shadow-split`, `cascades`, `cascade-atlas`, `indirect`, `reflections` |
| `GEKKOAOT_ENHANCED_VIEW_FILE` | Path of a file holding the view name, read live |
| `GEKKOAOT_ENHANCED_PROFILE=1` | CPU encoder time per pass |
| `GEKKOAOT_INTERP_PROBE=1`, `GEKKOAOT_REPLAY_TRACE=1` | Replay match rates and pairing |

GPU regressions for the renderer run against a locally built bridge:

```sh
GEKKOAOT_RUN_GPU_TESTS=1 \
GEKKOAOT_NATIVE_GX_TEST_LIBRARY=/absolute/path/libgekkoaot-native-gx.so \
python3 -m unittest tests/hardware/test_shader_compilation_gpu.py \
  tests/hardware/test_native_gx_ir_gpu.py
```

Vulkan and OpenGL are exercised on Linux. Direct3D 12 and Metal need their
own hosts.

## Limits

- Effects use the visible scene only. There is no off-screen geometry for
  reflections, bounce light or particle sources.
- Pre-rendered backgrounds are flat images: only 3D objects drawn over them
  receive effects.
- Generated materials and geometry are derived from the disc's textures.
  They cannot add detail the original assets do not contain.
- Results are validated per scene. A game with an unusual compositing chain
  can still lose effects in part of the screen; the diagnostics above show
  where.
