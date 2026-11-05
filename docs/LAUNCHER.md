# Launcher

The Qt launcher manages the game library, compilation, settings, memory cards,
mods and Discord Rich Presence. It starts games through `gekkoaotctl`, the
same controller the command line uses.

## Library and compilation

- **Import folder** adds a directory of disc images; **File → Open Game** adds
  one image. Banners, titles and IDs are read from the disc.
- **Play** compiles the game if needed and starts it. **Compile** prepares it
  in the background; the **Compilations** panel shows the queue.
- **Settings → Compilation** selects the compiler tier:

| Mode | Pipeline | Use |
| --- | --- | --- |
| Full (default) | LLVM -O3, ThinLTO, automatic PGO | Best performance |
| Balanced | No IR optimization, LLVM codegen -O2, no LTO | Shorter first compilation |
| Fast | No IR optimization, FastISel, no LTO | Shortest first compilation |

Each mode has its own cache, so switching back does not recompile. Fast code
runs noticeably slower on the main thread; use it to start a game quickly,
not to measure performance.

## TV mode

**TV mode** (title bar, or **Session → TV Mode**) is a full-screen interface
driven by a controller, keyboard or mouse, with Library, Recent and Settings.
**Desktop** returns to the window layout; a running game keeps playing.

## Settings

- **Graphics:** API, aspect ratio, resolution, anti-aliasing, texture
  filtering, presets and every Enhanced Renderer option. See
  [Graphics](GRAPHICS.md).
- **Frame rate:** FPS limit, VSync, the frame-rate unlock and the performance
  HUD items.
- **Controllers:** keyboard, gamepad or joystick per port, remapping, dead
  zones and axis inversion.

A new configuration enables NativeOS and NativeHW with parallel workers.
Saved settings are kept across versions; migrations run once.

## Memory cards

Open **Menu → Memory cards** (desktop) or **Settings → Memory cards** (TV
mode). Cards live under `<state>/user/<game>/cards`.

- RAW and GCP images: open, create, import and delete saves, export.
- MCI images: open and edit with their 64-byte wrapper preserved.
- GCI, GCS and SAV saves: inspect, import, export and convert; formats are
  detected from contents, not extensions.
- New cards of 59 to 2043 blocks, Western or Japanese encoding.
- Titles, comments, dates, banners and animated icons are read from the saves.

The manager validates the header, both directory and BAT copies, allocation
chains and free counts, and never formats or repairs a corrupt image
implicitly. Each edit stores the previous version in `<card>.backups/` and
replaces the file atomically. Cards in use by the running game are read-only.
F-Zero GX and PSO system saves get their card-serial binding updated on
import, using routines adapted from Dolphin's GCMemcard implementation (see
[third-party notices](../THIRD_PARTY_NOTICES.md)).

## Cheats and texture packs

**Cheat codes** and **Texture packs** share the memory card manager's
interface. See [Mods](MODS.md).

## Discord Rich Presence

The launcher uses the official Discord Social SDK 1.10+ on Windows, Linux and
macOS. The public application ID is `1560710093606228101`; sharing and an ID
override are in **Settings → Discord Rich Presence**.

The activity shows the game's title, session time and paused state, or
compilation and library browsing otherwise. File names, paths, arguments and
logs are never sent. Updates are coalesced and retried with backoff after a
Discord restart.

The SDK is optional at build time; see [Building](BUILDING.md#discord-rich-presence).
Builds without it report the feature as unavailable.

## Platforms

| Platform | Status |
| --- | --- |
| Linux (X11, XWayland) | Built and tested, including embedded game windows, TV mode and controller navigation. |
| Windows | Built by CI with MSVC. Embedded windows and icons have native code paths. |
| macOS | Dependencies resolve; games use a separate SDL window. Not built in CI. |

The launcher embeds Google Sans Flex (weight 500) and Doto, with their OFL
licenses available from **Appearance**.

## Tests

```sh
cmake --build build --target gekkoaot gekkoaot-test-launcher-ui \
  gekkoaot-test-living-room gekkoaot-test-shell-gamepad \
  gekkoaot-test-compilation-queue gekkoaot-test-graphics-dialog \
  gekkoaot-test-input-dialog
ctest --test-dir build --output-on-failure \
  -R '^(launcher\.(ui|living-room|shell-gamepad|compilation-queue)|graphics.dialog|input.dialog)$'
```
