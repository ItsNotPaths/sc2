#include "host.h"

// (hole input-method :tags input :sev gap) makes no text-input-v3 object; composition stays empty and committed text never reaches the ring.
void text_input_attach(Host *host) {
    (void)host;
}
