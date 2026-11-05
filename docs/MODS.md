# Mods

GekkoAOT loads Gecko data-write cheats and texture packs per game ID. Mods
work with the normal `gekkoaotctl run` workflow and do not need
`GEKKOAOT_NATIVE_ONLY`.

## Files

Put the files at `<GekkoAOT root>/mods/<SIX_CHARACTER_GAME_ID>/`:

```text
mods/
  GHSE69/
    cheats.txt
    textures/
      tex1_64x64_0123456789abcdef_6.png
      tex1_64x64_0123456789abcdef_6_mip1.png
```

The sample filename is only illustrative and will **not** match a real game
texture unless its hash, dimensions, and format match exactly.

The `gekkoaotctl` controller resolves the project-root path and forwards it to
the native runner automatically. Standalone `gekkoaot-native-run` resolves
`mods/<GAMEID>/` relative to its working directory, or you can pass explicit
`--cheats FILE` and `--texture-pack DIRECTORY` arguments.

Overrides (also useful in the GUI):

```bash
export GEKKOAOT_MOD_ROOT=/absolute/path/to/mods
# Optional individual overrides:
export GEKKOAOT_CHEATS_FILE=/absolute/path/to/cheats.txt
export GEKKOAOT_TEXTURE_PACK_DIR=/absolute/path/to/textures
```

An absent mod directory is skipped with no change to normal games.

## Cheats

`cheats.txt` holds unencrypted Gecko write codes (RAM and DOL TEXT) as hex pairs:

```text
# Example values only. Replace with codes verified for your game/revision.
$My data edits
04000100 12345678
02000200 0000ABCD
00000300 000000EF
06000400 00000009
11223344 55667788
99000000 00000000
```

Supported: `00` write 8-bit, `02` write 16-bit, `04` write 32-bit,
`06` write N raw bytes (payload packed in subsequent 64-bit lines).
All write targets are restricted to mapped, retail MEM1 data addresses;
big-endian byte layout is preserved. The entire file is validated at load
before enabling any code. Writes are checked once at each VI frame boundary;
unchanged bytes are skipped and actual changes invalidate NativeGX resource
caches using the existing write notification.

**AOT instruction patching:** if an enabled 32-bit (`04`) or full-word
`06` write targets the main DOL's TEXT segment, `gekkoaotctl` creates a
content-addressed patched DOL copy and compiles its PowerPC instructions with
DolRecomp. The extracted original DOL is never modified. The native module and
LLVM cache identities change with patched contents. On launch look for:
`GEKKOAOT_CHEATS_AOT=1 instructions=N`.

**Live cheat editing:** editing cheats from the GUI while a game is running
reloads simple RAM writes within ~30 VI frames. The previously written RAM
values are not rolled back automatically if a cheat is turned off. PPC changes
already compiled by LLVM **cannot** be safely hot-swapped: restart the game
so the controller recompiles an updated native module. No extra manual cache
cleaning is needed.

**Not supported yet:** encrypted Action Replay, Ocarina `.gct` binaries,
Gecko conditionals/pointers, write-repeat fields, Gecko assembly hooks,
code residing in REL or dynamically generated executable RAM, and replacing
native machine code while a game is running. Guest RAM writes alone cannot
replace AOT-compiled instructions. Unsupported code types are rejected with a file/line
error instead of silently doing something incorrect. Only load codes for the
exact game build; incorrect addresses can corrupt game state or saves.

## Texture packs

Uses **Aurora's existing** texture replacement registry and asynchronous
PNG/DDS loader, with its own source-content hash matching and cache.
Nothing is copied to guest memory; files are registered once when the Aurora
bridge starts and removed on shutdown.

Accepted filenames follow Aurora's `tex1_` replacement convention, such as
`tex1_{width}x{height}_{textureHash}_{format}.png` for non-paletted textures,
or a palette-hash variant for indexed textures. The texture hash is computed
by Aurora from the original GameCube texture data. Packs from other tools
(e.g. Dolphin) are not automatically compatible if they use different hashes
or formats; their assets must be renamed/reindexed appropriately.
Mip sidecars can be supplied as `_mip1`, `_mip2`, etc. Aurora handles image
decoding and asynchronous GPU uploads. An empty/incorrectly named pack
registers zero entries and leaves original textures intact.

Note: an error from `--cheats` aborts launch; invalid texture-pack directory
from `--texture-pack` also aborts. Missing default mod folders are optional.

## Validation

The `mods.gecko-aot-overlay` and `mods.cheat-hot-reload` CTests check
compile-time DOL text patching and hot-reload/preservation semantics.
The standalone `native.cheat-data` CTest checks correct Gecko parsing,
endianness, bulk writes, idempotency and failure handling. Full texture-pack
render verification requires Aurora/WebGPU and a matching game texture source;
a compiler-only test cannot establish a visible replacement.

## Launcher

Open **Menu / Settings → Cheat codes** or **Menu / Settings → Texture packs**.
Both dialogs share the Memory Card Manager's Shell UI, controller navigation,
file/folder picker, game selector, and animations.

- **Cheat codes:** select the game, add/edit Gecko 00/02/04/06 data-only hex
  pairs, import an existing `cheats.txt`, or check/uncheck individual named
  cheats. Entries are saved in `mods/<GAMEID>/cheats-gui.json` and only enabled
  cheats are exported to the runtime's `cheats.txt`. No cheats are injected
  until the game is next started. Existing plain `cheats.txt` entries are
  imported into the GUI on first open when no GUI metadata exists.
- **Texture packs:** add multiple existing PNG/DDS pack folders, check/uncheck
  them, import `tex1_*.png` / `tex1_*.dds` files to the local pack, preview PNG
  samples, and open their folder. The GUI saves `texture-packs-gui.json` plus
  `texture-packs.txt` (one enabled directory per line). Aurora registers the
  enabled directories and streams image data asynchronously. Packs may be
  outside the project; the GUI remembers absolute folder paths, **not** copies.
  A manifest with no enabled paths means texture replacements are disabled.
  Removing a pack from the GUI never deletes its files.

**Compatibility:** the old `mods/<GAMEID>/textures/` single-folder convention
still loads automatically if there is no `texture-packs.txt` manifest. The
new `--texture-pack-list FILE` option (or `GEKKOAOT_TEXTURE_PACK_LIST`) takes
precedence over the old `--texture-pack DIRECTORY`. The launcher supplies the
per-game manifest when present. The GUI changes take effect on the next launch
and do not require `NATIVE_ONLY=1`.

**Constraints:** cheats cannot patch native AOT instructions through Gecko
assembly hooks; only the existing data-write engine is supported. Texture
names must follow Aurora's content hash naming; Dolphin packs may need
conversion. Large PNG/DDS files remain subject to Aurora's texture streaming
and GPU memory limits.
