#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include "dispatcher.h"
#include "output.h"

volatile sig_atomic_t stop_requested = 0;

static void handle_stop(int signal_number) {
    (void)signal_number;
    stop_requested = 1;
}

int main(void) {
    start_clock();

    struct sigaction action = {0};
    action.sa_handler = handle_stop;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) == -1 ||
        sigaction(SIGTERM, &action, NULL) == -1) {
        perror("sigaction");
        return 1;
    }
    struct Person people[] = {{.id = 1,
                               .location = LOCATION_STATION,
                               .direction = DIRECTION_TO_SPACE,
                               .request_state = REQUEST_NOT_ARRIVED,
                               .arrival_tick = 0},
                              {.id = 2,
                               .location = LOCATION_SPACE,
                               .direction = DIRECTION_TO_STATION,
                               .request_state = REQUEST_NOT_ARRIVED,
                               .arrival_tick = 2,
                               .emergency = true},
                              {.id = 3,
                               .location = LOCATION_STATION,
                               .direction = DIRECTION_TO_SPACE,
                               .request_state = REQUEST_NOT_ARRIVED,
                               .arrival_tick = 0}};
    int count = (int)(sizeof(people) / sizeof(people[0]));

    struct Airlock airlock = {
        .capacity = 2,
        .inner = {.duration = 2},
        .outer = {.duration = 2},
        .pressure = {.value = PRESSURE_NORMAL, .duration = 3},
        .air = 20,
        .energy = 30};
    struct Dispatcher dispatcher = {.phase = PHASE_DONE,
                                    .strategy = GROUP_FILL,
                                    .emergency_priority = true,
                                    .failure_percent = 5};
    if (airlock.capacity <= 0 || airlock.capacity > MAX_GROUP_SIZE ||
        airlock.air < 0 || airlock.energy < 0 || airlock.people_inside != 0 ||
        airlock.inner.open || airlock.outer.open ||
        (airlock.pressure.value != PRESSURE_NORMAL &&
         airlock.pressure.value != PRESSURE_LOW) ||
        airlock.inner.duration <= 0 || airlock.outer.duration <= 0 ||
        airlock.pressure.duration <= 0 || airlock.inner.duration > 1000000 ||
        airlock.outer.duration > 1000000 ||
        airlock.pressure.duration > 1000000 ||
        (dispatcher.strategy != GROUP_SINGLE &&
         dispatcher.strategy != GROUP_FILL)) {
        print_event("ERROR: invalid parameters\n");
        return 1;
    }

    unsigned int seed = 42;
    srand(seed);
    if (dispatcher.failure_percent < 0 || dispatcher.failure_percent > 100) {
        print_event("ERROR: failure probability must be 0..100\n");
        return 1;
    }
    for (int i = 0; i < count; i++) {
        enum Location origin = people[i].direction == DIRECTION_TO_SPACE
                                   ? LOCATION_STATION
                                   : LOCATION_SPACE;
        if ((people[i].direction != DIRECTION_TO_SPACE &&
             people[i].direction != DIRECTION_TO_STATION) ||
            people[i].id <= 0 || people[i].arrival_tick < 0 ||
            people[i].arrival_tick > 1000000 ||
            people[i].request_state != REQUEST_NOT_ARRIVED ||
            people[i].location != origin ||
            (people[i].emergency &&
             people[i].direction != DIRECTION_TO_STATION)) {
            print_event("ERROR: invalid person or emergency direction\n");
            return 1;
        }
    }
    for (int i = 0; i < count; i++) {
        for (int j = 0; j < i; j++) {
            if (people[i].id == people[j].id) {
                print_event("ERROR: duplicate person id\n");
                return 1;
            }
        }
    }
    int tick = 0;
    int completed = 0;
    bool stopped = false;
    while (completed < count) {
        if (!stop_requested) {
            struct timespec delay = {.tv_sec = 1, .tv_nsec = 500000000};
            nanosleep(&delay, NULL);
        }
        if (stop_requested && dispatcher.phase != PHASE_ABORT) {
            print_event("USER INTERRUPTION: starting safe shutdown\n");
            begin_shutdown(&dispatcher, &airlock);
        }
        if (dispatcher.phase != PHASE_ABORT) {
            submit_requests(people, count, tick, dispatcher.emergency_priority);
        }
        if (dispatcher.phase == PHASE_DONE) {
            int next =
                find_next_person(people, count, dispatcher.emergency_priority);
            if (next == -1) {
                tick++;
                continue;
            }
            int air_needed, energy_needed;
            if (!enough_resources(&airlock, people[next].direction, &air_needed,
                                  &energy_needed)) {
                print_event("SAFE STOP: insufficient resources\n");
                dprintf(
                    STDOUT_FILENO,
                    "Required: air=%d energy=%d; available: air=%d energy=%d\n",
                    air_needed, energy_needed, airlock.air, airlock.energy);
                stopped = true;
                break;
            }
            if (!form_group(&dispatcher, &airlock, people, count)) {
                stopped = true;
                break;
            }
        }

        if (!simulation_step(&dispatcher, &airlock, people)) {
            print_event("ERROR: simulation step failed\n");
            stopped = true;
            break;
        }
        int actual_inside = 0;
        for (int i = 0; i < count; i++) {
            if (people[i].location == LOCATION_CHAMBER) actual_inside++;
        }
        if (!is_airlock_safe(airlock) ||
            actual_inside != airlock.people_inside) {
            print_event("ERROR: unsafe state\n");
            stopped = true;
            break;
        }
        if (dispatcher.phase == PHASE_HALTED) {
            stopped = true;
            break;
        }
        if (dispatcher.phase == PHASE_DONE) {
            for (int i = 0; i < dispatcher.group_size; i++) {
                int index = dispatcher.group[i];
                people[index].request_state = REQUEST_DONE;
                completed++;

                print_event("Person %d completed passage\n", people[index].id);
            }
            print_event("Resources: air=%d energy=%d\n", airlock.air,
                        airlock.energy);
        }
        tick++;
    }
    completed = 0;
    for (int i = 0; i < count; i++) {
        if (people[i].request_state == REQUEST_DONE)
            completed++;
        else if (people[i].request_state == REQUEST_ACTIVE)
            people[i].request_state = REQUEST_INTERRUPTED;
    }
    print_event("\nResult: %s\n",
                stopped ? "stopped" : "all passages completed");
    print_event("Completed: %d of %d\n", completed, count);
    print_event("People inside: %d\n", airlock.people_inside);
    print_event("Resources: air=%d energy=%d\n", airlock.air, airlock.energy);
    print_event("Doors: inner=%s outer=%s; safe=%d\n",
                airlock.inner.open ? "open" : "closed",
                airlock.outer.open ? "open" : "closed",
                is_airlock_safe(airlock));
    for (int i = 0; i < count; i++) {
        const char* state =
            people[i].request_state == REQUEST_DONE          ? "done"
            : people[i].request_state == REQUEST_WAITING     ? "waiting"
            : people[i].request_state == REQUEST_ACTIVE      ? "active"
            : people[i].request_state == REQUEST_INTERRUPTED ? "interrupted"
                                                             : "not arrived";
        print_event("Person %d: location=%s request=%s\n", people[i].id,
                    people[i].location == LOCATION_STATION ? "station"
                    : people[i].location == LOCATION_SPACE ? "space"
                                                           : "chamber",
                    state);
    }
    return stopped ? 1 : 0;
}
