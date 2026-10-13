#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD="${GEKKOAOT_BUILD_DIR:-$ROOT/build}"
JOBS="${GEKKOAOT_JOBS:-$(nproc 2>/dev/null || echo 4)}"
command -v cmake >/dev/null || { echo 'error: cmake is required' >&2; exit 1; }
command -v ninja >/dev/null || { echo 'error: ninja is required' >&2; exit 1; }
command -v git >/dev/null || { echo 'error: git is required' >&2; exit 1; }
CMAKE_ARGS=("$@")
BUILD_TYPE="${GEKKOAOT_BUILD_TYPE:-Release}"
if [[ "${GEKKOAOT_COMPAT_DEBUG:-0}" != "0" ]]; then
  BUILD_TYPE="RelWithDebInfo"
  CMAKE_ARGS+=("-DGEKKOAOT_NATIVE_DEBUGGABLE=ON")
fi
GUI_OPTION_EXPLICIT=0
for arg in "${CMAKE_ARGS[@]}"; do
  case "$arg" in
    -DGEKKOAOT_BUILD_GUI=*) GUI_OPTION_EXPLICIT=1 ;;
  esac
done
# A previous tools-only configure can leave GEKKOAOT_BUILD_GUI=OFF in the
# persistent CMake cache. The normal build script is the desktop build, so
# force the GUI back on unless the caller explicitly requested otherwise.
if [[ "$GUI_OPTION_EXPLICIT" -eq 0 ]]; then
  CMAKE_ARGS+=("-DGEKKOAOT_BUILD_GUI=ON")
fi
cmake -S "$ROOT" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE="$BUILD_TYPE" "${CMAKE_ARGS[@]}"
# Build the default graph rather than naming the optional GUI target. This also
# keeps ./build-linux.sh useful with -DGEKKOAOT_BUILD_GUI=OFF.
cmake --build "$BUILD" -j "$JOBS"
mkdir -p "$ROOT/bin"
[[ -x "$BUILD/bin/gekkoaot" ]] && install -m0755 "$BUILD/bin/gekkoaot" "$ROOT/bin/gekkoaot"
for f in gekkoaotctl gekkoaot-module-meta gekkoaot-secondary-build gekkoaot-native-run gekkoaot-native-disc; do
  [[ -x "$BUILD/bin/$f" ]] && install -m0755 "$BUILD/bin/$f" "$ROOT/bin/$f"
done
VERSION="$(tr -d '\r\n' < "$ROOT/version.txt")"
echo "Built GekkoAOT v${VERSION} in $ROOT/bin"
