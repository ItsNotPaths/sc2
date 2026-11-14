// A program that breaks the contract on purpose. tests/run.sh runs each mode under sc2host, which
// must keep running and exit with the program's status.
//   garbage  writes random bytes over everything it owns, every tick
//   stuck    leaves commit_sequence odd, as if it died halfway through a commit
//   shrink   tries to shrink the shared memory before mapping it
#define _POSIX_C_SOURCE 200809L
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "sc2.h"

enum { RUN_TICKS = 2 * 240, GARBAGE_CELLS = 65536 };

static uint32_t random_state = 2463534242u;

static uint32_t random_next(void) {  // xorshift32
    random_state ^= random_state << 13;
    random_state ^= random_state >> 17;
    random_state ^= random_state << 5;
    return random_state;
}

int main(int argument_count, char **arguments) {
    const char *mode = argument_count > 1 ? arguments[1] : "";
    if (strcmp(mode, "shrink") == 0) {
        const char *shared_fd = getenv("SC2_SHARED_FD");
        if (shared_fd && ftruncate(atoi(shared_fd), 0) == 0)
            fprintf(stderr, "hostile: shrank the shared memory\n");
    }
    sc2_screen *screen = sc2_open();
    if (!screen)
        return 1;
    sc2_shared *shared = (sc2_shared *)((char *)screen - offsetof(sc2_shared, screen));
    screen->idle_tick_rate = 240;  // so every mode reaches its last tick without input
    for (int tick = 0; sc2_tick(screen) && tick < RUN_TICKS; tick++) {
        if (strcmp(mode, "garbage") == 0) {
            uint8_t *owned = (uint8_t *)screen + offsetof(sc2_screen, nodes);
            for (size_t index = 0; index < sizeof(sc2_screen) - offsetof(sc2_screen, nodes); index++)
                owned[index] = (uint8_t)random_next();
            for (int index = 0; index < GARBAGE_CELLS; index++)
                screen->cells[index] = random_next();
            screen->tick_rate = 240;       // so the test ends on time
            screen->idle_tick_rate = 240;
            screen->copy_sequence = 0;     // so the test leaves the user's clipboard alone
            screen->paste_request = 0;
        }
        if (strcmp(mode, "stuck") == 0 && tick == 10)
            shared->header.commit_sequence |= 1;  // each commit adds 2, so it stays odd
    }
    sc2_close(screen);
    return 0;
}
