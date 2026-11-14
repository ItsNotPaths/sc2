#include "library.h"

// (hole window-toplevel :tags window :sev blocker) makes no surface or xdg toplevel; no configure, no frame callbacks, so columns and rows stay 0 and closed is never set.
bool window_open(Library *library, const char *title) {
    (void)title;
    library->window.scale = 120;
    library->window.cell_height = library->settings.font_size;
    library->window.cell_width = library->settings.font_size / 2;
    scale_attach(library);
    return true;
}

void window_close(Library *library) {
    (void)library;
}
