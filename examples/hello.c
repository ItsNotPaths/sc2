// Type, click, scroll, copy, paste and drop. Node 0 fills the window; node 1 is a scroll box, node 2 the lines inside it.
#include <stdio.h>
#include <string.h>

#include "otk.h"

enum { LINE_COUNT = 200, TYPED_CAPACITY = 256, CONTROL_C = 0x03, CONTROL_V = 0x16 };
enum { SCANCODE_LEFT_CONTROL = 29, SCANCODE_RIGHT_CONTROL = 97, ZOOM_STEP = 2 };

static otk_cell cell(uint32_t codepoint, uint32_t foreground, uint32_t background, uint32_t bold) {
    return codepoint << OTK_CELL_CODEPOINT_SHIFT | foreground << OTK_CELL_FOREGROUND_SHIFT
         | background << OTK_CELL_BACKGROUND_SHIFT | bold << OTK_CELL_BOLD_SHIFT;
}

static void put(otk_cell *cells, int width, int column, int row, const char *text, uint32_t foreground, uint32_t background) {
    for (int index = 0; text[index] && column + index < width; index++)
        cells[row * width + column + index] = cell((unsigned char)text[index], foreground, background, 0);
}

static int utf8_encode(uint32_t codepoint, uint8_t *bytes) {
    if (codepoint < 0x80) {
        bytes[0] = codepoint;
        return 1;
    }
    int count = codepoint < 0x800 ? 2 : codepoint < 0x10000 ? 3 : 4;
    for (int index = count - 1; index > 0; index--, codepoint >>= 6)
        bytes[index] = 0x80 | (codepoint & 0x3F);
    bytes[0] = (0xF0 << (4 - count)) | codepoint;
    return count;
}

static void fill(otk_cell *cells, int count, uint32_t background) {
    for (int index = 0; index < count; index++)
        cells[index] = cell(' ', 5, background, 0);
}

