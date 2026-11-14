#include <stddef.h>
#include <string.h>
#include <time.h>

#include "library.h"

static uint64_t now(void) {
    struct timespec time;
    clock_gettime(CLOCK_MONOTONIC, &time);
    return (uint64_t)time.tv_sec * 1000000000 + time.tv_nsec;
}

static bool copy_if_changed(void *destination, const void *source, size_t size) {
    if (memcmp(destination, source, size) == 0)
        return false;
    memcpy(destination, source, size);
    return true;
}

// Copies what the program wrote into committed, and wakes the library thread if anything changed.
void tick_publish(Library *library) {
    const otk_screen *screen = library->screen;
    otk_screen *committed = library->committed;
    pthread_mutex_lock(&library->lock);
    if (screen->copy_sequence != committed->copy_sequence)
        memcpy(committed->copy, screen->copy, screen->copy_length < COPY_CAPACITY ? screen->copy_length : COPY_CAPACITY);
    bool changed = copy_if_changed(&committed->nodes, &screen->nodes, sizeof(otk_screen) - offsetof(otk_screen, nodes));
    for (int index = 0; index < NODE_CAPACITY; index++) {
        const Node *node = &committed->nodes[index];
        changed |= copy_if_changed(&committed->cells[node->storage_offset], &screen->cells[node->storage_offset],
                                   node_cell_count(node) * sizeof(Cell));
    }
    if (changed)
        atomic_fetch_add(&library->commit_count, 1);
    pthread_mutex_unlock(&library->lock);
    if (changed)
        library_wake(library);
}

void tick_wait(Library *library) {
    // (hole tick-idle :tags core :sev gap) an idle program still wakes every tick; should sleep on input_arrived while animating is 0.
    uint32_t tick_rate = library->screen->tick_rate ? library->screen->tick_rate : 240;
    uint64_t period = 1000000000 / tick_rate;
    uint64_t current = now();
    library->next_tick += period;
    if (library->next_tick < current)  // overrun: skip missed ticks, do not queue them
        library->next_tick = current + period;
    struct timespec deadline = {
        .tv_sec = library->next_tick / 1000000000,
        .tv_nsec = library->next_tick % 1000000000,
    };
    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, nullptr);
}

// Copies the newest input into screen, so input holds still for the whole tick.
// The ring and paste go first: the field copy overwrites the indexes they compare.
void tick_receive(Library *library) {
    otk_screen *screen = library->screen;
    const otk_screen *incoming = library->incoming;
    pthread_mutex_lock(&library->lock);
    uint32_t read = screen->text_write_index, written = incoming->text_write_index;
    if (written - read > TEXT_CAPACITY)
        read = written - TEXT_CAPACITY;
    for (; read != written; read++)
        screen->text[read % TEXT_CAPACITY] = incoming->text[read % TEXT_CAPACITY];
    if (incoming->paste_sequence != screen->paste_sequence)
        memcpy(screen->paste, incoming->paste, incoming->paste_length);
    memcpy(&screen->key_transitions, &incoming->key_transitions,
           offsetof(otk_screen, nodes) - offsetof(otk_screen, key_transitions));
    pthread_mutex_unlock(&library->lock);
}
