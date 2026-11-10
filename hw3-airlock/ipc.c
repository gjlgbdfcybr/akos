#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "shared.h"

struct Shared* shared;
sem_t* sems[SEM_COUNT];
int log_fd = -1;
volatile sig_atomic_t local_stop;
char session[40];
static int initialized;
static bool owns_memory;

static bool set_session(const char* name) {
    if (!*name || strlen(name) >= sizeof(session)) return false;
    for (const char* p = name; *p; p++)
        if (!isalnum((unsigned char)*p) && *p != '_') return false;
    strcpy(session, name);
    return true;
}

static void object_name(char* out, int index) {
    snprintf(out, 96, "/akos3_%lu_%s_%d", (unsigned long)getuid(), session,
             index);
}

static void handler(int sig) {
    (void)sig;
    local_stop = 1;
}

void install_signals(void) {
    struct sigaction action = {0};
    action.sa_handler = handler;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) || sigaction(SIGTERM, &action, NULL)) {
        perror("sigaction");
        exit(1);
    }
}

void poll_signal(void) {
    if (local_stop && shared) shared->stop = true;
}

bool ipc_create(const char* name, int named) {
    if (!set_session(name)) return false;
    char path[96];
    object_name(path, -1);
    int fd = shm_open(path, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0) return false;
    owns_memory = true;
    if (ftruncate(fd, sizeof(*shared))) {
        close(fd);
        ipc_close(true);
        return false;
    }
    shared =
        mmap(NULL, sizeof(*shared), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (shared == MAP_FAILED) {
        shared = NULL;
        ipc_close(true);
        return false;
    }
    memset(shared, 0, sizeof(*shared));
    atomic_init(&shared->magic, 0);
    atomic_init(&shared->stop, false);
    atomic_init(&shared->shutdown, false);
    if (!atomic_is_lock_free(&shared->stop) ||
        !atomic_is_lock_free(&shared->magic)) {
        ipc_close(true);
        return false;
    }
    shared->named = named;
    shared->owner = getpid();
    shared->chamber.pressure.value = PRESSURE_NORMAL;
    for (int i = 0; i < SEM_COUNT; i++) {
        if (named) {
            object_name(path, i);
            sems[i] = sem_open(path, O_CREAT | O_EXCL, 0600, i == LOCK);
            if (sems[i] == SEM_FAILED) {
                sems[i] = NULL;
                ipc_close(true);
                return false;
            }
        }
        else {
            sems[i] = &shared->semaphores[i];
            if (sem_init(sems[i], 1, i == LOCK)) {
                ipc_close(true);
                return false;
            }
        }
        initialized++;
    }
    return true;
}

bool ipc_attach(const char* name) {
    if (!set_session(name)) return false;
    char path[96];
    object_name(path, -1);
    int fd = shm_open(path, O_RDWR, 0600);
    if (fd < 0) return false;
    struct stat st;
    if (fstat(fd, &st) || st.st_size != sizeof(*shared)) {
        close(fd);
        return false;
    }
    shared =
        mmap(NULL, sizeof(*shared), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (shared == MAP_FAILED) {
        shared = NULL;
        return false;
    }
    if (atomic_load(&shared->magic) != MAGIC || !shared->named) {
        ipc_close(false);
        return false;
    }
    for (int i = 0; i < SEM_COUNT; i++) {
        object_name(path, i);
        sems[i] = sem_open(path, 0);
        if (sems[i] == SEM_FAILED) {
            sems[i] = NULL;
            ipc_close(false);
            return false;
        }
        initialized++;
    }
    log_fd = open(shared->log_path, O_WRONLY | O_APPEND);
    if (log_fd < 0) {
        ipc_close(false);
        return false;
    }
    return true;
}

void ipc_close(bool owner) {
    char path[96];
    for (int i = 0; i < initialized; i++) {
        if (shared->named) {
            sem_close(sems[i]);
            if (owner) {
                object_name(path, i);
                sem_unlink(path);
            }
        }
        else if (owner)
            sem_destroy(sems[i]);
    }
    initialized = 0;
    if (shared) {
        munmap(shared, sizeof(*shared));
        shared = NULL;
    }
    if (owner && owns_memory) {
        object_name(path, -1);
        shm_unlink(path);
        owns_memory = false;
    }
    if (log_fd >= 0) {
        close(log_fd);
        log_fd = -1;
    }
}

bool wait_event(int id, int timeout_ms) {
    int elapsed = 0;
    for (;;) {
        poll_signal();
        struct timespec until;
        clock_gettime(CLOCK_REALTIME, &until);
        until.tv_nsec += 100000000;
        if (until.tv_nsec >= 1000000000) {
            until.tv_nsec -= 1000000000;
            until.tv_sec++;
        }
        if (!sem_timedwait(sems[id], &until)) return true;
        if (errno == EINTR) continue;
        if (errno != ETIMEDOUT) {
            shared->stop = true;
            return false;
        }
        if (id != LOCK && shared->shutdown) return false;
        if (getpid() != shared->owner && kill(shared->owner, 0) == -1 &&
            errno == ESRCH)
            return false;
        elapsed += 100;
        if (timeout_ms >= 0 && elapsed >= timeout_ms) return false;
    }
}

void lock_state(void) {
    if (!wait_event(LOCK, 10000)) {
        dprintf(STDERR_FILENO, "Shared state lock unavailable\n");
        _exit(2);
    }
}

void unlock_state(void) { post_event(LOCK); }

void post_event(int id) {
    if (sem_post(sems[id])) {
        perror("sem_post");
        shared->stop = true;
    }
}

void delay_ms(int ms) {
    while (ms > 0 && !shared->stop && !shared->shutdown) {
        poll_signal();
        int part = ms > 20 ? 20 : ms;
        struct timespec t = {0, part * 1000000L};
        while (nanosleep(&t, &t) && errno == EINTR && !local_stop) {
        }
        ms -= part;
    }
}
