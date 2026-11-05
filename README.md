<p align="center">
  <img src="assets/gekkoaot-logo.png" alt="GekkoAOT" width="320" />
</p>

<h1 align="center">GekkoAOT</h1>

<p align="center">
  <strong>GameCube games recompiled ahead of time and run as native PC programs</strong>
</p>

<p align="center">
  PowerPC → LLVM AOT · NativeOS · NativeVFS · AuroraGX · Enhanced Renderer · Native DSP
</p>

<p align="center">
  <a href="https://github.com/ant0-blase/GekkoAOT/actions/workflows/ci.yml"><img src="https://github.com/ant0-blase/GekkoAOT/actions/workflows/ci.yml/badge.svg" alt="CI" /></a>
  <a href="https://github.com/ant0-blase/GekkoAOT/releases"><img src="https://img.shields.io/github/v/release/ant0-blase/GekkoAOT?include_prereleases&label=release" alt="Release" /></a>
  <a href="LICENSE"><img src="https://img.shields.io/github/license/ant0-blase/GekkoAOT" alt="License" /></a>
  <img src="https://img.shields.io/badge/status-pre--alpha-orange" alt="Pre-alpha" />
  <a href="#download"><img src="assets/badges/windows.svg" alt="Windows" /></a>
  <a href="#download"><img src="assets/badges/linux.svg" alt="Linux" /></a>
</p>

<p align="center">
  <a href="#features">Features</a> ·
  <a href="#screenshots">Screenshots</a> ·
  <a href="#graphics">Graphics</a> ·
  <a href="#compatibility">Compatibility</a> ·
  <a href="#download">Download</a> ·
  <a href="#usage">Usage</a> ·
  <a href="#architecture">Architecture</a> ·
  <a href="#technical-details">Technical details</a> ·
  <a href="#roadmap">Roadmap</a> ·
  <a href="#documentation">Documentation</a>
</p>

<table>
  <tr>
    <td width="50%"><img src="assets/screenshots/medal-of-honor-frontline-enhanced-beach.jpg" alt="Medal of Honor: Frontline with the Enhanced Renderer, Omaha beach" /></td>
    <td width="50%"><img src="assets/screenshots/medal-of-honor-frontline-enhanced-landing-craft.jpg" alt="Medal of Honor: Frontline with the Enhanced Renderer, landing craft" /></td>
  </tr>
</table>

GekkoAOT reads a GameCube disc image, translates the game's PowerPC code into a
native x86-64 module with LLVM, and runs that module on a runtime that provides
the console's operating system, hardware and graphics services on the host.
There is no emulator core in the normal execution path: each game is compiled
once, and later launches start from the cache.

