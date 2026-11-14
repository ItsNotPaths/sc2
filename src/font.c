#include "library.h"

// (hole font-load :tags text :sev blocker) opens no font; FreeType or stb_truetype is not chosen yet.
bool font_load(const Settings *settings) {
    (void)settings;
    return true;
}

// Coverage is cell_width by cell_height, one byte per pixel; the glyph is padded to fill the 1:2 cell.
bool font_draw(uint32_t codepoint, uint32_t cell_width, uint32_t cell_height, uint8_t *coverage) {
    // (hole font-load) draws nothing; every glyph is blank.
    (void)codepoint, (void)cell_width, (void)cell_height, (void)coverage;
    return false;
}
