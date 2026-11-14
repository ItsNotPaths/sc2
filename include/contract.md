# The OTK contract

A program describes its screen in shared memory. A host draws that screen and writes input
back. This file and `otk.h` are the whole contract. `libotk/` implements the program's side,
and `host/` (otkhost) is one host. Anyone can write another host from these two files.

## How a host starts a program

The host:

1. Creates a memfd of `sizeof(otk_shared)` bytes, maps it, and writes `OTK_MAGIC` and
   `OTK_VERSION` into the header. Untouched pages cost nothing, so the size is mostly address
   space.
2. Writes the first input into `incoming`: at least `columns`, `rows`, `palette`, and
   `mouse_node` set to `OTK_MOUSE_OUTSIDE`.
3. Creates two eventfds: one the program writes after each commit, one the host writes after
   input changes.
4. Runs the program with the three fds inherited (close-on-exec cleared) and their numbers in
   `OTK_SHARED_FD`, `OTK_COMMIT_FD` and `OTK_INPUT_FD`.

`otk_open` returns 0 when those variables are missing, and when the magic or version differs.

## Memory

`otk_shared` holds a one-page header and three `otk_screen` regions.

| Region | Written by | Read by |
|---|---|---|
| `header` | the host, once; each sequence by its region's writer | both |
| `screen` | the program, at any time | the host, for `pixels` only |
| `incoming` | the host | the program, at each tick |
| `committed` | the program, at each tick | the host |

In `otk_screen`, the fields from `key_transitions` up to `nodes` are input. The fields from
`nodes` to the end, the cells of each cell node, and `copy` belong to the program.

## Sequences

`incoming` and `committed` each have one writer and a sequence in the header. The writer adds
1 to the sequence before it writes, so the sequence is odd while it writes, and adds 1 again
after. A reader reads the sequence, reads what it needs, then reads the sequence again. If the
first value was odd, or the two values differ, the read may be torn, and the reader reads
again. Neither side waits for the other in any other way. `libotk/contract.h` has this in C.

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

## Closing

The host sets `closed` in `incoming`. `otk_tick` then returns 0 and the program exits. The
host learns of the exit from the process itself; otkhost watches a pidfd.

The contract does not tell a program that its host died. otkhost sets `PR_SET_PDEATHSIG`, so
the program gets `SIGTERM` when otkhost exits.

## What the contract leaves to the host

The font and cell size, the scale, which keys reach the program, how the clipboard is reached,
and how many programs it shows and where. otkhost shows one program in one Wayland window.

## Versions

`OTK_VERSION` changes whenever the layout of `otk_shared` or a rule in this file changes.
