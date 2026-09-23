# Native VFS / DVD path

GekkoAOT v0.0.1 owns the GameCube disc path used by the standalone runtime.

- `gekkoaot-native-disc` uses **encounter/nod** to read supported GameCube disc/container formats.
- The GUI/controller caches `boot.bin`, `bi2.bin`, `main.dol`, `fst.bin` and executable metadata below `<state>/cache/discs/`.
- `gekkoaot-native-run` receives the original disc image with `--disc-image` plus the extracted system files needed by the current runtime path.
- There is no Python VFS patch stage.
- GekkoAOT does not rewrite C/C++ sources while a game is running. Versioned DolRecomp/Aurora patches are intentionally applied earlier, during the toolchain-build stage.

For a source checkout, `<state>` is normally `./.gekkoaot`. Installed builds use the platform user cache directory unless `GEKKOAOT_STATE_DIR` is set.

The intended normal user path is **Open Game → Play** in the GUI. Compatibility remains pre-alpha and title-dependent; disc access succeeding does not imply that the selected title is playable.
