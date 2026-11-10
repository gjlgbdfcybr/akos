#include "dispatcher.h"
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

struct Person people[MAX_PEOPLE];
struct Device devices[3];
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
sem_t arrivals;
_Atomic bool interrupted;
bool shutdown_requested;
struct Airlock airlock = {.pressure = {.value = PRESSURE_NORMAL}};
int failure, grouped, emergency_priority;
unsigned seed;
int count, log_fd;
struct timespec started;

static void stop_handler(int sig) {
    (void)sig;
    interrupted = true;
}

int main(int argc, char **argv) {
    if (argc != 3 || !read_input(argv[1])) {
        dprintf(STDERR_FILENO, "Usage: %s input.txt output.log (check input data)\n", argv[0]);
        return 1;
    }
    log_fd = open(argv[2], O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (log_fd < 0) { perror("Create a new log file"); return 1; }
    clock_gettime(CLOCK_MONOTONIC, &started);
    srand(seed);
    if (!atomic_is_lock_free(&interrupted)) { close(log_fd); return 1; }
    struct sigaction action = {0};
    action.sa_handler = stop_handler;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) || sigaction(SIGTERM, &action, NULL)) {
        perror("sigaction"); close(log_fd); return 1;
    }
    int initialized_people = 0, initialized_devices = 0, running_people = 0, running_devices = 0;
    bool ok = false;
    if (sem_init(&arrivals, 0, 0)) { close(log_fd); return 1; }
    for (int i = 0; i < count; i++) {
        if (sem_init(&people[i].request, 0, 0)) goto cleanup;
        if (sem_init(&people[i].finished, 0, 0)) {
            sem_destroy(&people[i].request); goto cleanup;
        }
        initialized_people++;
    }
    for (int i = 0; i < 3; i++) {
        devices[i].kind = i;
        if (sem_init(&devices[i].request, 0, 0)) goto cleanup;
        if (sem_init(&devices[i].finished, 0, 0)) {
            sem_destroy(&devices[i].request); goto cleanup;
        }
        initialized_devices++;
        if (pthread_create(&devices[i].thread, NULL, device_worker, &devices[i])) goto cleanup;
        running_devices++;
    }
    for (int i = 0; i < count; i++) {
        if (pthread_create(&people[i].thread, NULL, person_worker, &people[i])) goto cleanup;
        running_people++;
    }
    ok = dispatch();
    if (!ok) {
        pthread_mutex_lock(&mutex);
        print_event("Stopping: closing doors with emergency drive");
        pthread_mutex_unlock(&mutex);
        bool inner_closed = operate(0, 0, true);
        bool outer_closed = operate(1, 0, true);
        if (!inner_closed || !outer_closed) ok = false;
    }
cleanup:
    pthread_mutex_lock(&mutex);
    shutdown_requested = true;
    pthread_mutex_unlock(&mutex);
    for (int i = 0; i < running_people; i++) sem_post(&people[i].request);
    for (int i = 0; i < running_devices; i++) sem_post(&devices[i].request);
    for (int i = 0; i < running_people; i++) pthread_join(people[i].thread, NULL);
    for (int i = 0; i < running_devices; i++) pthread_join(devices[i].thread, NULL);
    int completed = 0;
    for (int i = 0; i < count; i++) {
        if (people[i].request_state == REQUEST_ACTIVE) people[i].request_state = REQUEST_INTERRUPTED;
        if (people[i].request_state == REQUEST_DONE) completed++;
        print_event("Person %d: %s, %s", people[i].id,
              people[i].location == LOCATION_CHAMBER ? "chamber" : people[i].location == LOCATION_SPACE ? "space" : "station",
              people[i].request_state == REQUEST_DONE ? "done" : people[i].request_state == REQUEST_INTERRUPTED ? "interrupted" : people[i].request_state == REQUEST_WAITING ? "waiting" : "not arrived");
    }
    print_event("Safe: %d", check_safety());
    print_event("Result: %s; completed=%d/%d; inside=%d; air=%d; energy=%d; doors=%d,%d",
          ok ? "success" : "stopped", completed, count, airlock.people_inside, airlock.air, airlock.energy, airlock.inner.open, airlock.outer.open);
    for (int i = 0; i < initialized_people; i++) {
        sem_destroy(&people[i].request); sem_destroy(&people[i].finished);
    }
    for (int i = 0; i < initialized_devices; i++) {
        sem_destroy(&devices[i].request); sem_destroy(&devices[i].finished);
    }
    sem_destroy(&arrivals);
    pthread_mutex_destroy(&mutex);
    if (close(log_fd)) return 1;
    return ok && !interrupted ? 0 : 1;
}
