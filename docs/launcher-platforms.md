# Launcher platform validation

The launcher embeds the unmodified Google Sans Flex variable font and selects weight 500 (Medium). It loads the font with Qt's application-font API, independently of installed system fonts. Doto remains the display font for dotted headings. The Google Sans Flex OFL, trademark notice and source information are embedded in Qt resources; the OFL is also available from Appearance. Installed desktop packages include these notices under `share/licenses/GekkoAOT/fonts`.

Current validation, 16 October 2026:

Discord Rich Presence uses a shared activity service and the optional official
Social SDK, with Windows/Linux/macOS deployment and an Android AAR/Prefab
backend. The public project Application ID is embedded in packaged builds.
SDK-free builds report the feature unavailable. See [setup and validation](discord-rich-presence.md).
Backend availability does not establish that a platform port has been tested.
Linux has additionally passed a real Discord Social SDK 1.10.19337 publication
and shutdown smoke test. The installed package loads the SDK from its own
`bin` directory through a relative runtime path.

| Platform | Evidence and limits |
| --- | --- |
| Linux / X11 / XWayland | Local Qt/SDL controller tests, compilation queue fixtures, and real cached Luigi's Mansion gameplay in an isolated GPU-backed Gamescope session. Includes fullscreen, dragging, Graphics previews, Home/resume, and TV → separate desktop game window + launcher, preserving the native rendering surface. |
| Windows | Existing CI builds this target. Win32 embedding, executable icons and portable Qt font loading have implementation paths. This session has not executed a Windows build or a Windows gameplay test. |
| macOS | Native runtime dependency selection includes macOS. The GUI uses Qt and bundled fonts; gameplay currently falls back to a standalone SDL window instead of the Windows/Linux embedded host. Not compiled or executed in this session. |
| Android | No Android application/package or supported native dependency bundle is configured here. Qt font resources are portable; a functional Android launcher/runtime needs a separate platform port and device tests. |

These checks do not establish 100% compatibility across operating systems, controllers, compositors or games. Cached-game smoke tests exercise the real runtime and presentation without recompiling an entire game.

## Local checks

```sh
cmake --build build --target gekkoaot gekkoaot-test-launcher-ui gekkoaot-test-living-room gekkoaot-test-shell-gamepad gekkoaot-test-compilation-queue gekkoaot-test-graphics-dialog gekkoaot-test-input-dialog
ctest --test-dir build -R '^(launcher\.(ui|living-room|shell-gamepad|compilation-queue)|graphics.dialog|input.dialog)$' --output-on-failure
```

`gekkoaot-test-launcher-ui --real-game WORK_DIRECTORY` is an optional integration mode. It requires an isolated GUI/controller executable directory, `WORK_DIRECTORY/config`, `WORK_DIRECTORY/state`, a selected cached game, and Gamescope/xdotool. Use a separate user directory and do not point it at personal memory cards. It checks a desktop launch and a TV launch, previews Graphics settings, verifies the same native render surface across presentation transitions, and stops both game sessions.
