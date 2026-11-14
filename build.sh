#!/usr/bin/env bash
# ./build.sh          debug build into build/libotk.a and build/libotk.so
# ./build.sh release  optimised build
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
BUILD="$ROOT/build"
PROTOCOLS="$ROOT/vendor/wayland-protocols"
CC=${CC:-clang}
PACKAGES=(wayland-client xkbcommon vulkan)

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

# shaders/NAME.comp -> build/gen/NAME.spv, for #embed
compile_shaders() {
    mkdir -p "$BUILD/gen"
    for shader in "$ROOT"/shaders/*.comp; do
        glslc --target-env=vulkan1.3 "$shader" -o "$BUILD/gen/$(basename "$shader" .comp).spv"
    done
}

# Only otk_* is visible, in the .so and in the .a, so internal names never clash with a program's.
build_library() {
    mkdir -p "$BUILD/objects"
    local objects=() source object
    for source in "$ROOT"/src/*.c "$BUILD"/gen/*.c; do
        [ -e "$source" ] || continue
        object="$BUILD/objects/$(basename "$source" .c).o"
        "$CC" -std=c23 -D_GNU_SOURCE -fPIC -fvisibility=hidden -Wall -Wextra "$@" \
            -I "$ROOT/include" -I "$BUILD/gen" --embed-dir="$BUILD/gen" $(pkg-config --cflags "${PACKAGES[@]}") \
            -c "$source" -o "$object"
        objects+=("$object")
    done
    ld -r -o "$BUILD/libotk.o" "${objects[@]}"
    objcopy --localize-hidden "$BUILD/libotk.o"
    rm -f "$BUILD/libotk.a"
    ar rcs "$BUILD/libotk.a" "$BUILD/libotk.o"
    "$CC" -shared -o "$BUILD/libotk.so" "${objects[@]}" $(pkg-config --libs "${PACKAGES[@]}")
}

case "${1:-debug}" in
    debug)   generate_protocols; compile_shaders; build_library -g -O0 ;;
    release) generate_protocols; compile_shaders; build_library -O2 ;;
    *)       echo "usage: $0 [debug|release]" >&2; exit 2 ;;
esac
