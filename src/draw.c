#include "library.h"

// At vsync: redraw only if commit_count or an image's sequence changed.
// (hole draw-frame :tags draw :sev blocker :needs (graphics-device draw-shader node-flatten glyph-atlas tick-exchange window-toplevel)) never dispatches or presents; nothing reaches the window.
void draw_frame(Library *library) {
    flatten(library->committed->nodes, library->placements);
    atlas_fill_misses(library);
    graphics_import_pixels(library);
}
