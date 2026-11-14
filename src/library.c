#include <signal.h>
#include <stdio.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#include "library.h"

// Vulkan's WSI reads the display too, so events may already sit in the queue: dispatch them
// before polling, and read through prepare/read so neither reader steals the other's events.
// Wayland callbacks run inside the lock, so they write incoming freely. Drawing runs outside it.
static void *library_run(void *argument) {
    Library *library = argument;
    struct wl_display *display = library->display;
    uint64_t count;
    while (!atomic_load(&library->stopping)) {
        if (draw_due(library))
            draw_frame(library);

        pthread_mutex_lock(&library->lock);
        while (wl_display_prepare_read(display) != 0)
            wl_display_dispatch_pending(display);
        pthread_mutex_unlock(&library->lock);
        wl_display_flush(display);

        struct pollfd waiting[3 + CLIPBOARD_PIPE_CAPACITY] = {
            {.fd = wl_display_get_fd(display), .events = POLLIN},
            {.fd = library->wake_event, .events = POLLIN},
            {.fd = library->repeat_timer, .events = POLLIN},
        };
        poll(waiting, 3 + clipboard_poll(library, &waiting[3]), -1);
        bool display_lost = false;
        if (waiting[0].revents)
            display_lost = wl_display_read_events(display) < 0;
        else
            wl_display_cancel_read(display);

        pthread_mutex_lock(&library->lock);
        if (display_lost || wl_display_dispatch_pending(display) < 0)
            library->incoming->closed = 1;
        if (waiting[1].revents && read(library->wake_event, &count, sizeof count) > 0)
            clipboard_serve(library);
        if (waiting[2].revents && read(library->repeat_timer, &count, sizeof count) > 0)
            keyboard_repeat(library);
        clipboard_transfer(library);
        bool closed = library->incoming->closed;
        pthread_cond_signal(&library->input_arrived);
        pthread_mutex_unlock(&library->lock);
        if (closed)
            break;
    }
    return nullptr;
}

bool library_start(Library *library) {
    library->wake_event = eventfd(0, EFD_CLOEXEC);
    library->repeat_timer = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
    if (library->wake_event < 0 || library->repeat_timer < 0)
        return failed("cannot make the wake event or the repeat timer");
    pthread_mutex_init(&library->lock, nullptr);
    pthread_cond_init(&library->input_arrived, nullptr);
    // Signals go to the program's threads. A write to a closed clipboard pipe then fails with EPIPE.
    sigset_t every_signal, program_signals;
    sigfillset(&every_signal);
    pthread_sigmask(SIG_SETMASK, &every_signal, &program_signals);
    int started = pthread_create(&library->thread, nullptr, library_run, library);
    pthread_sigmask(SIG_SETMASK, &program_signals, nullptr);
    if (started != 0)
        return failed("cannot start the library thread");
    atomic_store(&library->running, true);
    return true;
}

void library_stop(Library *library) {
    if (!atomic_load(&library->running))
        return;
    atomic_store(&library->stopping, true);
    library_wake(library);
    pthread_join(library->thread, nullptr);
    close(library->wake_event);
    close(library->repeat_timer);
    pthread_cond_destroy(&library->input_arrived);
    pthread_mutex_destroy(&library->lock);
}

void library_wake(Library *library) {
    uint64_t one = 1;
    write(library->wake_event, &one, sizeof one);
}

bool failed(const char *reason) {
    fprintf(stderr, "otk: %s\n", reason);
    return false;
}
