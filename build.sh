#!/usr/bin/env bash
# Configures and builds OSCQlock into build/, then (with --run) starts it.
#
#   ./build.sh                  # Release build
#   ./build.sh --debug          # Debug build
#   ./build.sh --run [args]     # build, then run OSCQlock with the arguments that follow
#
# Qt 6.4 or later must be found by CMake (CMAKE_PREFIX_PATH, or a Homebrew / distribution Qt).
set -euo pipefail
cd "$(dirname "$0")"

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
    # The executable carries the version in its name: application.json says which
    NAME=$(python3 -c 'import json; print(json.load(open("build/application.json"))["name"])')
    if [ -d "build/$NAME.app" ]; then exec "build/$NAME.app/Contents/MacOS/$NAME" "$@"; fi
    if [ -x "build/$NAME" ]; then exec "build/$NAME" "$@"; fi
    exec "build/$TYPE/$NAME.exe" "$@"
fi
