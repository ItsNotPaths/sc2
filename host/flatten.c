#include "host.h"

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
void flatten(const otk_node nodes[NODE_COUNT], Rectangle window, Placement placements[NODE_COUNT]) {
    for (int index = 0; index < NODE_COUNT; index++) {
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
uint8_t node_at(const Placement placements[NODE_COUNT], int32_t column, int32_t row) {
    for (int index = NODE_COUNT - 1; index > 0; index--)
        if (contains(placements[index].clip, column, row))
            return index;
    return 0;
}
