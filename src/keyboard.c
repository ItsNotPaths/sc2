#include "library.h"

// (hole keyboard-keys :tags input :sev blocker :needs (window-toplevel tick-exchange)) gets no wl_keyboard; no keymap, key_transitions never move and no text reaches the ring.
void keyboard_attach(Library *library) {
    (void)library;
}

// (hole key-repeat :tags input :sev gap :needs keyboard-keys) held keys never repeat; repeat_timer is never armed from the compositor's delay and rate.
void keyboard_repeat(Library *library) {
    (void)library;
}

// Caller holds the lock.
void text_append(Library *library, uint32_t codepoint) {
    otk_screen *incoming = library->incoming;
    incoming->text[incoming->text_write_index % TEXT_CAPACITY] = codepoint;
    incoming->text_write_index++;
}
