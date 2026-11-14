#!/usr/bin/env bash
# ./build.sh          debug build: build/libsc2.a and .so for programs, build/sc2host, build/examples
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

# host/shaders/NAME.comp -> build/gen/NAME.spv, for #embed
compile_shaders() {
    mkdir -p "$BUILD/gen"
    for shader in "$ROOT"/host/shaders/*.comp; do
        glslc --target-env=vulkan1.3 "$shader" -o "$BUILD/gen/$(basename "$shader" .comp).spv"
    done
}

# compile SOURCE FLAGS...: one object into build/objects, added to objects.
compile() {
    local source="$1" object
    shift
    object="$BUILD/objects/$(basename "$source" .c).o"
    "$CC" "$@" -c "$source" -o "$object"
    objects+=("$object")
}

# The library programs link: only the contract, no Wayland and no GPU. Only sc2_* stays global.
build_library() {
    mkdir -p "$BUILD/objects"
    local objects=() source
    for source in "$ROOT"/libsc2/*.c; do
        compile "$source" -std=c23 -D_GNU_SOURCE -Wall -Wextra -fPIC -fvisibility=hidden "$@" \
            -I "$ROOT/include" -I "$ROOT/libsc2"
    done
    ld -r -o "$BUILD/libsc2.o" "${objects[@]}"
    objcopy --wildcard --keep-global-symbol='sc2_*' "$BUILD/libsc2.o"
    rm -f "$BUILD/libsc2.a"
    ar rcs "$BUILD/libsc2.a" "$BUILD/libsc2.o"
    "$CC" -shared -Wl,--no-undefined -o "$BUILD/libsc2.so" "$BUILD/libsc2.o"
}

# The reference host, with FreeType built in from the modules in host/freetype_modules.h.
build_host() {
    mkdir -p "$BUILD/objects/host"
    local objects=() source file
    for source in "$ROOT"/host/*.c "$BUILD"/gen/*.c; do
        compile "$source" -std=c23 -D_GNU_SOURCE -Wall -Wextra "$@" \
            -I "$ROOT/include" -I "$ROOT/libsc2" -I "$BUILD/gen" -I "$FREETYPE/include" --embed-dir="$BUILD/gen" \
            $(pkg-config --cflags "${PACKAGES[@]}")
    done
    for file in "${FREETYPE_FILES[@]}"; do
        compile "$FREETYPE/src/$file" -DFT2_BUILD_LIBRARY -DFT_CONFIG_MODULES_H='"freetype_modules.h"' "$@" \
            -I "$FREETYPE/include" -I "$ROOT/host"
    done
    "$CC" -Wl,--no-undefined -o "$BUILD/sc2host" "${objects[@]}" $(pkg-config --libs "${PACKAGES[@]}") -lm
}

# examples/NAME.c -> build/examples/NAME, strict C99 like a binding, linked against libsc2.a alone.
build_examples() {
    mkdir -p "$BUILD/examples"
    local example
    for example in "$ROOT"/examples/*.c; do
        "$CC" -std=c99 -Wall -Wextra "$@" -I "$ROOT/include" "$example" "$BUILD/libsc2.a" \
            -o "$BUILD/examples/$(basename "$example" .c)"
    done
}

case "${1:-debug}" in
    debug)   generate_protocols; compile_shaders; build_library -g -O0; build_host -g -O0; build_examples -g -O0 ;;
    release) generate_protocols; compile_shaders; build_library -O2; build_host -O2; build_examples -O2 ;;
    *)       echo "usage: $0 [debug|release]" >&2; exit 2 ;;
esac
