#include <stdio.h>
#include <string.h>

#include "host.h"

static void window_manager_ping(void *, struct xdg_wm_base *window_manager, uint32_t serial) {
    xdg_wm_base_pong(window_manager, serial);
}

static const struct xdg_wm_base_listener window_manager_listener = {
    .ping = window_manager_ping,
};

// Ends a configure sequence: the size from the toplevel configure before it takes effect.
static void shell_surface_configure(void *data, struct xdg_surface *shell_surface, uint32_t serial) {
    Host *host = data;
    Window *window = &host->window;
    xdg_surface_ack_configure(shell_surface, serial);
    if (window->requested_width && window->requested_height) {
        window->surface_width = window->requested_width;
        window->surface_height = window->requested_height;
    }
    window->configured = true;
    window_resize(host);
}

static const struct xdg_surface_listener shell_surface_listener = {
    .configure = shell_surface_configure,
};

static void toplevel_configure(void *data, struct xdg_toplevel *, int32_t width, int32_t height, struct wl_array *) {
    Host *host = data;
    host->window.requested_width = width;
    host->window.requested_height = height;
}

static void toplevel_close(void *data, struct xdg_toplevel *) {
    Host *host = data;
    host->incoming->closed = 1;
}

static void toplevel_configure_bounds(void *, struct xdg_toplevel *, int32_t, int32_t) {}
static void toplevel_wm_capabilities(void *, struct xdg_toplevel *, struct wl_array *) {}

static const struct xdg_toplevel_listener toplevel_listener = {
    .configure = toplevel_configure,
    .close = toplevel_close,
    .configure_bounds = toplevel_configure_bounds,
    .wm_capabilities = toplevel_wm_capabilities,
};

static void layer_surface_configure(void *data, struct zwlr_layer_surface_v1 *layer_surface, uint32_t serial,
                                    uint32_t width, uint32_t height) {
    Host *host = data;
    Window *window = &host->window;
    zwlr_layer_surface_v1_ack_configure(layer_surface, serial);
    if (width && height) {
        window->surface_width = width;
        window->surface_height = height;
    }
    window->configured = true;
    window_resize(host);
}

static void layer_surface_closed(void *data, struct zwlr_layer_surface_v1 *) {
    Host *host = data;
    host->incoming->closed = 1;
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
    .configure = layer_surface_configure,
    .closed = layer_surface_closed,
};

enum { SURFACE_CELL_LIMIT = 1000 };  // a panel's or popup's cells across either side

#define ANCHOR_TOP    ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP
#define ANCHOR_BOTTOM ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM
#define ANCHOR_LEFT   ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT
#define ANCHOR_RIGHT  ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT

static const uint32_t position_anchors[] = {
    [SC2_POSITION_CENTER] = 0,
    [SC2_POSITION_TOP] = ANCHOR_TOP,
    [SC2_POSITION_BOTTOM] = ANCHOR_BOTTOM,
    [SC2_POSITION_LEFT] = ANCHOR_LEFT,
    [SC2_POSITION_RIGHT] = ANCHOR_RIGHT,
    [SC2_POSITION_TOP_LEFT] = ANCHOR_TOP | ANCHOR_LEFT,
    [SC2_POSITION_TOP_RIGHT] = ANCHOR_TOP | ANCHOR_RIGHT,
    [SC2_POSITION_BOTTOM_LEFT] = ANCHOR_BOTTOM | ANCHOR_LEFT,
    [SC2_POSITION_BOTTOM_RIGHT] = ANCHOR_BOTTOM | ANCHOR_RIGHT,
};

static bool cells_valid(uint16_t cells) {
    return cells > 0 && cells <= SURFACE_CELL_LIMIT;
}

static bool upright(uint8_t position) {
    return position == SC2_POSITION_LEFT || position == SC2_POSITION_RIGHT;
}

// The kind the host can make of what the commit asks: a window in place of a panel or popup
// that asks the impossible, or that the compositor cannot show.
static uint8_t kind_choose(const Host *host, const Commit *commit) {
    uint8_t position = commit->surface_position;
    bool edge = position >= SC2_POSITION_TOP && position <= SC2_POSITION_RIGHT;
    bool panel = commit->surface_kind == SC2_SURFACE_PANEL && edge
              && cells_valid(upright(position) ? commit->surface_columns : commit->surface_rows);
    bool popup = commit->surface_kind == SC2_SURFACE_POPUP && position <= SC2_POSITION_BOTTOM_RIGHT
              && cells_valid(commit->surface_columns) && cells_valid(commit->surface_rows);
    if (!host->globals.layer_shell || !(panel || popup))
        return SC2_SURFACE_WINDOW;
    return commit->surface_kind;
}

