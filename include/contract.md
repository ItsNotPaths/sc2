# The sc\*2 contract

A program describes its screen in shared memory. A host draws that screen and writes input
back. This file and `sc2.h` are the whole contract. `libsc2/` implements the program's side,
and `host/` (sc2host) is one host. Anyone can write another host from these two files.

## How a host starts a program

The host:

1. Creates a memfd of `sizeof(sc2_shared)` bytes, seals it against shrinking and growing,
   maps it, and writes `SC2_MAGIC` and `SC2_VERSION` into the header. Untouched pages cost
   nothing, so the size is mostly address space.
2. Writes the first input into `incoming`: at least `columns`, `rows`, `host_palette`, and
   `mouse_node` set to `SC2_MOUSE_OUTSIDE`.
3. Creates two eventfds: one the program writes after each commit, one the host writes after
   input changes.
4. Runs the program with the three fds inherited (close-on-exec cleared) and their numbers in
   `SC2_SHARED_FD`, `SC2_COMMIT_FD` and `SC2_INPUT_FD`.

`sc2_open` returns 0 when those variables are missing, and when the magic or version differs.

## Memory

`sc2_shared` holds a one-page header and three `sc2_screen` regions.

| Region | Written by | Read by |
|---|---|---|
| `header` | the host, once; each sequence by its region's writer | both |
| `screen` | the program, at any time | the host, for `pixels` only |
| `incoming` | the host | the program, at each tick |
| `committed` | the program, at each tick | the host |

In `sc2_screen`, the fields from `key_transitions` up to `nodes` are input. The fields from
`nodes` to the end, the cells of each cell node, and `copy` belong to the program.

## Sequences

`incoming` and `committed` each have one writer and a sequence in the header. The writer adds
1 to the sequence before it writes, so the sequence is odd while it writes, and adds 1 again
after. A reader reads the sequence, reads what it needs, then reads the sequence again. If the
first value was odd, or the two values differ, the read may be torn, and the reader reads
again. Neither side waits for the other in any other way. `libsc2/contract.h` has this in C.

## Commits

At each tick that changed something, the program copies into `committed`: the fields from
`nodes` to the end, the cells of every cell node, and `copy` when `copy_sequence` changed.
Then it writes 1 to the commit eventfd. A tick that changed nothing commits nothing.

The host takes nodes and cells from `committed`. It takes images from `screen.pixels`, which
the program never copies. A node's `sequence` changes when its image changes. `pixels` is at
offset 0 of `screen`, and `screen` starts after the one-page header, so a host can import
`pixels` into the GPU in place.

## Input

The host writes `incoming`, then writes 1 to the input eventfd if any input field changed. At
each tick the program copies `incoming` into `screen`. An idle program sleeps on the input
eventfd, and any write to it runs one tick. The program's own threads may write it too, to have
their work drawn; only the thread calling `sc2_tick` touches the screen.

- Text: the host writes ring entries, then advances `text_write_index`.
- Paste: the host writes the bytes into `paste`, then `paste_length`, `paste_data` and
  `paste_source`, then bumps `paste_sequence`. Every request and every drop gets exactly one
  answer, and `paste_length` 0 means it failed.

## Title and app_id

`title` and `app_id` are UTF-8 and end at the first 0 byte. The host applies them after each
commit, so a program can change its title while it runs. An empty `title` leaves the naming to
the host; sc2host shows the program's file name.

The host shows no window until the first commit, so a compositor sees the first commit's
`app_id` before the window maps; window rules match on it. Set `app_id` before the first tick.

## Placement

The program asks where it shows with `surface_kind`, read once, at the first commit, with
`surface_position`, `surface_columns` and `surface_rows`. Set them before the first tick.

- `SC2_SURFACE_WINDOW`: an ordinary window. The other fields are not read.
- `SC2_SURFACE_PANEL`: runs the full length of one edge (`SC2_POSITION_TOP`, `_BOTTOM`, `_LEFT`
  or `_RIGHT`), `surface_rows` thick on the top or bottom, `surface_columns` on the left or right.
  Windows keep clear of it. It takes no keyboard.
