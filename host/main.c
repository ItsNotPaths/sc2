// sc2host PROGRAM [ARGUMENT...]: runs one sc*2 program and draws it in a Wayland window.
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/timerfd.h>
#include <sys/wait.h>
#include <unistd.h>

#include "host.h"

enum {
    COMMIT_READ_PATIENCE = 100000000,  // nanoseconds a program may keep commit_sequence odd
    CLOSE_PATIENCE       = 2000,       // milliseconds a program may take to exit once closed
};

static Host host;

bool failed(const char *reason) {
    fprintf(stderr, "sc2host: %s\n", reason);
    return false;
}

// Between input_begin and input_end the host writes incoming. The program only reads whole
// input, and is woken only when it changed.
void input_begin(Host *host) {
    memcpy(host->input_before, (const char *)host->incoming + INPUT_START, INPUT_SIZE);
    sequence_write_begin(&host->shared->header.input_sequence);
}

void input_end(Host *host) {
    sequence_write_end(&host->shared->header.input_sequence);
    if (memcmp(host->input_before, (const char *)host->incoming + INPUT_START, INPUT_SIZE) == 0)
        return;
    uint64_t one = 1;
    write(host->input_event, &one, sizeof one);
}

CommitRead commit_read_begin(const Host *host) {
    return (CommitRead){
        .begun = sequence_read_begin(&host->shared->header.commit_sequence),
        .deadline = now() + COMMIT_READ_PATIENCE,
    };
}

// After a try at reading committed: false once a try saw one whole commit, or once the program
// has kept commit_sequence odd past the patience, so it cannot hang the host; true to try again.
bool commit_read_retry(const Host *host, CommitRead *read) {
    const uint32_t *sequence = &host->shared->header.commit_sequence;
    read->whole = sequence_read_valid(sequence, read->begun);
    if (read->whole || now() > read->deadline)
        return false;
    sched_yield();
    read->begun = sequence_read_begin(sequence);
    return true;
}

static bool commit_take(Host *host, Commit *commit) {
    const sc2_screen *committed = host->committed;
    CommitRead read = commit_read_begin(host);
    do {
        memcpy(commit->title, committed->title, sizeof commit->title);
        memcpy(commit->app_id, committed->app_id, sizeof commit->app_id);
        commit->paste_request = committed->paste_request;
        commit->paste_request_data = committed->paste_request_data;
        commit->copy_sequence = committed->copy_sequence;
        commit->font_size = committed->font_size;
    } while (commit_read_retry(host, &read));
    commit->title[sizeof commit->title - 1] = 0;
    commit->app_id[sizeof commit->app_id - 1] = 0;
    return read.whole;
}

