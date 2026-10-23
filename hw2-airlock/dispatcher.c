#include "dispatcher.h"
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

bool check_safety(void) {
    int actual = 0;
    for (int i = 0; i < count; i++) if (people[i].location == LOCATION_CHAMBER) actual++;
    return actual == airlock.people_inside && is_airlock_safe(airlock);
}

static void wait_sem(sem_t *sem) {
    while (sem_wait(sem) == -1) {
        if (errno != EINTR) abort();
    }
}

static void delay(int ms) {
    while (ms > 0 && !interrupted) {
        int part = ms > 20 ? 20 : ms;
        struct timespec t = {0, part * 1000000L};
        while (nanosleep(&t, &t) == -1 && errno == EINTR && !interrupted) {}
        ms -= part;
    }
}

void *person_worker(void *arg) {
    struct Person *p = arg;
    for (int elapsed = 0; elapsed < p->arrival_ms; elapsed += 20) {
        pthread_mutex_lock(&mutex);
        bool stop = shutdown_requested || interrupted;
        pthread_mutex_unlock(&mutex);
        if (stop) return NULL;
        delay(p->arrival_ms - elapsed < 20 ? p->arrival_ms - elapsed : 20);
    }
    pthread_mutex_lock(&mutex);
    if (shutdown_requested || interrupted) {
        pthread_mutex_unlock(&mutex);
        return NULL;
    }
    p->request_state = REQUEST_WAITING;
    print_event("Person %d requests %s%s", p->id,
          p->direction == 0 ? "space" : "station",
          p->emergency ? " (emergency)" : "");
    pthread_mutex_unlock(&mutex);
    sem_post(&arrivals);
    for (;;) {
        wait_sem(&p->request);
        pthread_mutex_lock(&mutex);
        if (shutdown_requested) {
            pthread_mutex_unlock(&mutex);
            return NULL;
        }
        p->success = false;
        if (p->command == 1) {
            p->success = p->direction == DIRECTION_TO_SPACE ? enter_from_station(&airlock, p) : enter_from_space(&airlock, p);
            if (p->success) print_event("Person %d enters chamber", p->id);
        } else {
            p->success = p->direction == DIRECTION_TO_SPACE ? exit_to_space(&airlock, p) : exit_to_station(&airlock, p);
            if (p->success) p->request_state = REQUEST_DONE;
            if (p->success) print_event("Person %d exits to %s", p->id,
                  p->location == LOCATION_SPACE ? "space" : "station");
        }
        pthread_mutex_unlock(&mutex);
        sem_post(&p->finished);
    }
}

void *device_worker(void *arg) {
    struct Device *d = arg;
    for (;;) {
        wait_sem(&d->request);
        pthread_mutex_lock(&mutex);
        if (shutdown_requested) {
            pthread_mutex_unlock(&mutex);
            return NULL;
        }
        int target = d->target;
        if (d->kind == 2) airlock.pressure.operation.active = true;
        print_event("%s operation started", d->kind == 0 ? "Inner door" :
              d->kind == 1 ? "Outer door" : "Pressure system");
        pthread_mutex_unlock(&mutex);
        delay(d->kind == 2 ? airlock.pressure.duration : airlock.inner.duration);
        pthread_mutex_lock(&mutex);
        d->success = false;
        if (d->kind == 0)
            d->success = target ? open_inner_door(&airlock) : close_inner_door(&airlock);
        else if (d->kind == 1)
            d->success = target ? open_outer_door(&airlock) : close_outer_door(&airlock);
        else {
            d->success = change_pressure(&airlock, (enum Pressure)target);
            airlock.pressure.operation.active = false;
        }
        if (d->success) {
            if (d->kind < 2) print_event("%s door %s", d->kind == 0 ? "Inner" : "Outer", target ? "opened" : "closed");
            else print_event("Pressure changed to %s", target == PRESSURE_LOW ? "low" : "normal");
        }
        d->success = d->success && check_safety();
        pthread_mutex_unlock(&mutex);
        sem_post(&d->finished);
    }
}

