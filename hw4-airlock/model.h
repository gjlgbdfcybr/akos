#ifndef MODEL_H
#define MODEL_H
#include "airlock.h"
#define MAX_PEOPLE 64
#define MAX_GROUP_SIZE 16
struct Model {
    struct Airlock chamber;
    struct Person persons[MAX_PEOPLE];
    int total, failure_rate, group_mode, priority;
    unsigned random_seed;
};
extern struct Model model;
bool read_input(const char* path);
#endif
