<!-- (swiss :tags (core window input clipboard draw text) :sev (blocker gap polish)) -->

# OTK

The design is `docs/design.md`. This plan is the route through it.

Severities: `blocker` = needed before a first real program (a file browser) can run: a window,
text on screen, keys and mouse. `gap` = the rest of the design. `polish` = a small chore.

## Structure

The scaffold is a sketch. Change any of it when a hole needs a different shape.

- `include/otk.h` is the contract. Bindings read only it. Strict C99, also parses as C++.
  Every constant is an enum, so no FFI needs a macro.
- Shared structs list fields largest to smallest. The areas come first, so `pixels` sits at
  offset 0 and always starts on a page. Each writer's fields stay together, so a tick copies
  one range in and one range out.
- `src/library.h` holds `Library`: all process-local state (Wayland objects, the GPU side,
  the commit copy). One sub-struct per system, and every function the files share.
- One `.c` file per system: `wayland`, `window`, `scale`, `keyboard`, `pointer`, `text_input`,
  `clipboard`, `settings`, `font`, `flatten`, `graphics`, `atlas`, `draw`.
- `otk.c` holds the three public functions and the one static `Library` (one window per process).
- `shaders/*.comp` compile to `build/gen/*.spv` and go in with `#embed`.
- FreeType 2.15.0 is vendored (`download-deps.sh`) and built in: only the modules in
  `src/freetype_modules.h` (TrueType, CFF, hinting, smooth rendering).
- `examples/*.c` build against `libotk.a` as strict C99, the way a binding would use it.
- A failed `otk_open` says why on stderr, one `otk:` line.
- Only `otk_*` is visible, in the `.so` and the `.a`, so a program's own names never clash
  with internal ones. `tests/run.sh` checks this and the C99 header.

Two thread rules:

- The library thread writes input only into `incoming`, and only under `lock`. Wayland
  callbacks run inside the lock, so they write freely. `tick_receive` copies it into `screen`.
- `tick_publish` copies what the program wrote into `committed`. The library thread reads
  only `committed`, never `screen`.

Drawing runs on the library thread, outside the lock. It holds the lock only to flatten
`committed` and pack its cells for the GPU. It draws when a frame callback has come back and
something changed: a commit, the window size, or glyphs the last frame missed. A tick that
changed nothing does not count as a commit.

Scroll is eighths modulo 8 (`scroll_column`, `scroll_row`), for smoothness only. It never
moves anything a whole cell: the program moves content by whole cells itself, by rewriting
cells, so nothing sits between cells at rest.

Seams for patches (users vendor OTK and patch their own copy; OTK ships no patches):

- A new protocol is one row in `wanted_globals` (`wayland.c`) and one field in `Globals`.
- A setting that changes at run time is a few lines in `settings_update` (`settings.c`), taking
  it from `committed`. Zoom, for example: add a `font_size` field the program writes, and use it
  when it is not 0. It runs before each draw. The font reloads whenever it no longer matches
  the settings or the scale, so a scale change and a font change take the same path.
- A new clipboard MIME type is one row in `data_types` (`clipboard.c`). Row order is the
  drop preference, and within one `DATA_` bit the paste preference.
- A new system is one file, one sub-struct in `Library`, and one call where it starts.

## Roadmap

Strict order: always work on the lowest unfinished milestone.

### M1: a window (built 2026-11-13)

`window-toplevel`, `tick-exchange`.
Done: a window opens, `columns` and `rows` follow a resize, and `otk_tick` returns 0 on close.

### M2: text (built 2026-11-13)

`node-flatten`, `font-load`, `graphics-device`, `draw-shader`, `glyph-atlas`, `draw-frame`.
Done: a cell node shows text in palette colors, and scrolling a parent moves it.

### M3: input (built 2026-11-13)

`keyboard-keys`, `key-repeat`, `pointer-input`, `cursor-shape`.
Done: typed text reaches the ring, clicks hit the right node, the wheel scrolls.

### M4: images (built 2026-11-14)

`image-import`.
Done: a pixel node shows its image scaled to fill its area, under and over other nodes.
Import covers pixels from 0 to the last byte a visible image reads, so untouched pages stay
untouched. Where import fails, each image is copied when its offset, size or `sequence`
changes. An image ignores its node's scroll. `examples/image.c` shows it.

### M5: clipboard and drop (built 2026-11-14)

