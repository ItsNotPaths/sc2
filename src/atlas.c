#include "library.h"

// (hole glyph-atlas :tags (text draw) :sev blocker :needs (graphics-device font-load)) makes no slot grid; the shader has no glyphs to read.
bool atlas_open(Library *library) {
    (void)library;
    return true;
}

// Codepoints the shader missed last frame get a slot, drawn by font_draw, and show one frame later.
void atlas_fill_misses(Library *library) {
    // (hole glyph-atlas) reads back no misses and draws no glyphs.
    (void)library;
}
