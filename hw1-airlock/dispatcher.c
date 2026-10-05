#include "dispatcher.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "output.h"

static bool operation_finished(struct Operation* operation, int duration,
                               const char* description) {
    if (!operation->active) {
        operation->ticks_left = duration;
        operation->active = true;
        print_event("%s started\n", description);
    }
    operation->ticks_left--;
    if (operation->ticks_left > 0) {
        return false;
    }
    operation->active = false;
    return true;
}

static struct Operation* current_operation(struct Dispatcher* dispatcher,
                                           struct Airlock* airlock) {
    bool to_space = dispatcher->direction == DIRECTION_TO_SPACE;
    switch (dispatcher->phase) {
        case PHASE_OPEN_ENTRY:
        case PHASE_CLOSE_ENTRY:
            return to_space ? &airlock->inner.operation
                            : &airlock->outer.operation;
        case PHASE_OPEN_EXIT:
        case PHASE_CLOSE_EXIT:
            return to_space ? &airlock->outer.operation
                            : &airlock->inner.operation;
        case PHASE_ABORT:
            return airlock->inner.open ? &airlock->inner.operation
                                       : &airlock->outer.operation;
        default:
            return &airlock->pressure.operation;
    }
}

void begin_shutdown(struct Dispatcher* dispatcher, struct Airlock* airlock) {
    airlock->inner.operation = (struct Operation){0};
    airlock->outer.operation = (struct Operation){0};
    airlock->pressure.operation = (struct Operation){0};
    dispatcher->phase = PHASE_ABORT;
}

static const char* phase_name(enum Phase phase) {
    switch (phase) {
        case PHASE_PREPARE:
            return "pressure preparation";
        case PHASE_OPEN_ENTRY:
            return "entry door opening";
        case PHASE_CLOSE_ENTRY:
            return "entry door closing";
        case PHASE_CHANGE_PRESSURE:
            return "pressure change";
        case PHASE_OPEN_EXIT:
            return "exit door opening";
        case PHASE_CLOSE_EXIT:
            return "exit door closing";
        default:
            return "unknown operation";
    }
}

static bool equipment_phase(enum Phase phase) {
    return phase == PHASE_PREPARE || phase == PHASE_OPEN_ENTRY ||
           phase == PHASE_CLOSE_ENTRY || phase == PHASE_CHANGE_PRESSURE ||
           phase == PHASE_OPEN_EXIT || phase == PHASE_CLOSE_EXIT;
}

bool simulation_step(struct Dispatcher* dispatcher, struct Airlock* airlock,
                     struct Person* people) {
    bool to_space = dispatcher->direction == DIRECTION_TO_SPACE;

    struct Operation* operation = current_operation(dispatcher, airlock);
    int door_duration = operation == &airlock->inner.operation
                            ? airlock->inner.duration
                            : airlock->outer.duration;
    bool operation_needed = true;
    if (dispatcher->phase == PHASE_PREPARE) {
        enum Pressure target = to_space ? PRESSURE_NORMAL : PRESSURE_LOW;
        operation_needed = airlock->pressure.value != target;
    }
    if (equipment_phase(dispatcher->phase) && operation_needed &&
        !operation->active && rand() % 100 < dispatcher->failure_percent) {
        print_event("EQUIPMENT FAILURE before %s; emergency shutdown\n",
                    phase_name(dispatcher->phase));
        begin_shutdown(dispatcher, airlock);
        return true;
    }

    switch (dispatcher->phase) {
        case PHASE_PREPARE: {
            enum Pressure target = to_space ? PRESSURE_NORMAL : PRESSURE_LOW;

            if (airlock->pressure.value != target &&
                !operation_finished(operation, airlock->pressure.duration,
                                    "Preparing pressure")) {
                break;
            }

            if (!change_pressure(airlock, target)) {
                return false;
            }

            print_event("Pressure prepared for boarding\n");
            dispatcher->phase = PHASE_OPEN_ENTRY;
            break;
        }

        case PHASE_OPEN_ENTRY:
            if (!operation_finished(operation, door_duration,
                                    "Opening entry door")) {
                break;
            }

            if (to_space) {
                if (!open_inner_door(airlock)) {
                    return false;
                }
                print_event("Inner door opened\n");
            }
            else {
                if (!open_outer_door(airlock)) {
                    return false;
                }
                print_event("Outer door opened\n");
            }

            dispatcher->phase = PHASE_BOARD;
            break;

        case PHASE_BOARD: {
            int index = dispatcher->group[dispatcher->group_position];
            struct Person* person = &people[index];
            bool success = to_space ? enter_from_station(airlock, person)
                                    : enter_from_space(airlock, person);
            if (!success) return false;
            print_event("Person %d entered chamber from %s\n", person->id,
                        to_space ? "station" : "space");
            dispatcher->group_position++;
            if (dispatcher->group_position == dispatcher->group_size) {
                dispatcher->group_position = 0;
                dispatcher->phase = PHASE_CLOSE_ENTRY;
            }
            break;
        }

        case PHASE_CLOSE_ENTRY:
            if (!operation_finished(operation, door_duration,
                                    "Closing entry door")) {
                break;
            }

            if (to_space) {
                if (!close_inner_door(airlock)) return false;
                print_event("Inner door closed\n");
            }
            else {
                if (!close_outer_door(airlock)) return false;
                print_event("Outer door closed\n");
            }

            dispatcher->phase = PHASE_CHANGE_PRESSURE;
            break;

        case PHASE_CHANGE_PRESSURE: {
            enum Pressure target = to_space ? PRESSURE_LOW : PRESSURE_NORMAL;

            if (!operation_finished(operation, airlock->pressure.duration,
                                    "Changing pressure")) {
                break;
            }

            if (!change_pressure(airlock, target)) {
                return false;
            }

            print_event("Pressure changed to %s\n",
                        to_space ? "low" : "normal");
            dispatcher->phase = PHASE_OPEN_EXIT;
            break;
        }

        case PHASE_OPEN_EXIT:
            if (!operation_finished(operation, door_duration,
                                    "Opening exit door")) {
                break;
            }

            if (to_space) {
                if (!open_outer_door(airlock)) {
                    return false;
                }
                print_event("Outer door opened\n");
            }
            else {
                if (!open_inner_door(airlock)) {
                    return false;
                }
                print_event("Inner door opened\n");
            }

            dispatcher->phase = PHASE_EXIT;
            break;

        case PHASE_EXIT: {
            int index = dispatcher->group[dispatcher->group_position];
            struct Person* person = &people[index];
            bool success = to_space ? exit_to_space(airlock, person)
                                    : exit_to_station(airlock, person);
            if (!success) return false;
            print_event("Person %d left chamber to %s\n", person->id,
                        to_space ? "space" : "station");
            person->request_state = REQUEST_DONE;
            dispatcher->group_position++;
            if (dispatcher->group_position == dispatcher->group_size) {
                dispatcher->phase = PHASE_CLOSE_EXIT;
            }
            break;
        }

        case PHASE_CLOSE_EXIT:
            if (!operation_finished(operation, door_duration,
                                    "Closing exit door")) {
                break;
            }

            if (to_space) {
                if (!close_outer_door(airlock)) return false;
                print_event("Outer door closed\n");
            }
            else {
                if (!close_inner_door(airlock)) return false;
                print_event("Inner door closed\n");
            }

            dispatcher->phase = PHASE_DONE;
            break;

        case PHASE_ABORT:

            if (airlock->inner.open || airlock->outer.open) {
                if (!operation_finished(operation, door_duration,
                                        "Emergency door closure")) {
                    break;
                }
                if (!close_inner_door(airlock) || !close_outer_door(airlock)) {
                    return false;
                }
            }
            print_event(
                "SAFE STOP: doors closed; pressure held; locations "
                "preserved\n");
            dispatcher->phase = PHASE_HALTED;
            break;

        case PHASE_HALTED:
        case PHASE_DONE:
            break;

        default:
            return false;
    }

    return true;
}