// Surface units for pixels at the drawn scale, rounded up so the cells fit.
static uint32_t surface_units(const Host *host, uint32_t pixels) {
    return (pixels * 120 + host->font.scale - 1) / host->font.scale;
}

static void toplevel_open(Host *host) {
    Window *window = &host->window;
    window->shell_surface = xdg_wm_base_get_xdg_surface(host->globals.window_manager, window->surface);
    xdg_surface_add_listener(window->shell_surface, &shell_surface_listener, host);
    window->toplevel = xdg_surface_get_toplevel(window->shell_surface);
    xdg_toplevel_add_listener(window->toplevel, &toplevel_listener, host);
}

// A panel runs the output's full length on its edge, keeps its thickness clear of windows and
// takes no keyboard. A popup sits over everything and takes the keyboard when the compositor gives it.
// The namespace is the program's app id, which rules match.
static void layer_open(Host *host, const char *app_id) {
    Window *window = &host->window;
    bool panel = window->kind == SC2_SURFACE_PANEL;
    uint32_t anchors = position_anchors[window->position];
    if (panel)
        anchors |= upright(window->position) ? ANCHOR_TOP | ANCHOR_BOTTOM : ANCHOR_LEFT | ANCHOR_RIGHT;
    window->layer_surface = zwlr_layer_shell_v1_get_layer_surface(
        host->globals.layer_shell, window->surface, nullptr,
        panel ? ZWLR_LAYER_SHELL_V1_LAYER_TOP : ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
        app_id[0] ? app_id : window->file_name);
    zwlr_layer_surface_v1_add_listener(window->layer_surface, &layer_surface_listener, host);
    zwlr_layer_surface_v1_set_anchor(window->layer_surface, anchors);
    zwlr_layer_surface_v1_set_keyboard_interactivity(window->layer_surface,
        panel ? ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE
              : ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND);
}

// Sizes a panel or popup to its cells in the current font, and commits it. A window keeps its size.
void window_fit(Host *host) {
    Window *window = &host->window;
    if (!window->layer_surface)
        return;
    uint32_t width = surface_units(host, window->columns * host->font.cell_width);
    uint32_t height = surface_units(host, window->rows * host->font.cell_height);
    if (window->kind == SC2_SURFACE_PANEL) {
        bool across = upright(window->position);
        zwlr_layer_surface_v1_set_size(window->layer_surface, across ? width : 0, across ? 0 : height);
        zwlr_layer_surface_v1_set_exclusive_zone(window->layer_surface, (int32_t)(across ? width : height));
    } else
        zwlr_layer_surface_v1_set_size(window->layer_surface, width, height);
    wl_surface_commit(window->surface);
}

// Gives the surface its role, as the program's first commit asks, in the font it asked for.
void window_place(Host *host, const Commit *commit) {
    Window *window = &host->window;
    if (!draw_font_apply(host))
        return;
    window->kind = kind_choose(host, commit);
    window->position = commit->surface_position;
    window->close_on_leave = window->kind == SC2_SURFACE_POPUP && commit->surface_close_on_leave == 1;
    window->columns = commit->surface_columns;
    window->rows = commit->surface_rows;
    window->named = true;
    if (window->kind != SC2_SURFACE_WINDOW) {
        layer_open(host, commit->app_id);
        window_fit(host);
        return;
    }
    window->surface_width = surface_units(host, host->settings.window_columns * host->font.cell_width);
    window->surface_height = surface_units(host, host->settings.window_rows * host->font.cell_height);
    toplevel_open(host);
    window_name(host, commit->title, commit->app_id);
    wl_surface_commit(window->surface);
}

static void frame_done(void *data, struct wl_callback *callback, uint32_t) {
    Host *host = data;
    wl_callback_destroy(callback);
    host->window.frame_pending = false;
}

static const struct wl_callback_listener frame_listener = {
    .done = frame_done,
};

