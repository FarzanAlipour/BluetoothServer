#!/usr/bin/env bash
# Runs the EditMode tests in Unity batchmode (-nographics) on macOS or Linux.
# Builds the native plugin first when it is missing. Override the Editor with UNITY=/path/to/Unity.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VERSION="2022.3.62f3"

case "$(uname -s)" in
  Darwin)
    DEFAULT_UNITY="/Applications/Unity/Hub/Editor/$VERSION/Unity.app/Contents/MacOS/Unity"
    PLUGIN="$HERE/Assets/Plugins/macOS/libBleServer.dylib" ;;
  Linux)
    DEFAULT_UNITY="$HOME/Unity/Hub/Editor/$VERSION/Editor/Unity"
    PLUGIN="$HERE/Assets/Plugins/Linux/x86_64/libBleServer.so" ;;
  *) echo "Unsupported OS; on Windows run run_tests.ps1" >&2; exit 1 ;;
esac
UNITY="${UNITY:-$DEFAULT_UNITY}"
[ -x "$UNITY" ] || { echo "Unity $VERSION not found at $UNITY (set UNITY=...)" >&2; exit 1; }

[ -f "$PLUGIN" ] || "$HERE/build_plugin.sh"
# Always (re)install the import settings, even for a binary that was built earlier.
cp "$HERE/PluginMeta/${PLUGIN#"$HERE/Assets/Plugins/"}.meta" "$PLUGIN.meta"

RESULTS="$HERE/TestResults-editmode.xml"
LOG="$HERE/test-run.log"
rm -f "$RESULTS"

set +e
"$UNITY" -batchmode -nographics -projectPath "$HERE" \
  -runTests -testPlatform EditMode \
  -testResults "$RESULTS" -logFile "$LOG"
CODE=$?
set -e

if [ -f "$RESULTS" ]; then
  grep -o '<test-run [^>]*>' "$RESULTS" | head -1
  grep -o '<test-case [^>]*>' "$RESULTS" | sed -E 's/.* fullname="([^"]*)".* result="([^"]*)".*/  \2  \1/'
fi
if [ "$CODE" -ne 0 ]; then echo "Unity exited with code $CODE (see $LOG)" >&2; fi
exit "$CODE"