- `SC2_SURFACE_POPUP`: `surface_columns` by `surface_rows`, at any `SC2_POSITION_`, over
  everything. It takes the keyboard when the compositor gives it, and the host closes it when
  it loses the keyboard.

A panel or popup keeps its cells when the font size changes. A host shows a window in place of
a panel or popup it cannot make, or one that asks for no cells or for a position its kind does
not take. sc2host makes panels and popups with wlr layer-shell, named by the program's `app_id`,
and holds their cells to 1000 a side.

## Font

The program picks its font with `font_path`: a scalable font file, ending at the first 0 byte, or
empty for the host's own font. The host loads it at the next draw, and again whenever the program
writes a different one. A font that will not load is reported by the host and not tried again;
the host draws in its own font in its place.

The program picks its text size with `font_size`: the cell height in pixels at scale 1, or 0 for
the host's own size, which the host reports in `host_font_size`. The host multiplies it by the
output's scale, works out how many cells fit, and reports `columns` and `rows`. A program that
zooms on Ctrl+= writes a new `font_size` and lays out in the grid that comes back.

The host also reports `cell_width` and `cell_height`: one cell in device pixels. An image node
whose `content_width` and `content_height` are its `columns` and `rows` times these draws one
image pixel per screen pixel, so a program can draw its own glyphs, lines and fills sharp.

## Styles

A cell names one of the program's `styles`: a foreground and a background in RGBA, with bold,
italic and underline. A background with alpha 0 shows whatever is under the cell. A wide cell
draws its glyph across itself and the next cell, in its own style; the next cell's own bits are
not drawn. The host reports its own colors in `host_palette`, in base16 order, for a program
that wants to follow the host's theme. Under every node is the window background, the host's
color 0.

The program can give the host its colors with `scheme_path`: a base16 scheme file, ending at the
first 0 byte, or empty for the host's own colors. Each color is a line `base0X: "#rrggbb"`;
indent, quotes and `#` are optional. The host reads it at the next commit that names a different
file or finds the file changed, and takes all 16 colors from it: the window background and
`host_palette` both follow. A scheme that will not read is reported by the host, which keeps its
own colors until the program names another file or the file changes.

## Closing

The host sets `closed` in `incoming`. `sc2_tick` then returns 0 and the program exits. The
host learns of the exit from the process itself; sc2host watches a pidfd.

The contract does not tell a program that its host died. sc2host sets `PR_SET_PDEATHSIG`, so
the program gets `SIGTERM` when sc2host exits.

## What a host must not trust

Everything the program writes may be wrong, by accident or on purpose. A host keeps working
whatever the program does:

- It seals the memfd, so the program cannot shrink the memory under the host's reads.
- It reads only the cells and bytes the node rules allow (`libsc2/contract.h`), clamps
  `copy_length`, ends `title`, `app_id`, `font_path` and `scheme_path` itself, and replaces bytes that are not valid UTF-8.
  It reads `scheme_path` only when it is a regular file; sc2host also skips one over 64 KB.
- It gives up on a read when the program keeps `commit_sequence` odd, and tries again at the
  next commit. sc2host waits 100 ms.
- It bounds its own work per frame. sc2host packs at most `SC2_CELL_CAPACITY` cells, however
  many nodes share them, and holds `font_size` between 6 and 200.
- It kills a program that has not exited some time after `closed`. sc2host waits 2 s.

`tests/hostile.c` breaks the contract on purpose, and `tests/run.sh` runs it under sc2host.

## What the contract leaves to the host

Its own font, the scale, which keys reach the program, how the clipboard is reached,
and how many programs it shows. sc2host shows one program in one Wayland surface.

## Versions

`SC2_VERSION` changes whenever the layout of `sc2_shared` or a rule in this file changes.