// The surface has no role until the program's first commit; the program starts at a guessed size.
bool window_open(Host *host, const char *file_name) {
    Window *window = &host->window;
    window->surface_width = surface_units(host, host->settings.window_columns * host->font.cell_width);
    window->surface_height = surface_units(host, host->settings.window_rows * host->font.cell_height);
    xdg_wm_base_add_listener(host->globals.window_manager, &window_manager_listener, host);
    window->surface = wl_compositor_create_surface(host->globals.compositor);
    window->file_name = file_name;
    scale_attach(host);
    window_resize(host);
    return true;
}

void window_close(Host *host) {
    Window *window = &host->window;
    scale_detach(host);
    if (window->layer_surface)
        zwlr_layer_surface_v1_destroy(window->layer_surface);
    if (window->toplevel)
        xdg_toplevel_destroy(window->toplevel);
    if (window->shell_surface)
        xdg_surface_destroy(window->shell_surface);
    if (window->surface)
        wl_surface_destroy(window->surface);
}

// Pixels and grid from the surface size and the scale; the viewport shows the pixels at surface size.
// Writes incoming, so it runs inside an input section.
void window_resize(Host *host) {
    Window *window = &host->window;
    uint32_t scale = host->font.scale;
    window->width = (window->surface_width * scale + 60) / 120;
    window->height = (window->surface_height * scale + 60) / 120;
    if (window->viewport)
        wp_viewport_set_destination(window->viewport, window->surface_width, window->surface_height);
    host->incoming->columns = window->width / host->font.cell_width;
    host->incoming->rows = window->height / host->font.cell_height;
    host->incoming->cell_width = host->font.cell_width;
    host->incoming->cell_height = host->font.cell_height;
}

// Bytes in the valid UTF-8 sequence at text (RFC 3629: no overlong forms, no surrogates,
// nothing past U+10FFFF), or 0. A 0 byte fails every check, so it never reads past the end.
static size_t utf8_length(const uint8_t *text) {
    uint8_t lead = text[0], low = 0x80, high = 0xBF;
    size_t length;
    if (lead < 0x80)
        return 1;
    if (lead >= 0xC2 && lead <= 0xDF)
        length = 2;
    else if (lead >= 0xE0 && lead <= 0xEF)
        length = 3, low = lead == 0xE0 ? 0xA0 : 0x80, high = lead == 0xED ? 0x9F : 0xBF;
    else if (lead >= 0xF0 && lead <= 0xF4)
        length = 4, low = lead == 0xF0 ? 0x90 : 0x80, high = lead == 0xF4 ? 0x8F : 0xBF;
    else
        return 0;
    if (text[1] < low || text[1] > high)
        return 0;
    for (size_t index = 2; index < length; index++)
        if (text[index] < 0x80 || text[index] > 0xBF)
            return 0;
    return length;
}

// Ends text within size, and replaces each byte that does not start valid UTF-8 with '?',
// since the compositor drops a client that sends it bad UTF-8.
static void utf8_mend(char *text, size_t size) {
    text[size - 1] = 0;
    for (size_t index = 0; text[index];) {
        size_t length = utf8_length((const uint8_t *)&text[index]);
        if (length)
            index += length;
        else
            text[index++] = '?';
    }
}

// Names the window as the program asks. An empty title shows the program's file name.
// A layer surface has no name.
void window_name(Host *host, const char *title, const char *app_id) {
    Window *window = &host->window;
    if (!window->toplevel)
        return;
    char shown_title[sizeof window->title], shown_app_id[sizeof window->app_id];
    snprintf(shown_title, sizeof shown_title, "%s", title[0] ? title : window->file_name);
    snprintf(shown_app_id, sizeof shown_app_id, "%s", app_id);
    utf8_mend(shown_title, sizeof shown_title);
    utf8_mend(shown_app_id, sizeof shown_app_id);
    if (strcmp(shown_title, window->title) != 0) {
        strcpy(window->title, shown_title);
        xdg_toplevel_set_title(window->toplevel, window->title);
    }
    if (strcmp(shown_app_id, window->app_id) != 0) {
        strcpy(window->app_id, shown_app_id);
        xdg_toplevel_set_app_id(window->toplevel, window->app_id);
    }
}

// The compositor answers when it wants the next frame. The next present carries the request.
void window_request_frame(Host *host) {
    struct wl_callback *callback = wl_surface_frame(host->window.surface);
    wl_callback_add_listener(callback, &frame_listener, host);
    host->window.frame_pending = true;
}
