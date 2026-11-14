// OTK: the whole interface. Strict C99 so every FFI can parse it.
// otk_screen holds no pointers; it lives in shared memory.
#ifndef OTK_H
#define OTK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    OTK_TEXT_CAPACITY  = 4096,         // codepoints in the typed text ring
    OTK_PASTE_CAPACITY = 67108864,     // 64 MB, a 4K screenshot as PNG
    OTK_COPY_CAPACITY  = 67108864,
    OTK_CELL_CAPACITY  = 16777216,
    OTK_PIXEL_CAPACITY = 1073741824    // reserved address space; untouched pages cost nothing
};

typedef uint32_t otk_cell;  // clear:1 | underline:1 | bold:1 | background:4 | foreground:4 | codepoint:21

enum {
    OTK_CELL_CODEPOINT_SHIFT   = 0,
    OTK_CELL_FOREGROUND_SHIFT  = 21,
    OTK_CELL_BACKGROUND_SHIFT  = 25,
    OTK_CELL_BOLD_SHIFT        = 29,
    OTK_CELL_UNDERLINE_SHIFT   = 30,
    OTK_CELL_CLEAR_SHIFT       = 31
};

enum { OTK_NODE_EMPTY, OTK_NODE_CELLS, OTK_NODE_PIXELS };
enum { OTK_PASTE_CLIPBOARD, OTK_PASTE_DROP };
enum {
    OTK_DATA_TEXT = 1, OTK_DATA_URI_LIST = 2, OTK_DATA_HTML = 4, OTK_DATA_PNG = 8,
    OTK_DATA_JPEG = 16, OTK_DATA_WEBP = 32, OTK_DATA_AVIF = 64
};

// mouse_transitions index: evdev BTN_LEFT to BTN_TASK
enum {
    OTK_MOUSE_LEFT, OTK_MOUSE_RIGHT, OTK_MOUSE_MIDDLE, OTK_MOUSE_SIDE,
    OTK_MOUSE_EXTRA, OTK_MOUSE_FORWARD, OTK_MOUSE_BACK, OTK_MOUSE_TASK
};

enum { OTK_MOUSE_OUTSIDE = 255 };  // mouse_node when the mouse is not over the window

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
} otk_node;

typedef struct {
    // areas; pixels at offset 0 starts on a page
    uint8_t  pixels[OTK_PIXEL_CAPACITY];
    otk_cell cells[OTK_CELL_CAPACITY];
    uint8_t  paste[OTK_PASTE_CAPACITY];      // written by library
    uint8_t  copy[OTK_COPY_CAPACITY];        // written by program

    // written by library, copied in at each tick
    uint32_t text[OTK_TEXT_CAPACITY];
    uint8_t  key_transitions[512];           // by scancode; odd means held
    uint32_t composition[32];                // input method preview
    uint32_t palette[16];                    // RGBA, base16 order
    uint8_t  mouse_transitions[8];
    uint32_t text_write_index;
    uint32_t paste_sequence;                 // bumped when a paste or drop is complete
    uint32_t paste_length;                   // 0 when the paste or drop failed
    int32_t  wheel_total;                    // eighths of a row
    uint16_t columns, rows;
    int16_t  mouse_column, mouse_row;        // eighths of a cell
    int16_t  drop_column, drop_row;          // eighths, where the drop happened
    uint8_t  mouse_node;                     // topmost node under the mouse, or OTK_MOUSE_OUTSIDE
    uint8_t  drop_node;
    uint8_t  paste_offered;                  // OTK_DATA_ bits on the clipboard now
    uint8_t  paste_source;                   // OTK_PASTE_CLIPBOARD or OTK_PASTE_DROP
    uint8_t  paste_data;                     // one OTK_DATA_ bit
    uint8_t  closed;

    // written by program, copied out at each tick
    otk_node nodes[255];
    uint32_t paste_request;                  // bump to ask for the clipboard
    uint32_t copy_sequence;                  // bump after writing copy
    uint32_t copy_length;
    uint16_t tick_rate;                      // ticks per second, default 240
    uint16_t idle_tick_rate;                 // ticks per second while idle; 0 sleeps until input
    uint8_t  paste_request_data;             // one OTK_DATA_ bit from paste_offered
    uint8_t  copy_data;                      // OTK_DATA_ bits the copy is offered as
} otk_screen;

otk_screen *otk_open(const char *title);   // 0 if no window could open
int         otk_tick(otk_screen *screen);  // commit, sleep to next tick; 0 once closed
void        otk_close(otk_screen *screen);

#ifdef __cplusplus
}
#endif

#endif
