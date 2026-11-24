#!/usr/bin/env bash
# Configures and builds OSCQlock into build/, then (with --run) starts it.
#
#   scripts/build.sh                  # Release build
#   scripts/build.sh --debug          # Debug build
#   scripts/build.sh --run [args]     # build, then run OSCQlock with the arguments that follow
#
# Qt 6.4 or later must be found by CMake (CMAKE_PREFIX_PATH, or a Homebrew / distribution Qt).
set -euo pipefail
cd "$(dirname "$0")/.."

TYPE=Release
RUN=0
while [ $# -gt 0 ]; do
    case "$1" in
        --debug) TYPE=Debug; shift ;;
        --run) RUN=1; shift; break ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done

cmake -S . -B build -DCMAKE_BUILD_TYPE="$TYPE"
cmake --build build --config "$TYPE" --parallel

if [ "$RUN" = 1 ]; then
    if [ -d build/OSCQlock.app ]; then exec build/OSCQlock.app/Contents/MacOS/OSCQlock "$@"; fi
    if [ -x build/OSCQlock ]; then exec build/OSCQlock "$@"; fi
    exec "build/$TYPE/OSCQlock.exe" "$@"
fi
