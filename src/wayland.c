#include <stddef.h>
#include <string.h>

#include "library.h"

typedef struct {
    const struct wl_interface *interface;
    uint32_t version;
    size_t   offset;  // into Globals
} WantedGlobal;

static const WantedGlobal wanted_globals[] = {
    {&wl_compositor_interface,                  6, offsetof(Globals, compositor)},
    {&wl_seat_interface,                        8, offsetof(Globals, seat)},
    {&wl_data_device_manager_interface,         3, offsetof(Globals, data_device_manager)},
    {&xdg_wm_base_interface,                    5, offsetof(Globals, window_manager)},
    {&wp_viewporter_interface,                  1, offsetof(Globals, viewporter)},
    {&wp_fractional_scale_manager_v1_interface, 1, offsetof(Globals, fractional_scale_manager)},
    {&wp_cursor_shape_manager_v1_interface,     1, offsetof(Globals, cursor_shape_manager)},
    {&zwp_text_input_manager_v3_interface,      1, offsetof(Globals, text_input_manager)},
};

static void registry_global(void *data, struct wl_registry *registry, uint32_t name,
                            const char *interface, uint32_t version) {
    Library *library = data;
    for (size_t index = 0; index < sizeof wanted_globals / sizeof *wanted_globals; index++) {
        const WantedGlobal *wanted = &wanted_globals[index];
        if (strcmp(interface, wanted->interface->name) != 0)
            continue;
        uint32_t bound_version = version < wanted->version ? version : wanted->version;
        void **slot = (void **)((char *)&library->globals + wanted->offset);
        *slot = wl_registry_bind(registry, name, wanted->interface, bound_version);
    }
}

static void registry_global_remove(void *, struct wl_registry *, uint32_t) {}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

static void seat_capabilities(void *data, struct wl_seat *, uint32_t capabilities) {
    Library *library = data;
    // (hole seat-loss :tags input :sev polish) keyboard and pointer are never released when the seat loses them.
    if (capabilities & WL_SEAT_CAPABILITY_KEYBOARD && !library->keyboard.keyboard)
        keyboard_attach(library);
    if (capabilities & WL_SEAT_CAPABILITY_POINTER && !library->pointer.pointer)
        pointer_attach(library);
}

static void seat_name(void *, struct wl_seat *, const char *) {}

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_capabilities,
    .name = seat_name,
};

bool wayland_connect(Library *library) {
    library->display = wl_display_connect(nullptr);
    if (!library->display)
        return false;
    struct wl_registry *registry = wl_display_get_registry(library->display);
    wl_registry_add_listener(registry, &registry_listener, library);
    wl_display_roundtrip(library->display);
    if (!library->globals.compositor || !library->globals.window_manager)
        return false;
    if (library->globals.seat) {
        wl_seat_add_listener(library->globals.seat, &seat_listener, library);
        clipboard_attach(library);
        text_input_attach(library);
    }
    wl_display_roundtrip(library->display);
    return true;
}

void wayland_disconnect(Library *library) {
    if (library->display)
        wl_display_disconnect(library->display);
}
