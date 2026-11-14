#include <sched.h>
#include <stddef.h>
#include <string.h>

#include "host.h"

// A node as the shader sees it, in pixels. Matches Node in draw.comp.
typedef struct {
    int32_t  clip_left, clip_top, clip_right, clip_bottom;
    int32_t  content_left, content_top;
    uint32_t kind;
    uint32_t storage_offset;                   // into the packed cells, or bytes into pixels
    uint32_t content_width, content_height;
    uint32_t area_width, area_height;          // an image is scaled to fill its area
} DrawNode;

// Everything one frame of draw.comp reads. Matches Frame in draw.comp.
typedef struct {
    VkDeviceAddress cells, coverage, glyph_table, miss_table, slot_frames, pixels;
    uint32_t palette[16];
    uint32_t cell_width, cell_height;
    uint32_t underline_top, underline_thickness;
    uint32_t glyph_table_mask, miss_table_mask;
    uint32_t node_count;                       // highest visible index + 1
    uint32_t frame_number;
    DrawNode nodes[NODE_COUNT];
} DrawFrame;

static_assert(offsetof(DrawFrame, nodes) == 144, "draw.comp reads the nodes at offset 144");
static_assert(sizeof(DrawNode) == 48, "draw.comp steps through the nodes 48 bytes at a time");

static int32_t to_pixels(int32_t eighths, uint32_t cell_size) {
    return (int32_t)floor_divide((int64_t)eighths * cell_size, 8);
}

static bool visible(Rectangle clip) {
    return clip.left < clip.right && clip.top < clip.bottom;
}

// The GPU's view of pixels up to end: imported in place, or a copy where import fails.
static bool pixels_reserve(Host *host, uint32_t end) {
    Graphics *graphics = &host->graphics;
    Drawing *drawing = &host->drawing;
    if (graphics->host_import) {
        if (drawing->pixels.size >= end)
            return true;
        buffer_destroy(graphics, &drawing->pixels);
        VkDeviceSize size = (end + graphics->import_alignment - 1) / graphics->import_alignment * graphics->import_alignment;
        if (buffer_import(graphics, host->screen->pixels, size, &drawing->pixels))
            return true;
        failed("cannot import pixels; copying images instead");
        graphics->host_import = false;
    }
    VkDeviceSize old_size = drawing->pixels.size;
    if (!buffer_reserve(graphics, &drawing->pixels, end))
        return false;
    if (drawing->pixels.size != old_size)
        memset(drawing->copied_images, 0, sizeof drawing->copied_images);
    return true;
}

// Copies a node's image to the GPU when it changed. Only where import failed.
static void image_copy(Host *host, int index, const otk_node *node, uint32_t size) {
    CopiedImage *copied = &host->drawing.copied_images[index];
    CopiedImage current = {node->storage_offset, size, node->sequence};
    if (host->graphics.host_import || memcmp(copied, &current, sizeof current) == 0)
        return;
    memcpy((uint8_t *)host->drawing.pixels.mapped + node->storage_offset, &host->screen->pixels[node->storage_offset], size);
    *copied = current;
}

