#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "library.h"

typedef struct {
    const char *mime_type;
    uint8_t     data;  // DATA_ bit
} DataType;

// A drop takes the first row its offer has. A paste takes the first row with the requested bit.
static const DataType data_types[] = {
    {"text/uri-list",            DATA_URI_LIST},
    {"image/png",                DATA_PNG},
    {"image/jpeg",               DATA_JPEG},
    {"image/webp",               DATA_WEBP},
    {"image/avif",               DATA_AVIF},
    {"text/plain;charset=utf-8", DATA_TEXT},
    {"text/plain",               DATA_TEXT},
    {"UTF8_STRING",              DATA_TEXT},
    {"text/html",                DATA_HTML},
};

enum { DATA_TYPE_COUNT = sizeof data_types / sizeof *data_types, DATA_ANY = UINT8_MAX };
static_assert(DATA_TYPE_COUNT <= 16, "an offer's types are one uint16_t, a bit per row");

// Folds an offer's rows into DATA_ bits.
static uint8_t data_bits(uint16_t types) {
    uint8_t bits = 0;
    for (int row = 0; row < DATA_TYPE_COUNT; row++)
        if (types & 1u << row)
            bits |= data_types[row].data;
    return bits;
}

// The first row the offer has with one of the wanted DATA_ bits, or -1.
static int data_type_find(uint16_t types, uint8_t wanted) {
    for (int row = 0; row < DATA_TYPE_COUNT; row++)
        if (types & 1u << row && data_types[row].data & wanted)
            return row;
    return -1;
}

// Every request and every drop gets one answer: the bytes, or none when the read failed.
static void receive_end(Library *library, bool arrived) {
    Clipboard *clipboard = &library->clipboard;
    Receive *receive = &clipboard->receive;
    otk_screen *incoming = library->incoming;
    uint32_t length = arrived ? receive->length : 0;
    if (length)
        memcpy(incoming->paste, clipboard->received, length);
    incoming->paste_length = length;
    incoming->paste_data = receive->data;
    incoming->paste_source = receive->source;
    if (receive->source == PASTE_DROP) {
        incoming->drop_column = receive->drop_column;
        incoming->drop_row = receive->drop_row;
        incoming->drop_node = receive->drop_node;
    }
    incoming->paste_sequence++;

    if (receive->pipe >= 0)
        close(receive->pipe);
    if (receive->dropped) {
        if (arrived && wl_data_offer_get_version(receive->dropped) >= 3)
            wl_data_offer_finish(receive->dropped);
        wl_data_offer_destroy(receive->dropped);
    }
    *receive = (Receive){};
}

// Reads one row of offer into received. A read still running is answered empty first.
static void receive_start(Library *library, Receive request, struct wl_data_offer *offer, int row) {
    Receive *receive = &library->clipboard.receive;
    if (receive->active)
        receive_end(library, false);
    *receive = request;
    receive->active = true;
    receive->pipe = -1;
    int pipe_ends[2];
    if (row < 0 || pipe2(pipe_ends, O_CLOEXEC | O_NONBLOCK) < 0) {
        receive_end(library, false);
        return;
    }
    wl_data_offer_receive(offer, data_types[row].mime_type, pipe_ends[1]);
    close(pipe_ends[1]);
    receive->pipe = pipe_ends[0];
}

// received holds one byte past PASTE_CAPACITY, so a paste too large to keep shows up.
static void receive_read(Library *library) {
    Clipboard *clipboard = &library->clipboard;
    Receive *receive = &clipboard->receive;
    ssize_t count = -1;
    while (receive->length <= PASTE_CAPACITY
           && (count = read(receive->pipe, clipboard->received + receive->length,
                            PASTE_CAPACITY + 1 - receive->length)) > 0)
        receive->length += count;
    if (receive->length > PASTE_CAPACITY)
        receive_end(library, false);
    else if (count == 0)
        receive_end(library, true);
    else if (errno != EAGAIN)
        receive_end(library, false);
}

static void paste_start(Library *library) {
    Clipboard *clipboard = &library->clipboard;
    uint8_t wanted = library->committed->paste_request_data;
    Receive request = {.data = wanted, .source = PASTE_CLIPBOARD};
    receive_start(library, request, clipboard->selection, data_type_find(clipboard->selection_types, wanted));
}

static void send_close(Clipboard *clipboard, uint32_t index) {
    close(clipboard->sends[index].pipe);
    clipboard->sends[index] = clipboard->sends[--clipboard->send_count];
}

