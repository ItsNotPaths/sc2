// OTK: the whole interface. Strict C99 so every FFI can parse it.
// otk_screen holds no pointers; it lives in shared memory.
#ifndef OTK_H
#define OTK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    TEXT_CAPACITY  = 4096,         // codepoints in the typed text ring
    PASTE_CAPACITY = 67108864,     // 64 MB, a 4K screenshot as PNG
    COPY_CAPACITY  = 67108864,
    CELL_CAPACITY  = 16777216,
    PIXEL_CAPACITY = 1073741824    // reserved address space; untouched pages cost nothing
};

typedef uint32_t Cell;  // clear:1 | underline:1 | bold:1 | background:4 | foreground:4 | codepoint:21

enum {
    CELL_CODEPOINT_SHIFT   = 0,
    CELL_FOREGROUND_SHIFT  = 21,
    CELL_BACKGROUND_SHIFT  = 25,
    CELL_BOLD_SHIFT        = 29,
    CELL_UNDERLINE_SHIFT   = 30,
    CELL_CLEAR_SHIFT       = 31
};

enum { NODE_EMPTY, NODE_CELLS, NODE_PIXELS };
enum { PASTE_CLIPBOARD, PASTE_DROP };
enum {
    DATA_TEXT = 1, DATA_URI_LIST = 2, DATA_HTML = 4, DATA_PNG = 8,
    DATA_JPEG = 16, DATA_WEBP = 32, DATA_AVIF = 64
};

// mouse_transitions index: evdev BTN_LEFT to BTN_TASK
enum {
    MOUSE_LEFT, MOUSE_RIGHT, MOUSE_MIDDLE, MOUSE_SIDE,
    MOUSE_EXTRA, MOUSE_FORWARD, MOUSE_BACK, MOUSE_TASK
};

enum { MOUSE_OUTSIDE = 255 };  // mouse_node when the mouse is not over the window

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
} Node;

typedef struct {
    // areas; pixels at offset 0 starts on a page
    uint8_t  pixels[PIXEL_CAPACITY];
    Cell     cells[CELL_CAPACITY];
    uint8_t  paste[PASTE_CAPACITY];          // written by library
    uint8_t  copy[COPY_CAPACITY];            // written by program

    // written by library, copied in at each tick
    uint32_t text[TEXT_CAPACITY];
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
    uint8_t  mouse_node;                     // topmost node under the mouse, or MOUSE_OUTSIDE
    uint8_t  drop_node;
    uint8_t  paste_offered;                  // DATA_ bits on the clipboard now
    uint8_t  paste_source;                   // PASTE_CLIPBOARD or PASTE_DROP
    uint8_t  paste_data;                     // one DATA_ bit
    uint8_t  closed;

    // written by program, copied out at each tick
    Node     nodes[255];
    uint32_t paste_request;                  // bump to ask for the clipboard
    uint32_t copy_sequence;                  // bump after writing copy
    uint32_t copy_length;
    uint16_t tick_rate;                      // ticks per second, default 240
    uint8_t  animating;
    uint8_t  paste_request_data;             // one DATA_ bit from paste_offered
    uint8_t  copy_data;                      // DATA_ bits the copy is offered as
} otk_screen;

otk_screen *otk_open(const char *title);   // 0 if no window could open
int         otk_tick(otk_screen *screen);  // commit, sleep to next tick; 0 once closed
void        otk_close(otk_screen *screen);

#ifdef __cplusplus
}
#endif

#endif
