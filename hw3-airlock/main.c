#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "shared.h"

#ifndef MODE
#define MODE 0
#endif

int main(int argc, char** argv) {
    install_signals();
    if (MODE == 3) {
        if (argc != 3) {
            dprintf(2,
                    "Usage: %s session slot "
                    "(0=inner,1=outer,2=pressure,3+=person)\n",
                    argv[0]);
            return 1;
        }
        char* end;
        errno = 0;
        long slot = strtol(argv[2], &end, 10);
        if (errno || !*argv[2] || *end || slot < 0 || slot >= MAX_WORKERS)
            return 1;
        if (!ipc_attach(argv[1])) {
            perror("Attach to ready session");
            return 1;
        }
        int result = run_worker((int)slot);
        ipc_close(false);
        return result;
    }
    if (argc != 4) {
        dprintf(2, "Usage: %s input.txt new.log session\n", argv[0]);
        return 1;
    }
    if (!ipc_create(argv[3], MODE != 0)) {
        perror("Create unique session");
        return 1;
    }
    if (!read_input(argv[1])) {
        dprintf(2, "Invalid input file\n");
        ipc_close(true);
        return 1;
    }
    log_fd = open(argv[2], O_WRONLY | O_CREAT | O_EXCL | O_APPEND, 0600);
    if (log_fd < 0) {
        perror("Create new log");
        ipc_close(true);
        return 1;
    }
    if (!realpath(argv[2], shared->log_path)) {
        perror("Log path");
        ipc_close(true);
        return 1;
    }
    clock_gettime(CLOCK_MONOTONIC, &shared->began);
    srand(shared->random_seed);
    atomic_store(&shared->magic, MAGIC);
    lock_state();
    const char* mode_name = "independent/named";
    if (MODE == 0) mode_name = "fork/unnamed";
    if (MODE == 1) mode_name = "fork/named";
    print_event("Session %s ready: %s, workers=%d", session, mode_name,
                shared->total + 3);
    unlock_state();
    int result = run_controller(MODE != 2);
    ipc_close(true);
    return result;
}
