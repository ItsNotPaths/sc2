// Type, click, scroll, copy, paste and drop. Node 0 fills the window; node 1 is a scroll box, node 2 the lines inside it.
#include <stdio.h>
#include <string.h>

#include "sc2.h"

enum { LINE_COUNT = 200, TYPED_CAPACITY = 256, CONTROL_C = 0x03, CONTROL_V = 0x16 };
enum { SCANCODE_LEFT_CONTROL = 29, SCANCODE_RIGHT_CONTROL = 97, ZOOM_STEP = 2 };

enum {
    STYLE_PLAIN, STYLE_BANNER, STYLE_HEADING, STYLE_STATUS, STYLE_TYPED, STYLE_CURSOR, STYLE_ITALIC,
    STYLE_BOX, STYLE_LINE  // STYLE_LINE to STYLE_LINE + 7: one per accent color
};

// Built from the host's palette each tick, so they follow its colors.
static void styles_build(sc2_screen *screen) {
    const uint32_t *palette = screen->host_palette;
    sc2_style *styles = screen->styles;
    styles[STYLE_PLAIN]   = (sc2_style){.foreground = palette[5], .background = palette[0]};
    styles[STYLE_BANNER]  = (sc2_style){.foreground = palette[13], .background = palette[0], .bold = 1};
    styles[STYLE_HEADING] = (sc2_style){.foreground = palette[4], .background = palette[0]};
    styles[STYLE_STATUS]  = (sc2_style){.foreground = palette[3], .background = palette[0]};
    styles[STYLE_TYPED]   = (sc2_style){.foreground = palette[6], .background = palette[0]};
    styles[STYLE_CURSOR]  = (sc2_style){.foreground = palette[6], .background = palette[0], .underline = 1};
    styles[STYLE_ITALIC]  = (sc2_style){.foreground = palette[12], .background = palette[0], .italic = 1};
    styles[STYLE_BOX]     = (sc2_style){.foreground = palette[5], .background = palette[1]};
    for (int accent = 0; accent < 8; accent++)
        styles[STYLE_LINE + accent] = (sc2_style){.foreground = palette[8 + accent], .background = palette[1]};
}

static sc2_cell cell(uint32_t codepoint, uint32_t style) {
    return codepoint << SC2_CELL_CODEPOINT_SHIFT | style << SC2_CELL_STYLE_SHIFT;
}

// Returns the column after the text.
static int put(sc2_cell *cells, int width, int column, int row, const char *text, uint32_t style) {
    for (; *text && column < width; text++, column++)
        cells[row * width + column] = cell((unsigned char)*text, style);
    return column;
}

