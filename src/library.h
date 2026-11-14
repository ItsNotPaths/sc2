// Library side: process-local state that must stay out of otk_screen.
#pragma once

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#define VK_USE_PLATFORM_WAYLAND_KHR
#include <vulkan/vulkan.h>
#include <wayland-client.h>
#include <wayland-cursor.h>
#include <xkbcommon/xkbcommon.h>
#include <ft2build.h>
#include FT_FREETYPE_H

#include "otk.h"
#include "cursor-shape-v1.h"
#include "fractional-scale-v1.h"
#include "text-input-unstable-v3.h"
#include "viewporter.h"
#include "xdg-shell.h"

enum { NODE_CAPACITY = 255, SWAPCHAIN_IMAGE_LIMIT = 8 };

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
    struct wl_surface   *surface;
    struct xdg_surface  *shell_surface;
    struct xdg_toplevel *toplevel;
    uint32_t width, height;                    // pixels
    uint32_t requested_width, requested_height;  // from the last toplevel configure; 0 lets us choose
    uint32_t scale;                            // 120ths
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

typedef struct {
    struct wl_data_device *data_device;
    struct wl_data_offer  *selection;          // the clipboard now
    struct wl_data_offer  *drop;               // dragged over the window now
    struct wl_data_source *source;             // set while the copy owns the clipboard
    uint32_t paste_request;                    // last request served
    uint32_t copy_sequence;                    // last copy offered
} Clipboard;

typedef struct {
    FT_Library freetype;
    FT_Face    face;
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
    Buffer   frame;                            // DrawFrame, see draw.c
    Buffer   cells;                            // committed cells, packed per node
    uint32_t drawn_commit;
    uint32_t frame_number;                     // of the last frame built
} Drawing;

typedef struct {
    char     font_path[256];
    uint32_t font_size;                        // cell height in pixels at scale 1
    uint32_t palette[16];                      // RGBA, base16 order
} Settings;

typedef struct { int32_t left, top, right, bottom; } Rectangle;  // eighths of a cell, screen space

typedef struct {
    Rectangle area;                            // the node's visible area
    Rectangle clip;                            // area cut by every ancestor; empty when hidden
    int32_t   content_left, content_top;       // area's top left, less the scroll
} Placement;

typedef struct {
    otk_screen *screen;                        // shared with the program
    otk_screen *incoming;                      // library writes input here; tick copies it into screen
    otk_screen *committed;                     // tick copies nodes and cells here; draw reads them
    int         memory_file;                   // memfd behind screen

    pthread_t       thread;
    pthread_mutex_t lock;                      // guards incoming and committed
    pthread_cond_t  input_arrived;
    int             wake_event;                // eventfd; the program thread wakes the library thread
    int             repeat_timer;              // timerfd; keyboard arms it
    atomic_bool     running, stopping;
    atomic_uint     commit_count;              // bumped by each tick that changed something
    uint64_t        next_tick;                 // CLOCK_MONOTONIC nanoseconds
    uint32_t        input_serial;              // of the last input event; copy needs it

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
    Settings  settings;
    Placement placements[NODE_CAPACITY];       // drawn nodes, flattened
} Library;

static inline int64_t floor_divide(int64_t dividend, int64_t divisor) {
    int64_t quotient = dividend / divisor;
    return quotient - (dividend % divisor != 0 && (dividend < 0) != (divisor < 0));
}

// For key_transitions and mouse_transitions: moves only when the held state changes. Odd means held.
static inline void transition_set(uint8_t *transitions, bool held) {
    if ((*transitions % 2 == 1) != held)
        (*transitions)++;
}

// library.c
bool library_start(Library *library);
void library_stop(Library *library);
void library_wake(Library *library);
bool failed(const char *reason);

// tick.c
void tick_publish(Library *library);
void tick_wait(Library *library);
void tick_receive(Library *library);

// wayland.c
bool wayland_connect(Library *library);
void wayland_disconnect(Library *library);

// window.c
bool window_open(Library *library, const char *title);
void window_close(Library *library);
void window_request_frame(Library *library);

// scale.c
void scale_attach(Library *library);

// keyboard.c
void keyboard_attach(Library *library);
void keyboard_detach(Library *library);
void keyboard_repeat(Library *library);
void text_append(Library *library, uint32_t codepoint);

// pointer.c
void pointer_attach(Library *library);
void pointer_detach(Library *library);
void pointer_hit(Library *library);

// text_input.c
void text_input_attach(Library *library);

// clipboard.c
void clipboard_attach(Library *library);
void clipboard_serve(Library *library);

// settings.c
void settings_load(Settings *settings);

// font.c
bool font_load(Font *font, const Settings *settings, uint32_t scale);
void font_close(Font *font);
void font_draw(const Font *font, uint32_t codepoint, bool bold, uint8_t *coverage);

// flatten.c
void     flatten(const Node nodes[NODE_CAPACITY], Rectangle window, Placement placements[NODE_CAPACITY]);
uint8_t  node_at(const Placement placements[NODE_CAPACITY], int32_t column, int32_t row);
uint32_t node_cell_count(const Node *node);

// graphics.c
bool graphics_open(Library *library);
void graphics_close(Graphics *graphics);
bool graphics_resize(Graphics *graphics, uint32_t width, uint32_t height);
void graphics_wait(Graphics *graphics);
bool graphics_present(Graphics *graphics, VkDeviceAddress frame);
void graphics_import_pixels(Library *library);
bool buffer_create(Graphics *graphics, VkDeviceSize size, Buffer *buffer);
bool buffer_reserve(Graphics *graphics, Buffer *buffer, VkDeviceSize size);
void buffer_destroy(Graphics *graphics, Buffer *buffer);

// atlas.c
bool atlas_open(Library *library);
void atlas_close(Library *library);
bool atlas_fill_misses(Library *library);

// draw.c
bool draw_open(Library *library);
void draw_close(Library *library);
bool draw_due(Library *library);
void draw_frame(Library *library);
