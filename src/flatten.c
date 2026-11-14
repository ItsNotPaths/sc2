#include "library.h"

static Rectangle intersect(Rectangle first, Rectangle second) {
    return (Rectangle){
        .left   = first.left   > second.left   ? first.left   : second.left,
        .top    = first.top    > second.top    ? first.top    : second.top,
        .right  = first.right  < second.right  ? first.right  : second.right,
        .bottom = first.bottom < second.bottom ? first.bottom : second.bottom,
    };
}

static bool contains(Rectangle rectangle, int32_t column, int32_t row) {
    return column >= rectangle.left && column < rectangle.right && row >= rectangle.top && row < rectangle.bottom;
}

// One pass in index order: a parent always has a lower index than its child.
// Node 0 is the window. A node that breaks the parent rule is hidden, and so are its children.
void flatten(const otk_node nodes[NODE_CAPACITY], Rectangle window, Placement placements[NODE_CAPACITY]) {
    for (int index = 0; index < NODE_CAPACITY; index++) {
        const otk_node *node = &nodes[index];
        Placement *placement = &placements[index];
        if (index == 0) {
            placement->area = window;
            placement->clip = window;
        } else if (node->parent >= index) {
            *placement = (Placement){};
            continue;
        } else {
            const Placement *parent = &placements[node->parent];
            int32_t left = parent->content_left + node->column * 8;
            int32_t top = parent->content_top + node->row * 8;
            placement->area = (Rectangle){left, top, left + node->columns * 8, top + node->rows * 8};
            placement->clip = intersect(placement->area, parent->clip);
        }
        placement->content_left = placement->area.left - node->scroll_column % 8;
        placement->content_top = placement->area.top - node->scroll_row % 8;
    }
}

// The topmost node whose clip holds the point; node 0 when none does.
uint8_t node_at(const Placement placements[NODE_CAPACITY], int32_t column, int32_t row) {
    for (int index = NODE_CAPACITY - 1; index > 0; index--)
        if (contains(placements[index].clip, column, row))
            return index;
    return 0;
}

// Cells a cell node uses; 0 for other kinds and for grids that run past OTK_CELL_CAPACITY.
uint32_t node_cell_count(const otk_node *node) {
    uint32_t count = (uint32_t)node->content_width * node->content_height;
    if (node->kind != OTK_NODE_CELLS || node->storage_offset > OTK_CELL_CAPACITY || count > OTK_CELL_CAPACITY - node->storage_offset)
        return 0;
    return count;
}

// Bytes a pixel node reads; 0 for other kinds, for an offset not a multiple of 4, and for images that run past OTK_PIXEL_CAPACITY.
uint32_t node_pixel_size(const otk_node *node) {
    uint64_t size = (uint64_t)node->content_width * node->content_height * 4;
    if (node->kind != OTK_NODE_PIXELS || node->storage_offset % 4 || node->storage_offset > OTK_PIXEL_CAPACITY
        || size > OTK_PIXEL_CAPACITY - node->storage_offset)
        return 0;
    return (uint32_t)size;
}
