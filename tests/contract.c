// A minimal host using only the contract, running itself as the program. Protects what any host
// relies on: whole commits, input waking an idle program, closed ending it, versions checked.
#include <poll.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include "contract.h"

enum { COLUMNS = 8, WAIT_MILLISECONDS = 2000 };

typedef struct {
    otk_shared *shared;
    int         commit_event, input_event;
    pid_t       program_id;
} Hosted;

static int failures;

static void check(bool passed, const char *requirement) {
    printf("%s %s\n", passed ? "ok  " : "FAIL", requirement);
    failures += !passed;
}

// The program: shows what was typed, or "ready" before anything is.
static int program_run(void) {
    otk_screen *screen = otk_open();
    if (!screen)
        return 1;
    uint32_t typed = 0;
    while (otk_tick(screen)) {
        typed = screen->text_write_index;
        screen->nodes[0] = (otk_node){.kind = OTK_NODE_CELLS, .columns = COLUMNS, .rows = 1,
                                      .content_width = COLUMNS, .content_height = 1};
        const char *ready = "ready";
        for (int column = 0; column < COLUMNS; column++)
            screen->cells[column] = typed ? (column < (int)typed ? screen->text[column] : ' ')
                                          : (column < 5 ? (uint32_t)ready[column] : ' ');
    }
    otk_close(screen);
    return 0;
}

static Hosted program_start(const char *self, uint32_t version) {
    Hosted hosted = {};
    int memory_file = memfd_create("contract-test", 0);
    ftruncate(memory_file, sizeof(otk_shared));
    hosted.shared = mmap(nullptr, sizeof(otk_shared), PROT_READ | PROT_WRITE, MAP_SHARED, memory_file, 0);
    hosted.shared->header = (otk_header){.magic = OTK_MAGIC, .version = version};
    hosted.shared->incoming.columns = COLUMNS;
    hosted.shared->incoming.rows = 1;
    hosted.commit_event = eventfd(0, 0);
    hosted.input_event = eventfd(0, 0);
    int inherited[] = {memory_file, hosted.commit_event, hosted.input_event};
    const char *names[] = {"OTK_SHARED_FD", "OTK_COMMIT_FD", "OTK_INPUT_FD"};
    for (int index = 0; index < 3; index++) {
        char number[16];
        snprintf(number, sizeof number, "%d", inherited[index]);
        setenv(names[index], number, 1);
    }
    hosted.program_id = fork();
    if (hosted.program_id == 0) {
        execl(self, self, "program", (char *)nullptr);
        _exit(127);
    }
    close(memory_file);
    return hosted;
}

static bool commit_arrives(const Hosted *hosted) {
    struct pollfd waiting = {.fd = hosted->commit_event, .events = POLLIN};
    uint64_t count;
    return poll(&waiting, 1, WAIT_MILLISECONDS) > 0 && read(hosted->commit_event, &count, sizeof count) > 0;
}

// The committed cells as text, read from one whole commit.
static void committed_text(const Hosted *hosted, char *text) {
    const uint32_t *sequence = &hosted->shared->header.commit_sequence;
    for (;;) {
        uint32_t begun = sequence_read_begin(sequence);
        for (int column = 0; column < COLUMNS; column++)
            text[column] = (char)hosted->shared->committed.cells[column];
        text[COLUMNS] = 0;
        if (sequence_read_valid(sequence, begun))
            return;
        sched_yield();
    }
}

// True once a commit shows expected; false when commits stop coming first.
static bool commit_shows(const Hosted *hosted, const char *expected) {
    char text[COLUMNS + 1];
    while (commit_arrives(hosted)) {
        committed_text(hosted, text);
        if (strcmp(text, expected) == 0)
            return true;
    }
    return false;
}

static void input_write(Hosted *hosted, const char *typed, bool closed) {
    otk_screen *incoming = &hosted->shared->incoming;
    sequence_write_begin(&hosted->shared->header.input_sequence);
    for (const char *character = typed; *character; character++)
        incoming->text[incoming->text_write_index++ % OTK_TEXT_CAPACITY] = (uint8_t)*character;
    incoming->closed = closed;
    sequence_write_end(&hosted->shared->header.input_sequence);
    uint64_t one = 1;
    write(hosted->input_event, &one, sizeof one);
}

static int exit_status(pid_t program_id) {
    int status;
    for (int waited = 0; waited < WAIT_MILLISECONDS; waited += 10) {
        if (waitpid(program_id, &status, WNOHANG) == program_id)
            return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        usleep(10000);
    }
    kill(program_id, SIGKILL);
    waitpid(program_id, &status, 0);
    return -1;
}

int main(int argument_count, char **arguments) {
    if (argument_count > 1 && strcmp(arguments[1], "program") == 0)
        return program_run();

    unsetenv("OTK_SHARED_FD");
    check(otk_open() == nullptr, "a program without a host fails to open");

    Hosted hosted = program_start(arguments[0], OTK_VERSION);
    check(commit_shows(&hosted, "ready   "), "the program's drawing arrives in a whole commit");

    input_write(&hosted, "hi", false);
    check(commit_shows(&hosted, "hi      "), "typed text wakes an idle program and reaches it");

    input_write(&hosted, "", true);
    check(exit_status(hosted.program_id) == 0, "closed ends the program");

    Hosted wrong_version = program_start(arguments[0], OTK_VERSION + 1);
    check(exit_status(wrong_version.program_id) == 1, "a program refuses another contract version");

    return failures ? 1 : 0;
}
