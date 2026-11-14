#include <errno.h>
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
        memcpy(committed->copy, screen->copy, screen->copy_length < OTK_COPY_CAPACITY ? screen->copy_length : OTK_COPY_CAPACITY);
    bool changed = copy_if_changed(&committed->nodes, &screen->nodes, sizeof(otk_screen) - offsetof(otk_screen, nodes));
    for (int index = 0; index < NODE_CAPACITY; index++) {
        const otk_node *node = &committed->nodes[index];
        changed |= copy_if_changed(&committed->cells[node->storage_offset], &screen->cells[node->storage_offset],
                                   node_cell_count(node) * sizeof(otk_cell));
    }
    if (changed)
        atomic_fetch_add(&library->commit_count, 1);
    library->output_changed = changed;
    pthread_mutex_unlock(&library->lock);
    if (changed)
        library_wake(library);
}

// Input the program has not seen yet: every field from key_transitions to nodes, which covers the
// ring's write index and paste_sequence. Caller holds the lock.
static bool input_waiting(const Library *library) {
    size_t start = offsetof(otk_screen, key_transitions);
    size_t size = offsetof(otk_screen, nodes) - start;
    return memcmp((const char *)library->incoming + start, (const char *)library->screen + start, size) != 0;
}

// Sleeps until input arrives, or until the next idle tick when idle_tick_rate is set.
static void idle_wait(Library *library) {
    uint16_t idle_tick_rate = library->screen->idle_tick_rate;
    struct timespec deadline = from_nanoseconds(now() + (idle_tick_rate ? 1000000000 / idle_tick_rate : 0));
    pthread_mutex_lock(&library->lock);
    int waited = 0;
    while (!input_waiting(library) && waited != ETIMEDOUT)
        waited = idle_tick_rate ? pthread_cond_timedwait(&library->input_arrived, &library->lock, &deadline)
                                : pthread_cond_wait(&library->input_arrived, &library->lock);
    pthread_mutex_unlock(&library->lock);
}

// A tick that brought no input and changed nothing is idle: the next one waits for input.
void tick_wait(Library *library) {
    if (!library->input_changed && !library->output_changed) {
        idle_wait(library);
        library->next_tick = now();
        return;
    }
    uint32_t tick_rate = library->screen->tick_rate ? library->screen->tick_rate : 240;
    uint64_t period = 1000000000 / tick_rate;
    uint64_t current = now();
    library->next_tick += period;
    if (library->next_tick < current)  // overrun: skip missed ticks, do not queue them
        library->next_tick = current + period;
    struct timespec deadline = from_nanoseconds(library->next_tick);
    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, nullptr);
}

// Copies the newest input into screen, so input holds still for the whole tick.
// The ring and paste go first: the field copy overwrites the indexes they compare.
void tick_receive(Library *library) {
    otk_screen *screen = library->screen;
    const otk_screen *incoming = library->incoming;
    pthread_mutex_lock(&library->lock);
    library->input_changed = input_waiting(library);
    uint32_t read = screen->text_write_index, written = incoming->text_write_index;
    if (written - read > OTK_TEXT_CAPACITY)
        read = written - OTK_TEXT_CAPACITY;
    for (; read != written; read++)
        screen->text[read % OTK_TEXT_CAPACITY] = incoming->text[read % OTK_TEXT_CAPACITY];
    if (incoming->paste_sequence != screen->paste_sequence)
        memcpy(screen->paste, incoming->paste, incoming->paste_length);
    memcpy(&screen->key_transitions, &incoming->key_transitions,
           offsetof(otk_screen, nodes) - offsetof(otk_screen, key_transitions));
    pthread_mutex_unlock(&library->lock);
}
