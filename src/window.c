#include "library.h"

enum { DEFAULT_COLUMNS = 80, DEFAULT_ROWS = 24 };

static void window_manager_ping(void *, struct xdg_wm_base *window_manager, uint32_t serial) {
    xdg_wm_base_pong(window_manager, serial);
}

static const struct xdg_wm_base_listener window_manager_listener = {
    .ping = window_manager_ping,
};

// Ends a configure sequence: the size from the toplevel configure before it takes effect.
static void shell_surface_configure(void *data, struct xdg_surface *shell_surface, uint32_t serial) {
    Library *library = data;
    Window *window = &library->window;
    xdg_surface_ack_configure(shell_surface, serial);
    if (window->requested_width && window->requested_height) {
        window->width = window->requested_width;
        window->height = window->requested_height;
    }
    window->configured = true;
    library->incoming->columns = window->width / library->font.cell_width;
    library->incoming->rows = window->height / library->font.cell_height;
}

static const struct xdg_surface_listener shell_surface_listener = {
    .configure = shell_surface_configure,
};

static void toplevel_configure(void *data, struct xdg_toplevel *, int32_t width, int32_t height, struct wl_array *) {
    Library *library = data;
    library->window.requested_width = width;
    library->window.requested_height = height;
}

static void toplevel_close(void *data, struct xdg_toplevel *) {
    Library *library = data;
    library->incoming->closed = 1;
}

static void toplevel_configure_bounds(void *, struct xdg_toplevel *, int32_t, int32_t) {}
static void toplevel_wm_capabilities(void *, struct xdg_toplevel *, struct wl_array *) {}

static const struct xdg_toplevel_listener toplevel_listener = {
    .configure = toplevel_configure,
    .close = toplevel_close,
    .configure_bounds = toplevel_configure_bounds,
    .wm_capabilities = toplevel_wm_capabilities,
};

static void frame_done(void *data, struct wl_callback *callback, uint32_t) {
    Library *library = data;
    wl_callback_destroy(callback);
    library->window.frame_pending = false;
}

static const struct wl_callback_listener frame_listener = {
    .done = frame_done,
};

// Waits for the first configure: the window has no size before it, and may not present.
bool window_open(Library *library, const char *title) {
    Globals *globals = &library->globals;
    Window *window = &library->window;
    window->width = DEFAULT_COLUMNS * library->font.cell_width;
    window->height = DEFAULT_ROWS * library->font.cell_height;

    xdg_wm_base_add_listener(globals->window_manager, &window_manager_listener, library);
    window->surface = wl_compositor_create_surface(globals->compositor);
    window->shell_surface = xdg_wm_base_get_xdg_surface(globals->window_manager, window->surface);
    xdg_surface_add_listener(window->shell_surface, &shell_surface_listener, library);
    window->toplevel = xdg_surface_get_toplevel(window->shell_surface);
    xdg_toplevel_add_listener(window->toplevel, &toplevel_listener, library);
    xdg_toplevel_set_title(window->toplevel, title ? title : "");
    scale_attach(library);
    wl_surface_commit(window->surface);

    while (!window->configured)
        if (wl_display_dispatch(library->display) < 0)
            return failed("the compositor never configured the window");
    return true;
}

void window_close(Library *library) {
    Window *window = &library->window;
    if (window->toplevel)
        xdg_toplevel_destroy(window->toplevel);
    if (window->shell_surface)
        xdg_surface_destroy(window->shell_surface);
    if (window->surface)
        wl_surface_destroy(window->surface);
}

// The compositor answers when it wants the next frame. The next present carries the request.
void window_request_frame(Library *library) {
    struct wl_callback *callback = wl_surface_frame(library->window.surface);
    wl_callback_add_listener(callback, &frame_listener, library);
    library->window.frame_pending = true;
}
