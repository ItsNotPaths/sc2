#!/usr/bin/env bash
# ./build.sh          debug build: build/libsc2.a and .so for programs, build/sc2host, build/examples
# ./build.sh release  optimised build
# ./build.sh --public --version TAG [--notes TEXT] [--prerelease]
#                     runs .github/workflows/release.yml on GitHub and watches it; builds nothing here
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

# The workflow builds the commit GitHub has, so a release never holds unpushed changes.
publish() {
    local workflow=release.yml previous_run run
    if [ -z "$version" ]; then
        echo "--public needs --version TAG" >&2
        exit 2
    fi
    previous_run=$(gh run list --workflow="$workflow" --limit 1 --json databaseId -q '.[0].databaseId')
    gh workflow run "$workflow" --field version="$version" --field notes="$notes" --field prerelease="$prerelease"
    for _ in $(seq 30); do
        sleep 2
        run=$(gh run list --workflow="$workflow" --limit 1 --json databaseId -q '.[0].databaseId')
        if [ -n "$run" ] && [ "$run" != "$previous_run" ]; then
            gh run watch "$run" --exit-status
            return
        fi
    done
    echo "no new run of $workflow appeared" >&2
    exit 1
}

usage() {
    echo "usage: $0 [debug|release] | --public --version TAG [--notes TEXT] [--prerelease]" >&2
    exit 2
}

mode=debug public=0 version="" notes="" prerelease=false
while [ $# -gt 0 ]; do
    case "$1" in
        debug|release) mode="$1"; shift ;;
        --public)      public=1; shift ;;
        --version)     [ $# -ge 2 ] || usage; version="$2"; shift 2 ;;
        --notes)       [ $# -ge 2 ] || usage; notes="$2"; shift 2 ;;
        --prerelease)  prerelease=true; shift ;;
        *)             usage ;;
    esac
done

if [ "$public" = 1 ]; then
    publish
elif [ "$mode" = debug ]; then
    generate_protocols; compile_shaders; build_library -g -O0; build_host -g -O0; build_examples -g -O0
else
    generate_protocols; compile_shaders; build_library -O2; build_host -O2; build_examples -O2
fi
