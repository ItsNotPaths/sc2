// Drag an animated, half see-through image with the left button; text sits on it in clear cells.
// The image moves a whole cell at a time, like everything on the grid.
#include "sc2.h"

enum { IMAGE_SIZE = 256, IMAGE_COLUMNS = 40, IMAGE_ROWS = 20 };
enum { IMAGE = 1, TEXT = 2 };

static sc2_cell cell(uint32_t codepoint, uint32_t foreground) {
    return codepoint << SC2_CELL_CODEPOINT_SHIFT | foreground << SC2_CELL_FOREGROUND_SHIFT | 1u << SC2_CELL_CLEAR_SHIFT;
}

// The mouse moves in eighths of a cell.
static int cell_from_eighths(int eighths) {
    return eighths >= 0 ? eighths / 8 : -((7 - eighths) / 8);
}

// A color sweep with an opaque disc; outside the disc is half see-through.
static void image_draw(uint8_t *pixels, int phase) {
    for (int y = 0; y < IMAGE_SIZE; y++) {
        for (int x = 0; x < IMAGE_SIZE; x++) {
            uint8_t *pixel = &pixels[(y * IMAGE_SIZE + x) * 4];
            int across = x - IMAGE_SIZE / 2, down = y - IMAGE_SIZE / 2;
            int radius = IMAGE_SIZE / 3;
            pixel[0] = (uint8_t)(x + phase);
            pixel[1] = (uint8_t)y;
            pixel[2] = (uint8_t)(255 - x);
            pixel[3] = across * across + down * down < radius * radius ? 255 : 96;
        }
    }
}

int main(void) {
    sc2_screen *screen = sc2_open();
    if (!screen)
        return 1;
    const char *caption = "drag me";
    for (int index = 0; index < IMAGE_COLUMNS * IMAGE_ROWS; index++)
        screen->cells[index] = cell(' ', 7);
    for (int index = 0; caption[index]; index++)
        screen->cells[IMAGE_ROWS / 2 * IMAGE_COLUMNS + (IMAGE_COLUMNS - 7) / 2 + index] = cell((unsigned char)caption[index], 7);

    int image_column = 2, image_row = 1;
    int grab_column = 0, grab_row = 0;  // from the image's top left to the mouse
    int dragging = 0;
    uint8_t left_transitions = screen->mouse_transitions[SC2_MOUSE_LEFT];
    for (int phase = 0; sc2_tick(screen); phase++) {
        int mouse_column = cell_from_eighths(screen->mouse_column);
        int mouse_row = cell_from_eighths(screen->mouse_row);
        uint8_t transitions = screen->mouse_transitions[SC2_MOUSE_LEFT];
        int pressed = transitions != left_transitions && transitions % 2 == 1;
        left_transitions = transitions;
        if (pressed && (screen->mouse_node == IMAGE || screen->mouse_node == TEXT)) {
            dragging = 1;
            grab_column = mouse_column - image_column;
            grab_row = mouse_row - image_row;
        }
        if (transitions % 2 == 0)
            dragging = 0;
        if (dragging) {
            image_column = mouse_column - grab_column;
            image_row = mouse_row - grab_row;
        }

        screen->nodes[IMAGE] = (sc2_node){
            .kind = SC2_NODE_PIXELS, .column = (int16_t)image_column, .row = (int16_t)image_row,
            .columns = IMAGE_COLUMNS, .rows = IMAGE_ROWS,
            .content_width = IMAGE_SIZE, .content_height = IMAGE_SIZE, .sequence = (uint32_t)phase,
        };
        screen->nodes[TEXT] = (sc2_node){
            .kind = SC2_NODE_CELLS, .column = (int16_t)image_column, .row = (int16_t)image_row,
            .columns = IMAGE_COLUMNS, .rows = IMAGE_ROWS,
            .content_width = IMAGE_COLUMNS, .content_height = IMAGE_ROWS,
        };
        image_draw(screen->pixels, phase);
    }
    sc2_close(screen);
    return 0;
}
