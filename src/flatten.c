#include <string.h>

#include "library.h"

// One pass in index order: a parent always has a lower index than its child.
// (hole node-flatten :tags draw :sev blocker) places nothing; every placement is empty, so nothing draws and no node is hit.
void flatten(const Node nodes[255], Placement placements[255]) {
    (void)nodes;
    memset(placements, 0, 255 * sizeof *placements);
}
