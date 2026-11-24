#!/usr/bin/env bash
# Builds OSCQlock into build/ and opens it.
#
#   ./build.sh                       build, then open OSCQlock
#   ./build.sh --no-run              build only
#   ./build.sh --debug               a Debug build
#   ./build.sh --clean               start from an empty build folder
#   ./build.sh -- --no-window --local   what follows `--` goes to OSCQlock (its options: OSCQlock --help)
#
# Qt 6.4 or later must be found by CMake (CMAKE_PREFIX_PATH, or a Homebrew / distribution Qt).
set -euo pipefail
cd "$(dirname "$0")"

fail() { printf '\nError: %s\n' "$*" >&2; exit 1; }

TYPE=Release RUN=1 CLEAN=0
while [ $# -gt 0 ]; do
    case "$1" in
        --no-run) RUN=0; shift ;;
        --debug) TYPE=Debug; shift ;;
        --clean) CLEAN=1; shift ;;
        -h|--help) sed -n '2,9p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        --) shift; break ;;
        *) fail "unknown option: $1 (see ./build.sh --help)" ;;
    esac
done

[ "$CLEAN" = 1 ] && rm -rf build
cmake -S . -B build -DCMAKE_BUILD_TYPE="$TYPE"
cmake --build build --config "$TYPE" --parallel

# The executable carries the version in its name: application.json says which
NAME=$(python3 -c 'import json; print(json.load(open("build/application.json"))["name"])')
case "$(uname -s)" in
    Darwin)
        APP="build/$NAME.app"
        [ -d "$APP" ] || fail "$APP was not built"
        echo; echo "OSCQlock is built: $(pwd)/$APP"
        if [ "$RUN" = 1 ]; then
            if [ $# -gt 0 ]; then open -n "$APP" --args "$@"; else open "$APP"; fi
        fi ;;
    MINGW*|MSYS*|CYGWIN*)
        APP="build/$TYPE/$NAME.exe"
        [ -x "$APP" ] || fail "$APP was not built"
        echo; echo "OSCQlock is built: $(pwd)/$APP"
        if [ "$RUN" = 1 ]; then "$APP" "$@" & fi ;;
    *)
        APP="build/$NAME"
        [ -x "$APP" ] || fail "$APP was not built"
        echo; echo "OSCQlock is built: $(pwd)/$APP"
        if [ "$RUN" = 1 ]; then nohup "$APP" "$@" >/dev/null 2>&1 & fi ;;
esac
