#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "shared.h"

void print_event(const char* format, ...) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long seconds = now.tv_sec - shared->began.tv_sec;
    if (now.tv_nsec < shared->began.tv_nsec) seconds--;
    char text[512];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    dprintf(STDOUT_FILENO, "[%02ld:%02ld:%02ld] %s\n", seconds / 3600,
            seconds / 60 % 60, seconds % 60, text);
    if (dprintf(log_fd, "[%02ld:%02ld:%02ld] %s\n", seconds / 3600,
                seconds / 60 % 60, seconds % 60, text) < 0)
        shared->stop = 1;
}