void submit_requests(struct Person* people, int count, int tick,
                     bool priority) {
    for (int i = 0; i < count; i++) {
        if (people[i].request_state == REQUEST_NOT_ARRIVED &&
            people[i].arrival_tick <= tick) {
            people[i].request_state = REQUEST_WAITING;
            if (people[i].emergency && priority) {
                print_event("EMERGENCY RETURN: person %d gets queue priority\n",
                            people[i].id);
            }
            print_event("Person %d is waiting\n", people[i].id);

            print_event("Person %d requested passage to %s\n", people[i].id,
                        people[i].direction == DIRECTION_TO_SPACE ? "space"
                                                                  : "station");
        }
    }
}

static bool precedes(const struct Person* candidate,
                     const struct Person* current, bool priority) {
    if (priority && candidate->emergency != current->emergency) {
        return candidate->emergency;
    }
    return candidate->arrival_tick < current->arrival_tick;
}

int find_next_person(struct Person* people, int count, bool priority) {
    int selected = -1;

    for (int i = 0; i < count; i++) {
        if (people[i].request_state != REQUEST_WAITING) {
            continue;
        }

        if (selected == -1 ||
            precedes(&people[i], &people[selected], priority)) {
            selected = i;
        }
    }

    return selected;
}

bool form_group(struct Dispatcher* dispatcher, struct Airlock* airlock,
                struct Person* people, int count) {
    int first = find_next_person(people, count, dispatcher->emergency_priority);

    if (first == -1) {
        return false;
    }

    dispatcher->direction = people[first].direction;
    dispatcher->group_size = 0;
    dispatcher->group_position = 0;
    airlock->inner.operation = (struct Operation){0};
    airlock->outer.operation = (struct Operation){0};
    airlock->pressure.operation = (struct Operation){0};

    while (
        dispatcher->group_size < airlock->capacity &&
        (dispatcher->strategy == GROUP_FILL || dispatcher->group_size == 0)) {
        int selected = -1;

        for (int i = 0; i < count; i++) {
            if (people[i].request_state != REQUEST_WAITING ||
                people[i].direction != dispatcher->direction) {
                continue;
            }

            if (selected == -1 || precedes(&people[i], &people[selected],
                                           dispatcher->emergency_priority)) {
                selected = i;
            }
        }

        if (selected == -1) {
            break;
        }

        dispatcher->group[dispatcher->group_size++] = selected;
        people[selected].request_state = REQUEST_ACTIVE;
    }

    dispatcher->phase = PHASE_PREPARE;

    print_event(
        "Group formed: %d people, direction: %s\n", dispatcher->group_size,
        dispatcher->direction == DIRECTION_TO_SPACE ? "space" : "station");

    return true;
}