`clipboard-offers`, `clipboard-paste`, `clipboard-copy`, `clipboard-drop`.
Done: `paste_offered` follows the clipboard. Every paste request and every drop gets one
answer through `paste_sequence`: the bytes, or `paste_length` 0 when the type is not offered,
the read fails, the bytes run past `PASTE_CAPACITY`, or a newer request replaces it. Bytes are
read into the library's own buffer and reach `incoming` only once complete. Copy serves reads
from `committed->copy`; a newer copy cuts off reads still running. All pipes are non-blocking
on the library thread's poll, and that thread blocks signals, so a closed pipe gives `EPIPE`.
`examples/hello.c` copies with Ctrl+C, pastes with Ctrl+V and shows drops.

### M6: the rest

`fractional-scale`, `tick-idle` and `user-settings` are built (2026-11-14). `input-method` is open:
it cannot be tested on hedl.

- **Idle.** `animating` is gone: a program would have to keep it in step with what it draws.
  A tick that brought no input and changed nothing is idle, and the next `otk_tick` sleeps
  until input differs from what the program last saw. `idle_tick_rate`, set once, keeps a
  program ticking while idle (a cursor blink, a pty); 0 sleeps until input. Idle `hello` uses
  no CPU after startup.
- **Scale** is a stopgap, marked `scale-redesign`: the cell is `font_size` times the scale, so
  the pixels grow and the grid stays the same. A scale change reloads the font and empties
  the atlas on the next draw. Tested by flipping 1x and 1.5x at run time in a scratch copy;
  hedl sends 1x.
- **Settings** are one struct of defaults in `src/settings.c`: font, size, palette, first
  window size, rows per wheel notch, and the cursor theme and size for the xcursor fallback.
  The library reads no files and no environment (the fallback no longer reads `XCURSOR_*`).
  Users patch the values, the same way hosts and programs patch anything else. A missing
  font or a 0 size fails `otk_open` with one `otk:` line.
`seat-loss` is built (2026-11-13). `atlas-eviction` and `cursor-fallback` are built (2026-11-14).

## Workstream H: host metadata

Open. On the host route (a terminal hosting OTK programs) the host needs metadata from each
child: the title, an app_id, a size request, see-through areas. Today the title only goes to
`otk_open`, so a host never sees it. Options:

- Fields in `otk_screen`, written by the program. Same model as everything else: no messages,
  works on all three routes, and the remote route syncs it with the other bytes. The title
  could then change at run time (a file browser showing its path).
- A socket next to the memfd. Needed only if something must pass file descriptors.

Also open: `Node`, `Cell` and the enum constants (`NODE_EMPTY`, `DATA_TEXT`, ...) have no
`otk` prefix, so they can clash in a program the way `Screen` did with Xlib.

## Workstream C: core

`otk_tick` order: publish, wait, receive. So the program reads the newest input.

<!-- (skeleton :tags core) -->

_No holes match this query._

<!-- /skeleton -->

## Workstream W: window

GNOME draws no borders for clients. Whether the library draws its own is not decided.

<!-- (skeleton :tags window) -->

_1 holes, 1 marks, 1 ready. Generated by `swiss sync`; edits inside this block are lost._

- **scale-redesign** · window · ready
  - [src/scale.c:3](src/scale.c#L3) · gap · stopgap: the cell is font_size times the scale, so a scale change reloads the font like a font_size change; to be redesigned with the glyph grid size.

<!-- /skeleton -->

## Workstream I: input

Input methods cannot be checked on hedl. They need sway or Hyprland with fcitx5.

<!-- (skeleton :tags input) -->

_1 holes, 1 marks, 1 ready. Generated by `swiss sync`; edits inside this block are lost._

- **input-method** · input · ready
  - [src/text_input.c:3](src/text_input.c#L3) · gap · makes no text-input-v3 object; composition stays empty and committed text never reaches the ring.

<!-- /skeleton -->

## Workstream D: draw and text

Fonts use FreeType, vendored, for hinting: small monospace text stays sharp on low-DPI screens.
Image import is checked on ANV and
llvmpipe only; the copy fallback covers RADV and NVIDIA.

<!-- (skeleton :tags (draw text)) -->

_No holes match this query._

<!-- /skeleton -->

## Workstream K: clipboard and drop

Not tried yet: text dropped from a browser, dragging out of the window, primary selection.

Decided 2026-11-14:
- A drop takes the first `data_types` row its offer has: uri-list, then images, then text,
  then HTML. A drag with no known type is refused, so it never drops.
- A copy needs a serial. The library uses the last key, button or keyboard enter, so a
  program can copy as soon as its window has focus.
- Paste tested with wl-copy: text, 10 MB, exactly 64 MB, and 64 MB + 1 (answered empty).
  Copy tested with wl-paste: text, and 10 MB read by two clients at once. Drop untested.

<!-- (skeleton :tags clipboard) -->

_No holes match this query._

<!-- /skeleton -->
