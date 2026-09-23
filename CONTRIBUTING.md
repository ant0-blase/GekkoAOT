# Contributing to GekkoAOT

GekkoAOT is an experimental GameCube static-recompilation and native-runtime project. Contributions are welcome when they keep the repository reproducible, legally clean and honest about compatibility.

## Before opening a change

- Do not commit game images, extracted commercial game assets, Nintendo SDK binaries/libraries, firmware dumps or other proprietary redistributable content.
- Keep upstream modifications as reviewable patches under `patches/` unless there is a clear architectural reason to move code into GekkoAOT itself.
- Prefer generic runtime fixes over title-specific address hacks.
- Do not mark a title `playable` from a boot/menu result.
- Keep compatibility observations reproducible: include disc ID/region, GekkoAOT revision, host OS/architecture and the furthest state reached.

See [docs/CLEAN_ROOM.md](docs/CLEAN_ROOM.md) and [docs/COMPATIBILITY.md](docs/COMPATIBILITY.md).

## Build before submitting

Linux:

```bash
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DGEKKOAOT_BUILD_GUI=ON \
  -DGEKKOAOT_BUILD_NATIVE_HOST=ON \
  -DGEKKOAOT_NATIVE_NOD=ON
cmake --build build --parallel
```

Windows builds are checked by CI with Visual Studio 2022 x64.

At minimum, verify that changed JSON is valid and that relevant shell/Python tooling still parses.

## Commit messages and releases

The repository uses **Conventional Commits** so Release Please can prepare the next version and changelog automatically.

Examples:

```text
fix(runtime): preserve VI state across native boundary
feat(dsp): add native AX voice path
perf(aot): reduce dispatcher returns in hot loops
docs(compat): update GHSE69 menu status
build(ci): deploy Qt runtime in release archives
```

Common types are `feat`, `fix`, `perf`, `refactor`, `docs`, `build`, `ci` and `test`.

Breaking changes should include `!` or a `BREAKING CHANGE:` footer.

Do not manually create routine version tags. The release workflow and Release Please are documented in [docs/RELEASING.md](docs/RELEASING.md).

## Compatibility reports

Use the compatibility issue template when possible. Useful evidence includes:

- six-character GameCube disc ID;
- region/revision;
- exact GekkoAOT commit;
- host OS and CPU architecture;
- backend/settings that differ from defaults;
- last visible state (`Boot`, `Menu`, `In-game`, etc.);
- concise logs around the first failure;
- screenshot/video only when it contains no copyrighted material beyond what is reasonably necessary to demonstrate the bug.

## Pull requests

Keep each pull request focused. In the description, state:

1. what changes;
2. why it is needed;
3. which titles or demos were tested;
4. whether it changes compatibility, performance, generated data or licensing/provenance assumptions.
