// otkhost: runs one OTK program and draws it in a Wayland window.
#pragma once

#include <poll.h>
#include <stdint.h>
#include <sys/types.h>
#define VK_USE_PLATFORM_WAYLAND_KHR
#include <vulkan/vulkan.h>
#include <wayland-client.h>
#include <wayland-cursor.h>
#include <xkbcommon/xkbcommon.h>
#include <ft2build.h>
#include FT_FREETYPE_H

#include "contract.h"
#include "cursor-shape-v1.h"
#include "fractional-scale-v1.h"
#include "text-input-unstable-v3.h"
#include "viewporter.h"
#include "xdg-shell.h"

enum { SWAPCHAIN_IMAGE_LIMIT = 8 };

enum {  // powers of two, so draw.comp wraps with a mask
    SLOT_CAPACITY        = 4096,
    GLYPH_TABLE_CAPACITY = 2 * SLOT_CAPACITY,  // at most half full, so a probe always ends
    MISS_TABLE_CAPACITY  = 1024,
};

typedef struct {
    struct wl_compositor                  *compositor;
    struct wl_seat                        *seat;
    struct wl_shm                         *shared_memory;
    struct wl_data_device_manager         *data_device_manager;
    struct xdg_wm_base                    *window_manager;
    struct wp_viewporter                  *viewporter;
    struct wp_fractional_scale_manager_v1 *fractional_scale_manager;
    struct wp_cursor_shape_manager_v1     *cursor_shape_manager;
    struct zwp_text_input_manager_v3      *text_input_manager;
} Globals;  // 0 where the compositor lacks one

typedef struct {
    struct wl_surface             *surface;
    struct xdg_surface            *shell_surface;
    struct xdg_toplevel           *toplevel;
    struct wp_viewport            *viewport;          // 0 without fractional scale
    struct wp_fractional_scale_v1 *fractional_scale;
    uint32_t surface_width, surface_height;    // the compositor's units
    uint32_t width, height;                    // pixels: the surface size times the scale
    uint32_t requested_width, requested_height;  // from the last toplevel configure; 0 lets us choose
    uint32_t scale;                            // 120ths, from the compositor; font.scale is the one drawn
    bool     configured;                       // the first configure is acknowledged
    bool     frame_pending;                    // a frame callback is outstanding
} Window;

typedef struct {
    struct wl_keyboard *keyboard;
    struct xkb_context *context;
    struct xkb_keymap  *keymap;
    struct xkb_state   *state;
    int32_t  repeat_rate, repeat_delay;        // per second, milliseconds
    uint32_t repeat_scancode;                  // 0 when nothing repeats
} Keyboard;

typedef struct {
    struct wl_pointer                *pointer;
    struct wp_cursor_shape_device_v1 *cursor_shape;
    struct wl_cursor_theme           *cursor_theme;    // only without cursor-shape-v1
    struct wl_surface                *cursor_surface;
    struct wl_cursor_image           *cursor_image;
    uint32_t axis_source;                      // of the current pointer frame
    double   wheel_remainder;                  // eighths not yet in wheel_total
    bool     inside;                           // the mouse is over the window
} Pointer;

typedef struct {
    struct zwp_text_input_v3 *text_input;
} TextInput;

enum { SEND_CAPACITY = 4, CLIPBOARD_PIPE_CAPACITY = 1 + SEND_CAPACITY };

typedef struct {
    struct wl_data_offer *dropped;             // a drop's offer, finished once its bytes arrive
    int      pipe;
    uint32_t length;                           // bytes read so far
    int16_t  drop_column, drop_row;            // eighths, where the drop happened
    uint8_t  drop_node;
    uint8_t  data;                             // OTK_DATA_ bit
    uint8_t  source;                           // OTK_PASTE_CLIPBOARD or OTK_PASTE_DROP
    bool     active;
} Receive;  // the paste or drop being read

typedef struct {
    int      pipe;
    uint32_t length;                           // bytes written so far
} Send;  // another client reading our copy

typedef struct {
    struct wl_data_device *data_device;
    struct wl_data_offer  *selection;          // the clipboard now
    struct wl_data_offer  *drag;               // dragged over the window now
    struct wl_data_source *source;             // set while the copy owns the clipboard
    uint8_t *received;                         // bytes read so far; incoming gets them once complete
    uint8_t *offered;                          // the copy we offer, taken from one whole commit
    Receive  receive;
    Send     sends[SEND_CAPACITY];
    uint32_t send_count;
    uint32_t paste_request;                    // last request served
    uint32_t copy_sequence;                    // last copy offered
    uint32_t copy_length;                      // of offered
    uint16_t newest_types;                     // a bit per data_types row, of the offer just announced
    uint16_t selection_types, drag_types;
    int16_t  drag_column, drag_row;            // eighths
} Clipboard;

