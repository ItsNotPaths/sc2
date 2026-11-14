#include "host.h"

// Users vendor OTK and patch these. otkhost stops when the font is missing.
static const Settings defaults = {
    .font_path = "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
    .font_size = 20,
    .palette = {  // base16 default dark
        0x181818ff, 0x282828ff, 0x383838ff, 0x585858ff, 0xb8b8b8ff, 0xd8d8d8ff, 0xe8e8e8ff, 0xf8f8f8ff,
        0xab4642ff, 0xdc9656ff, 0xf7ca88ff, 0xa1b56cff, 0x86c1b9ff, 0x7cafc2ff, 0xba8bafff, 0xa16946ff,
    },
    .window_columns = 80,
    .window_rows = 24,
    .wheel_rows = 3,
    .cursor_theme = "",
    .cursor_size = 24,
};

// Runs at start, and before each draw inside an input section. The seam for a setting that
// changes at run time: a patch takes it from committed here, such as a font_size field the
// program writes. A font or palette change shows on that draw.
void settings_update(Host *host) {
    host->settings = defaults;
}
