// sc*2: the whole contract between a program and its host. Strict C99 so every FFI can parse it.
// sc2_screen holds no pointers; it lives in memory shared with the host. include/contract.md has the rules.
#ifndef SC2_H
#define SC2_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    SC2_TEXT_CAPACITY  = 4096,         // codepoints in the typed text ring
    SC2_PASTE_CAPACITY = 67108864,     // 64 MB, a 4K screenshot as PNG
    SC2_COPY_CAPACITY  = 67108864,
    SC2_CELL_CAPACITY  = 16777216,
    SC2_PIXEL_CAPACITY = 1073741824    // reserved address space; untouched pages cost nothing
};

// A codepoint drawn in one of the program's styles. The cell after a wide cell is its right half:
// the wide glyph spans both in the wide cell's style, and the right half's own bits are not drawn.
typedef uint32_t sc2_cell;  // wide:1 | style:10 | codepoint:21

enum {
    SC2_CELL_CODEPOINT_SHIFT = 0,
    SC2_CELL_STYLE_SHIFT     = 21,
    SC2_CELL_WIDE_SHIFT      = 31
};

enum { SC2_STYLE_CAPACITY = 1024 };

// Colors are RGBA, 0xRRGGBBAA. A background with alpha 0 shows whatever is under the cell.
typedef struct {
    uint32_t foreground, background;
    uint8_t  bold, italic, underline;
} sc2_style;

enum { SC2_NODE_EMPTY, SC2_NODE_CELLS, SC2_NODE_PIXELS };
enum { SC2_PASTE_CLIPBOARD, SC2_PASTE_DROP };
enum {
    SC2_DATA_TEXT = 1, SC2_DATA_URI_LIST = 2, SC2_DATA_HTML = 4, SC2_DATA_PNG = 8,
    SC2_DATA_JPEG = 16, SC2_DATA_WEBP = 32, SC2_DATA_AVIF = 64
};

// mouse_transitions index: evdev BTN_LEFT to BTN_TASK
enum {
    SC2_MOUSE_LEFT, SC2_MOUSE_RIGHT, SC2_MOUSE_MIDDLE, SC2_MOUSE_SIDE,
    SC2_MOUSE_EXTRA, SC2_MOUSE_FORWARD, SC2_MOUSE_BACK, SC2_MOUSE_TASK
};

enum { SC2_MOUSE_OUTSIDE = 255 };  // mouse_node when the mouse is not over the window

// Fields run largest to smallest. Each writer's fields stay together, so a tick copies one range each way.
typedef struct {
    uint32_t storage_offset;                 // into cells, or pixels at a multiple of 4
    uint32_t sequence;                       // bumped after each image frame
    int16_t  column, row;                    // in parent's content
    uint16_t columns, rows;                  // visible area; clips children
    uint16_t content_width, content_height;  // cells, or pixels for images
    uint8_t  kind;
    uint8_t  parent;                         // lower index than this node
    uint8_t  scroll_column, scroll_row;      // eighths, used modulo 8: smooth scroll only, never a whole cell
} sc2_node;

typedef struct {
    // areas; pixels at offset 0 starts on a page
    uint8_t  pixels[SC2_PIXEL_CAPACITY];
    sc2_cell cells[SC2_CELL_CAPACITY];
    uint8_t  paste[SC2_PASTE_CAPACITY];      // written by host
    uint8_t  copy[SC2_COPY_CAPACITY];        // written by program

    // written by host, copied in at each tick
    uint32_t text[SC2_TEXT_CAPACITY];
    uint8_t  key_transitions[512];           // by scancode; odd means held
    uint32_t composition[32];                // input method preview
    uint32_t host_palette[16];               // the host's colors, RGBA, base16 order, to build styles from
    uint8_t  mouse_transitions[8];
    uint32_t text_write_index;
    uint32_t paste_sequence;                 // bumped when a paste or drop is complete
    uint32_t paste_length;                   // 0 when the paste or drop failed
    int32_t  wheel_total;                    // eighths of a row
    uint16_t columns, rows;
    uint16_t cell_width, cell_height;        // device pixels; an image with this many per cell draws 1:1
    uint16_t host_font_size;                 // the host's own font_size, to zoom from
    int16_t  mouse_column, mouse_row;        // eighths of a cell
    int16_t  drop_column, drop_row;          // eighths, where the drop happened
    uint8_t  mouse_node;                     // topmost node under the mouse, or SC2_MOUSE_OUTSIDE
    uint8_t  drop_node;
    uint8_t  paste_offered;                  // SC2_DATA_ bits on the clipboard now
    uint8_t  paste_source;                   // SC2_PASTE_CLIPBOARD or SC2_PASTE_DROP
    uint8_t  paste_data;                     // one SC2_DATA_ bit
    uint8_t  closed;

    // written by program, copied out at each tick
    sc2_style styles[SC2_STYLE_CAPACITY];
    sc2_node nodes[255];
    char     title[256];                     // UTF-8, ends at the first 0; empty shows the file name
    char     app_id[64];                     // for window rules; empty sets none
    uint32_t paste_request;                  // bump to ask for the clipboard
    uint32_t copy_sequence;                  // bump after writing copy
    uint32_t copy_length;
    uint16_t tick_rate;                      // ticks per second, default 240
    uint16_t idle_tick_rate;                 // ticks per second while idle; 0 sleeps until input
    uint16_t font_size;                      // cell height in pixels at scale 1; 0 uses host_font_size
    uint8_t  paste_request_data;             // one SC2_DATA_ bit from paste_offered
    uint8_t  copy_data;                      // SC2_DATA_ bits the copy is offered as
} sc2_screen;

// Host contract. The host runs the program with three inherited fds, named in the environment:
// SC2_SHARED_FD, a memfd holding sc2_shared; SC2_COMMIT_FD, an eventfd the program writes after
// each commit; SC2_INPUT_FD, an eventfd the host writes after input changes.

enum { SC2_MAGIC = 0x322a6373, SC2_VERSION = 2 };  // magic is "sc*2" in memory

// A region's sequence is odd while its writer writes it. A reader that saw an odd or changed
// sequence across its read saw a torn region, and reads again.
typedef struct {
    uint32_t magic, version;
    uint32_t input_sequence;                 // incoming, written by the host
    uint32_t commit_sequence;                // committed, written by the program at each tick
    uint8_t  page_rest[4080];                // so screen.pixels starts on a page
} sc2_header;

typedef struct {
    sc2_header header;
    sc2_screen screen;                       // the program's own; the host reads only pixels from it
    sc2_screen incoming;                     // input from the host; each tick copies it into screen
    sc2_screen committed;                    // each tick copies the program's fields and cells here
} sc2_shared;

sc2_screen *sc2_open(void);                  // 0 when not run by a host
int         sc2_tick(sc2_screen *screen);    // commit, sleep to next tick; 0 once closed
void        sc2_close(sc2_screen *screen);

#ifdef __cplusplus
}
#endif

#endif
