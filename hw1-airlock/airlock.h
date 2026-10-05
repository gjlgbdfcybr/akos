#ifndef AIRLOCK_H
#define AIRLOCK_H

#include "person.h"

#define DOOR_ENERGY 1
#define PRESSURE_ENERGY 3
#define AIR_FILL 5

enum Pressure { PRESSURE_LOW, PRESSURE_NORMAL };

struct Operation {
    int ticks_left;
    bool active;
};

struct Door {
    bool open;
    int duration;
    struct Operation operation;
};

struct PressureSystem {
    enum Pressure value;
    int duration;
    struct Operation operation;
};

struct Airlock {
    struct Door inner;
    struct Door outer;

    int people_inside;
    int capacity;

    struct PressureSystem pressure;
    int air;
    int energy;
};

bool enter_from_space(struct Airlock* airlock, struct Person* person);
bool exit_to_station(struct Airlock* airlock, struct Person* person);
bool is_airlock_safe(struct Airlock airlock);
bool open_outer_door(struct Airlock* airlock);
bool close_outer_door(struct Airlock* airlock);
bool open_inner_door(struct Airlock* airlock);
bool close_inner_door(struct Airlock* airlock);
bool change_pressure(struct Airlock* airlock, enum Pressure target);
bool enter_from_station(struct Airlock* airlock, struct Person* person);
bool exit_to_space(struct Airlock* airlock, struct Person* person);
bool enough_resources(const struct Airlock* airlock, enum Direction direction,
                      int* air_needed, int* energy_needed);

#endif
