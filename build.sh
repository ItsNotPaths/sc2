#!/usr/bin/env bash
# ./build.sh          debug build into build/libotk.a and build/libotk.so
# ./build.sh release  optimised build
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
BUILD="$ROOT/build"
PROTOCOLS="$ROOT/vendor/wayland-protocols"
FREETYPE="$ROOT/vendor/freetype"
CC=${CC:-clang}
PACKAGES=(wayland-client wayland-cursor xkbcommon vulkan)

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

# Unity files for the modules listed in src/freetype_modules.h, plus what they call.
FREETYPE_FILES=(
    base/ftsystem.c base/ftinit.c base/ftdebug.c base/ftbase.c base/ftbitmap.c base/ftsynth.c base/ftmm.c
    gzip/ftgzip.c sfnt/sfnt.c truetype/truetype.c cff/cff.c psaux/psaux.c psnames/psnames.c
    pshinter/pshinter.c autofit/autofit.c smooth/smooth.c
)

# shaders/NAME.comp -> build/gen/NAME.spv, for #embed
compile_shaders() {
    mkdir -p "$BUILD/gen"
    for shader in "$ROOT"/shaders/*.comp; do
        glslc --target-env=vulkan1.3 "$shader" -o "$BUILD/gen/$(basename "$shader" .comp).spv"
    done
}

# compile SOURCE FLAGS...: one object into build/objects, added to objects.
compile() {
    local source="$1" object
    shift
    object="$BUILD/objects/$(basename "$source" .c).o"
    "$CC" -fPIC -fvisibility=hidden "$@" -c "$source" -o "$object"
    objects+=("$object")
}

# Only otk_* stays global, in the .so and in the .a, so internal and FreeType names never clash with a program's.
build_library() {
    mkdir -p "$BUILD/objects"
    local objects=() source file
    for source in "$ROOT"/src/*.c "$BUILD"/gen/*.c; do
        compile "$source" -std=c23 -D_GNU_SOURCE -Wall -Wextra "$@" \
            -I "$ROOT/include" -I "$BUILD/gen" -I "$FREETYPE/include" --embed-dir="$BUILD/gen" \
            $(pkg-config --cflags "${PACKAGES[@]}")
    done
    for file in "${FREETYPE_FILES[@]}"; do
        compile "$FREETYPE/src/$file" -DFT2_BUILD_LIBRARY -DFT_CONFIG_MODULES_H='"freetype_modules.h"' "$@" \
            -I "$FREETYPE/include" -I "$ROOT/src"
    done
    ld -r -o "$BUILD/libotk.o" "${objects[@]}"
    objcopy --wildcard --keep-global-symbol='otk_*' "$BUILD/libotk.o"
    rm -f "$BUILD/libotk.a"
    ar rcs "$BUILD/libotk.a" "$BUILD/libotk.o"
    "$CC" -shared -Wl,--no-undefined -o "$BUILD/libotk.so" "$BUILD/libotk.o" $(pkg-config --libs "${PACKAGES[@]}")
}

# examples/NAME.c -> build/examples/NAME, strict C99 like a binding, linked against libotk.a.
build_examples() {
    mkdir -p "$BUILD/examples"
    local example
    for example in "$ROOT"/examples/*.c; do
        "$CC" -std=c99 -Wall -Wextra "$@" -I "$ROOT/include" "$example" "$BUILD/libotk.a" \
            $(pkg-config --libs "${PACKAGES[@]}") -lpthread -o "$BUILD/examples/$(basename "$example" .c)"
    done
}

case "${1:-debug}" in
    debug)   generate_protocols; compile_shaders; build_library -g -O0; build_examples -g -O0 ;;
    release) generate_protocols; compile_shaders; build_library -O2; build_examples -O2 ;;
    *)       echo "usage: $0 [debug|release]" >&2; exit 2 ;;
esac
