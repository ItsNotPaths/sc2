# sc\*2

sc\*2 (shared cell screen contract) lets a program describe its screen in shared
memory as a tree of nodes holding cell grids and images. A host reads that memory,
draws it, and writes the input back. The program makes no draw calls and sends no
messages: it writes its screen and calls `sc2_tick` once per frame.

This repo has three parts:

- `include/`: the contract. `sc2.h` is the shared memory layout, and
  `contract.md` has the rules for anyone writing their own host.
- `libsc2/`: the small library programs link. It has no Wayland and no GPU code.
- `host/`: `sc2host`, a reference host that draws one program in a Wayland
  window with Vulkan and FreeType.

## Build

You need clang, pkg-config, glslc, and the development files for wayland-client,
wayland-cursor, xkbcommon and vulkan.

```
./download-deps.sh   # wayland-protocols, wlr-protocols and FreeType into vendor/
./build.sh
```

## Run

```
build/sc2host build/examples/hello
build/sc2host build/examples/image
```

`sc2host PROGRAM [ARGUMENT...]` opens a window, starts the program, and exits
with the program's exit status. `sc2host --layer=EDGE:CELLS PROGRAM` opens a
wlr layer-shell panel instead, CELLS thick on EDGE (left, right, top, bottom),
its namespace the program's app id so compositor rules can match it.

## A program

```c
#include <sc2.h>

int main(void) {
    sc2_screen *screen = sc2_open();
    if (!screen)
        return 1;
    const char *text = "hello";
    while (sc2_tick(screen)) {
        screen->styles[1] = (sc2_style){.foreground = screen->host_palette[5], .background = screen->host_palette[0]};
        screen->nodes[0] = (sc2_node){.kind = SC2_NODE_CELLS,
                                      .content_width = screen->columns, .content_height = screen->rows};
        for (int index = 0; text[index]; index++)
            screen->cells[index] = (sc2_cell)text[index] << SC2_CELL_CODEPOINT_SHIFT | 1u << SC2_CELL_STYLE_SHIFT;
    }
    sc2_close(screen);
    return 0;
}
```

```
cc -std=c99 -I include program.c build/libsc2.a -o program
build/sc2host ./program
```

`examples/` has two larger programs: `hello` covers typing, the mouse, scrolling,
the clipboard and zoom, and `image` covers pixel nodes and dragging.

## Settings

sc2host reads no config files. Edit `host/settings.c` for the font, size, colors
and cursor, then rebuild. The default font is
`/usr/share/fonts/TTF/DejaVuSansMono.ttf`, and sc2host stops with an error if it
is missing.

## Tests

```
tests/run.sh
```

The contract tests need no display. The tests that run programs under sc2host
need a Wayland session and are skipped without one.

This was previously called omni when in development.