#ifndef DISPATCHER_H
#define DISPATCHER_H
#include "airlock.h"
#include <pthread.h>
#include <semaphore.h>
#include <stdatomic.h>
#include <signal.h>
#include <time.h>
#define MAX_PEOPLE 64
#define MAX_GROUP_SIZE 16
struct Device {
    int kind, target;
    bool success;
    sem_t request, finished;
    pthread_t thread;
};
extern struct Person people[MAX_PEOPLE];
extern struct Device devices[3];
extern struct Airlock airlock;
extern pthread_mutex_t mutex;
extern sem_t arrivals;
extern _Atomic bool interrupted;
extern bool shutdown_requested;
extern int failure, grouped, emergency_priority, count, log_fd;
extern unsigned seed;
extern struct timespec started;
void print_event(const char *format, ...);
bool read_input(const char *path);
bool check_safety(void);
bool dispatch(void);
bool operate(int kind, int target, bool emergency);
void *person_worker(void *arg);
void *device_worker(void *arg);
#endif
