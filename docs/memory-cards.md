# Memory cards

Open **Menu → Memory cards** on desktop, or **Settings → Memory cards** in TV mode. The manager discovers GekkoAOT’s per-game cards under `<state>/user/<game>/cards`, starts with the selected game’s card, and can open external files. New cards default to the selected game’s `MemoryCardA.raw` if it does not exist.

## Formats and operations

- RAW and GCP card images: open, create, import/delete saves, export a whole image.
- MCI images: open and edit while preserving their original 64-byte wrapper; export their contents to RAW/GCP. A new proprietary MCI wrapper is not synthesized.
- GCI, GameShark GCS and MaxDrive SAV individual saves: open, inspect, import, export and convert. Detection checks container contents and size, rather than trusting the extension.
- Create 59, 123, 251, 507, 1019 or 2043-block cards, with Western or Japanese encoding. GekkoAOT’s current native game service uses 251-block cards; other sizes can be managed and exported for external tools/hardware.
- Export one or several selected saves, export every save to a folder, or back up the complete card. Batch exports choose unique names rather than overwrite existing files.

The UI reads titles, comments, game/maker IDs, modification dates, block counts, RGB5A3/CI8 banners and animated icons from save data. Game-library titles supplement the embedded save names. Japanese names use an embedded Shift-JIS mapping, without relying on an OS-specific codec. No online account or metadata service is required.

## Editing

The manager validates the header, both directory/BAT checksums, allocation chains, ownership and free-block counts. It selects the newest consistent filesystem pair, and does not format or repair a corrupt image implicitly. It stages a complete edit before writing; duplicates require an explicit Replace action. F-Zero GX and PSO system saves have their card-serial binding/checksum updated when imported.

Before replacing an image, its current on-disk SHA-256 must match the file that was opened. Each successful edit first stores a byte-for-byte previous version in `<card-file>.backups/`, then uses Qt’s atomic file replacement. Read-only cards and cards used by the current running game cannot be edited. This does not coordinate writes by unrelated third-party programs; close other card editors before editing the same image.

## Interoperability references

The implementation uses the documented GameCube filesystem and save containers, with save-serial rebinding adapted from the GPL-compatible Dolphin project:

- [Dolphin GCMemcard structures and texture formats](https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/Core/HW/GCMemcard/GCMemcard.h)
- [Dolphin GCI/GCS/SAV container layouts](https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/Core/HW/GCMemcard/GCMemcardUtils.cpp)
- [GCMM format descriptions, including MCI](https://github.com/GameCubeHomebrew/homebrew-gcmm)
- [YAGCD memory-card filesystem](https://www.gc-forever.com/yagcd/)

No game save files, Nintendo artwork, or proprietary card contents are distributed with GekkoAOT. Tests generate their own save/art fixtures.
