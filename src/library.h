// Library side: process-local state that must stay out of otk_screen.
#pragma once

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <vulkan/vulkan.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include "otk.h"
#include "cursor-shape-v1.h"
#include "fractional-scale-v1.h"
#include "text-input-unstable-v3.h"
#include "viewporter.h"
#include "xdg-shell.h"

typedef struct {
    struct wl_compositor                  *compositor;
    struct wl_seat                        *seat;
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
    uint32_t width, height;            // pixels
    uint32_t scale;                    // 120ths
    uint32_t cell_width, cell_height;  // pixels, 1:2
} Window;

typedef struct {
    struct wl_keyboard *keyboard;
    struct xkb_context *context;
    struct xkb_keymap  *keymap;
    struct xkb_state   *state;
    int32_t  repeat_rate, repeat_delay;
    uint32_t repeat_scancode;          // 0 when nothing repeats
} Keyboard;

typedef struct {
    struct wl_pointer                *pointer;
    struct wp_cursor_shape_device_v1 *cursor_shape;
} Pointer;

typedef struct {
    struct zwp_text_input_v3 *text_input;
} TextInput;

typedef struct {
    struct wl_data_device *data_device;
    struct wl_data_offer  *selection;  // the clipboard now
    struct wl_data_offer  *drop;       // dragged over the window now
    struct wl_data_source *source;     // set while the copy owns the clipboard
    uint32_t paste_request;            // last request served
    uint32_t copy_sequence;            // last copy offered
} Clipboard;

typedef struct {
    VkInstance       instance;
    VkPhysicalDevice physical_device;
    VkDevice         device;
    VkQueue          queue;
    VkSurfaceKHR     surface;
    VkSwapchainKHR   swapchain;
    VkPipeline       pipeline;
} Graphics;

typedef struct {
    VkImage  image;                    // grid of cell-sized slots
    uint32_t slot_columns, slot_rows;
    uint32_t slots_used;
} Atlas;

typedef struct {
    char     font_path[256];
    uint32_t font_size;                // cell height in pixels at scale 1
    uint32_t palette[16];              // RGBA, base16 order
} Settings;

typedef struct { int32_t left, top, right, bottom; } Rectangle;  // eighths of a cell, screen space

typedef struct {
    Rectangle area;
    Rectangle clip;
} Placement;

typedef struct {
    otk_screen *screen;                // shared with the program
    otk_screen *incoming;              // library writes input here; tick copies it into screen
    otk_screen *committed;             // tick copies nodes and cells here; draw reads them
    int         memory_file;           // memfd behind screen

    pthread_t       thread;
    pthread_mutex_t lock;              // guards incoming and committed
    pthread_cond_t  input_arrived;
    int             wake_event;        // eventfd; the program thread wakes the library thread
    int             repeat_timer;      // timerfd; keyboard arms it
    atomic_bool     running, stopping;
    uint32_t        commit_count;
    uint64_t        next_tick;         // CLOCK_MONOTONIC nanoseconds
    uint32_t        input_serial;      // of the last input event; copy needs it

    struct wl_display *display;
    Globals   globals;
    Window    window;
    Keyboard  keyboard;
    Pointer   pointer;
    TextInput text_input;
    Clipboard clipboard;
    Graphics  graphics;
    Atlas     atlas;
    Settings  settings;
    Placement placements[255];         // committed nodes, flattened
} Library;

// library.c
bool library_start(Library *library);
void library_stop(Library *library);
void library_wake(Library *library);

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

// scale.c
void scale_attach(Library *library);

// keyboard.c
void keyboard_attach(Library *library);
void keyboard_repeat(Library *library);
void text_append(Library *library, uint32_t codepoint);

// pointer.c
void pointer_attach(Library *library);

// text_input.c
void text_input_attach(Library *library);

// clipboard.c
void clipboard_attach(Library *library);
void clipboard_serve(Library *library);

// settings.c
void settings_load(Settings *settings);

// font.c
bool font_load(const Settings *settings);
bool font_draw(uint32_t codepoint, uint32_t cell_width, uint32_t cell_height, uint8_t *coverage);

// flatten.c
void flatten(const Node nodes[255], Placement placements[255]);

// graphics.c
bool graphics_open(Library *library);
void graphics_close(Library *library);
void graphics_import_pixels(Library *library);

// atlas.c
bool atlas_open(Library *library);
void atlas_fill_misses(Library *library);

// draw.c
void draw_frame(Library *library);
