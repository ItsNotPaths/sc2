#include <linux/input-event-codes.h>
#include <stdlib.h>

#include "library.h"

enum { ROWS_PER_NOTCH = 3, DEFAULT_CURSOR_SIZE = 24 };

// Runs after the mouse moves, enters or leaves, and after each new layout.
void pointer_hit(Library *library) {
    otk_screen *incoming = library->incoming;
    incoming->mouse_node = library->pointer.inside
        ? node_at(library->placements, incoming->mouse_column, incoming->mouse_row)
        : MOUSE_OUTSIDE;
}

static void pointer_move(Library *library, wl_fixed_t x, wl_fixed_t y) {
    otk_screen *incoming = library->incoming;
    incoming->mouse_column = fixed_to_eighths(x, library->font.cell_width);
    incoming->mouse_row = fixed_to_eighths(y, library->font.cell_height);
    pointer_hit(library);
}

// Without cursor-shape-v1: the default cursor from the user's xcursor theme.
static void cursor_theme_load(Library *library) {
    Pointer *pointer = &library->pointer;
    const char *size_text = getenv("XCURSOR_SIZE");
    int size = size_text ? atoi(size_text) : 0;
    pointer->cursor_theme = wl_cursor_theme_load(getenv("XCURSOR_THEME"), size > 0 ? size : DEFAULT_CURSOR_SIZE,
                                                 library->globals.shared_memory);
    if (!pointer->cursor_theme)
        return;
    struct wl_cursor *cursor = wl_cursor_theme_get_cursor(pointer->cursor_theme, "default");
    if (!cursor)
        cursor = wl_cursor_theme_get_cursor(pointer->cursor_theme, "left_ptr");
    if (!cursor)
        return;
    pointer->cursor_image = cursor->images[0];
    pointer->cursor_surface = wl_compositor_create_surface(library->globals.compositor);
    wl_surface_attach(pointer->cursor_surface, wl_cursor_image_get_buffer(pointer->cursor_image), 0, 0);
    wl_surface_damage_buffer(pointer->cursor_surface, 0, 0, INT32_MAX, INT32_MAX);
    wl_surface_commit(pointer->cursor_surface);
}

static void cursor_set(Library *library, uint32_t serial) {
    Pointer *pointer = &library->pointer;
    if (pointer->cursor_shape)
        wp_cursor_shape_device_v1_set_shape(pointer->cursor_shape, serial, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT);
    else if (pointer->cursor_surface)
        wl_pointer_set_cursor(pointer->pointer, serial, pointer->cursor_surface,
                              pointer->cursor_image->hotspot_x, pointer->cursor_image->hotspot_y);
}

// Keeps the fraction, so slow touchpad scrolls still add up.
static void wheel_add(Library *library, double eighths) {
    Pointer *pointer = &library->pointer;
    double total = pointer->wheel_remainder + eighths;
    int32_t whole = (int32_t)total;
    pointer->wheel_remainder = total - whole;
    library->incoming->wheel_total += whole;
}

static void pointer_enter(void *data, struct wl_pointer *, uint32_t serial, struct wl_surface *, wl_fixed_t x, wl_fixed_t y) {
    Library *library = data;
    cursor_set(library, serial);
    library->pointer.inside = true;
    pointer_move(library, x, y);
}

static void pointer_leave(void *data, struct wl_pointer *, uint32_t, struct wl_surface *) {
    Library *library = data;
    library->pointer.inside = false;
    pointer_hit(library);
}

static void pointer_motion(void *data, struct wl_pointer *, uint32_t, wl_fixed_t x, wl_fixed_t y) {
    pointer_move(data, x, y);
}

static void pointer_button(void *data, struct wl_pointer *, uint32_t serial, uint32_t, uint32_t button, uint32_t state) {
    Library *library = data;
    library->input_serial = serial;
    if (button >= BTN_LEFT && button <= BTN_TASK)
        transition_set(&library->incoming->mouse_transitions[button - BTN_LEFT], state == WL_POINTER_BUTTON_STATE_PRESSED);
}

// Wheels count through axis_value120; touchpads and other smooth sources in pixels here.
static void pointer_axis(void *data, struct wl_pointer *, uint32_t, uint32_t axis, wl_fixed_t value) {
    Library *library = data;
    uint32_t source = library->pointer.axis_source;
    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL
        && (source == WL_POINTER_AXIS_SOURCE_FINGER || source == WL_POINTER_AXIS_SOURCE_CONTINUOUS))
        wheel_add(library, wl_fixed_to_double(value) * 8 / library->font.cell_height);
}

static void pointer_axis_value120(void *data, struct wl_pointer *, uint32_t axis, int32_t value120) {
    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL)
        wheel_add(data, value120 * ROWS_PER_NOTCH * 8 / 120.0);
}

static void pointer_axis_source(void *data, struct wl_pointer *, uint32_t source) {
    ((Library *)data)->pointer.axis_source = source;
}

static void pointer_frame(void *data, struct wl_pointer *) {
    ((Library *)data)->pointer.axis_source = WL_POINTER_AXIS_SOURCE_WHEEL;
}

static void pointer_axis_stop(void *, struct wl_pointer *, uint32_t, uint32_t) {}
static void pointer_axis_discrete(void *, struct wl_pointer *, uint32_t, int32_t) {}

static const struct wl_pointer_listener pointer_listener = {
    .enter = pointer_enter,
    .leave = pointer_leave,
    .motion = pointer_motion,
    .button = pointer_button,
    .axis = pointer_axis,
    .frame = pointer_frame,
    .axis_source = pointer_axis_source,
    .axis_stop = pointer_axis_stop,
    .axis_discrete = pointer_axis_discrete,
    .axis_value120 = pointer_axis_value120,
};

void pointer_attach(Library *library) {
    Pointer *pointer = &library->pointer;
    pointer->pointer = wl_seat_get_pointer(library->globals.seat);
    wl_pointer_add_listener(pointer->pointer, &pointer_listener, library);
    if (library->globals.cursor_shape_manager)
        pointer->cursor_shape = wp_cursor_shape_manager_v1_get_pointer(library->globals.cursor_shape_manager, pointer->pointer);
    else if (library->globals.shared_memory)
        cursor_theme_load(library);
}

void pointer_detach(Library *library) {
    Pointer *pointer = &library->pointer;
    if (!pointer->pointer)
        return;
    if (pointer->cursor_shape)
        wp_cursor_shape_device_v1_destroy(pointer->cursor_shape);
    if (pointer->cursor_surface)
        wl_surface_destroy(pointer->cursor_surface);
    if (pointer->cursor_theme)
        wl_cursor_theme_destroy(pointer->cursor_theme);
    wl_pointer_release(pointer->pointer);
    *pointer = (Pointer){};
    pointer_hit(library);
}
