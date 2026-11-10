#ifndef SHARED_H
#define SHARED_H
#include <semaphore.h>
#include <signal.h>
#include <stdatomic.h>
#include <sys/types.h>
#include <time.h>

#include "airlock.h"
#define MAX_PEOPLE 64
#define MAX_GROUP_SIZE 16
#define MAX_WORKERS (MAX_PEOPLE + 3)
#define SEM_COUNT (5 + 2 * MAX_WORKERS)
#define LOCK 0
#define ARRIVALS 1
#define READY 2
#define START 3
#define EXITED 4
#define REQUEST(i) (5 + 2 * (i))
#define FINISHED(i) (6 + 2 * (i))
#define MAGIC 0x41495233

struct Device {
    int kind, target;
    bool success;
};

struct Shared {
    _Atomic unsigned magic;
    int named;
    pid_t owner;
    struct Airlock chamber;
    struct Person persons[MAX_PEOPLE];
    struct Device equipment[3];
    int total, failure_rate, group_mode, priority;
    unsigned random_seed;
    _Atomic bool stop, shutdown;
    pid_t workers[MAX_WORKERS];
    struct timespec began;
    char log_path[4096];
    sem_t semaphores[SEM_COUNT];
};

extern struct Shared* shared;
extern sem_t* sems[SEM_COUNT];
extern int log_fd;
extern volatile sig_atomic_t local_stop;
extern char session[40];
bool ipc_create(const char* name, int named);
bool ipc_attach(const char* name);
void ipc_close(bool owner);
bool wait_event(int id, int timeout_ms);
void post_event(int id);
void lock_state(void);
void unlock_state(void);
void poll_signal(void);
void install_signals(void);
void delay_ms(int ms);
bool read_input(const char* path);
void print_event(const char* format, ...);
bool check_safety(void);
bool dispatch(void);
bool operate(int kind, int target, bool emergency);
void person_worker(int index);
void device_worker(int index);
int run_worker(int slot);
int run_controller(bool spawn);
#endif
