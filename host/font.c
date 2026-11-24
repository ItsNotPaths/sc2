#include <stdio.h>
#include <string.h>

#include "font.h"

#include FT_SYNTHESIS_H

static int32_t round_pixels(FT_Pos value) {  // 26.6 fixed point
    return (int32_t)((value + 32) >> 6);
}

// The cell is fixed at 1:2. Picks the largest size whose line height and advance both fit,
// then pads: the line is centered in the cell height, each glyph in the cell width.
static bool font_failed(Font *font, const char *problem) {
    font->problem = problem;
    return false;
}

bool font_load(Font *font, const char *path, uint32_t size, uint32_t scale) {
    snprintf(font->path, sizeof font->path, "%s", path);
    font->size = size;
    font->scale = scale;
    if (!size)
        return font_failed(font, "the font size is 0");
    if (FT_Init_FreeType(&font->freetype) || FT_New_Face(font->freetype, path, 0, &font->face))
        return font_failed(font, "cannot open the font file");
    FT_Face face = font->face;
    if (!FT_IS_SCALABLE(face) || FT_Load_Char(face, 'M', FT_LOAD_NO_SCALE))
        return font_failed(font, "the font must be scalable and have an M");
    font->cell_height = size * scale / 120;
    font->cell_width = font->cell_height / 2;

    FT_Long line_height = face->ascender - face->descender;  // font units
    FT_Long advance = face->glyph->advance.x;
    FT_Long size_by_height = font->cell_height * face->units_per_EM / line_height;
    FT_Long size_by_width = font->cell_width * face->units_per_EM / advance;
    if (FT_Set_Pixel_Sizes(face, 0, size_by_height < size_by_width ? size_by_height : size_by_width))
        return font_failed(font, "cannot size the font");

    const FT_Size_Metrics *metrics = &face->size->metrics;
    int32_t ascender = round_pixels(metrics->ascender);
    font->baseline = ((int32_t)font->cell_height - (ascender - round_pixels(metrics->descender))) / 2 + ascender;
    int32_t thickness = round_pixels(FT_MulFix(face->underline_thickness, metrics->y_scale));
    font->underline_thickness = thickness > 0 ? thickness : 1;
    int32_t underline_top = font->baseline - round_pixels(FT_MulFix(face->underline_position, metrics->y_scale))
                          - (int32_t)font->underline_thickness / 2;
    int32_t lowest_top = (int32_t)(font->cell_height - font->underline_thickness);
    font->underline_top = underline_top < lowest_top ? underline_top : lowest_top;
    return true;
}

bool font_matches(const Font *font, const char *path, uint32_t size, uint32_t scale) {
    return font->size == size && font->scale == scale && strcmp(font->path, path) == 0;
}

void font_close(Font *font) {
    if (font->face)
        FT_Done_Face(font->face);
    if (font->freetype)
        FT_Done_FreeType(font->freetype);
}

// A wide glyph spans two cells and this fills one half. A glyph wider than its cells, such as CJK in one
// cell, is squeezed to fit. A missing glyph draws as the font's .notdef.
void font_draw(const Font *font, uint32_t key, uint8_t *coverage) {
    FT_Face face = font->face;
    FT_UInt glyph = FT_Get_Char_Index(face, key & GLYPH_CODEPOINT_MASK);
    if (FT_Load_Glyph(face, glyph, FT_LOAD_NO_BITMAP))
        return;
    bool wide = key & (GLYPH_LEFT_HALF | GLYPH_RIGHT_HALF);
    FT_Pos width = ((FT_Pos)font->cell_width * (wide ? 2 : 1)) << 6;
    if (face->glyph->advance.x > width) {
        FT_Matrix squeeze = {.xx = FT_DivFix(width, face->glyph->advance.x), .yy = 0x10000};
        FT_Set_Transform(face, &squeeze, nullptr);
        FT_Error error = FT_Load_Glyph(face, glyph, FT_LOAD_NO_BITMAP);
        FT_Set_Transform(face, nullptr, nullptr);
        if (error)
            return;
    }
    FT_GlyphSlot slot = face->glyph;
    FT_Pos advance = slot->advance.x;
    if (key & GLYPH_BOLD)
        FT_GlyphSlot_Embolden(slot);
    if (key & GLYPH_ITALIC)
        FT_GlyphSlot_Oblique(slot);
    if (FT_Render_Glyph(slot, FT_RENDER_MODE_NORMAL) || slot->bitmap.pixel_mode != FT_PIXEL_MODE_GRAY)
        return;

    int32_t left = round_pixels(width - advance) / 2 + slot->bitmap_left;
    if (key & GLYPH_RIGHT_HALF)
        left -= (int32_t)font->cell_width;
    int32_t top = font->baseline - slot->bitmap_top;
    for (uint32_t row = 0; row < slot->bitmap.rows; row++) {
        int32_t cell_row = top + (int32_t)row;
        if (cell_row < 0 || cell_row >= (int32_t)font->cell_height)
            continue;
        for (uint32_t column = 0; column < slot->bitmap.width; column++) {
            int32_t cell_column = left + (int32_t)column;
            if (cell_column >= 0 && cell_column < (int32_t)font->cell_width)
                coverage[cell_row * font->cell_width + cell_column] = slot->bitmap.buffer[row * slot->bitmap.pitch + column];
        }
    }
}
