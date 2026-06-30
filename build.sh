#!/bin/bash
# build.sh — thin wrapper around the Taskfile (go-task). CMake/Make removed.
#
#   ./build.sh            # build dev channel + run
#   ./build.sh dev|live   # pick a channel + run
#   ./build.sh windows    # cross-compile a Windows .exe (mingw-w64)
#   ./build.sh clean      # remove build artifacts
set -e
cd "$(dirname "$0")"
TASK="$(command -v go-task || command -v task)"
CHANNEL="${1:-dev}"

case "$CHANNEL" in
    clean|distclean|windows) exec "$TASK" "$CHANNEL" ;;
esac

"$TASK" build CHANNEL="$CHANNEL"
echo ""
echo "=== START OF OUTPUT ==="
exec "./build/main-${CHANNEL}"
