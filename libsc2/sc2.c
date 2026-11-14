#include <poll.h>
#include <sched.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "contract.h"

static_assert(offsetof(sc2_screen, pixels) == 0, "the host imports pixels in place, from a page");
static_assert(offsetof(sc2_shared, screen) % 4096 == 0, "the host imports pixels in place, from a page");
static_assert(sizeof(sc2_node) == 24, "bindings read the sc2_node layout from sc2.h");
static_assert(offsetof(sc2_screen, text) + sizeof(uint32_t[SC2_TEXT_CAPACITY]) == offsetof(sc2_screen, key_transitions),
              "receive copies the ring, then every field from key_transitions to nodes");

enum { DEFAULT_TICK_RATE = 240 };

typedef struct {
    sc2_shared *shared;
    int         commit_event, input_event;
    uint64_t    next_tick;                     // CLOCK_MONOTONIC nanoseconds
    bool        input_changed;                 // the last tick brought input
    bool        output_changed;                // the last tick's program changed something
} Program;

static Program program;  // one host connection per process

static bool failed(const char *reason) {
    fprintf(stderr, "sc2: %s\n", reason);
    return false;
}

static uint64_t now(void) {
    struct timespec time;
    clock_gettime(CLOCK_MONOTONIC, &time);
    return (uint64_t)time.tv_sec * 1000000000 + time.tv_nsec;
}

static int environment_fd(const char *name) {
    const char *text = getenv(name);
    return text ? atoi(text) : -1;
}

static bool region_differs(const sc2_screen *first, const sc2_screen *second, size_t start, size_t size) {
    return memcmp((const char *)first + start, (const char *)second + start, size) != 0;
}

static bool output_differs(const sc2_screen *screen, const sc2_screen *committed) {
    if (region_differs(screen, committed, OUTPUT_START, OUTPUT_SIZE))
        return true;
    for (int index = 0; index < NODE_COUNT; index++) {
        const sc2_node *node = &screen->nodes[index];
        if (memcmp(&committed->cells[node->storage_offset], &screen->cells[node->storage_offset],
                   node_cell_count(node) * sizeof(sc2_cell)) != 0)
            return true;
    }
    return false;
}

// Copies what the program wrote into committed as one whole commit, and tells the host.
// A tick that changed nothing is no commit.
static void tick_publish(void) {
    sc2_shared *shared = program.shared;
    const sc2_screen *screen = &shared->screen;
    sc2_screen *committed = &shared->committed;
    program.output_changed = output_differs(screen, committed);
    if (!program.output_changed)
        return;
    sequence_write_begin(&shared->header.commit_sequence);
    if (screen->copy_sequence != committed->copy_sequence)
        memcpy(committed->copy, screen->copy,
               screen->copy_length < SC2_COPY_CAPACITY ? screen->copy_length : SC2_COPY_CAPACITY);
    for (int index = 0; index < NODE_COUNT; index++) {
        const sc2_node *node = &screen->nodes[index];
        memcpy(&committed->cells[node->storage_offset], &screen->cells[node->storage_offset],
               node_cell_count(node) * sizeof(sc2_cell));
    }
    memcpy((char *)committed + OUTPUT_START, (const char *)screen + OUTPUT_START, OUTPUT_SIZE);
    sequence_write_end(&shared->header.commit_sequence);
    uint64_t one = 1;
    write(program.commit_event, &one, sizeof one);
}

// Input the program has not seen yet. Read without the sequence: a torn read only wakes early.
static bool input_waiting(void) {
    return region_differs(&program.shared->incoming, &program.shared->screen, INPUT_START, INPUT_SIZE);
}

// Sleeps until input arrives, or until the next idle tick when idle_tick_rate is set.
static void idle_wait(void) {
    uint16_t idle_tick_rate = program.shared->screen.idle_tick_rate;
    uint64_t deadline = now() + (idle_tick_rate ? 1000000000 / idle_tick_rate : 0);
    struct pollfd waiting = {.fd = program.input_event, .events = POLLIN};
    while (!input_waiting()) {
        int timeout = -1;
        if (idle_tick_rate) {
            uint64_t current = now();
            if (current >= deadline)
                return;
            timeout = (int)((deadline - current + 999999) / 1000000);
        }
        uint64_t count;
        if (poll(&waiting, 1, timeout) > 0)
            read(program.input_event, &count, sizeof count);
    }
}