typedef struct {
    char     font_path[256];
    char     cursor_theme[64];                 // only without cursor-shape-v1; "" is the default theme
    uint32_t palette[16];                      // RGBA, base16 order
    uint32_t font_size;                        // cell height in pixels at scale 1
    uint32_t cursor_size;                      // pixels
    uint16_t window_columns, window_rows;      // the window's size until the compositor picks one
    uint8_t  wheel_rows;                       // rows per wheel notch
} Settings;

typedef struct {
    FT_Library freetype;
    FT_Face    face;
    char       path[256];                      // what it was loaded from, to tell when settings change
    uint32_t   size, scale;
    uint32_t   cell_width, cell_height;        // pixels, 1:2
    int32_t    baseline;                       // pixels from the cell top
    uint32_t   underline_top, underline_thickness;
} Font;

typedef struct {
    VkBuffer        buffer;
    VkDeviceMemory  memory;
    VkDeviceAddress address;
    void           *mapped;
    VkDeviceSize    size;
} Buffer;  // host visible, read by shaders through its address

typedef struct {
    VkInstance            instance;
    VkPhysicalDevice      physical_device;
    VkDevice              device;
    uint32_t              queue_family;
    VkQueue               queue;
    VkSurfaceKHR          surface;
    VkFormat              format;
    VkSwapchainKHR        swapchain;
    VkExtent2D            extent;              // 0 forces a new swapchain
    uint32_t              image_count;
    VkImage               images[SWAPCHAIN_IMAGE_LIMIT];
    VkImageView           views[SWAPCHAIN_IMAGE_LIMIT];
    VkDescriptorSet       descriptor_sets[SWAPCHAIN_IMAGE_LIMIT];
    VkSemaphore           rendered[SWAPCHAIN_IMAGE_LIMIT];
    VkDescriptorSetLayout descriptor_layout;
    VkDescriptorPool      descriptor_pool;
    VkPipelineLayout      pipeline_layout;
    VkPipeline            pipeline;
    VkCommandPool         command_pool;
    VkCommandBuffer       commands;
    VkSemaphore           acquired;
    VkFence               frame_done;          // one frame in flight
    bool                  host_import;         // pixels import in place; false copies images instead
    VkDeviceSize          import_alignment;
    PFN_vkGetMemoryHostPointerPropertiesEXT memory_host_pointer_properties;
} Graphics;

typedef struct {
    Buffer   coverage;                         // one cell-sized slot per glyph, a byte per pixel
    Buffer   glyph_table;                      // key, slot pairs; the shader looks glyphs up here
    Buffer   miss_table;                       // keys the shader found no slot for
    Buffer   slot_frames;                      // per slot, the last frame that drew it
    uint32_t free_slots[SLOT_CAPACITY];
    uint32_t free_count;
} Atlas;

typedef struct {
    uint32_t storage_offset, size, sequence;
} CopiedImage;  // what a node's image was when last copied to the GPU

typedef struct {
    Buffer   frame;                            // DrawFrame, see draw.c
    Buffer   cells;                            // committed cells, packed per node
    Buffer   pixels;                           // pixels from 0 to the last byte drawn: imported, or a copy
    CopiedImage copied_images[NODE_COUNT];  // copy only; all zero after the copy grows
    uint32_t drawn_sequence;                   // commit_sequence of the commit last drawn
    uint32_t frame_number;                     // of the last frame built
} Drawing;


typedef struct { int32_t left, top, right, bottom; } Rectangle;  // eighths of a cell, screen space

typedef struct {
    Rectangle area;                            // the node's visible area
    Rectangle clip;                            // area cut by every ancestor; empty when hidden
    int32_t   content_left, content_top;       // area's top left, less the scroll
} Placement;