// Each letter takes two cells: the wide cell, then its right half.
static void put_wide(sc2_cell *cells, int width, int column, int row, const char *text, uint32_t style) {
    for (; *text && column + 1 < width; text++, column += 2) {
        cells[row * width + column] = cell((unsigned char)*text, style) | 1u << SC2_CELL_WIDE_SHIFT;
        cells[row * width + column + 1] = cell(' ', style);
    }
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

static void fill(sc2_cell *cells, int count, uint32_t style) {
    for (int index = 0; index < count; index++)
        cells[index] = cell(' ', style);
}

int main(void) {
    sc2_screen *screen = sc2_open();
    if (!screen)
        return 1;
    strcpy(screen->app_id, "hello");
    uint32_t typed[TYPED_CAPACITY];
    int typed_length = 0;
    uint32_t text_read_index = 0, paste_sequence = 0;
    char paste_line[128] = "Ctrl+C copies the line, Ctrl+V pastes, Ctrl+= Ctrl+- Ctrl+0 zoom, or drop something here";
    while (sc2_tick(screen)) {
        int control = screen->key_transitions[SCANCODE_LEFT_CONTROL] % 2 || screen->key_transitions[SCANCODE_RIGHT_CONTROL] % 2;
        int font_size = screen->font_size ? screen->font_size : screen->host_font_size;
        for (; text_read_index != screen->text_write_index; text_read_index++) {
            uint32_t codepoint = screen->text[text_read_index % SC2_TEXT_CAPACITY];
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
                screen->copy_data = SC2_DATA_TEXT;
                screen->copy_sequence++;
            } else if (codepoint == CONTROL_V && screen->paste_offered & SC2_DATA_TEXT) {
                screen->paste_request_data = SC2_DATA_TEXT;
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
            for (uint32_t index = 0; screen->paste_source == SC2_PASTE_CLIPBOARD && index < screen->paste_length; index++)
                if (screen->paste[index] >= ' ' && screen->paste[index] < 0x7F && typed_length < TYPED_CAPACITY)
                    typed[typed_length++] = screen->paste[index];
            int written = snprintf(paste_line, sizeof paste_line, "%s: %u bytes, data %d",
                                   screen->paste_source == SC2_PASTE_DROP ? "drop" : "paste", screen->paste_length,
                                   screen->paste_data);
            if (screen->paste_source == SC2_PASTE_DROP)
                snprintf(paste_line + written, sizeof paste_line - written, ", node %d at %d,%d: %.*s",
                         screen->drop_node, screen->drop_column, screen->drop_row,
                         (int)(screen->paste_length < 60 ? screen->paste_length : 60), (const char *)screen->paste);
            for (char *character = paste_line; *character; character++)
                if ((unsigned char)*character < ' ')  // a uri-list ends its lines with \r\n
                    *character = ' ';
        }

        styles_build(screen);
        int columns = screen->columns, rows = screen->rows;
        int box_columns = columns > 4 ? columns - 4 : 0, box_rows = rows > 8 ? rows - 8 : 0;
        sc2_node *window = &screen->nodes[0], *box = &screen->nodes[1], *lines = &screen->nodes[2];
        *window = (sc2_node){.kind = SC2_NODE_CELLS, .columns = columns, .rows = rows,
                         .content_width = columns, .content_height = rows};
        // The box scrolls by eighths only; whole lines move by rewriting which lines the grid holds.
        int scroll_limit = (LINE_COUNT - box_rows) * 8;
        int scroll = screen->wheel_total < 0 ? 0 : screen->wheel_total > scroll_limit ? scroll_limit : screen->wheel_total;
        int first_line = scroll / 8, shown_lines = box_rows + 1;
        *box = (sc2_node){.kind = SC2_NODE_EMPTY, .parent = 0, .column = 2, .row = 6, .columns = box_columns, .rows = box_rows,
                      .scroll_row = (uint8_t)scroll};
        *lines = (sc2_node){.kind = SC2_NODE_CELLS, .parent = 1, .columns = box_columns, .rows = shown_lines,
                        .content_width = box_columns, .content_height = shown_lines,
                        .storage_offset = columns * rows};

        sc2_cell *cells = screen->cells;
        fill(cells, columns * rows, STYLE_PLAIN);
        int column = put(cells, columns, 0, 0, "sc*2 ", STYLE_BANNER);
        put(cells, columns, column, 0, "hello: type, click, scroll the box", STYLE_HEADING);
        column = put(cells, columns, 0, 1, "styles: ", STYLE_PLAIN);
        column = put(cells, columns, column, 1, "italic ", STYLE_ITALIC);
        column = put(cells, columns, column, 1, "bold ", STYLE_BANNER);
        put_wide(cells, columns, column, 1, "wide", STYLE_HEADING);

        char status[128];
        snprintf(status, sizeof status, "mouse %d,%d (eighths) over node %d   left presses %d   wheel %d",
                 screen->mouse_column, screen->mouse_row, screen->mouse_node,
                 (screen->mouse_transitions[SC2_MOUSE_LEFT] + 1) / 2, screen->wheel_total);
        put(cells, columns, 0, 2, status, STYLE_STATUS);
        put(cells, columns, 0, 3, paste_line, STYLE_STATUS);
        for (int index = 0; index < typed_length && index < columns; index++)
            cells[4 * columns + index] = cell(typed[index], STYLE_TYPED);
        if (typed_length < columns)
            cells[4 * columns + typed_length] = cell(' ', STYLE_CURSOR);

        sc2_cell *line_cells = &cells[lines->storage_offset];
        fill(line_cells, box_columns * shown_lines, STYLE_BOX);
        for (int row = 0; row < shown_lines && first_line + row < LINE_COUNT; row++) {
            int line_number = first_line + row;
            char line[64];
            snprintf(line, sizeof line, "line %d  The quick brown fox jumps over the lazy dog", line_number + 1);
            put(line_cells, box_columns, 1, row, line, STYLE_LINE + line_number % 8);
        }
    }
    sc2_close(screen);
    return 0;
}
