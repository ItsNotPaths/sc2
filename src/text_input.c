#include "library.h"

// (hole input-method :tags input :sev gap :needs keyboard-keys) makes no text-input-v3 object; composition stays empty and committed text never reaches the ring.
void text_input_attach(Library *library) {
    (void)library;
}
