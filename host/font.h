// The cell font: glyph coverage in 1:2 cells. Apart from the host, so another program can draw
// the same glyphs (fig's oranger draws its tab row with it).
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <ft2build.h>
#include FT_FREETYPE_H

// An atlas key: a codepoint and how it is drawn; 0 marks an empty entry. draw.comp builds the same keys.
enum {
    GLYPH_CODEPOINT_MASK = (1u << 21) - 1,
    GLYPH_BOLD           = 1u << 21,
    GLYPH_ITALIC         = 1u << 22,
    GLYPH_LEFT_HALF      = 1u << 23,  // of a wide glyph
    GLYPH_RIGHT_HALF     = 1u << 24,
};

typedef struct {
    FT_Library  freetype;
    FT_Face     face;
    char        path[256];                     // what it was loaded from, to tell when settings change
    uint32_t    size, scale;
    uint32_t    cell_width, cell_height;       // pixels, 1:2
    int32_t     baseline;                      // pixels from the cell top
    uint32_t    underline_top, underline_thickness;
    const char *problem;                       // why the last font_load failed
} Font;

// size is the cell height in pixels at scale 1; scale is in 120ths.
bool font_load(Font *font, const char *path, uint32_t size, uint32_t scale);
bool font_matches(const Font *font, const char *path, uint32_t size, uint32_t scale);
void font_close(Font *font);
// Fills coverage for one glyph key: cell_width by cell_height bytes, zeroed by the caller.
void font_draw(const Font *font, uint32_t key, uint8_t *coverage);
