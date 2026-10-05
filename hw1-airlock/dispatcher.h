#ifndef DISPATCHER_H
#define DISPATCHER_H

#include "airlock.h"

#define MAX_GROUP_SIZE 16

enum Phase {
    PHASE_PREPARE,
    PHASE_OPEN_ENTRY,
    PHASE_BOARD,
    PHASE_CLOSE_ENTRY,
    PHASE_CHANGE_PRESSURE,
    PHASE_OPEN_EXIT,
    PHASE_EXIT,
    PHASE_CLOSE_EXIT,
    PHASE_DONE,
    PHASE_ABORT,
    PHASE_HALTED
};

enum GroupStrategy { GROUP_SINGLE, GROUP_FILL };

struct Dispatcher {
    enum Phase phase;
    enum Direction direction;

    enum GroupStrategy strategy;
    bool emergency_priority;

    int group[MAX_GROUP_SIZE];
    int group_size;
    int failure_percent;
    int group_position;
};

void begin_shutdown(struct Dispatcher* dispatcher, struct Airlock* airlock);
bool simulation_step(struct Dispatcher* dispatcher, struct Airlock* airlock,
                     struct Person* people);
void submit_requests(struct Person* people, int count, int tick, bool priority);
int find_next_person(struct Person* people, int count, bool priority);
bool form_group(struct Dispatcher* dispatcher, struct Airlock* airlock,
                struct Person* people, int count);

#endif
