#include "airlock.h"

bool enter_from_space(struct Airlock* airlock, struct Person* person) {
    if (!airlock->outer.open || airlock->capacity <= airlock->people_inside ||
        person->location != LOCATION_SPACE) {
        return false;
    }

    person->location = LOCATION_CHAMBER;
    airlock->people_inside += 1;
    return true;
}

bool exit_to_station(struct Airlock* airlock, struct Person* person) {
    if (!airlock->inner.open || airlock->people_inside <= 0 ||
        person->location != LOCATION_CHAMBER) {
        return false;
    }

    person->location = LOCATION_STATION;
    airlock->people_inside -= 1;
    return true;
}

bool is_airlock_safe(struct Airlock airlock) {
    if (airlock.pressure.operation.active &&
        (airlock.inner.open || airlock.outer.open))
        return false;
    if (airlock.inner.open && airlock.outer.open) {
        return false;
    }
    if (airlock.people_inside < 0 || airlock.air < 0 || airlock.energy < 0 ||
        airlock.people_inside > airlock.capacity) {
        return false;
    }
    if (airlock.outer.open && airlock.pressure.value != PRESSURE_LOW) {
        return false;
    }
    if (airlock.inner.open && airlock.pressure.value != PRESSURE_NORMAL) {
        return false;
    }

    return true;
}

bool open_outer_door(struct Airlock* airlock) {
    if (airlock->inner.open || airlock->pressure.value != PRESSURE_LOW) {
        return false;
    }

    if (airlock->outer.open) return true;
    if (airlock->energy < DOOR_ENERGY) return false;
    airlock->energy -= DOOR_ENERGY;
    airlock->outer.open = true;
    return true;
}

bool close_outer_door(struct Airlock* airlock) {
    if (!airlock->outer.open) return true;
    if (airlock->energy < DOOR_ENERGY) return false;
    airlock->energy -= DOOR_ENERGY;
    airlock->outer.open = false;
    return true;
}

bool open_inner_door(struct Airlock* airlock) {
    if (airlock->outer.open || airlock->pressure.value != PRESSURE_NORMAL) {
        return false;
    }

    if (airlock->inner.open) return true;
    if (airlock->energy < DOOR_ENERGY) return false;
    airlock->energy -= DOOR_ENERGY;
    airlock->inner.open = true;
    return true;
}

bool close_inner_door(struct Airlock* airlock) {
    if (!airlock->inner.open) return true;
    if (airlock->energy < DOOR_ENERGY) return false;
    airlock->energy -= DOOR_ENERGY;
    airlock->inner.open = false;
    return true;
}

bool change_pressure(struct Airlock* airlock, enum Pressure target) {
    if (airlock->inner.open || airlock->outer.open) {
        return false;
    }

    if (airlock->pressure.value == target) return true;
    int air_cost = target == PRESSURE_NORMAL ? AIR_FILL : 0;
    if (airlock->energy < PRESSURE_ENERGY || airlock->air < air_cost)
        return false;
    airlock->energy -= PRESSURE_ENERGY;
    airlock->air -= air_cost;
    airlock->pressure.value = target;
    return true;
}

bool enter_from_station(struct Airlock* airlock, struct Person* person) {
    if (airlock->inner.open == false ||
        airlock->people_inside >= airlock->capacity ||
        person->location != LOCATION_STATION) {
        return false;
    }

    person->location = LOCATION_CHAMBER;
    airlock->people_inside += 1;
    return true;
}

bool exit_to_space(struct Airlock* airlock, struct Person* person) {
    if (airlock->outer.open == false || airlock->people_inside <= 0 ||
        person->location != LOCATION_CHAMBER) {
        return false;
    }

    person->location = LOCATION_SPACE;
    airlock->people_inside -= 1;
    return true;
}

bool enough_resources(const struct Airlock* airlock, enum Direction direction,
                      int* air_needed, int* energy_needed) {
    enum Pressure entry =
        direction == DIRECTION_TO_SPACE ? PRESSURE_NORMAL : PRESSURE_LOW;
    *energy_needed = 4 * DOOR_ENERGY + PRESSURE_ENERGY;
    *air_needed = direction == DIRECTION_TO_STATION ? AIR_FILL : 0;
    if (airlock->pressure.value != entry) {
        *energy_needed += PRESSURE_ENERGY;
        if (entry == PRESSURE_NORMAL) *air_needed += AIR_FILL;
    }
    return airlock->air >= *air_needed && airlock->energy >= *energy_needed;
}
