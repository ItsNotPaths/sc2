#include <poll.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#include "library.h"

// Wayland callbacks run inside the lock, so they write incoming freely.
static void *library_run(void *argument) {
    Library *library = argument;
    uint64_t count;
    while (!atomic_load(&library->stopping)) {
        wl_display_flush(library->display);
        struct pollfd waiting[] = {
            {.fd = wl_display_get_fd(library->display), .events = POLLIN},
            {.fd = library->wake_event, .events = POLLIN},
            {.fd = library->repeat_timer, .events = POLLIN},
        };
        poll(waiting, 3, -1);
        pthread_mutex_lock(&library->lock);
        if (waiting[0].revents && wl_display_dispatch(library->display) < 0)
            library->incoming->closed = 1;
        if (waiting[1].revents && read(library->wake_event, &count, sizeof count) > 0)
            clipboard_serve(library);
        if (waiting[2].revents && read(library->repeat_timer, &count, sizeof count) > 0)
            keyboard_repeat(library);
        pthread_cond_signal(&library->input_arrived);
        pthread_mutex_unlock(&library->lock);
        if (library->incoming->closed)
            break;
    }
    return nullptr;
}

bool library_start(Library *library) {
    library->wake_event = eventfd(0, EFD_CLOEXEC);
    library->repeat_timer = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
    if (library->wake_event < 0 || library->repeat_timer < 0)
        return false;
    pthread_mutex_init(&library->lock, nullptr);
    pthread_cond_init(&library->input_arrived, nullptr);
    if (pthread_create(&library->thread, nullptr, library_run, library) != 0)
        return false;
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
