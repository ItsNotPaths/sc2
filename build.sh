#!/usr/bin/env bash
# ./build.sh          debug build into build/libotk.a and build/libotk.so
# ./build.sh release  optimised build
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
BUILD="$ROOT/build"
PROTOCOLS="$ROOT/vendor/wayland-protocols"
CC=${CC:-clang}

PROTOCOL_FILES=(
    stable/xdg-shell/xdg-shell.xml
    stable/viewporter/viewporter.xml
    stable/tablet/tablet-v2.xml  # cursor-shape refers to it
    staging/cursor-shape/cursor-shape-v1.xml
    staging/fractional-scale/fractional-scale-v1.xml
    unstable/text-input/text-input-unstable-v3.xml
)

generate_protocols() {
    mkdir -p "$BUILD/gen"
    for file in "${PROTOCOL_FILES[@]}"; do
        local name
        name="$(basename "$file" .xml)"
        wayland-scanner client-header "$PROTOCOLS/$file" "$BUILD/gen/$name.h"
        wayland-scanner private-code "$PROTOCOLS/$file" "$BUILD/gen/$name.c"
    done
}

build_library() {
    mkdir -p "$BUILD/objects"
    local objects=() source object
    for source in "$ROOT"/src/*.c "$BUILD"/gen/*.c; do
        [ -e "$source" ] || continue
        object="$BUILD/objects/$(basename "$source" .c).o"
        "$CC" -std=c23 -fPIC -Wall -Wextra "$@" -I "$ROOT/include" -I "$BUILD/gen" -c "$source" -o "$object"
        objects+=("$object")
    done
    rm -f "$BUILD/libotk.a"
    ar rcs "$BUILD/libotk.a" "${objects[@]}"
    "$CC" -shared -o "$BUILD/libotk.so" "${objects[@]}" -lwayland-client
}

case "${1:-debug}" in
    debug)   generate_protocols; build_library -g -O0 ;;
    release) generate_protocols; build_library -O2 ;;
    *)       echo "usage: $0 [debug|release]" >&2; exit 2 ;;
esac