bool operate(int kind, int target, bool emergency) {
    pthread_mutex_lock(&mutex);
    if ((kind == 0 ? airlock.inner.open : kind == 1 ? airlock.outer.open : (int)airlock.pressure.value) == target) {
        pthread_mutex_unlock(&mutex);
        return true;
    }
    if (!emergency && (interrupted || rand() % 100 < failure)) {
        if (!interrupted) print_event("Equipment failure; emergency shutdown");
        pthread_mutex_unlock(&mutex);
        return false;
    }
    devices[kind].target = target;
    pthread_mutex_unlock(&mutex);
    sem_post(&devices[kind].request);
    wait_sem(&devices[kind].finished);
    pthread_mutex_lock(&mutex);
    bool result = devices[kind].success;
    pthread_mutex_unlock(&mutex);
    return result && (emergency || !interrupted);
}

static bool move_person(int index, int command) {
    pthread_mutex_lock(&mutex);
    struct Person *p = &people[index];
    bool allowed = !interrupted;
    p->command = command;
    pthread_mutex_unlock(&mutex);
    if (!allowed) return false;
    sem_post(&p->request);
    wait_sem(&p->finished);
    pthread_mutex_lock(&mutex);
    bool success = p->success && check_safety();
    pthread_mutex_unlock(&mutex);
    return success && !interrupted;
}

static bool precedes(int a, int b) {
    return b < 0 || (emergency_priority && people[a].emergency > people[b].emergency) ||
        ((!emergency_priority || people[a].emergency == people[b].emergency) &&
         people[a].arrival_ms < people[b].arrival_ms);
}

bool dispatch(void) {
    int completed = 0;
    while (completed < count && !interrupted) {
        pthread_mutex_lock(&mutex);
        int first = -1;
        for (int i = 0; i < count; i++)
            if (people[i].request_state == REQUEST_WAITING && precedes(i, first)) first = i;
        if (first < 0) {
            pthread_mutex_unlock(&mutex);
            struct timespec until;
            clock_gettime(CLOCK_REALTIME, &until);
            until.tv_sec++;
            sem_timedwait(&arrivals, &until);
            continue;
        }
        enum Direction direction = people[first].direction;
        int needed_energy, needed_air;
        if (!enough_resources(&airlock, (enum Direction)direction, &needed_air, &needed_energy)) {
            print_event("Insufficient resources; stopping before boarding");
            pthread_mutex_unlock(&mutex);
            return false;
        }
        int group[MAX_PEOPLE], size = 0;
        while (size < (grouped ? airlock.capacity : 1)) {
            int next = -1;
            for (int i = 0; i < count; i++)
                if (people[i].request_state == REQUEST_WAITING && people[i].direction == direction &&
                    precedes(i, next)) next = i;
            if (next < 0) break;
            group[size++] = next;
            people[next].request_state = REQUEST_ACTIVE;
        }
        print_event("Group: %d people, destination %s", size, direction ? "station" : "space");
        pthread_mutex_unlock(&mutex);
        if (!operate(2, direction == DIRECTION_TO_SPACE ? PRESSURE_NORMAL : PRESSURE_LOW, false) || !operate(direction, 1, false)) return false;
        for (int i = 0; i < size; i++) if (!move_person(group[i], 1)) return false;
        if (!operate(direction, 0, false) || !operate(2, direction == DIRECTION_TO_SPACE ? PRESSURE_LOW : PRESSURE_NORMAL, false) ||
            !operate(1 - direction, 1, false)) return false;
        for (int i = 0; i < size; i++) if (!move_person(group[i], 2)) return false;
        if (!operate(1 - direction, 0, false)) return false;
        completed += size;
    }
    return completed == count && !interrupted;
}