// Flattens the committed nodes, packs the cells of the visible ones and readies their images.
static bool frame_build(Host *host) {
    const otk_screen *committed = host->committed;
    const Font *font = &host->font;
    const Atlas *atlas = &host->atlas;
    Drawing *drawing = &host->drawing;
    Placement *placements = host->placements;

    Rectangle window = {0, 0, host->window.width * 8 / font->cell_width, host->window.height * 8 / font->cell_height};
    flatten(committed->nodes, window, placements);

    uint32_t node_count = 0;
    uint64_t cell_count = 0;
    uint32_t pixel_end = 0;
    for (int index = 0; index < NODE_COUNT; index++) {
        const otk_node *node = &committed->nodes[index];
        if (!visible(placements[index].clip))
            continue;
        node_count = index + 1;
        cell_count += node_cell_count(node);
        uint32_t pixel_size = node_pixel_size(node);
        if (pixel_size && node->storage_offset + pixel_size > pixel_end)
            pixel_end = node->storage_offset + pixel_size;
    }
    if (!buffer_reserve(&host->graphics, &drawing->cells, cell_count * sizeof(otk_cell)))
        return failed("cannot grow the cell buffer");
    if (!pixels_reserve(host, pixel_end))
        return failed("cannot grow the pixel buffer");

    DrawFrame *frame = drawing->frame.mapped;
    frame->cells = drawing->cells.address;
    frame->coverage = atlas->coverage.address;
    frame->glyph_table = atlas->glyph_table.address;
    frame->miss_table = atlas->miss_table.address;
    frame->slot_frames = atlas->slot_frames.address;
    frame->pixels = drawing->pixels.address;
    memcpy(frame->palette, host->settings.palette, sizeof frame->palette);
    frame->cell_width = font->cell_width;
    frame->cell_height = font->cell_height;
    frame->underline_top = font->underline_top;
    frame->underline_thickness = font->underline_thickness;
    frame->glyph_table_mask = GLYPH_TABLE_CAPACITY - 1;
    frame->miss_table_mask = MISS_TABLE_CAPACITY - 1;
    frame->node_count = node_count;
    frame->frame_number = ++drawing->frame_number;

    otk_cell *packed_cells = drawing->cells.mapped;
    uint32_t packed = 0;
    for (uint32_t index = 0; index < node_count; index++) {
        const otk_node *node = &committed->nodes[index];
        const Placement *placement = &placements[index];
        Rectangle clip = placement->clip;
        uint32_t count = visible(clip) ? node_cell_count(node) : 0;
        uint32_t pixel_size = visible(clip) ? node_pixel_size(node) : 0;
        Rectangle area = placement->area;
        int32_t content_left = pixel_size ? area.left : placement->content_left;  // images ignore scroll
        int32_t content_top = pixel_size ? area.top : placement->content_top;
        frame->nodes[index] = (DrawNode){
            .clip_left = to_pixels(clip.left, font->cell_width),
            .clip_top = to_pixels(clip.top, font->cell_height),
            .clip_right = to_pixels(clip.right, font->cell_width),
            .clip_bottom = to_pixels(clip.bottom, font->cell_height),
            .content_left = to_pixels(content_left, font->cell_width),
            .content_top = to_pixels(content_top, font->cell_height),
            .kind = count ? OTK_NODE_CELLS : pixel_size ? OTK_NODE_PIXELS : OTK_NODE_EMPTY,
            .storage_offset = pixel_size ? node->storage_offset : packed,
            .content_width = node->content_width,
            .content_height = node->content_height,
            .area_width = to_pixels(area.right, font->cell_width) - to_pixels(area.left, font->cell_width),
            .area_height = to_pixels(area.bottom, font->cell_height) - to_pixels(area.top, font->cell_height),
        };
        memcpy(&packed_cells[packed], &committed->cells[node->storage_offset], count * sizeof(otk_cell));
        packed += count;
        if (pixel_size)
            image_copy(host, index, node, pixel_size);
    }
    return true;
}

// Loads the font the settings and scale ask for, and empties the atlas. The GPU must be idle.
// A failure closes the window, since nothing can draw without a font and an atlas.
static bool font_reload(Host *host) {
    font_close(&host->font);
    host->font = (Font){};
    atlas_close(host);
    host->atlas = (Atlas){};
    bool loaded = font_load(&host->font, &host->settings, host->window.scale) && atlas_open(host);
    input_begin(host);
    if (loaded)
        window_resize(host);
    else
        host->incoming->closed = 1;
    input_end(host);
    return loaded;
}

// Builds from one whole commit: a commit that lands meanwhile means building again.
static bool frame_build_whole(Host *host) {
    const uint32_t *sequence = &host->shared->header.commit_sequence;
    for (;;) {
        uint32_t begun = sequence_read_begin(sequence);
        if (begun % 2 == 0 && !frame_build(host))
            return false;
        if (sequence_read_valid(sequence, begun)) {
            host->drawing.drawn_sequence = begun;
            return true;
        }
        sched_yield();
    }
}

bool draw_open(Host *host) {
    if (!buffer_create(&host->graphics, sizeof(DrawFrame), &host->drawing.frame))
        return failed("cannot make the frame buffer");
    return true;
}

void draw_close(Host *host) {
    buffer_destroy(&host->graphics, &host->drawing.frame);
    buffer_destroy(&host->graphics, &host->drawing.cells);
    buffer_destroy(&host->graphics, &host->drawing.pixels);
}

// Due when the compositor wants a frame and something changed: a commit, the scale, the window
// size, or glyphs the last frame missed, now in the atlas. A settings change comes with a commit.
bool draw_due(Host *host) {
    const Window *window = &host->window;
    const VkExtent2D extent = host->graphics.extent;
    if (!window->configured || window->frame_pending)
        return false;
    return sequence_read_begin(&host->shared->header.commit_sequence) != host->drawing.drawn_sequence
        || !font_matches(&host->font, &host->settings, window->scale)
        || extent.width != window->width || extent.height != window->height
        || atlas_fill_misses(host);
}

void draw_frame(Host *host) {
    Graphics *graphics = &host->graphics;
    Window *window = &host->window;
    Drawing *drawing = &host->drawing;
    graphics_wait(graphics);
    input_begin(host);
    settings_update(host);
    memcpy(host->incoming->palette, host->settings.palette, sizeof host->settings.palette);
    input_end(host);
    if (!font_matches(&host->font, &host->settings, window->scale) && !font_reload(host))
        return;
    atlas_fill_misses(host);
    if ((graphics->extent.width != window->width || graphics->extent.height != window->height)
        && !graphics_resize(graphics, window->width, window->height))
        return;

    if (!frame_build_whole(host))
        return;
    input_begin(host);
    pointer_hit(host);  // the layout may have moved under the mouse
    input_end(host);

    window_request_frame(host);
    if (!graphics_present(graphics, drawing->frame.address))
        window->frame_pending = false;
}
