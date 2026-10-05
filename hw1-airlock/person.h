#ifndef PERSON_H
#define PERSON_H

#include <stdbool.h>

enum RequestState {
    REQUEST_NOT_ARRIVED,
    REQUEST_WAITING,
    REQUEST_ACTIVE,
    REQUEST_DONE,
    REQUEST_INTERRUPTED
};

enum Location { LOCATION_STATION, LOCATION_CHAMBER, LOCATION_SPACE };

enum Direction { DIRECTION_TO_SPACE, DIRECTION_TO_STATION };

struct Person {
    int id;
    enum Location location;
    enum Direction direction;
    enum RequestState request_state;
    int arrival_tick;
    bool emergency;
};

#endif
