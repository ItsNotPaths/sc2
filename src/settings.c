#include <string.h>

#include "library.h"

static const uint32_t default_palette[16] = {  // base16 default dark
    0x181818ff, 0x282828ff, 0x383838ff, 0x585858ff, 0xb8b8b8ff, 0xd8d8d8ff, 0xe8e8e8ff, 0xf8f8f8ff,
    0xab4642ff, 0xdc9656ff, 0xf7ca88ff, 0xa1b56cff, 0x86c1b9ff, 0x7cafc2ff, 0xba8bafff, 0xa16946ff,
};

// (hole user-settings :tags text :sev gap) font, size and palette are built-in defaults; where the user sets them is not decided.
void settings_load(Settings *settings) {
    strcpy(settings->font_path, "/usr/share/fonts/TTF/DejaVuSansMono.ttf");
    settings->font_size = 20;
    memcpy(settings->palette, default_palette, sizeof default_palette);
}
