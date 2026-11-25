#include <string.h>

#include "host.h"

enum { FONT_SIZE_LOWEST = 6, FONT_SIZE_HIGHEST = 200 };  // a program's font_size is held to these

// Users vendor sc*2 and patch these. sc2host stops when its own font is missing.
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
// changes at run time. A font or palette change shows on that draw.
void settings_update(Host *host) {
    host->settings = defaults;
    if (host->font_path_asked[0] && strcmp(host->font_path_asked, host->font_path_refused) != 0)
        memcpy(host->settings.font_path, host->font_path_asked, sizeof host->settings.font_path);
    uint16_t asked = host->font_size_asked;
    if (asked)
        host->settings.font_size = asked < FONT_SIZE_LOWEST ? FONT_SIZE_LOWEST
                                 : asked > FONT_SIZE_HIGHEST ? FONT_SIZE_HIGHEST : asked;
    host->incoming->host_font_size = defaults.font_size;
    memcpy(host->incoming->host_palette, host->settings.palette, sizeof host->settings.palette);
}
