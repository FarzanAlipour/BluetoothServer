#!/usr/bin/env bash
# Builds the BleServer native plugin (CMake, Release) and copies it into this Unity project.
#   macOS : universal arm64+x86_64 libBleServer.dylib -> Assets/Plugins/macOS/
#   Linux : libBleServer.so                            -> Assets/Plugins/Linux/x86_64/
# Windows: use build_plugin.ps1. Uses the repo's -DUNITY_PROJECT_PATH CMake option for the copy.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
BUILD_DIR="$REPO/build-unity-sample"

case "$(uname -s)" in
  Darwin) OUT="$HERE/Assets/Plugins/macOS/libBleServer.dylib" ;;
  Linux)
    case "$(uname -m)" in
      x86_64) ;;
      *) echo "Linux plugin slot is x86_64 only (host is $(uname -m))." >&2; exit 1 ;;
    esac
    OUT="$HERE/Assets/Plugins/Linux/x86_64/libBleServer.so" ;;
  *) echo "Unsupported OS $(uname -s). On Windows run build_plugin.ps1." >&2; exit 1 ;;
esac

command -v cmake >/dev/null || { echo "cmake not found" >&2; exit 1; }

cmake -S "$REPO" -B "$BUILD_DIR" \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=OFF -DBUILD_EXAMPLES=OFF \
  -DUNITY_PROJECT_PATH="$HERE"
cmake --build "$BUILD_DIR" --config Release --target BleServer -j

[ -f "$OUT" ] || { echo "Build finished but $OUT is missing" >&2; exit 1; }
# Install the committed import settings (see README, "Why PluginMeta/"): Unity deletes a .meta whose
# binary is missing, so the canonical copies live in PluginMeta/ and are installed next to the binary.
cp "$HERE/PluginMeta/${OUT#"$HERE/Assets/Plugins/"}.meta" "$OUT.meta"
echo "Plugin ready: $OUT"
if [ "$(uname -s)" = "Darwin" ]; then
  lipo -info "$OUT"
fi