> [!NOTE]
> GekkoAOT is pre-alpha. Many games boot and reach gameplay, but none has been
> verified from start to finish. Reaching a menu or an early 3D scene does not
> mean a game is playable. See [Compatibility](#compatibility).

> [!IMPORTANT]
> GekkoAOT does not include any game, BIOS/IPL, firmware or proprietary code.
> Use your own legally obtained disc dumps. Each game is compiled locally on
> your machine from your own disc, and the generated code never leaves it.

## Features

- **Ahead-of-time recompilation.** PowerPC code is compiled to native code
  before the game starts, through a pinned DolRecomp + LLVM pipeline. Main
  executables, REL modules and secondary executables are handled.
- **Native operating system.** NativeOS implements the GameCube SDK scheduler,
  threads, mutexes, message queues, alarms and contexts in the host, while
  keeping the guest data structures the game expects.
- **Native graphics.** GX commands go to AuroraGX, which renders through
  Vulkan, Direct3D 12 or OpenGL. Recognized SDK calls are translated directly
  instead of being decoded from the command FIFO.
- **Widescreen without game patches.** 3D scenes are widened to 16:9 while
  menus, HUDs, fades and screen effects keep their original proportions.
- **Low CPU use.** Busy-wait loops and idle threads are detected and parked
  instead of being executed. Most tested games use 2–8 % of an 8-core laptop
  CPU.
- **Performance HUD.** Frame rate, frame time, game speed, CPU and GPU load,
  drawn over the game.
- **Desktop and TV launcher.** A game library with banners and a compilation
  queue, plus a controller-driven TV mode for the living room and the Steam
  Deck.
- **Discord Rich Presence** for the game being played.
- **Enhanced Renderer.** Ambient occlusion, contact shadows, per-fragment GX
  lighting, sun shadow cascades, one-bounce indirect light, screen-space
  reflections, particle lights, depth of field and generated materials, added
  on top of the game's own rendering without changing its output.
- **AI texture reconstruction.** Original textures upscaled 2× or 4× by
  Real-ESRGAN on the GPU or ESPCN on the CPU, with rules for fonts, pixel art
  and HUD palettes. No external runtime is needed.
- **Three compilation modes.** Full for performance, Balanced and Fast for a
  shorter first launch.
- **Frame-rate unlock (experimental).** The game's real geometry is replayed at
  the display rate; no image is synthesized and game speed is unchanged.
- **Mods.** Gecko data cheats and texture packs per game, managed from the
  launcher.
- **Post-processing.** Sharpening, bloom, tone mapping, local contrast, FXAA
  and color controls applied after the game's own output.

### At a glance

| Area | Implementation |
| --- | --- |
| CPU | PowerPC → LLVM ahead-of-time compilation through a pinned DolRecomp |
| Runtime | GekkoAOT module ABI, NativeOS and native hardware services |
| Graphics | Typed native GX commands and the retail GX FIFO, rendered by AuroraGX |
| Enhancement | Scene capture, AO, shadows, GI, SSR, materials and neural texture reconstruction |
| Disc | encounter/nod and NativeVFS |
| Audio | Native AX and JAudio, DSP LLE fallback |
| Input | SDL3 keyboard, gamepad and joystick |
| Launcher | Qt 6 desktop and TV interfaces, `gekkoaotctl` controller |
| Hosts | Windows x86-64 and Linux x86-64 |

### What's new in 0.0.9

- Enhanced Renderer with Remastered and Remake presets: lighting, shadows,
  reflections and materials added to any game whose scene can be captured.
- Neural texture reconstruction with Real-ESRGAN on the GPU.
- Full, Balanced and Fast compilation modes.
- Lower CPU use: the Harry Potter and the Chamber of Secrets cutscene went
  from 118 % to 54 % of one core, Star Fox Adventures with every effect on
  from 4 to 37 fps.
- Exact `lfs`/`stfs` in non-IEEE mode, which fixes the random crashes of
  GoldenEye: Rogue Agent. Medal of Honor: Rising Sun now gets past its first
  loading screen.
- Experimental frame-rate unlock by geometry replay.

The full list is in the [changelog](CHANGELOG.md).

## Screenshots

### Enhanced Renderer

The same scenes with the Enhanced Renderer off and on.

<table>
  <tr>
    <td width="50%"><img src="assets/screenshots/medal-of-honor-frontline-original-shore.jpg" alt="Medal of Honor: Frontline — original" /></td>
    <td width="50%"><img src="assets/screenshots/medal-of-honor-frontline-enhanced-shore.jpg" alt="Medal of Honor: Frontline — Enhanced" /></td>
  </tr>
  <tr>
    <td align="center">Original</td>
    <td align="center">Enhanced</td>
  </tr>
  <tr>
    <td width="50%"><img src="assets/screenshots/medal-of-honor-frontline-original-landing.jpg" alt="Medal of Honor: Frontline — original" /></td>
    <td width="50%"><img src="assets/screenshots/medal-of-honor-frontline-enhanced-landing.jpg" alt="Medal of Honor: Frontline — Enhanced" /></td>
  </tr>
  <tr>
    <td align="center">Original</td>
    <td align="center">Enhanced</td>
  </tr>
  <tr>
    <td width="50%"><img src="assets/screenshots/medal-of-honor-frontline-original-street.jpg" alt="Medal of Honor: Frontline — original" /></td>
    <td width="50%"><img src="assets/screenshots/medal-of-honor-frontline-enhanced-street.jpg" alt="Medal of Honor: Frontline — Enhanced" /></td>
  </tr>
  <tr>
    <td align="center">Original</td>
    <td align="center">Enhanced</td>
  </tr>
  <tr>
    <td width="50%"><img src="assets/screenshots/tales-of-symphonia-original.jpg" alt="Tales of Symphonia — original" /></td>
    <td width="50%"><img src="assets/screenshots/tales-of-symphonia-enhanced.jpg" alt="Tales of Symphonia — Enhanced" /></td>
  </tr>
  <tr>
    <td align="center">Original</td>
    <td align="center">Enhanced</td>
  </tr>
  <tr>
    <td width="50%"><img src="assets/screenshots/tales-of-symphonia-original-town.jpg" alt="Tales of Symphonia — original" /></td>
    <td width="50%"><img src="assets/screenshots/tales-of-symphonia-enhanced-town.jpg" alt="Tales of Symphonia — Enhanced" /></td>
  </tr>
  <tr>
    <td align="center">Original</td>
    <td align="center">Enhanced</td>
  </tr>
  <tr>
    <td width="50%"><img src="assets/screenshots/super-monkey-ball-original.jpg" alt="Super Monkey Ball — original" /></td>
    <td width="50%"><img src="assets/screenshots/super-monkey-ball-enhanced.jpg" alt="Super Monkey Ball — Enhanced" /></td>
  </tr>
  <tr>
    <td align="center">Original</td>
    <td align="center">Enhanced</td>
  </tr>
  <tr>
    <td width="50%"><img src="assets/screenshots/terminator-3-original-road.jpg" alt="Terminator 3: The Redemption — original" /></td>
    <td width="50%"><img src="assets/screenshots/terminator-3-enhanced-road.jpg" alt="Terminator 3: The Redemption — Enhanced" /></td>
  </tr>
  <tr>
    <td align="center">Original</td>
    <td align="center">Enhanced</td>
  </tr>
  <tr>
    <td width="50%"><img src="assets/screenshots/terminator-3-original-sign.jpg" alt="Terminator 3: The Redemption — original" /></td>
    <td width="50%"><img src="assets/screenshots/terminator-3-enhanced-sign.jpg" alt="Terminator 3: The Redemption — Enhanced" /></td>
  </tr>
  <tr>
    <td align="center">Original</td>
    <td align="center">Enhanced</td>
  </tr>
  <tr>
    <td width="50%"><img src="assets/screenshots/mario-kart-double-dash-original.jpg" alt="Mario Kart: Double Dash!! — original" /></td>
    <td width="50%"><img src="assets/screenshots/mario-kart-double-dash-enhanced.jpg" alt="Mario Kart: Double Dash!! — Enhanced" /></td>
  </tr>
  <tr>
    <td align="center">Original</td>
    <td align="center">Enhanced</td>
  </tr>
  <tr>
    <td width="50%"><img src="assets/screenshots/killer7-original-lobby.jpg" alt="Killer7 — original" /></td>
    <td width="50%"><img src="assets/screenshots/killer7-enhanced-lobby.jpg" alt="Killer7 — Enhanced" /></td>
  </tr>
  <tr>
    <td align="center">Original</td>
    <td align="center">Enhanced</td>
  </tr>
  <tr>
    <td width="50%"><img src="assets/screenshots/killer7-original-reload.jpg" alt="Killer7 — original" /></td>
    <td width="50%"><img src="assets/screenshots/killer7-enhanced-reload.jpg" alt="Killer7 — Enhanced" /></td>
  </tr>
  <tr>
    <td align="center">Original</td>
    <td align="center">Enhanced</td>
  </tr>
  <tr>
    <td width="50%"><img src="assets/screenshots/killer7-original-targets.jpg" alt="Killer7 — original" /></td>
    <td width="50%"><img src="assets/screenshots/killer7-enhanced-targets.jpg" alt="Killer7 — Enhanced" /></td>
  </tr>
  <tr>
    <td align="center">Original</td>
    <td align="center">Enhanced</td>
  </tr>
  <tr>
    <td width="50%"><img src="assets/screenshots/medal-of-honor-frontline-original-emmerich.jpg" alt="Medal of Honor: Frontline — original" /></td>
    <td width="50%"><img src="assets/screenshots/medal-of-honor-frontline-enhanced-emmerich.jpg" alt="Medal of Honor: Frontline — Enhanced" /></td>
  </tr>
  <tr>
    <td align="center">Original</td>
    <td align="center">Enhanced</td>
  </tr>
  <tr>
    <td width="50%"><img src="assets/screenshots/medal-of-honor-frontline-original-balcony.jpg" alt="Medal of Honor: Frontline — original" /></td>
    <td width="50%"><img src="assets/screenshots/medal-of-honor-frontline-enhanced-balcony.jpg" alt="Medal of Honor: Frontline — Enhanced" /></td>
  </tr>
  <tr>
    <td align="center">Original</td>
    <td align="center">Enhanced</td>
  </tr>
  <tr>
    <td width="50%"><img src="assets/screenshots/medal-of-honor-frontline-original-square.jpg" alt="Medal of Honor: Frontline — original" /></td>
    <td width="50%"><img src="assets/screenshots/medal-of-honor-frontline-enhanced-square.jpg" alt="Medal of Honor: Frontline — Enhanced" /></td>
  </tr>
  <tr>
    <td align="center">Original</td>
    <td align="center">Enhanced</td>
  </tr>
  <tr>
    <td width="50%"><img src="assets/screenshots/medal-of-honor-frontline-original-emmerich-cutscene.jpg" alt="Medal of Honor: Frontline — original" /></td>
    <td width="50%"><img src="assets/screenshots/medal-of-honor-frontline-enhanced-emmerich-cutscene.jpg" alt="Medal of Honor: Frontline — Enhanced" /></td>
  </tr>
  <tr>
    <td align="center">Original</td>
    <td align="center">Enhanced</td>
  </tr>
  <tr>
    <td width="50%"><img src="assets/screenshots/harry-potter-chamber-of-secrets-original-yard.jpg" alt="Harry Potter and the Chamber of Secrets — original" /></td>
    <td width="50%"><img src="assets/screenshots/harry-potter-chamber-of-secrets-enhanced-yard.jpg" alt="Harry Potter and the Chamber of Secrets — Enhanced" /></td>
  </tr>
  <tr>
    <td align="center">Original</td>
    <td align="center">Enhanced</td>
  </tr>
  <tr>
    <td width="50%"><img src="assets/screenshots/harry-potter-chamber-of-secrets-original-gate.jpg" alt="Harry Potter and the Chamber of Secrets — original" /></td>
    <td width="50%"><img src="assets/screenshots/harry-potter-chamber-of-secrets-enhanced-gate.jpg" alt="Harry Potter and the Chamber of Secrets — Enhanced" /></td>
  </tr>
  <tr>
    <td align="center">Original</td>
    <td align="center">Enhanced</td>
  </tr>
  <tr>
    <td width="50%"><img src="assets/screenshots/call-of-duty-finest-hour-original-village.jpg" alt="Call of Duty: Finest Hour — original" /></td>
    <td width="50%"><img src="assets/screenshots/call-of-duty-finest-hour-enhanced-village.jpg" alt="Call of Duty: Finest Hour — Enhanced" /></td>
  </tr>
  <tr>
    <td align="center">Original</td>
    <td align="center">Enhanced</td>
  </tr>
</table>

<table>
  <tr>
    <td width="50%"><img src="assets/screenshots/medal-of-honor-frontline-enhanced-water.jpg" alt="Medal of Honor: Frontline, Omaha beach" /></td>
    <td width="50%"><img src="assets/screenshots/medal-of-honor-frontline-enhanced-beach-2.jpg" alt="Medal of Honor: Frontline, Omaha beach" /></td>
  </tr>
  <tr>
    <td align="center">Enhanced</td>
    <td align="center">Enhanced</td>
  </tr>
  <tr>
    <td width="50%"><img src="assets/screenshots/medal-of-honor-frontline-enhanced-field.jpg" alt="Medal of Honor: Frontline, Normandy" /></td>
    <td width="50%"><img src="assets/screenshots/medal-of-honor-frontline-enhanced-town.jpg" alt="Medal of Honor: Frontline, town square" /></td>
  </tr>
  <tr>
    <td align="center">Enhanced</td>
    <td align="center">Enhanced</td>
  </tr>
  <tr>
    <td width="50%"><img src="assets/screenshots/medal-of-honor-frontline-enhanced-farm.jpg" alt="Medal of Honor: Frontline, Netherlands" /></td>
    <td width="50%"><img src="assets/screenshots/medal-of-honor-frontline-enhanced-trench.jpg" alt="Medal of Honor: Frontline, Netherlands" /></td>
  </tr>
  <tr>
    <td align="center">Enhanced</td>
    <td align="center">Enhanced</td>
  </tr>
  <tr>
    <td width="50%"><img src="assets/screenshots/luigis-mansion-enhanced-foyer.jpg" alt="Luigi's Mansion, foyer" /></td>
    <td width="50%"><img src="assets/screenshots/luigis-mansion-enhanced-key.jpg" alt="Luigi's Mansion, key" /></td>
  </tr>
  <tr>
    <td align="center">Enhanced</td>
    <td align="center">Enhanced</td>
  </tr>
  <tr>
    <td width="50%"><img src="assets/screenshots/luigis-mansion-enhanced-dark.jpg" alt="Luigi's Mansion, lights out" /></td>
    <td width="50%"><img src="assets/screenshots/luigis-mansion-enhanced-flashlight.jpg" alt="Luigi's Mansion, flashlight" /></td>
  </tr>
  <tr>
    <td align="center">Enhanced</td>
    <td align="center">Enhanced</td>
  </tr>
  <tr>
    <td width="50%"><img src="assets/screenshots/medal-of-honor-frontline-enhanced-windmill.jpg" alt="Medal of Honor: Frontline, field" /></td>
    <td width="50%"><img src="assets/screenshots/medal-of-honor-frontline-enhanced-patrol.jpg" alt="Medal of Honor: Frontline, Emmerich" /></td>
  </tr>
  <tr>
    <td align="center">Enhanced</td>
    <td align="center">Enhanced</td>
  </tr>
  <tr>
    <td width="50%"><img src="assets/screenshots/medal-of-honor-frontline-enhanced-street-2.jpg" alt="Medal of Honor: Frontline, street" /></td>
    <td width="50%"><img src="assets/screenshots/cars-enhanced-radiator-springs.jpg" alt="Disney/Pixar Cars, Radiator Springs" /></td>
  </tr>
  <tr>
    <td align="center">Enhanced</td>
    <td align="center">Enhanced</td>
  </tr>
  <tr>
    <td width="50%"><img src="assets/screenshots/tales-of-symphonia-enhanced-battle-results.jpg" alt="Tales of Symphonia, battle results" /></td>
    <td width="50%"><img src="assets/screenshots/tales-of-symphonia-enhanced-battle.jpg" alt="Tales of Symphonia, battle" /></td>
  </tr>
  <tr>
    <td align="center">Enhanced</td>
    <td align="center">Enhanced</td>
  </tr>
  <tr>
    <td width="50%"><img src="assets/screenshots/goldeneye-rogue-agent-enhanced-casino.jpg" alt="GoldenEye: Rogue Agent, casino" /></td>
    <td width="50%"><img src="assets/screenshots/harry-potter-chamber-of-secrets-enhanced-garden.jpg" alt="Harry Potter and the Chamber of Secrets, the Burrow garden" /></td>
  </tr>
  <tr>
    <td align="center">Enhanced</td>
    <td align="center">Enhanced</td>
  </tr>
  <tr>
    <td width="50%"><img src="assets/screenshots/harry-potter-chamber-of-secrets-enhanced-garden-2.jpg" alt="Harry Potter and the Chamber of Secrets, the Burrow garden" /></td>
    <td width="50%"><img src="assets/screenshots/harry-potter-chamber-of-secrets-enhanced-ron.jpg" alt="Harry Potter and the Chamber of Secrets, Ron" /></td>
  </tr>
  <tr>
    <td align="center">Enhanced</td>
    <td align="center">Enhanced</td>
  </tr>
  <tr>
    <td width="50%"><img src="assets/screenshots/harry-potter-chamber-of-secrets-enhanced-weasleys.jpg" alt="Harry Potter and the Chamber of Secrets, the Weasleys" /></td>
    <td width="50%"><img src="assets/screenshots/harry-potter-chamber-of-secrets-enhanced-gnome.jpg" alt="Harry Potter and the Chamber of Secrets, de-gnoming" /></td>
  </tr>
  <tr>
    <td align="center">Enhanced</td>
    <td align="center">Enhanced</td>
  </tr>
</table>

### Launcher and games

<table>
  <tr>
    <td width="50%"><img src="assets/screenshots/launcher-desktop.png" alt="Desktop library" /></td>
    <td width="50%"><img src="assets/screenshots/launcher-tv-mode.png" alt="TV mode" /></td>
  </tr>
  <tr>
    <td align="center">Desktop library</td>
    <td align="center">TV mode</td>
  </tr>
</table>

<table>
  <tr>
    <td width="50%"><img src="assets/screenshots/luigis-mansion-hud.png" alt="Luigi's Mansion" /></td>
    <td width="50%"><img src="assets/screenshots/mario-kart-double-dash-hud.png" alt="Mario Kart: Double Dash!!" /></td>
  </tr>
  <tr>
    <td align="center">Luigi's Mansion</td>
    <td align="center">Mario Kart: Double Dash!!</td>
  </tr>
  <tr>
    <td width="50%"><img src="assets/screenshots/star-fox-adventures-hud.png" alt="Star Fox Adventures" /></td>
    <td width="50%"><img src="assets/screenshots/tales-of-symphonia-hud.png" alt="Tales of Symphonia" /></td>
  </tr>
  <tr>
    <td align="center">Star Fox Adventures</td>
    <td align="center">Tales of Symphonia</td>
  </tr>
  <tr>
    <td width="50%"><img src="assets/screenshots/super-monkey-ball-hud.png" alt="Super Monkey Ball" /></td>
    <td width="50%"><img src="assets/screenshots/super-monkey-ball-adventure-hud.png" alt="Super Monkey Ball Adventure" /></td>
  </tr>
  <tr>
    <td align="center">Super Monkey Ball</td>
    <td align="center">Super Monkey Ball Adventure</td>
  </tr>
  <tr>
    <td width="50%"><img src="assets/screenshots/super-mario-strikers-hud.png" alt="Super Mario Strikers" /></td>
    <td width="50%"><img src="assets/screenshots/driven-hud.png" alt="Driven" /></td>
  </tr>
  <tr>
    <td align="center">Super Mario Strikers</td>
    <td align="center">Driven</td>
  </tr>
  <tr>
    <td width="50%"><img src="assets/screenshots/metroid-prime-hud.png" alt="Metroid Prime" /></td>
    <td width="50%"><img src="assets/screenshots/wind-waker-hud.png" alt="The Legend of Zelda: The Wind Waker (NativeOS disabled)" /></td>
  </tr>
  <tr>
    <td align="center">Metroid Prime</td>
    <td align="center">The Legend of Zelda: The Wind Waker (NativeOS disabled)</td>
  </tr>
  <tr>
    <td width="50%"><img src="assets/screenshots/harry-potter-chamber-of-secrets-hud.png" alt="Harry Potter and the Chamber of Secrets" /></td>
    <td width="50%"><img src="assets/screenshots/harry-potter-sorcerers-stone-hud.png" alt="Harry Potter and the Sorcerer's Stone" /></td>
  </tr>
  <tr>
    <td align="center">Harry Potter and the Chamber of Secrets</td>
    <td align="center">Harry Potter and the Sorcerer's Stone</td>
  </tr>
  <tr>
    <td width="50%"><img src="assets/screenshots/harry-potter-prisoner-of-azkaban-hud.png" alt="Harry Potter and the Prisoner of Azkaban" /></td>
    <td width="50%"><img src="assets/screenshots/killer7-hud.png" alt="Killer7" /></td>
  </tr>
  <tr>
    <td align="center">Harry Potter and the Prisoner of Azkaban</td>
    <td align="center">Killer7</td>
  </tr>
  <tr>
    <td width="50%"><img src="assets/screenshots/call-of-duty-finest-hour-hud.png" alt="Call of Duty: Finest Hour" /></td>
    <td width="50%"><img src="assets/screenshots/medal-of-honor-frontline-hud.png" alt="Medal of Honor: Frontline" /></td>
  </tr>
  <tr>
    <td align="center">Call of Duty: Finest Hour</td>
    <td align="center">Medal of Honor: Frontline</td>
  </tr>
  <tr>
    <td width="50%"><img src="assets/screenshots/goldeneye-rogue-agent-hud.png" alt="GoldenEye: Rogue Agent" /></td>
    <td width="50%"><img src="assets/screenshots/cars-hud.png" alt="Disney/Pixar Cars" /></td>
  </tr>
  <tr>
    <td align="center">GoldenEye: Rogue Agent</td>
    <td align="center">Disney/Pixar Cars</td>
  </tr>
  <tr>
    <td width="50%"><img src="assets/screenshots/magical-mirror-mickey-mouse-hud.png" alt="Disney's Magical Mirror Starring Mickey Mouse" /></td>
    <td width="50%"><img src="assets/screenshots/hide-and-sneak-hud.png" alt="Disney's Hide & Sneak" /></td>
  </tr>
  <tr>
    <td align="center">Disney's Magical Mirror Starring Mickey Mouse</td>
    <td align="center">Disney's Hide & Sneak</td>
  </tr>
</table>

In-game captures were taken on Linux with an Intel Core Ultra 7 258V and its
integrated Arc 140V GPU, in widescreen. The Enhanced captures use the
Remastered presets; the other in-game captures show the performance HUD.

## Graphics

GekkoAOT can add modern rendering on top of a game without patching it. While
the game draws, opaque 3D geometry also writes view normals, depth and a
camera identity to extra targets; the game's own image, EFB copies and CPU
reads are untouched. The effects read those targets and compose a corrected
image:

| Effect | What it uses |
| --- | --- |
| Ambient occlusion and contact shadows | Captured depth and normals, per camera |
| Fragment lighting | The game's own GX lights, evaluated per pixel |
| Sun and sky light, sun cascades | The game's directional light, or a configured sun on baked scenes |
| Indirect light and reflections | The visible scene, on surfaces with GX reflection evidence |
| Particle lights | Additive and alpha-over particles, on the GPU |
| Materials | Normals, roughness and height generated from each texture |
| Textures | Real-ESRGAN or ESPCN reconstruction, cached on disk |

Nothing is keyed on a game: effects apply where the command stream proves
they are valid. Pre-rendered backgrounds and interface layers keep their
original look. Presets range from **Original** to **Remastered** and
**Remake — AI textures**. Details are in [Graphics](docs/GRAPHICS.md).

## Compatibility

38 titles are part of the regular test set. The table below summarizes the
furthest state reached in development testing; the
[compatibility list](docs/COMPATIBILITY.md) has per-game notes and known issues.

| State | Games |
| --- | --- |
| Gameplay | 22 |
| Cutscene | 5 |
| Menu | 8 |
| Boots | 2 |
| Untested | 1 |

Games that reach gameplay: Animaniacs: The Great Edgar Hunt, Call of Duty: Finest Hour, Disney/Pixar Cars, Driven, GoldenEye: Rogue Agent, Harry Potter and the Chamber of Secrets, Harry Potter and the Prisoner of Azkaban, Harry Potter and the Sorcerer's Stone, Killer7, Luigi's Mansion, Mario Kart: Double Dash!!, Medal of Honor: Frontline, Nintendo Developer Demo, Resident Evil, Shadow the Hedgehog, Star Fox Adventures, Super Mario Strikers, Super Mario Sunshine, Super Monkey Ball, Super Monkey Ball Adventure, Tales of Symphonia and Terminator 3: The Redemption.

Results describe the tested scenes only. Later levels, transitions and
features such as movies can still fail. A successful compilation is not
counted as a launch.

## Download

Prebuilt packages are published on the
[Releases](https://github.com/ant0-blase/GekkoAOT/releases) page.

| Package | Contents |
| --- | --- |
| Windows x86-64 (ZIP) | Portable. Qt, platform plugins, the Visual C++ runtime, prebuilt DolRecomp/AuroraGX and the compiler toolchain. |
| Linux x86-64 (AppImage) | Single file. Qt, application libraries, prebuilt engine and the compiler toolchain. |
| Linux x86-64 (tar.gz) | Portable directory tree. |

Playing a game does not require a separately patched DolRecomp or Aurora
checkout.

**Requirements**

- A 64-bit x86 CPU. More cores shorten the first compilation of a game.
- A GPU with Vulkan, Direct3D 12 or OpenGL 4.4 support.
- Disc images you dumped yourself: ISO, GCM, RVZ, WIA, WBFS or GCZ.

The first launch of a game compiles it. In Fast mode this takes from about a
minute to several minutes depending on the game and your CPU; Full mode
produces faster code and takes longer. Later launches reuse the compiled
module.

### Building from source

Linux needs CMake, Ninja, Git, Qt 6 Widgets, a C/C++ compiler and LLVM 19 or
20:

```sh
./build-linux.sh
./bin/gekkoaot
```

Windows uses Visual Studio 2022 (MSVC). The helper can install the pinned
Qt 6.8.3 MSVC 2022 x64 package under `.deps/Qt/` when Qt is missing:

```bat
build-windows.cmd
```

Discord Rich Presence is optional and needs the Discord Social SDK at build
time. Details are in [Building](docs/BUILDING.md) and
[Launcher](docs/LAUNCHER.md#discord-rich-presence).

## Usage

### Launcher

1. Start GekkoAOT and choose **Import folder** to add the directory that holds
   your disc images, or open a single image with **File → Open Game**.
2. Select a game and press **Play**. GekkoAOT reads the disc, compiles the game
   if needed and starts it.
3. Use **Compile** to prepare a game in the background without starting it.
   The **Compilations** panel shows the queue.

**TV mode** (from the title bar, or **Session → TV Mode**) switches to a
full-screen interface that can be driven entirely with a controller. Pressing
**Desktop** returns to the window layout; a running game keeps playing.

Settings cover compilation mode, graphics (API, aspect ratio, anti-aliasing,
texture filtering, presets, Enhanced Renderer, texture reconstruction,
post-processing), frame rate and the performance HUD, controllers, memory
cards, cheats, texture packs and Discord Rich Presence. Games with a game pack also get a
**Game Enhancements** menu for their own options. A new configuration enables
NativeOS and NativeHW with parallel workers; saved settings are kept.

### Command line

The `gekkoaotctl` controller runs the same pipeline as the launcher:

```sh
./bin/gekkoaotctl status
GEKKOAOT_ISO=/path/to/game.rvz ./bin/gekkoaotctl inspect   # disc and executable information
GEKKOAOT_ISO=/path/to/game.rvz ./bin/gekkoaotctl compile   # compile only
GEKKOAOT_ISO=/path/to/game.rvz ./bin/gekkoaotctl run       # compile if needed, then play
GEKKOAOT_ISO=/path/to/game.rvz ./bin/gekkoaotctl pgo       # profile-guided optimization run
./bin/gekkoaotctl clean
```

NativeHW and its nine device services are enabled by default for direct
runtime launches. `GEKKOAOT_NATIVE_HW=0` selects the compatibility profile, and
`GEKKOAOT_NATIVE_HW_<DEVICE>=0` disables a single device.

## Architecture

```text
                          GameCube disc
                               │
                               ▼
                         encounter/nod
                ┌──────────────┼──────────────┐
                ▼              ▼              ▼
             boot.bin       fst.bin     main.dol · REL · ELF
                                              │
                                              ▼
                                  DolRecomp + LLVM (AOT)
                                              │
                                              ▼
                                  per-game native module
                                              │
                                              ▼
                                   GekkoAOT native runtime
                ┌──────────────────────┼──────────────────────┐
                ▼                      ▼                      ▼
             NativeOS           hardware and DSP          NativeVFS
                └──────────────────────┼──────────────────────┘
                                       ▼
                             NativeGX → AuroraGX
                                       ▼
                          Vulkan · Direct3D 12 · OpenGL
```

The Qt launcher builds the launch configuration and starts `gekkoaotctl`. The
controller extracts the disc, compiles what is missing, then starts the native
runner with the game module and the AuroraGX renderer plugin. Python is only
used by optional development tooling such as adaptive PGO and CrossGameDB
helpers.

More detail: [Architecture](docs/ARCHITECTURE.md) · [Graphics](docs/GRAPHICS.md) ·
[Performance](docs/OPTIMIZATIONS.md)

## Technical details

GekkoAOT is split into a static compiler path and a native runtime path. The
goal is to move as much work as possible out of run-time emulation and into
ahead-of-time translation and native host services.

### PowerPC to native code

```text
main.dol / REL / ELF code
          │
          ▼
  PowerPC control-flow and register analysis
          │
          ▼
  DolRecomp LLVM backend
          ├─ static functions, direct calls and tail calls
          ├─ native re-entry boundaries
          ├─ execution-budget barriers
          ├─ direct SDK intercept lowering
          ├─ computed-CTR fencing
          └─ PGO instrumentation and use
          │
          ▼
  LLVM IR and objects
          │
          ▼
  per-game native module with the GekkoAOT ABI
```

Ordinary known control flow becomes ordinary host control flow. A dispatcher
remains only for targets that are truly dynamic: function pointers, computed
branches, code written at run time and exception returns.

REL modules are relocated by the game at run time. GekkoAOT compiles them at
canonical addresses inside a fixed native code window, packs them densely
(757 modules for Twilight Princess) and translates addresses between the
canonical and live layouts. Imports from modules that are absent from the disc
stay unresolved, as `OSLink` leaves them, and RELs stored inside archive files
are extracted and compiled too.

When several executables share an address range, as in games that ship more
than one main ELF, the runtime identifies the image in memory by chunk hashes
and their neighbors, so a chunk the game patches at run time (the SDK VM
library's exception handlers, for example) still belongs to its image.

### Compilation modes

| Mode | IR | Code generation | Link |
| --- | --- | --- | --- |
| Full | -O3 | -O2 | ThinLTO, PGO when a profile exists |
| Balanced | none | -O2 | per object |
| Fast | none | FastISel | per object |

Each mode has its own cache. Secondary executables get the same lowering and
ThinLTO as the main DOL.

### Proof-gated code generation

Before compiling, the controller scans every executable section of a game.
FPSCR status bits are not computed when no instruction reads them, and
reservations are not cleared on each store when the game has no `lwarx`. In
microbenchmarks this makes single-precision arithmetic 1.96× faster and guest
stores 3.97× faster; 75 of 96 tested executables qualify for the first.

### Floating point

Rounding modes, FPSCR, NaN propagation and paired-single quantization follow
the Gekko; native fast paths are used only when a proof or a run-time check
shows the result is the same. Non-IEEE mode (`FPSCR[NI]`, set by `OSInit`)
maps to flush-to-zero for results only: `lfs` and `stfs` copy any 32-bit
pattern unchanged, because compilers move integers and pointers through
floating-point registers.

### Guest/host boundaries

A recompiled function cannot call into a native service without first writing
back the PowerPC state that service may observe. Native service boundaries
are explicit ABI transitions covering:

- GPRs, CR, LR, CTR and XER;
- SRR0/SRR1 around exceptions;
- GQRs, FPR0–31, paired singles PS0–31 and FPSCR;
- MSR[EE] and interrupt-visible state;
- cycle accounting before returning to the runtime.

For computed `bctr`/`bctrl` control flow, state is fenced before the target is
routed so stale host temporaries cannot leak across dynamic call edges.
Enabling MSR[EE] or returning through `rfi` ends the current native chain, so
a pending interrupt is delivered at the correct instruction.

### NativeOS and compiled SDK code

NativeOS provides host implementations of most of the GameCube OS:

- scheduler and thread selection;
- thread creation and lifecycle, including the SDK's active-thread list;
- ready and wait queues, priorities, suspend/resume and sleep/wakeup;
- mutexes, condition variables and message queues;
- alarms and timers;
- `OSContext` save, load and current context;
- FPR, paired-single and FPSCR capture, with lazy FPU ownership.

It is not an all-or-nothing switch. Small retail leaves can stay as compiled
PowerPC when their exact semantics matter more than replacing them:

```text
OSDisableInterrupts
OSEnableInterrupts
OSRestoreInterrupts
```

These run natively as compiled code anyway, while scheduler and context
services use the host implementation. Interrupt-entry boundaries keep
asynchronous events visible at the correct PowerPC instruction.

### SDK resolver

The build identifies SDK functions by structure and emits a per-game
native-intercept manifest:

```text
DOL code
   │
   ▼
SDK scanner
   ├─ exact normalized signatures
   ├─ tiny-leaf ABI proofs
   ├─ call-target and adjacency proofs
   └─ context and scheduler semantic proofs
   │
   ▼
native-intercepts manifest
   │
   ▼
compile-time direct lowering
```

The resolver fails closed: an uncertain match stays as compiled guest code.
Covered families include `OSContext`, interrupts, alarms, message queues,
mutexes and conditions, the scheduler and threads, time and tick helpers,
`PADRead`, `DVDFastOpen`, memory and string functions, cache maintenance,
`GXSetArray` and GX matrix uploads.

### Memory model

```text
MEM1                24 MiB reported, 32 MiB host backing
virtual memory      0x7E000000 + 32 MiB, flat or paged through the guest MMU
locked cache        0xE0000000, shared with locked-cache DMA
MMIO                device handlers
write-gather pipe   GX FIFO
```

Proven MEM1, locked-cache and resident paged-VM accesses use direct host
pointers. Bounds, permissions, cached and uncached aliases, page reference and
change bits and DSI/ISI restarts remain exact. An optional 4 GiB host address
reservation supports a `base + address` window; it reserves address space,
not memory, and does not remove translation checks.

```text
guest access
   ├─ MEM1 / virtual memory  → direct native memory
   ├─ MMIO                   → device register semantics
   ├─ write-gather pipe      → GX FIFO
   └─ locked cache           → locked-cache semantics
```

On Linux the main module's code is moved onto transparent huge pages to reduce
instruction-TLB misses. REL modules stay file-backed, so only the code that
actually runs is loaded.

### Hardware services

```text
CP   command processor        AI   audio interface
PE   pixel engine             DI   disc interface
PI   processor interface      EXI  external interface
MI   memory interface         SI   serial interface
VI   video interface          LC   locked cache
```

Interrupts are routed through the guest-visible PI state rather than as
arbitrary host callbacks. The EXI service also provides memory cards, the RTC,
SRAM and the IPL ROM; when no IPL is supplied it serves a boot-ROM font drawn
for GekkoAOT, so games that call `OSInitFont` start normally.

### Clocks and idle time

CPU execution and hardware time are separate:

```text
compiled CPU code   runs as fast as the host allows
time base           real time
decrementer         real time
VI                  real time
DSP / AI            real time
presentation        independent host frame scheduler
```

This keeps game logic, audio and timers at their original speed when the host
presents frames at a different rate.

Many games wait in loops: polling a clock, a hardware register or a memory
flag, yielding to no one, or servicing a stream of short timers while every
thread is idle. GekkoAOT recognizes these patterns and sleeps until the next
event that could end the wait. Clock polls are analyzed in a sandbox to find
the exact time the loop would exit, so the game observes the same values.

### NativeVFS and nod

```text
ISO · GCM · RVZ · WIA · WBFS · GCZ
              │
              ▼
        encounter/nod
              ├─ system files
              ├─ FST
              ├─ main.dol and modules
              └─ game metadata and banner
              │
              ▼
          NativeVFS
```

The controller caches extracted system files and executables per title and
reads the game partition through the native path. DI transfers complete
asynchronously, so a read is not visible to the game at the instant the host
finishes the I/O. Nested FST directories are preserved, so embedded executable
modules are found.

### AuroraGX

```text
game GX code
   ├─ recognized SDK calls → typed native commands ─┐
   └─ write-gather pipe → CP FIFO → CP / XF / BP ───┤
                                                    ▼
                                                AuroraGX → GPU
```

Recognized array and matrix calls are sent as typed commands in the same
ordered queue as the FIFO, so the renderer does not decode the equivalent
CP/XF packets again. Custom display lists and unrecognized streams keep the
FIFO path. AuroraGX caches vertex attribute plans, indexed-array windows, bind
groups and texture versions, merges compatible adjacent draws and reuses GPU
snapshots; guest writes, cache flushes and EFB copies invalidate the affected
resources.

Compatibility work in the renderer covers:

- CP/XF matrix index A/B synchronization and 6-bit PNMTXIDX/TEXMTXIDX;
- partial XF transform and normal loads;
- dynamic indexed-array coherency;
- first-use pipeline correctness;
- EFB color and depth peeks and pokes;
- EFB-to-texture and display copies;
- TEV alpha and destination alpha;
- bounding boxes;
- Z textures.

#### Z textures and depth copies

`GXSetZTexture` is implemented with the Z8, Z16 and Z24X8 source formats and
the `GX_ZT_ADD` and `GX_ZT_REPLACE` operations. EFB depth copies support Z4,
Z8, Z16, Z24X8, Z8M, Z8L, Z16R and Z16L. Several retail games depend on these
depth paths, which simpler GX implementations often miss.

### Widescreen

The EFB stays 4:3 and presentation is widened. Each draw is classified:

| Content | Treatment |
| --- | --- |
| Perspective 3D | Widened horizontally (hor+) |
| Orthographic menus and HUDs | Kept at 4:3, centered |
| Fades, letterbox bars, screen fills | Stretched to the full width |
| Screen copies for bloom or blur, region restores | Original width |
| Small HUD viewports with live 3D | Moved with the UI |
| Frames on a pre-rendered background | Presented in 4:3 |

No per-game patch is involved.

### Presentation and HUD

After scan-out, optional post-processing applies sharpening, bloom, filmic tone
mapping, local contrast, vignette, grain and color adjustments. The
performance HUD is drawn by the renderer plugin in the same pass. It reads
frame times, the game and VI rates, game speed and CPU use published by the
runtime, and GPU load from DRM (Linux), sysfs (AMD), NVML (NVIDIA) or PDH
(Windows).

### DSP and audio

```text
game audio code
   ├─ AX HLE      VPB voices, ADPCM/PCM, resampling, mixer and sends
   ├─ JAudio HLE  AFC/PCM voices, filters, command processing
   └─ DSP LLE     unknown microcode
```

AX and recognized JAudio workloads are decoded and mixed natively; unknown
microcode runs on a standalone DSP core. DVD audio streaming (DTK ADPCM) is
mixed into the host output.

Movies play through the game's own decoders, compiled like the rest of the
game.

### Host threads

```text
main thread       compiled game code, NativeOS, MMIO, interrupt delivery
GX worker         FIFO and native command decoding, AuroraGX submission
render worker     command encoding, Enhanced passes and presentation
compute           neural texture reconstruction
audio             DSP, mixer, host audio
I/O workers       nod/DVD, shader compilation, caches
```

These are logical roles. GekkoAOT does not pin threads to cores unless
profiling shows a benefit.

### Runtime fast paths

Performance work removes layers before it tunes instructions:

- bounded native superchains and fewer dispatcher round trips;
- compile-time pruning of intercept lookups;
- RAM-first guest pointer paths;
- O(1) NativeOS ready-thread caches;
- batched host clock sampling;
- write-gather pipe fast paths;
- static direct-call lowering;
- idle and busy-wait parking;
- precomputed draw runs and per-page texture write tracking;
- LTO/IPO, LLVM PGO, renderer PGO and CrossGameDB profile reuse.

Fast paths stay behind architectural barriers wherever interrupts, MMIO,
native service boundaries or timing can become visible.

## PGO and profiling

With the LLVM backend, GekkoAOT can build an instrumented module, run
representative gameplay and merge the profiles with `llvm-profdata`:

```text
AOT module → instrumented build → gameplay → .profraw → llvm-profdata merge → PGO build
```

Profiles are stored per game and reused automatically by later builds.
CrossGameDB collects architecture-neutral information to find hot paths and
compatibility patterns shared across games.

Keep raw captures and reports under `.gekkoaot/diagnostics/`; they are not
release files. When comparing runs, use the same launcher arguments, native
options, graphics settings, scene and save.

## Reproducibility

| Component | Pin |
| --- | --- |
| DolRecomp | `71ce7f97419b1bb1ba9a9596c41507f6629e0fb0` |
| Aurora | `9c0bf66f1ed3276b60ad1cd746e2fb48818a6298` (overridable) |
| encounter/nod | `v2.0.0-alpha.12` |

Changes to upstream components are kept as patches and overlay scripts in the
repository and checked with `git apply --check` before they are applied.
Mutable sources, builds, caches and profiles live outside the installed,
read-only resources.

## GekkoTranslate and whole-program reconstruction

The DolRecomp/LLVM path is the practical foundation. GekkoTranslate is the
longer-term work that moves from instruction translation toward
reconstructing the whole program.

### Direct LLVM path

```text
PowerPC → GekkoAOT IR → LLVM IR → -O3 / LTO / PGO → native x86-64 or ARM64
```

This remains the lowest-level, fidelity-oriented path and does not involve
generated C++.

### Reconstructed C++ path

```text
main.dol + REL modules
   │
   ▼
PowerPC disassembly
   │
   ▼
CFG, data-flow and ABI analysis
   │
   ▼
whole-program reconstruction
   ├─ functions and direct calls
   ├─ loops, if and switch
   ├─ callbacks and function pointers
   ├─ SDA/SDA2 usage
   └─ types and structures where provable
   │
   ▼
portable C++ → Clang/LLVM → native executable
```

Generated C++ is an independent reconstruction of the executable's behavior,
not recovered proprietary source code.

### Reconstruction levels

| Level | Input | Result |
| --- | --- | --- |
| 1 | PowerPC instructions | Equivalent native/LLVM semantics |
| 2 | Functions and CFG | Loops, if/else, switch, direct calls |
| 3 | Whole program | DOL/REL relationships, callback targets, resolved function pointers, inferred types, native SDK interfaces, readable C++ |

Level 3 is deliberately ambitious and is not required for AOT compatibility.

### Whole-program DOL + REL

```text
main.dol
   ├─ REL A
   ├─ REL B
   ├─ REL C
   └─ runtime-loaded references
        │
        ▼
global symbol database → global call graph → GekkoAOT IR
        │
        ├─ direct LLVM
        └─ reconstructed C++
        │
        ▼
whole-program optimization
```

The more indirect calls can be proven statically, the less run-time routing
is needed.

### Removing the guest dispatcher

An early AOT runtime still does:

```text
guest PC → lookup → host function pointer → AOT function
```

For known code the target is a direct native call. The dispatcher should
eventually remain only for unresolved indirect branches, runtime-loaded REL
targets, unproven function pointers, dynamic callbacks, exception vectors,
unsupported code and diagnostic fallbacks.

### GCINE

GCINE is the name reserved for the long-term native GameCube compatibility
environment below the recompiled game logic:

```text
                    GCINE
      ┌───────────────┼───────────────┐
      ▼               ▼               ▼
      OS          hardware           I/O
   threads        PI · MI            DVD
   mutexes        VI · AI            SI
   queues         DSP · GX           EXI
   alarms                            input
   timers
   interrupts
```

| SDK family | Native replacement |
| --- | --- |
| `OS*` | Scheduler, synchronization and timers |
| `GX*` | AuroraGX |
| `DSP*` | Native DSP, HLE and LLE |
| `AI*` | Native audio |
| `DVD*` | nod and asynchronous I/O |
| `SI*` | Native input |
| `EXI*` | Native device services |

The host implementation can differ completely inside, as long as the
GameCube-visible contract each game relies on is preserved.

## Roadmap

```text
GameCube binaries
      │
      ▼
GekkoTranslate
      ├─ direct LLVM backend
      └─ reconstructed portable C++
      │
      ▼
native code
      ├─ GCINE / NativeOS / native hardware
      ├─ AuroraGX
      └─ native DSP and audio
      │
      ▼
host OS
```

Main goals:

- reduce dispatcher and runtime translation overhead;
- resolve known calls statically;
- extend native SDK and hardware coverage;
- reconstruct DOL and REL modules as one program;
- add a canonical GekkoAOT IR;
- support both the direct LLVM and reconstructed C++ outputs;
- target x86-64, ARM64 and later WebAssembly/WebGPU;
- decouple the frame rate from game logic;
- start any game within a minute on a cold cache.

The full plan is in the [roadmap](docs/ROADMAP.md).

## Documentation

| Document | Contents |
| --- | --- |
| [Compatibility](docs/COMPATIBILITY.md) | Per-game status, known issues and how to report a problem |
| [Building](docs/BUILDING.md) | Building from source on Linux and Windows |
| [Architecture](docs/ARCHITECTURE.md) | Compiler, runtime and process model |
| [Graphics](docs/GRAPHICS.md) | Enhanced Renderer, texture reconstruction, presets, widescreen, frame-rate unlock |
| [Performance](docs/OPTIMIZATIONS.md) | Performance features, options and measurement |
| [Environment variables](docs/ENVIRONMENT.md) | Every `GEKKOAOT_*` variable, its default and its readers |
| [Launcher](docs/LAUNCHER.md) | Compilation modes, memory cards, Discord Rich Presence, platforms |
| [Mods](docs/MODS.md) | Cheats and texture packs |
| [Testing](docs/TESTING.md) | Tests, compatibility laboratory, frame capture, profiling |
| [Roadmap](docs/ROADMAP.md) | Planned work |
| [Clean-room policy](docs/CLEAN_ROOM.md) | Source separation rules |
| [Changelog](CHANGELOG.md) | Release history |

## Repository layout

<details>
<summary>Show the repository structure</summary>

```text
src/              Qt launcher (desktop and TV interfaces)
tools/            gekkoaotctl controller and development tools
runtime/native/   native runner, module loader, disc path, game-library framework
runtime/module/   per-game AOT module linker
runtime/os/       NativeOS
runtime/sdk/      SDK recognition and native services
runtime/hw/       GameCube hardware: register blocks and host device services
runtime/gx/       NativeGX, AuroraGX bridge, Enhanced Renderer, widescreen, HUD
runtime/dsp/      DSP and audio
runtime/perf/     frame capture and sampling
patches/          pinned upstream patches
crossgame-db/     compiler hints and structural metadata
game-packs/       per-game configuration
assets/           logo, fonts, icons, screenshots
docs/             documentation
```

</details>

## Clean-room policy and legal

GekkoAOT is written from documentation, executable analysis, hardware
behavior, public research and behavioral testing. Nintendo source code,
libraries, object files and copyrighted SDK resources are not part of the
project. The planned C++ backend reconstructs equivalent behavior from
executables; it does not reproduce original source code. See the
[clean-room policy](docs/CLEAN_ROOM.md).

GekkoAOT does not include games, console firmware or SDK files. You need your
own legally obtained disc images. Games are compiled locally on your machine,
and the generated code is never uploaded or distributed. SDK functions are
recognized from non-reversible fingerprints, not stored code. The replacement boot-ROM font used when no IPL ROM is
supplied was drawn for GekkoAOT and contains no Nintendo data.

GameCube is a trademark of Nintendo. This project is not affiliated with or
endorsed by Nintendo. Game titles and screenshots are shown to document
compatibility and belong to their respective owners.

## License

GekkoAOT is licensed under the [GNU General Public License v3.0 or later](LICENSE).
It builds on [DolRecomp](https://github.com/ExpansionPak/DolRecomp),
[Aurora](https://github.com/encounter/aurora), [nod](https://github.com/encounter/nod),
LLVM, SDL3 and Qt. See the [third-party notices](THIRD_PARTY_NOTICES.md) for
their licenses.

---

<p align="center">
  <a href="https://github.com/ant0-blase/GekkoAOT/releases">Releases</a> ·
  <a href="docs/COMPATIBILITY.md">Compatibility</a> ·
  <a href="docs/ROADMAP.md">Roadmap</a> ·
  <a href="CONTRIBUTING.md">Contributing</a>
</p>
