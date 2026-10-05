#include "output.h"

#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

extern volatile sig_atomic_t stop_requested;

static struct timespec started_at;

void start_clock(void) { clock_gettime(CLOCK_MONOTONIC, &started_at); }

void print_timestamp(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    long seconds = now.tv_sec - started_at.tv_sec;
    if (now.tv_nsec < started_at.tv_nsec) {
        seconds--;
    }

    dprintf(STDOUT_FILENO, "[%02ld:%02ld:%02ld] ", seconds / 3600,
            (seconds / 60) % 60, seconds % 60);
}

void print_event(const char* format, ...) {
    va_list args;
    va_start(args, format);
    print_timestamp();
    vdprintf(STDOUT_FILENO, format, args);
    va_end(args);

    if (!stop_requested) {
        struct timespec delay = {.tv_sec = 0, .tv_nsec = 300000000};
        nanosleep(&delay, NULL);
    }
}