#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "shared.h"

bool check_safety(void) {
    int actual = 0;
    for (int i = 0; i < shared->total; i++)
        if (shared->persons[i].location == LOCATION_CHAMBER) actual++;
    return actual == shared->chamber.people_inside &&
           is_airlock_safe(shared->chamber);
}

void person_worker(int index) {
    struct Person* p = &shared->persons[index];
    delay_ms(p->arrival_ms);
    lock_state();
    if (shared->shutdown || shared->stop) {
        unlock_state();
        return;
    }
    p->request_state = REQUEST_WAITING;
    print_event("Person %d requests %s%s", p->id,
                p->direction == 0 ? "space" : "station",
                p->emergency ? " (emergency)" : "");
    unlock_state();
    post_event(ARRIVALS);
    for (;;) {
        if (!wait_event(REQUEST(index + 3), -1)) return;
        lock_state();
        if (shared->shutdown) {
            unlock_state();
            return;
        }
        p->success = false;
        if (p->command == 1) {
            if (p->direction == DIRECTION_TO_SPACE) {
                p->success = enter_from_station(&shared->chamber, p);
            }
            else {
                p->success = enter_from_space(&shared->chamber, p);
            }
            if (p->success) print_event("Person %d enters chamber", p->id);
        }
        else {
            if (p->direction == DIRECTION_TO_SPACE) {
                p->success = exit_to_space(&shared->chamber, p);
            }
            else {
                p->success = exit_to_station(&shared->chamber, p);
            }
            if (p->success) p->request_state = REQUEST_DONE;
            if (p->success)
                print_event(
                    "Person %d exits to %s", p->id,
                    p->location == LOCATION_SPACE ? "space" : "station");
        }
        unlock_state();
        post_event(FINISHED(index + 3));
    }
}

void device_worker(int index) {
    struct Device* d = &shared->equipment[index];
    for (;;) {
        if (!wait_event(REQUEST(index), -1)) return;
        lock_state();
        if (shared->shutdown) {
            unlock_state();
            return;
        }
        int target = d->target;
        if (d->kind == 2) shared->chamber.pressure.operation.active = true;
        const char* name = "Pressure system";
        if (d->kind == 0) name = "Inner door";
        if (d->kind == 1) name = "Outer door";
        print_event("%s operation started", name);
        unlock_state();
        int duration = shared->chamber.inner.duration;
        if (d->kind == 1) duration = shared->chamber.outer.duration;
        if (d->kind == 2) duration = shared->chamber.pressure.duration;
        delay_ms(duration);
        lock_state();
        d->success = false;
        if (d->kind == 0)
            d->success = target ? open_inner_door(&shared->chamber)
                                : close_inner_door(&shared->chamber);
        else if (d->kind == 1)
            d->success = target ? open_outer_door(&shared->chamber)
                                : close_outer_door(&shared->chamber);
        else {
            d->success =
                change_pressure(&shared->chamber, (enum Pressure)target);
            shared->chamber.pressure.operation.active = false;
        }
        if (d->success) {
            if (d->kind < 2)
                print_event("%s door %s", d->kind == 0 ? "Inner" : "Outer",
                            target ? "opened" : "closed");
            else
                print_event("Pressure changed to %s",
                            target == PRESSURE_LOW ? "low" : "normal");
        }
        d->success = d->success && check_safety();
        unlock_state();
        post_event(FINISHED(index));
    }
}

bool operate(int kind, int target, bool emergency) {
    lock_state();
    int current;
    if (kind == 0) {
        current = shared->chamber.inner.open;
    }
    else if (kind == 1) {
        current = shared->chamber.outer.open;
    }
    else {
        current = shared->chamber.pressure.value;
    }
    if (current == target) {
        unlock_state();
        return true;
    }
    if (!emergency && (shared->stop || rand() % 100 < shared->failure_rate)) {
        if (!shared->stop) print_event("Equipment failure; emergency shutdown");
        unlock_state();
        return false;
    }
    shared->equipment[kind].target = target;
    unlock_state();
    post_event(REQUEST(kind));
    if (!wait_event(FINISHED(kind), 70000)) return false;
    lock_state();
    bool result = shared->equipment[kind].success;
    unlock_state();
    return result && (emergency || !shared->stop);
}

static bool move_person(int index, int command) {
    lock_state();
    struct Person* p = &shared->persons[index];
    bool allowed = !shared->stop;
    p->command = command;
    unlock_state();
    if (!allowed) return false;
    post_event(REQUEST(index + 3));
    if (!wait_event(FINISHED(index + 3), 10000)) return false;
    lock_state();
    bool success = p->success && check_safety();
    unlock_state();
    return success && !shared->stop;
}

static bool precedes(int candidate, int current) {
    if (current < 0) {
        return true;
    }
    struct Person* a = &shared->persons[candidate];
    struct Person* b = &shared->persons[current];
    if (shared->priority && a->emergency != b->emergency) {
        return a->emergency;
    }
    return a->arrival_ms < b->arrival_ms;
}

static bool serve_group(int* group, int size, enum Direction direction) {
    int entry_door = 0;
    int exit_door = 1;
    enum Pressure entry_pressure = PRESSURE_NORMAL;
    enum Pressure exit_pressure = PRESSURE_LOW;
    if (direction == DIRECTION_TO_STATION) {
        entry_door = 1;
        exit_door = 0;
        entry_pressure = PRESSURE_LOW;
        exit_pressure = PRESSURE_NORMAL;
    }
    if (!operate(2, entry_pressure, false)) return false;
    if (!operate(entry_door, 1, false)) return false;
    for (int i = 0; i < size; i++) {
        if (!move_person(group[i], 1)) return false;
    }
    if (!operate(entry_door, 0, false)) return false;
    if (!operate(2, exit_pressure, false)) return false;
    if (!operate(exit_door, 1, false)) return false;
    for (int i = 0; i < size; i++) {
        if (!move_person(group[i], 2)) return false;
    }
    return operate(exit_door, 0, false);
}

bool dispatch(void) {
    int completed = 0;
    while (completed < shared->total && !shared->stop) {
        poll_signal();
        lock_state();
        int first = -1;
        for (int i = 0; i < shared->total; i++)
            if (shared->persons[i].request_state == REQUEST_WAITING &&
                precedes(i, first))
                first = i;
        if (first < 0) {
            unlock_state();
            wait_event(ARRIVALS, 100);
            continue;
        }
        enum Direction direction = shared->persons[first].direction;
        int needed_energy, needed_air;
        if (!enough_resources(&shared->chamber, (enum Direction)direction,
                              &needed_air, &needed_energy)) {
            print_event("Insufficient resources; stopping before boarding");
            unlock_state();
            return false;
        }
        int group[MAX_GROUP_SIZE];
        int size = 0;
        int limit = 1;
        if (shared->group_mode) limit = shared->chamber.capacity;
        while (size < limit) {
            int next = -1;
            for (int i = 0; i < shared->total; i++)
                if (shared->persons[i].request_state == REQUEST_WAITING &&
                    shared->persons[i].direction == direction &&
                    precedes(i, next))
                    next = i;
            if (next < 0) break;
            group[size] = next;
            size++;
            shared->persons[next].request_state = REQUEST_ACTIVE;
        }
        print_event("Group: %d people, destination %s", size,
                    direction ? "station" : "space");
        unlock_state();
        if (!serve_group(group, size, direction)) {
            return false;
        }
        completed += size;
    }
    return completed == shared->total && !shared->stop;
}
