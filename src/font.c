#include <string.h>

#include "library.h"

#include FT_SYNTHESIS_H

static int32_t round_pixels(FT_Pos value) {  // 26.6 fixed point
    return (int32_t)((value + 32) >> 6);
}

// The cell is fixed at 1:2. Picks the largest size whose line height and advance both fit,
// then pads: the line is centered in the cell height, each glyph in the cell width.
bool font_load(Font *font, const Settings *settings, uint32_t scale) {
    strcpy(font->path, settings->font_path);
    font->size = settings->font_size;
    font->scale = scale;
    if (!settings->font_size)
        return failed("font_size is 0 in src/settings.c");
    if (FT_Init_FreeType(&font->freetype) || FT_New_Face(font->freetype, settings->font_path, 0, &font->face))
        return failed("cannot open font_path from src/settings.c");
    FT_Face face = font->face;
    if (!FT_IS_SCALABLE(face) || FT_Load_Char(face, 'M', FT_LOAD_NO_SCALE))
        return failed("the font must be scalable and have an M");
    font->cell_height = settings->font_size * scale / 120;
    font->cell_width = font->cell_height / 2;

    FT_Long line_height = face->ascender - face->descender;  // font units
    FT_Long advance = face->glyph->advance.x;
    FT_Long size_by_height = font->cell_height * face->units_per_EM / line_height;
    FT_Long size_by_width = font->cell_width * face->units_per_EM / advance;
    if (FT_Set_Pixel_Sizes(face, 0, size_by_height < size_by_width ? size_by_height : size_by_width))
        return failed("cannot size the font");

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

bool font_matches(const Font *font, const Settings *settings, uint32_t scale) {
    return font->size == settings->font_size && font->scale == scale && strcmp(font->path, settings->font_path) == 0;
}

void font_close(Font *font) {
    if (font->face)
        FT_Done_Face(font->face);
    if (font->freetype)
        FT_Done_FreeType(font->freetype);
}

// Fills coverage: cell_width by cell_height bytes, zeroed by the caller.
// A glyph wider than the cell, such as CJK, is squeezed to fit. A missing glyph draws as the font's .notdef.
void font_draw(const Font *font, uint32_t codepoint, bool bold, uint8_t *coverage) {
    FT_Face face = font->face;
    FT_UInt glyph = FT_Get_Char_Index(face, codepoint);
    if (FT_Load_Glyph(face, glyph, FT_LOAD_NO_BITMAP))
        return;
    FT_Pos cell_width = (FT_Pos)font->cell_width << 6;
    if (face->glyph->advance.x > cell_width) {
        FT_Matrix squeeze = {.xx = FT_DivFix(cell_width, face->glyph->advance.x), .yy = 0x10000};
        FT_Set_Transform(face, &squeeze, nullptr);
        FT_Error error = FT_Load_Glyph(face, glyph, FT_LOAD_NO_BITMAP);
        FT_Set_Transform(face, nullptr, nullptr);
        if (error)
            return;
    }
    FT_GlyphSlot slot = face->glyph;
    FT_Pos advance = slot->advance.x;
    if (bold)
        FT_GlyphSlot_Embolden(slot);
    if (FT_Render_Glyph(slot, FT_RENDER_MODE_NORMAL) || slot->bitmap.pixel_mode != FT_PIXEL_MODE_GRAY)
        return;

    int32_t left = round_pixels(cell_width - advance) / 2 + slot->bitmap_left;
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