// Sealed, so the program cannot shrink the memory under the host's reads.
static bool shared_create(Host *host) {
    host->memory_file = memfd_create("sc2-shared", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (host->memory_file < 0 || ftruncate(host->memory_file, sizeof(sc2_shared)) < 0
        || fcntl(host->memory_file, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL) < 0)
        return failed("cannot make the shared memory");
    void *address = mmap(nullptr, sizeof(sc2_shared), PROT_READ | PROT_WRITE, MAP_SHARED, host->memory_file, 0);
    if (address == MAP_FAILED)
        return failed("cannot map the shared memory");
    host->shared = address;
    host->shared->header = (sc2_header){.magic = SC2_MAGIC, .version = SC2_VERSION};
    host->screen = &host->shared->screen;
    host->incoming = &host->shared->incoming;
    host->committed = &host->shared->committed;
    host->incoming->mouse_node = SC2_MOUSE_OUTSIDE;
    return true;
}

static bool events_create(Host *host) {
    host->commit_event = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    host->input_event = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    host->repeat_timer = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
    if (host->commit_event < 0 || host->input_event < 0 || host->repeat_timer < 0)
        return failed("cannot make the events");
    return true;
}

// Runs before the GPU starts its threads: between fork and exec only async-signal-safe calls.
static bool program_start(Host *host, char **command) {
    int inherited[] = {host->memory_file, host->commit_event, host->input_event};
    const char *names[] = {"SC2_SHARED_FD", "SC2_COMMIT_FD", "SC2_INPUT_FD"};
    for (int index = 0; index < 3; index++) {
        char number[16];
        snprintf(number, sizeof number, "%d", inherited[index]);
        setenv(names[index], number, 1);
    }
    pid_t program = fork();
    if (program < 0)
        return failed("cannot start the program");
    if (program == 0) {
        prctl(PR_SET_PDEATHSIG, SIGTERM);  // the program ends with its host
        for (int index = 0; index < 3; index++)
            fcntl(inherited[index], F_SETFD, 0);
        execvp(command[0], command);
        _exit(127);
    }
    host->program_id = program;
    host->program_exit = (int)syscall(SYS_pidfd_open, program, 0);
    if (host->program_exit < 0)
        return failed("cannot watch the program");
    return true;
}

// Vulkan's WSI reads the display too, so events may already sit in the queue: dispatch them
// before polling, and read through prepare/read so neither reader steals the other's events.
// Wayland callbacks run inside an input section, so they write incoming freely.
// Runs until the program exits or is closed; false when the compositor was lost.
static bool host_run(Host *host) {
    struct wl_display *display = host->display;
    uint64_t count;
    for (;;) {
        if (draw_due(host))
            draw_frame(host);

        input_begin(host);
        while (wl_display_prepare_read(display) != 0)
            wl_display_dispatch_pending(display);
        input_end(host);
        wl_display_flush(display);

        struct pollfd waiting[4 + CLIPBOARD_PIPE_CAPACITY] = {
            {.fd = wl_display_get_fd(display), .events = POLLIN},
            {.fd = host->commit_event, .events = POLLIN},
            {.fd = host->repeat_timer, .events = POLLIN},
            {.fd = host->program_exit, .events = POLLIN},
        };
        poll(waiting, 4 + clipboard_poll(host, &waiting[4]), -1);
        bool display_lost = false;
        if (waiting[0].revents)
            display_lost = wl_display_read_events(display) < 0;
        else
            wl_display_cancel_read(display);

        input_begin(host);
        display_lost |= wl_display_dispatch_pending(display) < 0;
        if (display_lost)
            host->incoming->closed = 1;
        Commit commit;
        if (waiting[1].revents && read(host->commit_event, &count, sizeof count) > 0 && commit_take(host, &commit)) {
            clipboard_serve(host, &commit);
            window_name(host, commit.title, commit.app_id);
            host->window.named = true;
            host->font_size_asked = commit.font_size;  // settings_update applies it at the next draw
        }
        if (waiting[2].revents && read(host->repeat_timer, &count, sizeof count) > 0)
            keyboard_repeat(host);
        clipboard_transfer(host);
        input_end(host);
        if (display_lost)
            return failed("lost the compositor");
        if (waiting[3].revents || host->incoming->closed)
            return true;
    }
}

static void host_close(Host *host) {
    graphics_wait(&host->graphics);
    draw_close(host);
    atlas_close(host);
    graphics_close(&host->graphics);
    font_close(&host->font);
    window_close(host);
    wayland_disconnect(host);
    if (host->shared)
        munmap(host->shared, sizeof(sc2_shared));
}

// The program learns of a close through closed. One that has not exited after the patience is killed,
// so a program cannot keep its window from closing.
static int program_wait(Host *host) {
    int status = 0;
    if (host->program_id <= 0)
        return 1;
    input_begin(host);
    host->incoming->closed = 1;
    input_end(host);
    struct pollfd exited = {.fd = host->program_exit, .events = POLLIN};
    if (poll(&exited, 1, CLOSE_PATIENCE) <= 0)
        kill(host->program_id, SIGKILL);
    waitpid(host->program_id, &status, 0);
    if (WIFEXITED(status) && WEXITSTATUS(status) == 127)
        failed("cannot run the program");
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

int main(int argument_count, char **arguments) {
    if (argument_count < 2) {
        fprintf(stderr, "usage: sc2host PROGRAM [ARGUMENT...]\n");
        return 2;
    }
    signal(SIGPIPE, SIG_IGN);  // a closed clipboard pipe fails with EPIPE instead
    const char *slash = strrchr(arguments[1], '/');
    host.window.scale = 120;
    bool started = shared_create(&host);
    if (started)
        settings_update(&host);
    started = started
        && events_create(&host)
        && wayland_connect(&host)
        && font_load(&host.font, &host.settings, host.window.scale)
        && window_open(&host, slash ? slash + 1 : arguments[1])
        && program_start(&host, arguments + 1)
        && graphics_open(&host)
        && atlas_open(&host)
        && draw_open(&host);
    bool kept_display = started && host_run(&host);
    int status = program_wait(&host);
    host_close(&host);
    return kept_display ? status : 1;
}
