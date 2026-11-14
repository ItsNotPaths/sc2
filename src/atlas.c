#include <string.h>

#include "library.h"

// Both tables are open addressing with linear probing, the same in draw.comp.
// A key is the codepoint with the cell's bold bit; 0 marks an empty entry.
enum {
    BOLD_BIT       = 1u << CELL_BOLD_SHIFT,
    CODEPOINT_MASK = (1u << CELL_FOREGROUND_SHIFT) - 1,
};

static uint32_t glyph_hash(uint32_t key) {
    return key * 2654435761u;
}

bool atlas_open(Library *library) {
    Graphics *graphics = &library->graphics;
    Atlas *atlas = &library->atlas;
    VkDeviceSize slot_size = library->font.cell_width * library->font.cell_height;
    if (!buffer_create(graphics, SLOT_CAPACITY * slot_size, &atlas->coverage)
        || !buffer_create(graphics, GLYPH_TABLE_CAPACITY * 2 * sizeof(uint32_t), &atlas->glyph_table)
        || !buffer_create(graphics, MISS_TABLE_CAPACITY * sizeof(uint32_t), &atlas->miss_table))
        return failed("cannot make the glyph atlas");
    return true;
}

void atlas_close(Library *library) {
    buffer_destroy(&library->graphics, &library->atlas.coverage);
    buffer_destroy(&library->graphics, &library->atlas.glyph_table);
    buffer_destroy(&library->graphics, &library->atlas.miss_table);
}

// Whether the last frame met glyphs with no slot. Waits for that frame to finish.
bool atlas_missed(Library *library) {
    graphics_wait(&library->graphics);
    const uint32_t *misses = library->atlas.miss_table.mapped;
    for (int index = 0; index < MISS_TABLE_CAPACITY; index++)
        if (misses[index])
            return true;
    return false;
}

// Gives each glyph the last frame missed a slot, drawn by the font. The GPU must be done with the frame.
// (hole atlas-eviction :tags (text draw) :sev polish) a full atlas never frees slots; glyphs past SLOT_CAPACITY stay blank and redraw every frame.
void atlas_fill_misses(Library *library) {
    Atlas *atlas = &library->atlas;
    uint32_t *misses = atlas->miss_table.mapped;
    uint32_t *glyph_table = atlas->glyph_table.mapped;
    uint32_t slot_size = library->font.cell_width * library->font.cell_height;
    for (int miss = 0; miss < MISS_TABLE_CAPACITY; miss++) {
        uint32_t key = misses[miss];
        if (!key || atlas->slots_used == SLOT_CAPACITY)
            continue;
        uint32_t entry = glyph_hash(key) % GLYPH_TABLE_CAPACITY;
        while (glyph_table[entry * 2] && glyph_table[entry * 2] != key)
            entry = (entry + 1) % GLYPH_TABLE_CAPACITY;
        if (glyph_table[entry * 2] == key)
            continue;
        uint32_t slot = atlas->slots_used++;
        font_draw(&library->font, key & CODEPOINT_MASK, key & BOLD_BIT, (uint8_t *)atlas->coverage.mapped + slot * slot_size);
        glyph_table[entry * 2] = key;
        glyph_table[entry * 2 + 1] = slot;
    }
    memset(misses, 0, MISS_TABLE_CAPACITY * sizeof *misses);
}