// Writes what each pipe takes now. A newer copy cuts the older sends off, since their bytes are gone.
static void sends_write(Library *library) {
    Clipboard *clipboard = &library->clipboard;
    const uint8_t *copy = library->committed->copy;
    bool current = library->committed->copy_sequence == clipboard->copy_sequence;
    for (uint32_t index = 0; index < clipboard->send_count;) {
        Send *send = &clipboard->sends[index];
        ssize_t count = -1;
        while (current && send->length < clipboard->copy_length
               && (count = write(send->pipe, copy + send->length, clipboard->copy_length - send->length)) > 0)
            send->length += count;
        bool waiting = current && send->length < clipboard->copy_length && count < 0 && errno == EAGAIN;
        if (waiting)
            index++;
        else
            send_close(clipboard, index);
    }
}

static void source_send(void *data, struct wl_data_source *, const char *, int32_t pipe) {
    Clipboard *clipboard = &((Library *)data)->clipboard;
    if (clipboard->send_count == SEND_CAPACITY) {
        close(pipe);
        return;
    }
    fcntl(pipe, F_SETFL, O_NONBLOCK);
    clipboard->sends[clipboard->send_count++] = (Send){.pipe = pipe};
}

// Another client took the clipboard.
static void source_cancelled(void *data, struct wl_data_source *source) {
    wl_data_source_destroy(source);
    ((Library *)data)->clipboard.source = nullptr;
}

static void source_target(void *, struct wl_data_source *, const char *) {}
static void source_drop_performed(void *, struct wl_data_source *) {}
static void source_finished(void *, struct wl_data_source *) {}
static void source_action(void *, struct wl_data_source *, uint32_t) {}

static const struct wl_data_source_listener source_listener = {
    .target = source_target,
    .send = source_send,
    .cancelled = source_cancelled,
    .dnd_drop_performed = source_drop_performed,
    .dnd_finished = source_finished,
    .action = source_action,
};

// copy_data 0 empties the clipboard.
static void copy_offer(Library *library) {
    Clipboard *clipboard = &library->clipboard;
    const otk_screen *committed = library->committed;
    struct wl_data_source *source = nullptr;
    if (committed->copy_data) {
        source = wl_data_device_manager_create_data_source(library->globals.data_device_manager);
        wl_data_source_add_listener(source, &source_listener, library);
        for (int row = 0; row < DATA_TYPE_COUNT; row++)
            if (data_types[row].data & committed->copy_data)
                wl_data_source_offer(source, data_types[row].mime_type);
    }
    wl_data_device_set_selection(clipboard->data_device, source, library->input_serial);
    if (clipboard->source)
        wl_data_source_destroy(clipboard->source);
    clipboard->source = source;
    clipboard->copy_length = committed->copy_length < COPY_CAPACITY ? committed->copy_length : COPY_CAPACITY;
}

static void offer_type(void *data, struct wl_data_offer *, const char *mime_type) {
    Clipboard *clipboard = &((Library *)data)->clipboard;
    for (int row = 0; row < DATA_TYPE_COUNT; row++)
        if (strcmp(mime_type, data_types[row].mime_type) == 0)
            clipboard->newest_types |= 1u << row;
}

static void offer_source_actions(void *, struct wl_data_offer *, uint32_t) {}
static void offer_action(void *, struct wl_data_offer *, uint32_t) {}

static const struct wl_data_offer_listener offer_listener = {
    .offer = offer_type,
    .source_actions = offer_source_actions,
    .action = offer_action,
};

// An offer's types arrive right after it is announced, before the enter or selection that uses it.
static void device_data_offer(void *data, struct wl_data_device *, struct wl_data_offer *offer) {
    Library *library = data;
    library->clipboard.newest_types = 0;
    wl_data_offer_add_listener(offer, &offer_listener, library);
}

static void device_selection(void *data, struct wl_data_device *, struct wl_data_offer *offer) {
    Library *library = data;
    Clipboard *clipboard = &library->clipboard;
    if (clipboard->selection)
        wl_data_offer_destroy(clipboard->selection);
    clipboard->selection = offer;
    clipboard->selection_types = offer ? clipboard->newest_types : 0;
    library->incoming->paste_offered = data_bits(clipboard->selection_types);
}

static void drag_move(Library *library, wl_fixed_t x, wl_fixed_t y) {
    Clipboard *clipboard = &library->clipboard;
    clipboard->drag_column = fixed_to_eighths(x, library->font.cell_width);
    clipboard->drag_row = fixed_to_eighths(y, library->font.cell_height);
}

static void drag_discard(Clipboard *clipboard) {
    if (clipboard->drag)
        wl_data_offer_destroy(clipboard->drag);
    clipboard->drag = nullptr;
}

