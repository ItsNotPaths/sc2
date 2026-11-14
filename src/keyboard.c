#include <sys/mman.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include "library.h"

enum { KEY_CAPACITY = 512, XKB_KEYCODE_OFFSET = 8 };  // xkb keycodes are evdev scancodes + 8

// Scancode 0 stops the repeat.
static void repeat_arm(Library *library, uint32_t scancode) {
    Keyboard *keyboard = &library->keyboard;
    keyboard->repeat_scancode = scancode;
    struct itimerspec timing = {};
    if (scancode && keyboard->repeat_rate > 0) {
        uint64_t interval = 1000000000 / keyboard->repeat_rate;
        uint64_t delay = keyboard->repeat_delay > 0 ? keyboard->repeat_delay * 1000000ull : interval;
        timing = (struct itimerspec){.it_value = from_nanoseconds(delay), .it_interval = from_nanoseconds(interval)};
    }
    timerfd_settime(library->repeat_timer, 0, &timing, nullptr);
}

static void keyboard_keymap(void *data, struct wl_keyboard *, uint32_t format, int32_t file, uint32_t size) {
    Keyboard *keyboard = &((Library *)data)->keyboard;
    char *text = format == WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 ? mmap(nullptr, size, PROT_READ, MAP_PRIVATE, file, 0) : MAP_FAILED;
    close(file);
    if (text == MAP_FAILED)
        return;
    struct xkb_keymap *keymap = xkb_keymap_new_from_string(keyboard->context, text, XKB_KEYMAP_FORMAT_TEXT_V1,
                                                           XKB_KEYMAP_COMPILE_NO_FLAGS);
    munmap(text, size);
    if (!keymap)
        return;
    xkb_state_unref(keyboard->state);
    xkb_keymap_unref(keyboard->keymap);
    keyboard->keymap = keymap;
    keyboard->state = xkb_state_new(keymap);
}

// Keys held on enter are ignored: they were pressed for another window. Its serial lets a copy happen before any key.
static void keyboard_enter(void *data, struct wl_keyboard *, uint32_t serial, struct wl_surface *, struct wl_array *) {
    ((Library *)data)->input_serial = serial;
}

// Releases every held key, since their releases go to the window that has focus now.
static void keyboard_leave(void *data, struct wl_keyboard *, uint32_t, struct wl_surface *) {
    Library *library = data;
    for (int scancode = 0; scancode < KEY_CAPACITY; scancode++)
        transition_set(&library->incoming->key_transitions[scancode], false);
    repeat_arm(library, 0);
}

static void keyboard_key(void *data, struct wl_keyboard *, uint32_t serial, uint32_t, uint32_t scancode, uint32_t state) {
    Library *library = data;
    Keyboard *keyboard = &library->keyboard;
    bool pressed = state == WL_KEYBOARD_KEY_STATE_PRESSED;
    library->input_serial = serial;
    if (scancode < KEY_CAPACITY)
        transition_set(&library->incoming->key_transitions[scancode], pressed);
    if (!keyboard->state)
        return;
    if (!pressed) {
        if (scancode == keyboard->repeat_scancode)
            repeat_arm(library, 0);
        return;
    }
    uint32_t codepoint = xkb_state_key_get_utf32(keyboard->state, scancode + XKB_KEYCODE_OFFSET);
    if (!codepoint)
        return;
    text_append(library, codepoint);
    if (xkb_keymap_key_repeats(keyboard->keymap, scancode + XKB_KEYCODE_OFFSET))
        repeat_arm(library, scancode);
}

static void keyboard_modifiers(void *data, struct wl_keyboard *, uint32_t, uint32_t depressed, uint32_t latched,
                               uint32_t locked, uint32_t group) {
    Keyboard *keyboard = &((Library *)data)->keyboard;
    if (keyboard->state)
        xkb_state_update_mask(keyboard->state, depressed, latched, locked, 0, 0, group);
}

static void keyboard_repeat_info(void *data, struct wl_keyboard *, int32_t rate, int32_t delay) {
    Keyboard *keyboard = &((Library *)data)->keyboard;
    keyboard->repeat_rate = rate;
    keyboard->repeat_delay = delay;
}

static const struct wl_keyboard_listener keyboard_listener = {
    .keymap = keyboard_keymap,
    .enter = keyboard_enter,
    .leave = keyboard_leave,
    .key = keyboard_key,
    .modifiers = keyboard_modifiers,
    .repeat_info = keyboard_repeat_info,
};

void keyboard_attach(Library *library) {
    Keyboard *keyboard = &library->keyboard;
    keyboard->context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    keyboard->keyboard = wl_seat_get_keyboard(library->globals.seat);
    wl_keyboard_add_listener(keyboard->keyboard, &keyboard_listener, library);
}

// Leaves the repeat timer armed: a late tick finds no repeat_scancode and types nothing.
void keyboard_detach(Library *library) {
    Keyboard *keyboard = &library->keyboard;
    if (keyboard->keyboard)
        wl_keyboard_release(keyboard->keyboard);
    xkb_state_unref(keyboard->state);
    xkb_keymap_unref(keyboard->keymap);
    xkb_context_unref(keyboard->context);
    *keyboard = (Keyboard){};
}

// The repeat timer fired: the held key types its text again, under the modifiers held now.
void keyboard_repeat(Library *library) {
    Keyboard *keyboard = &library->keyboard;
    if (!keyboard->repeat_scancode || !keyboard->state)
        return;
    uint32_t codepoint = xkb_state_key_get_utf32(keyboard->state, keyboard->repeat_scancode + XKB_KEYCODE_OFFSET);
    if (codepoint)
        text_append(library, codepoint);
}

// Caller holds the lock.
void text_append(Library *library, uint32_t codepoint) {
    otk_screen *incoming = library->incoming;
    incoming->text[incoming->text_write_index % OTK_TEXT_CAPACITY] = codepoint;
    incoming->text_write_index++;
}
