#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

#include "shared.h"

int run_worker(int slot) {
    lock_state();
    if (shared->shutdown || slot < 0 || slot >= shared->total + 3 ||
        shared->workers[slot]) {
        unlock_state();
        return 1;
    }
    shared->workers[slot] = getpid();
    print_event("Worker %d connected, pid=%ld", slot, (long)getpid());
    unlock_state();
    post_event(READY);
    bool begin = false;
    while (!shared->shutdown && !shared->stop) {
        if (wait_event(START, 100)) {
            begin = true;
            break;
        }
    }
    if (begin && !shared->shutdown) {
        if (slot < 3)
            device_worker(slot);
        else
            person_worker(slot - 3);
    }
    post_event(EXITED);
    return 0;
}

static bool all_ready(int expected) {
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    int ready = 0;
    while (!shared->stop) {
        if (wait_event(READY, 100)) ready++;
        if (ready == expected) return true;
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long elapsed_ms = (now.tv_sec - start.tv_sec) * 1000;
        elapsed_ms += (now.tv_nsec - start.tv_nsec) / 1000000;
        if (elapsed_ms >= 60000) return false;
    }
    return false;
}

int run_controller(bool spawn) {
    pid_t children[MAX_WORKERS];
    int created = 0;
    bool ok = true;
    int expected = shared->total + 3;
    for (int i = 0; i < 3; i++) shared->equipment[i].kind = i;
    if (spawn) {
        for (int i = 0; i < expected; i++) {
            pid_t pid = fork();
            if (pid < 0) {
                perror("fork");
                ok = false;
                break;
            }
            if (!pid) {
                int result = run_worker(i);
                ipc_close(false);
                _exit(result);
            }
            children[created++] = pid;
        }
    }
    if (ok) ok = all_ready(expected);
    if (ok) {
        clock_gettime(CLOCK_MONOTONIC, &shared->began);
        for (int i = 0; i < expected; i++) post_event(START);
        ok = dispatch();
        if (!ok) {
            lock_state();
            print_event("Stopping: emergency door closure");
            unlock_state();
            bool inner_closed = operate(0, 0, true);
            bool outer_closed = operate(1, 0, true);
            if (!inner_closed || !outer_closed) {
                lock_state();
                print_event("ERROR: emergency closure failed");
                unlock_state();
            }
        }
    }
    else {
        lock_state();
        print_event("Startup stopped: workers missing or interruption");
        unlock_state();
    }
    lock_state();
    shared->shutdown = true;
    int connected = 0;
    for (int i = 0; i < expected; i++)
        if (shared->workers[i]) connected++;
    unlock_state();
    for (int i = 0; i < expected; i++) {
        post_event(START);
        post_event(REQUEST(i));
    }
    if (spawn) {
        for (int i = 0; i < created; i++) {
            int status = 0;
            pid_t result;
            do {
                result = waitpid(children[i], &status, 0);
            } while (result < 0 && errno == EINTR);
            if (result < 0 || !WIFEXITED(status) || WEXITSTATUS(status))
                ok = false;
        }
    }
    else {
        int finished = 0;
        for (int elapsed = 0; elapsed < 10000 && finished < connected;
             elapsed += 100) {
            if (wait_event(EXITED, 100)) finished++;
        }
        if (finished != connected) ok = false;
    }
    lock_state();
    int completed = 0;
    for (int i = 0; i < shared->total; i++) {
        struct Person* p = &shared->persons[i];
        if (p->request_state == REQUEST_ACTIVE)
            p->request_state = REQUEST_INTERRUPTED;
        if (p->request_state == REQUEST_DONE) completed++;
        const char* location = "station";
        if (p->location == LOCATION_CHAMBER) location = "chamber";
        if (p->location == LOCATION_SPACE) location = "space";
        const char* status = "not arrived";
        if (p->request_state == REQUEST_WAITING) status = "waiting";
        if (p->request_state == REQUEST_INTERRUPTED) status = "interrupted";
        if (p->request_state == REQUEST_DONE) status = "done";
        print_event("Person %d: %s, %s", p->id, location, status);
    }
    ok = ok && !shared->stop && check_safety() && completed == shared->total;
    print_event("Safe: %d", check_safety());
    print_event(
        "Result: %s; completed=%d/%d; inside=%d; air=%d; energy=%d; "
        "doors=%d,%d",
        ok ? "success" : "stopped", completed, shared->total,
        shared->chamber.people_inside, shared->chamber.air,
        shared->chamber.energy, shared->chamber.inner.open,
        shared->chamber.outer.open);
    unlock_state();
    return ok ? 0 : 1;
}
