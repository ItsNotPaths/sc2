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

static void frame_done(void *data, struct wl_callback *callback, uint32_t) {
    Host *host = data;
    wl_callback_destroy(callback);
    host->window.frame_pending = false;
}

static const struct wl_callback_listener frame_listener = {
    .done = frame_done,
};

// Waits for the first configure: the window has no size before it, and may not present.
bool window_open(Host *host, const char *title) {
    Globals *globals = &host->globals;
    Window *window = &host->window;
    window->surface_width = host->settings.window_columns * host->font.cell_width;
    window->surface_height = host->settings.window_rows * host->font.cell_height;

    xdg_wm_base_add_listener(globals->window_manager, &window_manager_listener, host);
    window->surface = wl_compositor_create_surface(globals->compositor);
    window->shell_surface = xdg_wm_base_get_xdg_surface(globals->window_manager, window->surface);
    xdg_surface_add_listener(window->shell_surface, &shell_surface_listener, host);
    window->toplevel = xdg_surface_get_toplevel(window->shell_surface);
    xdg_toplevel_add_listener(window->toplevel, &toplevel_listener, host);
    xdg_toplevel_set_title(window->toplevel, title ? title : "");
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
}

// The compositor answers when it wants the next frame. The next present carries the request.
void window_request_frame(Host *host) {
    struct wl_callback *callback = wl_surface_frame(host->window.surface);
    wl_callback_add_listener(callback, &frame_listener, host);
    host->window.frame_pending = true;
}
