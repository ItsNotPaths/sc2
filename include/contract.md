# The sc\*2 contract

A program describes its screen in shared memory. A host draws that screen and writes input
back. This file and `sc2.h` are the whole contract. `libsc2/` implements the program's side,
and `host/` (sc2host) is one host. Anyone can write another host from these two files.

## How a host starts a program

The host:

1. Creates a memfd of `sizeof(sc2_shared)` bytes, seals it against shrinking and growing,
   maps it, and writes `SC2_MAGIC` and `SC2_VERSION` into the header. Untouched pages cost
   nothing, so the size is mostly address space.
2. Writes the first input into `incoming`: at least `columns`, `rows`, `palette`, and
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
eventfd.

- Text: the host writes ring entries, then advances `text_write_index`.
- Paste: the host writes the bytes into `paste`, then `paste_length`, `paste_data` and
  `paste_source`, then bumps `paste_sequence`. Every request and every drop gets exactly one
  answer, and `paste_length` 0 means it failed.

## Title and app_id

`title` and `app_id` are UTF-8 and end at the first 0 byte. The host applies them after each
commit, so a program can change its title while it runs. An empty `title` leaves the naming to
the host; sc2host shows the program's file name.

## Font size

The program picks its text size with `font_size`: the cell height in pixels at scale 1, or 0 for
the host's own size, which the host reports in `host_font_size`. The host multiplies it by the
output's scale, works out how many cells fit, and reports `columns` and `rows`. A program that
zooms on Ctrl+= writes a new `font_size` and lays out in the grid that comes back.

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
  `copy_length`, ends `title` and `app_id` itself, and replaces bytes that are not valid UTF-8.
- It gives up on a read when the program keeps `commit_sequence` odd, and tries again at the
  next commit. sc2host waits 100 ms.
- It bounds its own work per frame. sc2host packs at most `SC2_CELL_CAPACITY` cells, however
  many nodes share them, and holds `font_size` between 6 and 200.
- It kills a program that has not exited some time after `closed`. sc2host waits 2 s.

`tests/hostile.c` breaks the contract on purpose, and `tests/run.sh` runs it under sc2host.

## What the contract leaves to the host

The font file, the scale, which keys reach the program, how the clipboard is reached,
and how many programs it shows and where. sc2host shows one program in one Wayland window.

## Versions

`SC2_VERSION` changes whenever the layout of `sc2_shared` or a rule in this file changes.
