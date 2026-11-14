#include "library.h"

// (hole scale-redesign :tags window :sev gap) stopgap: the cell is font_size times the scale, so a scale change reloads the font like a font_size change; to be redesigned with the glyph grid size.

static void preferred_scale(void *data, struct wp_fractional_scale_v1 *, uint32_t scale) {
    ((Library *)data)->window.scale = scale;
}

static const struct wp_fractional_scale_v1_listener fractional_scale_listener = {
    .preferred_scale = preferred_scale,
};

// Without fractional-scale-v1 or a viewporter the window stays at scale 1.
void scale_attach(Library *library) {
    Globals *globals = &library->globals;
    Window *window = &library->window;
    if (!globals->viewporter || !globals->fractional_scale_manager)
        return;
    window->viewport = wp_viewporter_get_viewport(globals->viewporter, window->surface);
    window->fractional_scale = wp_fractional_scale_manager_v1_get_fractional_scale(globals->fractional_scale_manager,
                                                                                  window->surface);
    wp_fractional_scale_v1_add_listener(window->fractional_scale, &fractional_scale_listener, library);
}

void scale_detach(Library *library) {
    Window *window = &library->window;
    if (window->fractional_scale)
        wp_fractional_scale_v1_destroy(window->fractional_scale);
    if (window->viewport)
        wp_viewport_destroy(window->viewport);
}
