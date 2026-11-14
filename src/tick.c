#include <time.h>

#include "library.h"

static uint64_t now(void) {
    struct timespec time;
    clock_gettime(CLOCK_MONOTONIC, &time);
    return (uint64_t)time.tv_sec * 1000000000 + time.tv_nsec;
}

// (hole tick-exchange :tags core :sev blocker) copies nothing out; nodes, cells, program-written fields and copy never reach committed.
void tick_publish(Library *library) {
    (void)library;
}

void tick_wait(Library *library) {
    // (hole tick-idle :tags core :sev gap :needs tick-exchange) an idle program still wakes every tick; should sleep on input_arrived while animating is 0.
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

// (hole tick-exchange) copies nothing in; the program never sees input, resize, close, paste bytes or the palette.
void tick_receive(Library *library) {
    (void)library;
}
