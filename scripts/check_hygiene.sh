#!/bin/sh
# Fails if build output or OS junk is tracked.
cd "$(dirname "$0")/.." || exit 2
bad=$(git ls-files | grep -E '^build/|(^|/)\.DS_Store$|\.vcxproj\.backup$')
if [ -n "$bad" ]; then echo "$bad"; echo "check_hygiene: FAIL" >&2; exit 1; fi
echo "check_hygiene: OK"