// A drag of nothing we know is refused here, so it never drops.
static void device_enter(void *data, struct wl_data_device *, uint32_t serial, struct wl_surface *,
                         wl_fixed_t x, wl_fixed_t y, struct wl_data_offer *offer) {
    Library *library = data;
    Clipboard *clipboard = &library->clipboard;
    drag_discard(clipboard);
    if (!offer)
        return;
    clipboard->drag = offer;
    clipboard->drag_types = clipboard->newest_types;
    int row = data_type_find(clipboard->drag_types, DATA_ANY);
    wl_data_offer_accept(offer, serial, row < 0 ? nullptr : data_types[row].mime_type);
    if (wl_data_offer_get_version(offer) >= 3)
        wl_data_offer_set_actions(offer, WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY,
                                  WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);
    drag_move(library, x, y);
}

static void device_leave(void *data, struct wl_data_device *) {
    drag_discard(&((Library *)data)->clipboard);
}

static void device_motion(void *data, struct wl_data_device *, uint32_t, wl_fixed_t x, wl_fixed_t y) {
    drag_move(data, x, y);
}

// The bytes arrive after the drag ends, so the receive keeps the offer and where it dropped.
static void device_drop(void *data, struct wl_data_device *) {
    Library *library = data;
    Clipboard *clipboard = &library->clipboard;
    if (!clipboard->drag)
        return;
    int row = data_type_find(clipboard->drag_types, DATA_ANY);
    Receive request = {
        .source = PASTE_DROP,
        .data = row < 0 ? 0 : data_types[row].data,
        .dropped = clipboard->drag,
        .drop_column = clipboard->drag_column,
        .drop_row = clipboard->drag_row,
        .drop_node = node_at(library->placements, clipboard->drag_column, clipboard->drag_row),
    };
    clipboard->drag = nullptr;
    receive_start(library, request, request.dropped, row);
}

static const struct wl_data_device_listener device_listener = {
    .data_offer = device_data_offer,
    .enter = device_enter,
    .leave = device_leave,
    .motion = device_motion,
    .drop = device_drop,
    .selection = device_selection,
};

void clipboard_attach(Library *library) {
    Clipboard *clipboard = &library->clipboard;
    if (!library->globals.data_device_manager)
        return;
    void *received = mmap(nullptr, PASTE_CAPACITY + 1, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (received == MAP_FAILED)
        return;
    clipboard->received = received;
    clipboard->data_device = wl_data_device_manager_get_data_device(library->globals.data_device_manager,
                                                                    library->globals.seat);
    wl_data_device_add_listener(clipboard->data_device, &device_listener, library);
}

void clipboard_detach(Library *library) {
    Clipboard *clipboard = &library->clipboard;
    if (!clipboard->data_device)
        return;
    if (clipboard->receive.active)
        receive_end(library, false);
    while (clipboard->send_count)
        send_close(clipboard, 0);
    drag_discard(clipboard);
    if (clipboard->selection)
        wl_data_offer_destroy(clipboard->selection);
    if (clipboard->source)
        wl_data_source_destroy(clipboard->source);
    wl_data_device_release(clipboard->data_device);
    munmap(clipboard->received, PASTE_CAPACITY + 1);
    *clipboard = (Clipboard){};
}

// Runs on the library thread after each wake. Without a data device, a paste is answered empty.
void clipboard_serve(Library *library) {
    Clipboard *clipboard = &library->clipboard;
    const otk_screen *committed = library->committed;
    if (committed->paste_request != clipboard->paste_request) {
        clipboard->paste_request = committed->paste_request;
        paste_start(library);
    }
    if (committed->copy_sequence != clipboard->copy_sequence && clipboard->data_device) {
        clipboard->copy_sequence = committed->copy_sequence;
        copy_offer(library);
    }
}

size_t clipboard_poll(const Library *library, struct pollfd *waiting) {
    const Clipboard *clipboard = &library->clipboard;
    size_t count = 0;
    if (clipboard->receive.active)
        waiting[count++] = (struct pollfd){.fd = clipboard->receive.pipe, .events = POLLIN};
    for (uint32_t index = 0; index < clipboard->send_count; index++)
        waiting[count++] = (struct pollfd){.fd = clipboard->sends[index].pipe, .events = POLLOUT};
    return count;
}

// Pipes are non-blocking, so every pipe in flight is tried, ready or not.
void clipboard_transfer(Library *library) {
    if (library->clipboard.receive.active)
        receive_read(library);
    sends_write(library);
}