// A tick that brought no input and changed nothing is idle: the next one waits for input.
static void tick_wait(void) {
    if (!program.input_changed && !program.output_changed) {
        idle_wait();
        program.next_tick = now();
        return;
    }
    uint16_t tick_rate = program.shared->screen.tick_rate;
    uint64_t period = 1000000000 / (tick_rate ? tick_rate : DEFAULT_TICK_RATE);
    uint64_t current = now();
    program.next_tick += period;
    if (program.next_tick < current)  // overrun: skip missed ticks, do not queue them
        program.next_tick = current + period;
    struct timespec deadline = {.tv_sec = program.next_tick / 1000000000, .tv_nsec = program.next_tick % 1000000000};
    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, nullptr);
}

// Copies the newest whole input into screen, so input holds still for the whole tick. A torn
// copy is read again; the program's own read positions are kept from before the first try.
static void tick_receive(void) {
    sc2_shared *shared = program.shared;
    sc2_screen *screen = &shared->screen;
    const sc2_screen *incoming = &shared->incoming;
    uint32_t seen_text = screen->text_write_index, seen_paste = screen->paste_sequence;
    program.input_changed = input_waiting();
    for (;;) {
        uint32_t begun = sequence_read_begin(&shared->header.input_sequence);
        uint32_t written = incoming->text_write_index, read = seen_text;
        if (written - read > SC2_TEXT_CAPACITY)
            read = written - SC2_TEXT_CAPACITY;
        for (; read != written; read++)
            screen->text[read % SC2_TEXT_CAPACITY] = incoming->text[read % SC2_TEXT_CAPACITY];
        if (incoming->paste_sequence != seen_paste)
            memcpy(screen->paste, incoming->paste,
                   incoming->paste_length < SC2_PASTE_CAPACITY ? incoming->paste_length : SC2_PASTE_CAPACITY);
        memcpy((char *)screen + INPUT_START, (const char *)incoming + INPUT_START, INPUT_SIZE);
        if (sequence_read_valid(&shared->header.input_sequence, begun))
            return;
        sched_yield();
    }
}

[[gnu::visibility("default")]]
sc2_screen *sc2_open(void) {
    if (program.shared)
        return nullptr;
    int shared_fd = environment_fd("SC2_SHARED_FD");
    program.commit_event = environment_fd("SC2_COMMIT_FD");
    program.input_event = environment_fd("SC2_INPUT_FD");
    if (shared_fd < 0 || program.commit_event < 0 || program.input_event < 0) {
        failed("no host; run under one, such as sc2host");
        return nullptr;
    }
    void *address = mmap(nullptr, sizeof(sc2_shared), PROT_READ | PROT_WRITE, MAP_SHARED, shared_fd, 0);
    close(shared_fd);
    if (address == MAP_FAILED) {
        failed("cannot map the host's shared memory");
        return nullptr;
    }
    program.shared = address;
    if (program.shared->header.magic != SC2_MAGIC || program.shared->header.version != SC2_VERSION) {
        failed("the host speaks another version of the contract");
        sc2_close(&program.shared->screen);
        return nullptr;
    }
    sc2_screen *screen = &program.shared->screen;
    screen->tick_rate = DEFAULT_TICK_RATE;
    program.next_tick = now();
    tick_receive();
    return screen;
}

[[gnu::visibility("default")]]
int sc2_tick(sc2_screen *screen) {
    tick_publish();
    tick_wait();
    tick_receive();
    return !screen->closed;
}

[[gnu::visibility("default")]]
void sc2_close(sc2_screen *screen) {
    if (!program.shared || screen != &program.shared->screen)
        return;
    munmap(program.shared, sizeof(sc2_shared));
    close(program.commit_event);
    close(program.input_event);
    program = (Program){};
}
