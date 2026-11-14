#include "library.h"

// (hole clipboard-offers :tags clipboard :sev gap) makes no data device; paste_offered stays 0 and MIME names are never folded into DATA_ bits.
void clipboard_attach(Library *library) {
    // (hole clipboard-drop :tags clipboard :sev gap :needs clipboard-paste) ignores drag enter, motion and drop; no drop position, drop_node or PASTE_DROP paste.
    (void)library;
}

// Runs on the library thread after each wake.
void clipboard_serve(Library *library) {
    // (hole clipboard-paste :tags clipboard :sev gap :needs clipboard-offers) ignores paste_request; nothing is read, so paste and paste_sequence never change.
    // (hole clipboard-copy :tags clipboard :sev gap :needs clipboard-offers) ignores copy_sequence; makes no data source, so nothing reaches the clipboard.
    (void)library;
}
