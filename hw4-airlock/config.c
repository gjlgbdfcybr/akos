#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>

#include "model.h"

static bool read_number(FILE* file, int* value, int minimum, int maximum) {
    char text[64];
    if (fscanf(file, "%63s", text) != 1) return false;
    int next = fgetc(file);
    if (next != EOF && !isspace(next)) return false;
    char* end;
    errno = 0;
    long number = strtol(text, &end, 10);
    if (errno != 0 || *end != '\0') return false;
    if (number < minimum || number > maximum) return false;
    *value = (int)number;
    return true;
}

bool read_input(const char* path) {
    FILE* file = fopen(path, "r");
    if (file == NULL) return false;

    struct Airlock* chamber = &model.chamber;
    bool ok = true;

    int random_seed = 0;
    if (!read_number(file, &chamber->capacity, 1, MAX_GROUP_SIZE)) ok = false;
    if (!read_number(file, &chamber->air, 0, INT_MAX)) ok = false;
    if (!read_number(file, &chamber->energy, 0, INT_MAX)) ok = false;
    if (!read_number(file, &chamber->inner.duration, 1, 60000)) ok = false;
    if (!read_number(file, &chamber->pressure.duration, 1, 60000)) ok = false;
    if (!read_number(file, &model.failure_rate, 0, 100)) ok = false;
    if (!read_number(file, &random_seed, 0, INT_MAX)) ok = false;
    if (!read_number(file, &model.group_mode, 0, 1)) ok = false;
    if (!read_number(file, &model.priority, 0, 1)) ok = false;
    if (!read_number(file, &model.total, 1, MAX_PEOPLE)) ok = false;

    model.random_seed = (unsigned)random_seed;
    chamber->outer.duration = chamber->inner.duration;

    for (int i = 0; ok && i < model.total; i++) {
        struct Person* person = &model.persons[i];
        int direction = 0;
        int emergency = 0;

        if (!read_number(file, &person->id, 1, INT_MAX)) ok = false;
        if (!read_number(file, &direction, 0, 1)) ok = false;
        if (!read_number(file, &person->arrival_ms, 0, 3600000)) ok = false;
        if (!read_number(file, &emergency, 0, 1)) ok = false;

        person->direction = (enum Direction)direction;
        person->emergency = emergency == 1;
        person->location = LOCATION_STATION;

        if (person->direction == DIRECTION_TO_STATION) {
            person->location = LOCATION_SPACE;
        }
        else if (person->emergency) {
            ok = false;
        }
        for (int j = 0; j < i; j++) {
            if (model.persons[j].id == person->id) ok = false;
        }
    }
    char extra;
    if (fscanf(file, " %c", &extra) == 1) ok = false;
    fclose(file);

    return ok;
}
