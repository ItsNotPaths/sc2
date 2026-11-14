#include <string.h>

#include "host.h"

// Both tables are open addressing with linear probing, the same in draw.comp.
// A key is the codepoint with the cell's bold bit; 0 marks an empty entry.
enum {
    BOLD_BIT       = 1u << SC2_CELL_BOLD_SHIFT,
    CODEPOINT_MASK = (1u << SC2_CELL_FOREGROUND_SHIFT) - 1,
};

static uint32_t glyph_hash(uint32_t key) {
    return key * 2654435761u;
}

// Where key sits in the glyph table, or the empty entry where it would go.
static uint32_t glyph_entry(const uint32_t *glyph_table, uint32_t key) {
    uint32_t entry = glyph_hash(key) % GLYPH_TABLE_CAPACITY;
    while (glyph_table[entry * 2] && glyph_table[entry * 2] != key)
        entry = (entry + 1) % GLYPH_TABLE_CAPACITY;
    return entry;
}

static void glyph_insert(uint32_t *glyph_table, uint32_t key, uint32_t slot) {
    uint32_t entry = glyph_entry(glyph_table, key);
    glyph_table[entry * 2] = key;
    glyph_table[entry * 2 + 1] = slot;
}

bool atlas_open(Host *host) {
    Graphics *graphics = &host->graphics;
    Atlas *atlas = &host->atlas;
    VkDeviceSize slot_size = host->font.cell_width * host->font.cell_height;
    if (!buffer_create(graphics, SLOT_CAPACITY * slot_size, &atlas->coverage)
        || !buffer_create(graphics, GLYPH_TABLE_CAPACITY * 2 * sizeof(uint32_t), &atlas->glyph_table)
        || !buffer_create(graphics, MISS_TABLE_CAPACITY * sizeof(uint32_t), &atlas->miss_table)
        || !buffer_create(graphics, SLOT_CAPACITY * sizeof(uint32_t), &atlas->slot_frames))
        return failed("cannot make the glyph atlas");
    for (uint32_t slot = 0; slot < SLOT_CAPACITY; slot++)
        atlas->free_slots[slot] = slot;
    atlas->free_count = SLOT_CAPACITY;
    return true;
}

void atlas_close(Host *host) {
    buffer_destroy(&host->graphics, &host->atlas.coverage);
    buffer_destroy(&host->graphics, &host->atlas.glyph_table);
    buffer_destroy(&host->graphics, &host->atlas.miss_table);
    buffer_destroy(&host->graphics, &host->atlas.slot_frames);
}

// Frees each slot the last frame did not draw. Linear probing cannot delete in place, so the table is rebuilt.
static void atlas_evict(Atlas *atlas, uint32_t last_frame) {
    const uint32_t *slot_frames = atlas->slot_frames.mapped;
    uint32_t *glyph_table = atlas->glyph_table.mapped;
    uint32_t old_table[GLYPH_TABLE_CAPACITY * 2];
    memcpy(old_table, glyph_table, sizeof old_table);
    memset(glyph_table, 0, sizeof old_table);
    for (int entry = 0; entry < GLYPH_TABLE_CAPACITY; entry++) {
        uint32_t key = old_table[entry * 2];
        uint32_t slot = old_table[entry * 2 + 1];
        if (!key)
            continue;
        if (slot_frames[slot] == last_frame)
            glyph_insert(glyph_table, key, slot);
        else
            atlas->free_slots[atlas->free_count++] = slot;
    }
}

// Gives each glyph the last frame missed a slot, drawn by the font. True if any glyph was added.
// A full atlas evicts once; glyphs past what one frame can hold stay blank.
bool atlas_fill_misses(Host *host) {
    graphics_wait(&host->graphics);
    Atlas *atlas = &host->atlas;
    uint32_t last_frame = host->drawing.frame_number;
    uint32_t *misses = atlas->miss_table.mapped;
    uint32_t *glyph_table = atlas->glyph_table.mapped;
    uint32_t *slot_frames = atlas->slot_frames.mapped;
    uint32_t slot_size = host->font.cell_width * host->font.cell_height;
    bool evicted = false;
    bool added = false;
    for (int miss = 0; miss < MISS_TABLE_CAPACITY; miss++) {
        uint32_t key = misses[miss];
        if (!key)
            continue;
        if (atlas->free_count == 0 && !evicted) {
            atlas_evict(atlas, last_frame);
            evicted = true;
        }
        if (atlas->free_count == 0)
            break;
        uint32_t slot = atlas->free_slots[--atlas->free_count];
        font_draw(&host->font, key & CODEPOINT_MASK, key & BOLD_BIT, (uint8_t *)atlas->coverage.mapped + slot * slot_size);
        glyph_insert(glyph_table, key, slot);
        slot_frames[slot] = last_frame;  // safe from eviction until a frame can draw it
        added = true;
    }
    memset(misses, 0, MISS_TABLE_CAPACITY * sizeof *misses);
    return added;
}
