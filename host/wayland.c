#include <stddef.h>
#include <string.h>

#include "host.h"

typedef struct {
    const struct wl_interface *interface;
    uint32_t version;
    size_t   offset;  // into Globals
} WantedGlobal;

static const WantedGlobal wanted_globals[] = {
    {&wl_compositor_interface,                  6, offsetof(Globals, compositor)},
    {&wl_seat_interface,                        8, offsetof(Globals, seat)},
    {&wl_shm_interface,                         1, offsetof(Globals, shared_memory)},
    {&wl_data_device_manager_interface,         3, offsetof(Globals, data_device_manager)},
    {&xdg_wm_base_interface,                    5, offsetof(Globals, window_manager)},
    {&wp_viewporter_interface,                  1, offsetof(Globals, viewporter)},
    {&wp_fractional_scale_manager_v1_interface, 1, offsetof(Globals, fractional_scale_manager)},
    {&wp_cursor_shape_manager_v1_interface,     1, offsetof(Globals, cursor_shape_manager)},
    {&zwp_text_input_manager_v3_interface,      1, offsetof(Globals, text_input_manager)},
    {&zwlr_layer_shell_v1_interface,            4, offsetof(Globals, layer_shell)},
};

static void registry_global(void *data, struct wl_registry *registry, uint32_t name,
                            const char *interface, uint32_t version) {
    Host *host = data;
    for (size_t index = 0; index < sizeof wanted_globals / sizeof *wanted_globals; index++) {
        const WantedGlobal *wanted = &wanted_globals[index];
        if (strcmp(interface, wanted->interface->name) != 0)
            continue;
        uint32_t bound_version = version < wanted->version ? version : wanted->version;
        void **slot = (void **)((char *)&host->globals + wanted->offset);
        *slot = wl_registry_bind(registry, name, wanted->interface, bound_version);
    }
}

static void registry_global_remove(void *, struct wl_registry *, uint32_t) {}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

static void seat_capabilities(void *data, struct wl_seat *, uint32_t capabilities) {
    Host *host = data;
    bool has_keyboard = capabilities & WL_SEAT_CAPABILITY_KEYBOARD;
    bool has_pointer = capabilities & WL_SEAT_CAPABILITY_POINTER;
    if (has_keyboard && !host->keyboard.keyboard)
        keyboard_attach(host);
    if (!has_keyboard && host->keyboard.keyboard)
        keyboard_detach(host);
    if (has_pointer && !host->pointer.pointer)
        pointer_attach(host);
    if (!has_pointer && host->pointer.pointer)
        pointer_detach(host);
}

static void seat_name(void *, struct wl_seat *, const char *) {}

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_capabilities,
    .name = seat_name,
};

bool wayland_connect(Host *host) {
    host->display = wl_display_connect(nullptr);
    if (!host->display)
        return failed("cannot connect to the Wayland compositor");
    struct wl_registry *registry = wl_display_get_registry(host->display);
    wl_registry_add_listener(registry, &registry_listener, host);
    wl_display_roundtrip(host->display);
    if (!host->globals.compositor || !host->globals.window_manager)
        return failed("the compositor lacks wl_compositor or xdg_wm_base");
    if (host->globals.seat) {
        wl_seat_add_listener(host->globals.seat, &seat_listener, host);
        clipboard_attach(host);
        text_input_attach(host);
    }
    wl_display_roundtrip(host->display);
    return true;
}

void wayland_disconnect(Host *host) {
    keyboard_detach(host);
    pointer_detach(host);
    clipboard_detach(host);
    if (host->display)
        wl_display_disconnect(host->display);
}
