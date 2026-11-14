#!/usr/bin/env bash
# Fetches third-party deps into vendor/. Run once before building, and again after a rev bump.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
VENDOR="$ROOT/vendor"

# fetch NAME URL REV: clone into vendor/NAME, check out REV, then apply patches/NAME/*.patch.
fetch() {
    local name="$1" url="$2" rev="$3"
    local source="$VENDOR/$name"
    if [ ! -d "$source/.git" ]; then
        rm -rf "$source"
        git clone -q "$url" "$source"
    fi
    if [ "$(git -C "$source" rev-parse HEAD)" != "$rev" ]; then
        git -C "$source" fetch -q origin
        git -C "$source" checkout -q -f --detach "$rev"
    fi
    echo "  at $(git -C "$source" log --oneline -1)"
    for patch in "$ROOT/patches/$name"/*.patch; do
        [ -e "$patch" ] || continue
        if git -C "$source" apply --reverse --check "$patch" 2>/dev/null; then
            echo "  patched: $(basename "$patch")"
        else
            git -C "$source" apply "$patch"
            echo "  applied: $(basename "$patch")"
        fi
    done
}

echo "==> wayland-protocols 1.50 (xdg-shell, text-input, cursor-shape, fractional-scale)"
fetch wayland-protocols "https://gitlab.freedesktop.org/wayland/wayland-protocols.git" \
    "3eab2dccf4bfdd85c16d312a6c8c8fbc2a6e73a8"

echo "==> wlr-protocols (layer-shell, pinned 2026-11-13)"
fetch wlr-protocols "https://gitlab.freedesktop.org/wlroots/wlr-protocols.git" \
    "1106a4a1c57562ddd438c547da4c0d6dc150d624"
