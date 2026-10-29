#!/bin/sh
# Build Release and run ble_test_server (macOS / Linux) with verbose logging.
# The log goes to logs/hw-test-<os>-<timestamp>.log (git-ignored): attach it to bug reports.
# Extra arguments are passed to ble_test_server, e.g.:  scripts/run_hw_test.sh --name MyServer --heartbeat-ms 500
# Procedure and checklist: docs/TESTING.md
set -eu
cd "$(dirname "$0")/.."

case "$(uname -s)" in
    Darwin) os=macos ;;
    Linux)  os=linux ;;
    *) echo "run_hw_test.sh supports macOS and Linux; on Windows use scripts/run_hw_test.ps1" >&2; exit 2 ;;
esac

build_dir=build-hw
cmake -S . -B "$build_dir" -DCMAKE_BUILD_TYPE=Release
cmake --build "$build_dir" --config Release -j 4 --target ble_test_server

exe="$build_dir/examples/ble_test_server"
[ -x "$exe" ] || exe="$build_dir/examples/Release/ble_test_server"

mkdir -p logs
log="logs/hw-test-$os-$(date +%Y%m%d-%H%M%S).log"
{
    echo "# ble_test_server hardware test"
    echo "# os: $os  $(uname -srm)"
    echo "# commit: $(git rev-parse --short HEAD 2>/dev/null || echo unknown)"
    echo "# started: $(date)"
} > "$log"

echo "Logging to $log (Ctrl-C to stop; commands: n = notify, l = large message, s = stop/start advertising, q = quit)"
# `|| true`: a non-zero exit is already in the log; the script should still print where it is.
"$exe" --verbose "$@" 2>&1 | tee -a "$log" || true
echo "Log saved: $log"
