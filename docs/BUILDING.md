# Building GekkoAOT

## Supported CI targets

The public CI validates:

- Linux x86_64;
- Windows x86_64 (Visual Studio 2022 / MSVC).

Other hosts may work but are not part of the v0.0.1 release matrix.

## Common requirements

- Git;
- CMake 3.25+;
- a C++20 compiler;
- network access on first setup so pinned external dependencies can be fetched;
- Qt 6.5+ when `GEKKOAOT_BUILD_GUI=ON`;
- Python 3 only for optional Adaptive PGO/CrossGameDB tools.

The normal controller/runtime is native C/C++. Python is **not** a runtime CPU/GX/DSP dependency, but current optional profiling/database workflows do call `tools/adaptive_pgo.py` and `tools/crossgame_db.py`.

## Linux

Recommended development dependencies:

```bash
# Package names vary by distribution.
# Arch example:
sudo pacman -S --needed base-devel cmake ninja git qt6-base python
```

Build the normal desktop target:

```bash
./build-linux.sh
```

Or configure manually:

```bash
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DGEKKOAOT_BUILD_GUI=ON \
  -DGEKKOAOT_BUILD_NATIVE_HOST=ON
cmake --build build --parallel
```

Core-only CI-style build:

```bash
cmake -S . -B build-core -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DGEKKOAOT_BUILD_GUI=OFF \
  -DGEKKOAOT_BUILD_NATIVE_HOST=ON \
  -DGEKKOAOT_NATIVE_NOD=OFF
cmake --build build-core --parallel
```

## Windows

Recommended host toolchain:

- Windows 10/11 x86_64;
- Visual Studio 2022 or Build Tools with **Desktop development with C++**;
- CMake;
- Git;
- Python 3 only for the automatic Qt bootstrap helper.

You do **not** need to install Qt manually anymore for the normal GekkoAOT Windows build.

Run:

```bat
build-windows.cmd
```

On the first run, if Qt is not already available through `GEKKOAOT_QT_DIR`, GekkoAOT installs the pinned Qt 6.8.3 MSVC 2022 x64 package into:

```text
.deps/Qt/6.8.3/msvc2022_64/
```

The helper uses `aqtinstall` only to download the official Qt binary packages. To install Qt without building:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\install-qt6-windows.ps1
```

To use an existing Qt installation instead:

```powershell
$env:GEKKOAOT_QT_DIR = "C:\Qt\6.8.3\msvc2022_64"
.\build-windows.cmd
```

The Windows helper uses the Visual Studio 2022 CMake generator directly, so Ninja is no longer required.

The finished local deployable tree is:

```text
dist/GekkoAOT/bin/
    gekkoaot.exe
    gekkoaotctl.exe
    gekkoaot-native-run.exe
    Qt6Core.dll
    Qt6Gui.dll
    Qt6Widgets.dll
    platforms/qwindows.dll
    ...
```

`cmake --install` invokes Qt's deployment tooling on Windows, so the GUI package carries the Qt runtime and platform plugin beside the executable.

## encounter/nod

With `GEKKOAOT_NATIVE_NOD=ON`, CMake first looks for an installed `nod` package. If none is found, the native host fetches the pinned prebuilt encounter/nod package for supported desktop targets.

Current pin: `v2.0.0-alpha.12`.

## DolRecomp and Aurora

They are not vendored as dirty source trees. On first game compile/run, `gekkoaotctl`:

1. clones/fetches the pinned upstream revision;
2. resets the tool-owned checkout to that exact revision;
3. cleans untracked files;
4. validates every GekkoAOT patch with `git apply --check`;
5. applies the patch series;
6. builds the required upstream tool/backend.

This behavior is intentional and reproducible. See `patches/dolrecomp/` and `patches/aurora/`.

## Portable end-user packages

Release CI prepares a self-contained user toolchain instead of requiring Git,
CMake, Ninja, MSVC/GCC/Clang or Qt to be installed on the target machine.

The release builder precompiles the pinned DolRecomp engine and AuroraGX bridge,
ships the patched DolRecomp source required by module glue, bundles portable
CMake + Ninja + Zig, deploys Qt/VC runtime on Windows, and creates an AppImage
on Linux.

At runtime `share/gekkoaot/toolchain/engine.ready` switches the controller into
portable mode:

```text
disc image
   |
   v
bundled DolRecomp
   |
   v
LLVM AOT object chunks
   |
   v
bundled CMake + Ninja + Zig
   |
   v
native game module
   |
   v
prebuilt GekkoAOT runtime + AuroraGX
```

The Windows ZIP is intended to need no separate Qt, Git, CMake, Ninja, Visual
Studio Build Tools or MSVC installation.

The Linux AppImage is the preferred no-install Linux artifact. The host still
provides the Linux kernel and working GPU/audio/input drivers; those hardware
and kernel components cannot be meaningfully bundled into an application.

## Installed layout

A CMake install uses:

```text
<prefix>/bin/
    gekkoaot
    gekkoaotctl
    gekkoaot-module-meta
    gekkoaot-secondary-build
    gekkoaot-native-run
    gekkoaot-native-disc

<prefix>/share/gekkoaot/
    runtime/
    patches/
    game-packs/
    crossgame-db/
    docs/
    tools/
    README.md
    CHANGELOG.md
    CONTRIBUTING.md
    LICENSE
    COPYING
    THIRD_PARTY_NOTICES.md
    version.txt
```

The installed `share/gekkoaot` tree is treated as read-only. Mutable downloads/builds/caches live in the user state directory.
