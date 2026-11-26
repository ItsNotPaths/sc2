#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "host.h"

enum { SCHEME_SIZE_LIMIT = 65536 };  // a base16 file is about 1 KB

// 0 to 15, or -1 when it is not a hex digit.
static int digit_value(char character) {
    const char *digits = "0123456789abcdef";
    const char *found = character ? strchr(digits, tolower((unsigned char)character)) : nullptr;
    return found ? (int)(found - digits) : -1;
}

// Each color is a line `base0X: "#rrggbb"`, anywhere in the file; indent, quotes and # are optional.
static bool scheme_read(const char *path, uint32_t palette[16]) {
    FILE *stream = fopen(path, "r");
    if (!stream)
        return false;
    uint32_t found[16], seen = 0;
    char     line[512];
    while (fgets(line, sizeof line, stream)) {
        const char *at = line + strspn(line, " \t");
        int         index = strncmp(at, "base0", 5) == 0 && at[6] == ':' ? digit_value(at[5]) : -1;
        if (index < 0)
            continue;
        const char *value = at + 7 + strspn(at + 7, " \t\"'#");
        uint32_t    color = 0;
        int         count = 0;
        for (; count < 6 && digit_value(value[count]) >= 0; count++)
            color = color << 4 | digit_value(value[count]);
        if (count == 6 && digit_value(value[6]) < 0) {
            found[index] = color << 8 | 0xff;
            seen |= 1u << index;
        }
    }
    fclose(stream);
    if (seen != 0xffff)
        return false;
    memcpy(palette, found, sizeof found);
    return true;
}

// Reads the scheme a commit names when it names another file, or the file changed since the last
// read. settings_update applies it at the next draw.
void scheme_take(Host *host, const char *path) {
    Scheme     *scheme = &host->scheme;
    struct stat status = {};
    stat(path, &status);
    if (strcmp(path, scheme->path) == 0 && status.st_mtim.tv_sec == scheme->changed.tv_sec
        && status.st_mtim.tv_nsec == scheme->changed.tv_nsec)
        return;
    memcpy(scheme->path, path, sizeof scheme->path);
    scheme->changed = status.st_mtim;
    scheme->read = S_ISREG(status.st_mode) && status.st_size <= SCHEME_SIZE_LIMIT
                && scheme_read(path, scheme->palette);
    if (path[0] && !scheme->read)
        fprintf(stderr, "sc2host: the program's scheme_path is not a readable base16 scheme; using the host's colors\n");
}
