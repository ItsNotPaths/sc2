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

// A panel along one edge, the output's full length, its thickness kept clear of windows. Made at
// the program's first commit, so its namespace is the program's app id, which rules match.
void layer_open(Host *host, const char *app_id) {
    Window *window = &host->window;
    bool upright = window->layer_edge & (ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    uint32_t thickness = window->layer_cells * (upright ? host->font.cell_width : host->font.cell_height);
    uint32_t ends = upright ? ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM
                            : ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
    window->layer_surface = zwlr_layer_shell_v1_get_layer_surface(host->globals.layer_shell, window->surface, nullptr,
                                                                  ZWLR_LAYER_SHELL_V1_LAYER_TOP,
                                                                  app_id[0] ? app_id : window->file_name);
    zwlr_layer_surface_v1_add_listener(window->layer_surface, &layer_surface_listener, host);
    zwlr_layer_surface_v1_set_anchor(window->layer_surface, window->layer_edge | ends);
    zwlr_layer_surface_v1_set_size(window->layer_surface, upright ? thickness : 0, upright ? 0 : thickness);
    zwlr_layer_surface_v1_set_exclusive_zone(window->layer_surface, (int32_t)thickness);
    zwlr_layer_surface_v1_set_keyboard_interactivity(window->layer_surface,
                                                     ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND);
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

// Waits for the first configure: the window has no size before it, and may not present. A layer
// surface waits for the program's first commit instead, and starts the program at a guessed size.
bool window_open(Host *host, const char *file_name) {
    Globals *globals = &host->globals;
    Window *window = &host->window;
    window->surface_width = host->settings.window_columns * host->font.cell_width;
    window->surface_height = host->settings.window_rows * host->font.cell_height;

    xdg_wm_base_add_listener(globals->window_manager, &window_manager_listener, host);
    window->surface = wl_compositor_create_surface(globals->compositor);
    window->file_name = file_name;
    if (window->layer_edge) {
        if (!globals->layer_shell)
            return failed("the compositor lacks wlr layer-shell");
        scale_attach(host);
        window_resize(host);
        return true;
    }
    window->shell_surface = xdg_wm_base_get_xdg_surface(globals->window_manager, window->surface);
    xdg_surface_add_listener(window->shell_surface, &shell_surface_listener, host);
    window->toplevel = xdg_surface_get_toplevel(window->shell_surface);
    xdg_toplevel_add_listener(window->toplevel, &toplevel_listener, host);
    window_name(host, "", "");
    scale_attach(host);
    wl_surface_commit(window->surface);

    while (!window->configured)
        if (wl_display_dispatch(host->display) < 0)
            return failed("the compositor never configured the window");
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
