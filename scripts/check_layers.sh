#!/bin/sh
# Fails if platform-specific code leaks into src/common (layer rule).
cd "$(dirname "$0")/.." || exit 2
pat='windows\.h|winrt/|dbus/|gio/|glib\.h|glib-2\.0|gio-2\.0|CoreBluetooth|Foundation/|Foundation\.h|#[[:space:]]*(if|ifdef|ifndef|elif)[^/]*(_WIN32|__APPLE__|__linux__|__unix__|TARGET_OS)'
if grep -rnE "$pat" src/common; then
    echo "check_layers: FAIL - OS-specific code in src/common" >&2
    exit 1
fi
echo "check_layers: OK"
