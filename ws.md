<!-- (swiss :tags (core window input clipboard draw text) :sev (blocker gap polish)) -->

# sc\*2

The design is `docs/design.md`. This plan is the route through it.

Severities: `blocker` = needed before a first real program (a file browser) can run: a window,
text on screen, keys and mouse. `gap` = the rest of the design. `polish` = a small chore.

## Structure

sc\*2 is a contract, a tiny library for programs, and a reference host (2026-11-14). A program
describes its screen in shared memory; a host draws it. Hosts are the product: sc2host is the
suckless-style one, and anyone can write another from the contract alone.

- **Contract** (`include/`): `sc2.h` and `contract.md`. Bindings and hosts read only these.
  `sc2.h` is strict C99, also parses as C++, and every constant is an enum. Every public name
  has the `sc2` prefix, so none can clash in a program the way `Screen` did with Xlib.
- **libsc2** (`libsc2/`): what a program links. `sc2_open` maps the memory its host named in
  the environment, `sc2_tick` commits, sleeps and receives. No Wayland, no GPU; about 20 KB.
  `contract.h` holds the contract's rules once, for libsc2 and sc2host: the sequence counters,
  and the cells and bytes a node uses.
- **sc2host** (`host/`): `sc2host PROGRAM [ARGUMENT...]` runs one program and draws it in a
  Wayland window. `host.h` holds `Host`: one sub-struct per system and every function the files
  share. One `.c` file per system: `wayland`, `window`, `scale`, `keyboard`, `pointer`,
  `text_input`, `clipboard`, `settings`, `font`, `flatten`, `graphics`, `atlas`, `draw`, and
  `main` (start, the poll loop, the program's lifetime). Single threaded.
- `host/shaders/*.comp` compile to `build/gen/*.spv` and go in with `#embed`.
- FreeType 2.15.0 is vendored (`download-deps.sh`) and built into sc2host: only the modules in
  `host/freetype_modules.h` (TrueType, CFF, hinting, smooth rendering).
- `examples/*.c` are programs: strict C99, linked against `libsc2.a` alone. Run them with
  `build/sc2host build/examples/hello`.
- Shared structs list fields largest to smallest. The areas come first, so `pixels` sits at
  offset 0, and a one-page header puts `screen` on a page. Each writer's fields stay together.
- A program with no host fails `sc2_open` with one `sc2:` line; sc2host fails with one
  `sc2host:` line.
- Only `sc2_*` is visible in `libsc2.a` and `.so`. `tests/run.sh` checks this, the C99 header,
  and the contract itself with a minimal host (`tests/contract.c`) that needs no Wayland.

The rules between program and host (`include/contract.md` has them in full):

- The host writes input only into `incoming`, between `input_begin` and `input_end`, which keep
  `input_sequence` odd while it writes. Wayland callbacks run inside one, so they write freely.
  `input_end` wakes the program only when input changed.
- Each tick that changed something copies the program's fields and cells into `committed`, with
  `commit_sequence` odd meanwhile. The host reads `committed` only, and reads again when the
  sequence shows a commit landed during its read. The one exception is `pixels`, which the host
  imports from `screen` in place; `sequence` tells it when an image changed.
- Neither side waits for the other: the program ticks at its own rate, the host draws at vsync
  from the newest whole commit.

The host draws when a frame callback has come back and something changed: a commit, the window
size, the scale, or glyphs the last frame missed.

Scroll is eighths modulo 8 (`scroll_column`, `scroll_row`), for smoothness only. It never
moves anything a whole cell: the program moves content by whole cells itself, by rewriting
cells, so nothing sits between cells at rest.

Seams for patches (users vendor sc\*2 and patch their own copy; sc\*2 ships no patches):

- A new protocol is one row in `wanted_globals` (`wayland.c`) and one field in `Globals`.
- A setting that changes at run time is a few lines in `settings_update` (`settings.c`), which
  runs before each draw; `font_size` is the built-in example. The font reloads whenever it no
  longer matches the settings or the scale, so a scale change and a font change take one path.
- A new clipboard MIME type is one row in `data_types` (`clipboard.c`). Row order is the
  drop preference, and within one `DATA_` bit the paste preference.
- A new system is one file, one sub-struct in `Host`, and one call where it starts.

## Roadmap

Strict order: always work on the lowest unfinished milestone.

### M1: a window (built 2026-11-13)

`window-toplevel`, `tick-exchange`.
Done: a window opens, `columns` and `rows` follow a resize, and `sc2_tick` returns 0 on close.

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
the read fails, the bytes run past `SC2_PASTE_CAPACITY`, or a newer request replaces it. Bytes are
read into the host's own buffer and reach `incoming` only once complete. Copy serves reads
from the host's own copy (`offered`), taken from one whole commit; a newer copy cuts off reads
still running. All pipes are non-blocking on the host's poll, and the host ignores `SIGPIPE`,
so a closed pipe gives `EPIPE`.
`examples/hello.c` copies with Ctrl+C, pastes with Ctrl+V and shows drops.

### M6: the rest

`fractional-scale`, `tick-idle` and `user-settings` are built (2026-11-14). `input-method` is open:
it cannot be tested on hedl.

- **Idle.** `animating` is gone: a program would have to keep it in step with what it draws.
  A tick that brought no input and changed nothing is idle, and the next `sc2_tick` sleeps
  until input differs from what the program last saw. `idle_tick_rate`, set once, keeps a
  program ticking while idle (a cursor blink, a pty); 0 sleeps until input. Idle `hello` uses
  no CPU after startup.
- **Scale and font size** (settled 2026-11-14): the program picks `font_size` (0 is the host's,
  reported in `host_font_size`); the cell is that times the output scale, and the host reports
  the grid that fits. A scale change keeps the grid; a font size change zooms it. Either
  reloads the font and empties the atlas on the next draw. Scale tested by flipping 1x and 1.5x
  in a scratch copy (hedl sends 1x); font size under sc2host: 40 gives 95x25 where 20 gives
  190x50. `hello` zooms with Ctrl+=, Ctrl+- and Ctrl+0.
- **Styles** (settled 2026-11-14): a cell is `wide:1 | style:10 | codepoint:21`, and the program
  owns `styles[1024]` (RGBA foreground and background, bold, italic, underline). Cells stay
  32 bits; the 12 KB table is copied only when it changes. Alpha 0 replaced the `clear` bit.
  The host's 16 colors became the `host_palette` input, for programs that follow its theme.
  Italic is FreeType's synthetic oblique; a wide glyph is two atlas slots, one per half.
- **Settings** are one struct of defaults in `host/settings.c`: font, size, palette, first
  window size, rows per wheel notch, and the cursor theme and size for the xcursor fallback.
  sc2host reads no files and no environment (the fallback no longer reads `XCURSOR_*`).
  Users patch the values, the same way they patch anything else. A missing font or a 0 size
  stops sc2host with one `sc2host:` line.
`seat-loss` is built (2026-11-13). `atlas-eviction` and `cursor-fallback` are built (2026-11-14).

## Workstream H: program metadata

Built 2026-11-14: `title` and `app_id`, written by the program and applied after each commit.
An empty title shows the program's file name. Size requests and see-through areas are left to
hosts that want them (patches); sc2host is one program, one window.

Hardened the same day against programs that break the contract (`include/contract.md`, "What
a host must not trust"): sealed memfd, commit reads that give up after 100 ms, at most
`SC2_CELL_CAPACITY` packed cells per frame, strict UTF-8 for names, and a 2 s grace after
`closed` before the program is killed. A lost compositor makes sc2host exit 1.
`tests/hostile.c` covers the first three under a Wayland session.

## Workstream C: core

`sc2_tick` order: publish, wait, receive. So the program reads the newest input.

<!-- (skeleton :tags core) -->

_No holes match this query._

<!-- /skeleton -->

## Workstream W: window

GNOME draws no borders for clients. Whether the library draws its own is not decided.

<!-- (skeleton :tags window) -->

_No holes match this query._

<!-- /skeleton -->

## Workstream I: input

Input methods cannot be checked on hedl. They need sway or Hyprland with fcitx5.

<!-- (skeleton :tags input) -->

_1 holes, 1 marks, 1 ready. Generated by `swiss sync`; edits inside this block are lost._

- **input-method** · input · ready
  - [host/text_input.c:3](host/text_input.c#L3) · gap · makes no text-input-v3 object; composition stays empty and committed text never reaches the ring.

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