typedef struct {
    otk_shared *shared;                        // the memory shared with the program
    otk_screen *screen;                        // the program's own; read only for pixels
    otk_screen *incoming;                      // written only between input_begin and input_end
    otk_screen *committed;                     // read only under commit_sequence
    uint8_t     input_before[INPUT_SIZE];      // incoming's input at input_begin, to tell if it changed
    int         memory_file;                   // memfd behind shared
    int         commit_event;                  // eventfd; the program writes it after each commit
    int         input_event;                   // eventfd; input_end writes it when input changed
    int         repeat_timer;                  // timerfd; keyboard arms it
    int         program_exit;                  // pidfd; readable once the program exits
    pid_t       program_id;
    uint32_t    input_serial;                  // of the last input event; copy needs it

    struct wl_display *display;
    Globals   globals;
    Window    window;
    Keyboard  keyboard;
    Pointer   pointer;
    TextInput text_input;
    Clipboard clipboard;
    Font      font;
    Graphics  graphics;
    Atlas     atlas;
    Drawing   drawing;
    Settings  settings;                        // changed only in settings_update
    Placement placements[NODE_COUNT];       // drawn nodes, flattened
} Host;

static inline int64_t floor_divide(int64_t dividend, int64_t divisor) {
    int64_t quotient = dividend / divisor;
    return quotient - (dividend % divisor != 0 && (dividend < 0) != (divisor < 0));
}

// wl_fixed is 24.8 fixed point, in surface units; scale in 120ths turns them into pixels.
static inline int16_t fixed_to_eighths(wl_fixed_t position, uint32_t cell_size, uint32_t scale) {
    return floor_divide((int64_t)position * 8 * scale, (int64_t)cell_size * 256 * 120);
}

static inline struct timespec from_nanoseconds(uint64_t nanoseconds) {
    return (struct timespec){.tv_sec = nanoseconds / 1000000000, .tv_nsec = nanoseconds % 1000000000};
}

// For key_transitions and mouse_transitions: moves only when the held state changes. Odd means held.
static inline void transition_set(uint8_t *transitions, bool held) {
    if ((*transitions % 2 == 1) != held)
        (*transitions)++;
}

// main.c
bool failed(const char *reason);
void input_begin(Host *host);
void input_end(Host *host);

// wayland.c
bool wayland_connect(Host *host);
void wayland_disconnect(Host *host);

// window.c
bool window_open(Host *host, const char *title);
void window_close(Host *host);
void window_request_frame(Host *host);
void window_resize(Host *host);

// scale.c
void scale_attach(Host *host);
void scale_detach(Host *host);

// keyboard.c
void keyboard_attach(Host *host);
void keyboard_detach(Host *host);
void keyboard_repeat(Host *host);
void text_append(Host *host, uint32_t codepoint);

// pointer.c
void pointer_attach(Host *host);
void pointer_detach(Host *host);
void pointer_hit(Host *host);

// text_input.c
void text_input_attach(Host *host);

// clipboard.c
void   clipboard_attach(Host *host);
void   clipboard_detach(Host *host);
void   clipboard_serve(Host *host);
size_t clipboard_poll(const Host *host, struct pollfd *waiting);
void   clipboard_transfer(Host *host);

// settings.c
void settings_update(Host *host);

// font.c
bool font_load(Font *font, const Settings *settings, uint32_t scale);
bool font_matches(const Font *font, const Settings *settings, uint32_t scale);
void font_close(Font *font);
void font_draw(const Font *font, uint32_t codepoint, bool bold, uint8_t *coverage);

// flatten.c
void     flatten(const otk_node nodes[NODE_COUNT], Rectangle window, Placement placements[NODE_COUNT]);
uint8_t  node_at(const Placement placements[NODE_COUNT], int32_t column, int32_t row);

// graphics.c
bool graphics_open(Host *host);
void graphics_close(Graphics *graphics);
bool graphics_resize(Graphics *graphics, uint32_t width, uint32_t height);
void graphics_wait(Graphics *graphics);
bool graphics_present(Graphics *graphics, VkDeviceAddress frame);
bool buffer_create(Graphics *graphics, VkDeviceSize size, Buffer *buffer);
bool buffer_import(Graphics *graphics, void *host_memory, VkDeviceSize size, Buffer *buffer);
bool buffer_reserve(Graphics *graphics, Buffer *buffer, VkDeviceSize size);
void buffer_destroy(Graphics *graphics, Buffer *buffer);

// atlas.c
bool atlas_open(Host *host);
void atlas_close(Host *host);
bool atlas_fill_misses(Host *host);

// draw.c
bool draw_open(Host *host);
void draw_close(Host *host);
bool draw_due(Host *host);
void draw_frame(Host *host);
