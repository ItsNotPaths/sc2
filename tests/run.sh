#!/usr/bin/env bash
# Checks the promises bindings and hosts rely on. Run after ./build.sh.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build"

echo "otk.h parses as strict C99"
echo '#include "otk.h"' | clang -std=c99 -pedantic-errors -Werror -fsyntax-only -I "$ROOT/include" -x c -

echo "otk.h parses as C++"
echo '#include "otk.h"' | clang -std=c++17 -pedantic-errors -Werror -fsyntax-only -I "$ROOT/include" -x c++ -

echo "libotk.so and libotk.a export only otk_*"
extra="$( { nm -D --defined-only "$BUILD/libotk.so"; nm -g --defined-only "$BUILD/libotk.a"; } \
    | awk 'NF == 3 { print $3 }' | grep -v '^otk_' || true)"
if [ -n "$extra" ]; then
    echo "unexpected exports:" >&2
    echo "$extra" >&2
    exit 1
fi

echo "the contract between libotk and a host"
"${CC:-clang}" -std=c23 -D_GNU_SOURCE -Wall -Wextra -I "$ROOT/include" -I "$ROOT/libotk" \
    "$ROOT/tests/contract.c" "$BUILD/libotk.a" -o "$BUILD/contract"
"$BUILD/contract"

echo "all passed"
