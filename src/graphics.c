#include "library.h"

alignas(uint32_t) static const unsigned char draw_shader[] = {
#embed "draw.spv"
};

// (hole graphics-device :tags draw :sev blocker :needs window-toplevel) makes no Vulkan 1.3 device, surface, swapchain or compute pipeline.
bool graphics_open(Library *library) {
    (void)library, (void)draw_shader;
    return true;
}

void graphics_close(Library *library) {
    (void)library;
}

// (hole image-import :tags draw :sev gap :needs graphics-device) pixels are never imported with VK_EXT_external_memory_host, and there is no copy fallback; pixel nodes draw nothing.
void graphics_import_pixels(Library *library) {
    (void)library;
}
