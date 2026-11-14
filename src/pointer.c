#include "library.h"

// (hole pointer-input :tags input :sev blocker :needs (window-toplevel node-flatten tick-exchange)) gets no wl_pointer; mouse position, mouse_node, buttons and wheel never change.
void pointer_attach(Library *library) {
    // (hole cursor-shape :tags input :sev gap :needs pointer-input) sets no cursor on enter; some compositors then show none over the window.
    (void)library;
}
