#include <linux/input-event-codes.h>

#include "library.h"

enum { ROWS_PER_NOTCH = 3 };

// Runs after the mouse moves, enters or leaves, and after each new layout.
void pointer_hit(Library *library) {
    otk_screen *incoming = library->incoming;
    incoming->mouse_node = library->pointer.inside
        ? node_at(library->placements, incoming->mouse_column, incoming->mouse_row)
        : MOUSE_OUTSIDE;
}

// wl_fixed is 24.8 fixed point, in pixels.
static void pointer_move(Library *library, wl_fixed_t x, wl_fixed_t y) {
    otk_screen *incoming = library->incoming;
    incoming->mouse_column = floor_divide((int64_t)x * 8, (int64_t)library->font.cell_width * 256);
    incoming->mouse_row = floor_divide((int64_t)y * 8, (int64_t)library->font.cell_height * 256);
    pointer_hit(library);
}

// Keeps the fraction, so slow touchpad scrolls still add up.
static void wheel_add(Library *library, double eighths) {
    Pointer *pointer = &library->pointer;
    double total = pointer->wheel_remainder + eighths;
    int32_t whole = (int32_t)total;
    pointer->wheel_remainder = total - whole;
    library->incoming->wheel_total += whole;
}

// (hole cursor-fallback :tags input :sev polish) without cursor-shape-v1 no cursor is set; some compositors then show none.
static void pointer_enter(void *data, struct wl_pointer *, uint32_t serial, struct wl_surface *, wl_fixed_t x, wl_fixed_t y) {
    Library *library = data;
    if (library->pointer.cursor_shape)
        wp_cursor_shape_device_v1_set_shape(library->pointer.cursor_shape, serial, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT);
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
}

void pointer_detach(Library *library) {
    Pointer *pointer = &library->pointer;
    if (!pointer->pointer)
        return;
    if (pointer->cursor_shape)
        wp_cursor_shape_device_v1_destroy(pointer->cursor_shape);
    wl_pointer_release(pointer->pointer);
    *pointer = (Pointer){};
    pointer_hit(library);
}