int main(void) {
    otk_screen *screen = otk_open();
    if (!screen)
        return 1;
    strcpy(screen->app_id, "hello");
    uint32_t typed[TYPED_CAPACITY];
    int typed_length = 0;
    uint32_t text_read_index = 0, paste_sequence = 0;
    char paste_line[128] = "Ctrl+C copies the line, Ctrl+V pastes, Ctrl+= Ctrl+- Ctrl+0 zoom, or drop something here";
    while (otk_tick(screen)) {
        int control = screen->key_transitions[SCANCODE_LEFT_CONTROL] % 2 || screen->key_transitions[SCANCODE_RIGHT_CONTROL] % 2;
        int font_size = screen->font_size ? screen->font_size : screen->host_font_size;
        for (; text_read_index != screen->text_write_index; text_read_index++) {
            uint32_t codepoint = screen->text[text_read_index % OTK_TEXT_CAPACITY];
            if (control && (codepoint == '=' || codepoint == '+')) {
                screen->font_size = font_size + ZOOM_STEP;
            } else if (control && codepoint == '-' && font_size > ZOOM_STEP) {
                screen->font_size = font_size - ZOOM_STEP;
            } else if (control && codepoint == '0') {
                screen->font_size = 0;
            } else if (codepoint == CONTROL_C) {
                screen->copy_length = 0;
                for (int index = 0; index < typed_length; index++)
                    screen->copy_length += utf8_encode(typed[index], &screen->copy[screen->copy_length]);
                screen->copy_data = OTK_DATA_TEXT;
                screen->copy_sequence++;
            } else if (codepoint == CONTROL_V && screen->paste_offered & OTK_DATA_TEXT) {
                screen->paste_request_data = OTK_DATA_TEXT;
                screen->paste_request++;
            } else if (codepoint == '\b' && typed_length > 0)
                typed_length--;
            else if (codepoint == '\r')
                typed_length = 0;
            else if (codepoint >= ' ' && typed_length < TYPED_CAPACITY)
                typed[typed_length++] = codepoint;
        }

        // The window's title is the typed line; empty shows the file name.
        int title_length = 0;
        for (int index = 0; index < typed_length && title_length + 4 < (int)sizeof screen->title; index++)
            title_length += utf8_encode(typed[index], (uint8_t *)&screen->title[title_length]);
        screen->title[title_length] = 0;

        // Pasted text is added byte by byte, so only ASCII comes through intact.
        if (screen->paste_sequence != paste_sequence) {
            paste_sequence = screen->paste_sequence;
            for (uint32_t index = 0; screen->paste_source == OTK_PASTE_CLIPBOARD && index < screen->paste_length; index++)
                if (screen->paste[index] >= ' ' && screen->paste[index] < 0x7F && typed_length < TYPED_CAPACITY)
                    typed[typed_length++] = screen->paste[index];
            int written = snprintf(paste_line, sizeof paste_line, "%s: %u bytes, data %d",
                                   screen->paste_source == OTK_PASTE_DROP ? "drop" : "paste", screen->paste_length,
                                   screen->paste_data);
            if (screen->paste_source == OTK_PASTE_DROP)
                snprintf(paste_line + written, sizeof paste_line - written, ", node %d at %d,%d: %.*s",
                         screen->drop_node, screen->drop_column, screen->drop_row,
                         (int)(screen->paste_length < 60 ? screen->paste_length : 60), (const char *)screen->paste);
            for (char *character = paste_line; *character; character++)
                if ((unsigned char)*character < ' ')  // a uri-list ends its lines with \r\n
                    *character = ' ';
        }

        int columns = screen->columns, rows = screen->rows;
        int box_columns = columns > 4 ? columns - 4 : 0, box_rows = rows > 8 ? rows - 8 : 0;
        otk_node *window = &screen->nodes[0], *box = &screen->nodes[1], *lines = &screen->nodes[2];
        *window = (otk_node){.kind = OTK_NODE_CELLS, .columns = columns, .rows = rows,
                         .content_width = columns, .content_height = rows};
        // The box scrolls by eighths only; whole lines move by rewriting which lines the grid holds.
        int scroll_limit = (LINE_COUNT - box_rows) * 8;
        int scroll = screen->wheel_total < 0 ? 0 : screen->wheel_total > scroll_limit ? scroll_limit : screen->wheel_total;
        int first_line = scroll / 8, shown_lines = box_rows + 1;
        *box = (otk_node){.kind = OTK_NODE_EMPTY, .parent = 0, .column = 2, .row = 6, .columns = box_columns, .rows = box_rows,
                      .scroll_row = (uint8_t)scroll};
        *lines = (otk_node){.kind = OTK_NODE_CELLS, .parent = 1, .columns = box_columns, .rows = shown_lines,
                        .content_width = box_columns, .content_height = shown_lines,
                        .storage_offset = columns * rows};

        otk_cell *cells = screen->cells;
        fill(cells, columns * rows, 0);
        for (int index = 0; index < 4 && index < columns; index++)
            cells[index] = cell("OTK "[index], 13, 0, 1);
        put(cells, columns, 4, 0, "hello: type, click, scroll the box", 4, 0);

        char status[128];
        snprintf(status, sizeof status, "mouse %d,%d (eighths) over node %d   left presses %d   wheel %d",
                 screen->mouse_column, screen->mouse_row, screen->mouse_node,
                 (screen->mouse_transitions[OTK_MOUSE_LEFT] + 1) / 2, screen->wheel_total);
        put(cells, columns, 0, 2, status, 3, 0);
        put(cells, columns, 0, 3, paste_line, 3, 0);
        for (int index = 0; index < typed_length && index < columns; index++)
            cells[4 * columns + index] = cell(typed[index], 6, 0, 0);
        if (typed_length < columns)
            cells[4 * columns + typed_length] = cell(' ', 6, 0, 0) | 1u << OTK_CELL_UNDERLINE_SHIFT;

        otk_cell *line_cells = &cells[lines->storage_offset];
        fill(line_cells, box_columns * shown_lines, 1);
        for (int row = 0; row < shown_lines && first_line + row < LINE_COUNT; row++) {
            int line_number = first_line + row;
            char line[64];
            snprintf(line, sizeof line, "line %d  The quick brown fox jumps over the lazy dog", line_number + 1);
            put(line_cells, box_columns, 1, row, line, 8 + line_number % 8, 1);
        }
    }
    otk_close(screen);
    return 0;
}
