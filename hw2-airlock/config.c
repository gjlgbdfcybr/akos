#include "dispatcher.h"
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

bool read_input(const char *path) {
    FILE *file = fopen(path, "r");
    if (!file) return false;
    bool ok = fscanf(file, "%d%d%d%d%d%d%u%d%d", &airlock.capacity, &airlock.air, &airlock.energy,
                     &airlock.inner.duration, &airlock.pressure.duration, &failure, &seed, &grouped, &emergency_priority) == 9 &&
        airlock.capacity > 0 && airlock.capacity <= MAX_GROUP_SIZE && airlock.air >= 0 && airlock.energy >= 0 &&
        airlock.inner.duration > 0 && airlock.inner.duration <= 60000 && airlock.pressure.duration > 0 && airlock.pressure.duration <= 60000 &&
        (emergency_priority == 0 || emergency_priority == 1) && failure >= 0 && failure <= 100 && (grouped == 0 || grouped == 1) &&
        fscanf(file, "%d", &count) == 1 && count > 0 && count <= MAX_PEOPLE;
    for (int i = 0; ok && i < count; i++) {
        struct Person *p = &people[i];
        int direction = -1, emergency = -1;
        ok = fscanf(file, "%d%d%d%d", &p->id, &direction, &p->arrival_ms, &emergency) == 4;
        p->direction = (enum Direction)direction;
        p->emergency = emergency == 1;
        ok = ok && (emergency == 0 || emergency == 1) &&
            p->id > 0 && (p->direction == 0 || p->direction == 1) &&
            p->arrival_ms >= 0 && p->arrival_ms <= 3600000 &&
            (p->emergency == 0 || (p->emergency == 1 && p->direction == 1));
        for (int j = 0; j < i; j++) if (people[j].id == p->id) ok = false;
        p->location = p->direction == 0 ? LOCATION_STATION : LOCATION_SPACE;
    }
    char extra;
    if (ok && fscanf(file, " %c", &extra) == 1) ok = false;
    airlock.outer.duration = airlock.inner.duration;
    fclose(file);
    return ok;
}

