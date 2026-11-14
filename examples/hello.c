// Type, click and scroll. Node 0 fills the window; node 1 is a scroll box, node 2 the lines inside it.
#include <stdio.h>
#include <string.h>

#include "otk.h"

enum { LINE_COUNT = 200, TYPED_CAPACITY = 256 };

static Cell cell(uint32_t codepoint, uint32_t foreground, uint32_t background, uint32_t bold) {
    return codepoint << CELL_CODEPOINT_SHIFT | foreground << CELL_FOREGROUND_SHIFT
         | background << CELL_BACKGROUND_SHIFT | bold << CELL_BOLD_SHIFT;
}

static void put(Cell *cells, int width, int column, int row, const char *text, uint32_t foreground, uint32_t background) {
    for (int index = 0; text[index] && column + index < width; index++)
        cells[row * width + column + index] = cell((unsigned char)text[index], foreground, background, 0);
}

static void fill(Cell *cells, int count, uint32_t background) {
    for (int index = 0; index < count; index++)
        cells[index] = cell(' ', 5, background, 0);
}

int main(void) {
    otk_screen *screen = otk_open("hello");
    if (!screen)
        return 1;
    uint32_t typed[TYPED_CAPACITY];
    int typed_length = 0;
    uint32_t text_read_index = 0;
    while (otk_tick(screen)) {
        for (; text_read_index != screen->text_write_index; text_read_index++) {
            uint32_t codepoint = screen->text[text_read_index % TEXT_CAPACITY];
            if (codepoint == '\b' && typed_length > 0)
                typed_length--;
            else if (codepoint == '\r')
                typed_length = 0;
            else if (codepoint >= ' ' && typed_length < TYPED_CAPACITY)
                typed[typed_length++] = codepoint;
        }

        int columns = screen->columns, rows = screen->rows;
        int box_columns = columns > 4 ? columns - 4 : 0, box_rows = rows > 8 ? rows - 8 : 0;
        Node *window = &screen->nodes[0], *box = &screen->nodes[1], *lines = &screen->nodes[2];
        *window = (Node){.kind = NODE_CELLS, .columns = columns, .rows = rows,
                         .content_width = columns, .content_height = rows};
        int scroll_limit = (LINE_COUNT - box_rows) * 8;
        int scroll = screen->wheel_total < 0 ? 0 : screen->wheel_total > scroll_limit ? scroll_limit : screen->wheel_total;
        *box = (Node){.kind = NODE_EMPTY, .parent = 0, .column = 2, .row = 6, .columns = box_columns, .rows = box_rows,
                      .scroll_row = scroll};
        *lines = (Node){.kind = NODE_CELLS, .parent = 1, .columns = box_columns, .rows = LINE_COUNT,
                        .content_width = box_columns, .content_height = LINE_COUNT,
                        .storage_offset = columns * rows};

        Cell *cells = screen->cells;
        fill(cells, columns * rows, 0);
        for (int index = 0; index < 4 && index < columns; index++)
            cells[index] = cell("OTK "[index], 13, 0, 1);
        put(cells, columns, 4, 0, "hello: type, click, scroll the box", 4, 0);

        char status[128];
        snprintf(status, sizeof status, "mouse %d,%d (eighths) over node %d   left presses %d   wheel %d",
                 screen->mouse_column, screen->mouse_row, screen->mouse_node,
                 (screen->mouse_transitions[MOUSE_LEFT] + 1) / 2, screen->wheel_total);
        put(cells, columns, 0, 2, status, 3, 0);
        for (int index = 0; index < typed_length && index < columns; index++)
            cells[4 * columns + index] = cell(typed[index], 6, 0, 0);
        if (typed_length < columns)
            cells[4 * columns + typed_length] = cell(' ', 6, 0, 0) | 1u << CELL_UNDERLINE_SHIFT;

        Cell *line_cells = &cells[lines->storage_offset];
        fill(line_cells, box_columns * LINE_COUNT, 1);
        for (int row = 0; row < LINE_COUNT; row++) {
            char line[64];
            snprintf(line, sizeof line, "line %d  The quick brown fox jumps over the lazy dog", row + 1);
            put(line_cells, box_columns, 1, row, line, 8 + row % 8, 1);
        }
    }
    otk_close(screen);
    return 0;
}
