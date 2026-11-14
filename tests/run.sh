#!/usr/bin/env bash
# Checks the promises bindings and hosts rely on. Run after ./build.sh.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build"

echo "sc2.h parses as strict C99"
echo '#include "sc2.h"' | clang -std=c99 -pedantic-errors -Werror -fsyntax-only -I "$ROOT/include" -x c -

echo "sc2.h parses as C++"
echo '#include "sc2.h"' | clang -std=c++17 -pedantic-errors -Werror -fsyntax-only -I "$ROOT/include" -x c++ -

echo "libsc2.so and libsc2.a export only sc2_*"
extra="$( { nm -D --defined-only "$BUILD/libsc2.so"; nm -g --defined-only "$BUILD/libsc2.a"; } \
    | awk 'NF == 3 { print $3 }' | grep -v '^sc2_' || true)"
if [ -n "$extra" ]; then
    echo "unexpected exports:" >&2
    echo "$extra" >&2
    exit 1
fi

echo "the contract between libsc2 and a host"
"${CC:-clang}" -std=c23 -D_GNU_SOURCE -Wall -Wextra -I "$ROOT/include" -I "$ROOT/libsc2" \
    "$ROOT/tests/contract.c" "$BUILD/libsc2.a" -o "$BUILD/contract"
"$BUILD/contract"

if [ -n "${WAYLAND_DISPLAY:-}" ]; then
    echo "sc2host survives programs that break the contract (each opens a window for 2 s)"
    "${CC:-clang}" -std=c99 -Wall -Wextra -I "$ROOT/include" "$ROOT/tests/hostile.c" "$BUILD/libsc2.a" \
        -o "$BUILD/hostile"
    for mode in garbage stuck shrink; do
        status=0
        timeout 20 "$BUILD/sc2host" "$BUILD/hostile" "$mode" || status=$?
        if [ "$status" -ne 0 ]; then
            echo "FAIL sc2host with a $mode program: exit $status" >&2
            exit 1
        fi
        echo "ok   $mode"
    done
else
    echo "skipped sc2host against programs that break the contract: needs a Wayland session"
fi

echo "all passed"
