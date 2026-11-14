#include <stddef.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "library.h"

static_assert(offsetof(otk_screen, pixels) == 0, "graphics import needs pixels on a page");
static_assert(sizeof(Node) == 28, "bindings read the Node layout from otk.h");

static Library library;  // one window per process

static otk_screen *map_screen(int memory_file) {
    int flags = memory_file < 0 ? MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE : MAP_SHARED;
    void *address = mmap(nullptr, sizeof(otk_screen), PROT_READ | PROT_WRITE, flags, memory_file, 0);
    return address == MAP_FAILED ? nullptr : address;
}

static bool screen_map(Library *library) {
    library->memory_file = memfd_create("otk-screen", MFD_CLOEXEC);
    if (library->memory_file < 0 || ftruncate(library->memory_file, sizeof(otk_screen)) < 0)
        return false;
    library->screen = map_screen(library->memory_file);
    library->incoming = map_screen(-1);
    library->committed = map_screen(-1);
    return library->screen && library->incoming && library->committed;
}

static void screen_unmap(Library *library) {
    otk_screen *mappings[] = {library->screen, library->incoming, library->committed};
    for (size_t index = 0; index < 3; index++)
        if (mappings[index])
            munmap(mappings[index], sizeof(otk_screen));
    if (library->memory_file >= 0)
        close(library->memory_file);
}

[[gnu::visibility("default")]]
otk_screen *otk_open(const char *title) {
    if (library.screen)
        return nullptr;
    library.memory_file = -1;
    settings_load(&library.settings);
    bool opened = screen_map(&library)
        && wayland_connect(&library)
        && window_open(&library, title)
        && graphics_open(&library)
        && font_load(&library.settings)
        && atlas_open(&library);
    if (opened) {
        library.screen->tick_rate = 240;
        memcpy(library.incoming->palette, library.settings.palette, sizeof library.settings.palette);
        opened = library_start(&library);
    }
    if (!opened) {
        otk_close(library.screen);
        return nullptr;
    }
    return library.screen;
}

[[gnu::visibility("default")]]
int otk_tick(otk_screen *screen) {
    tick_publish(&library);
    tick_wait(&library);
    tick_receive(&library);
    return !screen->closed;
}

[[gnu::visibility("default")]]
void otk_close(otk_screen *screen) {
    if (screen != library.screen)
        return;
    library_stop(&library);
    graphics_close(&library);
    window_close(&library);
    wayland_disconnect(&library);
    screen_unmap(&library);
    library = (Library){};
}
