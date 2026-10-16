# Building GekkoAOT

CI builds and tests two targets:

- Linux x86-64;
- Windows x86-64 with Visual Studio 2022 (MSVC).

Other hosts may work but are not part of the release matrix. See
[launcher platforms](launcher-platforms.md) for what has been tested where.

## Requirements

- Git and CMake 3.25 or newer;
- a C++20 compiler;
- LLVM 19 or 20 development files (used to build DolRecomp);
- Qt 6.5 or newer for the launcher (`GEKKOAOT_BUILD_GUI=ON`);
- network access on the first build, to fetch the pinned dependencies;
- Python 3 only for optional tooling (adaptive PGO, CrossGameDB, the Windows
  Qt installer).

The controller and runtime are C/C++. Python is never part of the CPU, GX or
DSP paths.

## Linux

Install the dependencies (package names vary by distribution):

```sh
# Arch Linux
sudo pacman -S --needed base-devel cmake ninja git qt6-base llvm python
```

Build and install into `bin/`:

```sh
./build-linux.sh
./bin/gekkoaot
```

Or configure manually:

```sh
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DGEKKOAOT_BUILD_GUI=ON \
  -DGEKKOAOT_BUILD_NATIVE_HOST=ON
cmake --build build --parallel
```

A core-only build, as used by CI:

```sh
cmake -S . -B build-core -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DGEKKOAOT_BUILD_GUI=OFF \
  -DGEKKOAOT_BUILD_NATIVE_HOST=ON \
  -DGEKKOAOT_NATIVE_NOD=OFF
cmake --build build-core --parallel
```

## Windows

You need Windows 10 or 11 (x86-64), Visual Studio 2022 or its Build Tools with
**Desktop development with C++**, CMake and Git. Ninja is not required: the
helper uses the Visual Studio generator.

```bat
build-windows.cmd
```

If Qt is not found through `GEKKOAOT_QT_DIR`, the first run installs the pinned
Qt 6.8.3 MSVC 2022 x64 package into `.deps/Qt/6.8.3/msvc2022_64/`, using
`aqtinstall` to download the official binaries. To install Qt without
building:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\install-qt6-windows.ps1
```

To use an existing Qt installation:

```powershell
$env:GEKKOAOT_QT_DIR = "C:\Qt\6.8.3\msvc2022_64"
.\build-windows.cmd
```

The result is a deployable tree:

```text
dist/GekkoAOT/bin/
    gekkoaot.exe
    gekkoaotctl.exe
    gekkoaot-native-run.exe
    Qt6Core.dll, Qt6Gui.dll, Qt6Widgets.dll
    platforms/qwindows.dll
    ...
```

`cmake --install` runs Qt's deployment tool, so the package carries the Qt
runtime and platform plugin next to the executable.

## Discord Rich Presence

Discord Rich Presence needs the Discord Social SDK, which Discord distributes
under its own terms and which this repository does not include. Download it
for your target from the Discord Developer Portal and point CMake at it:

```sh
cmake -S . -B build \
  -DGEKKOAOT_DISCORD_SDK=ON \
  -DGEKKOAOT_DISCORD_SDK_ROOT=/absolute/path/to/discord_social_sdk
```

`AUTO` (the default) enables the feature when the SDK is found, `ON` fails if
it is missing and `OFF` leaves it out. Builds without the SDK work normally and
report the feature as unavailable. `build-linux.sh` installs the SDK library
next to the binaries when the build produced it. See
[Discord Rich Presence](discord-rich-presence.md) for details.

## Dependencies fetched by the build

**encounter/nod.** With `GEKKOAOT_NATIVE_NOD=ON`, CMake uses an installed `nod`
package, or downloads the pinned prebuilt release (`v2.0.0-alpha.12`) for
supported desktop targets.

**DolRecomp and Aurora.** They are not vendored. On the first compilation of a
game, `gekkoaotctl`:

1. fetches the pinned upstream revision;
2. resets its own checkout to that revision and removes untracked files;
3. checks every GekkoAOT patch with `git apply --check`;
4. applies the patches and overlay scripts;
5. builds the compiler and the renderer plugin.

The patches are in `patches/dolrecomp/` and `patches/aurora/`; Aurora overlay
scripts are in `runtime/gx/aurora/`.

## Portable packages

Release CI produces packages that do not need Git, CMake, Ninja, a compiler or
Qt on the user's machine. The release builder precompiles DolRecomp and the
AuroraGX plugin, ships the patched DolRecomp sources needed to link game
modules, bundles portable CMake, Ninja and Zig, deploys Qt and the Visual C++
runtime on Windows, and builds an AppImage on Linux.

The file `share/gekkoaot/toolchain/engine.ready` switches the controller to
portable mode:

```text
disc image → bundled DolRecomp → LLVM objects → bundled CMake/Ninja/Zig
           → native game module → prebuilt runtime + AuroraGX
```

The host still provides the kernel and the GPU, audio and input drivers.

## Installed layout

```text
<prefix>/bin/
    gekkoaot
    gekkoaotctl
    gekkoaot-module-meta
    gekkoaot-secondary-build
    gekkoaot-native-run
    gekkoaot-native-disc

<prefix>/share/gekkoaot/
    runtime/  patches/  game-packs/  crossgame-db/  docs/  tools/
    README.md  CHANGELOG.md  CONTRIBUTING.md  LICENSE  COPYING
    THIRD_PARTY_NOTICES.md  version.txt
```

`share/gekkoaot` is read-only at run time. Downloads, builds, caches and saves
live in the user state directory.
