#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "model.h"
#include "net.h"

#define MAX_CLIENTS 128
#define BUFFER_SIZE 32768
enum Role { ROLE_UNKNOWN, ROLE_PERSON, ROLE_OBSERVER };
enum Phase {
    PHASE_IDLE = -1,
    PHASE_PREPARE,
    PHASE_OPEN_ENTRY,
    PHASE_BOARD,
    PHASE_CLOSE_ENTRY,
    PHASE_CHANGE_PRESSURE,
    PHASE_OPEN_EXIT,
    PHASE_EXIT,
    PHASE_CLOSE_EXIT,
    PHASE_COMPLETE
};

struct Connection {
    int fd, role, person;
    char input[256], output[BUFFER_SIZE];
    size_t used, queued;
    long long connected;
};
struct Model model;
static struct Connection clients[MAX_CLIENTS];
static int log_fd, listener;
static long long began, due;
static int group[MAX_GROUP_SIZE], group_size, phase = PHASE_IDLE;
static int direction, sequence, pending[MAX_PEOPLE];
static bool acknowledged[MAX_PEOPLE], finished, failed;

static const char* location_name(enum Location location) {
    if (location == LOCATION_CHAMBER) return "chamber";
    if (location == LOCATION_SPACE) return "space";
    return "station";
}

static const char* status_name(enum RequestState state) {
    if (state == REQUEST_WAITING) return "waiting";
    if (state == REQUEST_ACTIVE) return "active";
    if (state == REQUEST_DONE) return "done";
    if (state == REQUEST_INTERRUPTED) return "interrupted";
    return "not-arrived";
}

static const char* direction_name(enum Direction value) {
    if (value == DIRECTION_TO_STATION) return "to-station";
    return "to-space";
}

static const char* device_name(int kind) {
    if (kind == 0) return "inner-door";
    if (kind == 1) return "outer-door";
    return "pressure-system";
}

static void disconnect_client(int index) {
    close(clients[index].fd);
    clients[index].fd = -1;
    clients[index].queued = 0;
}

static void queue_line(int index, const char* line) {
    struct Connection* client = &clients[index];
    if (client->fd < 0) return;
    size_t size = strlen(line);
    if (size > sizeof(client->output) - client->queued) {
        disconnect_client(index);
        return;
    }
    memcpy(client->output + client->queued, line, size);
    client->queued += size;
}

static void event(int person, const char* format, ...) {
    char text[1024], line[1200];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    snprintf(line, sizeof(line), "EVENT %lld %s\n", now_ms() - began, text);
    if (dprintf(log_fd, "%s", line) < 0) {
        perror("Write log");
        failed = true;
    }
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].role == ROLE_OBSERVER ||
            (person >= 0 && clients[i].role == ROLE_PERSON &&
             clients[i].person == person))
            queue_line(i, line);
    }
}

static void snapshot(int index) {
    char line[256];
    struct Airlock* a = &model.chamber;
    snprintf(line, sizeof(line),
             "STATE inside=%d capacity=%d pressure=%d inner=%d outer=%d air=%d "
             "energy=%d safe=%d\n",
             a->people_inside, a->capacity, a->pressure.value, a->inner.open,
             a->outer.open, a->air, a->energy, is_airlock_safe(*a));
    queue_line(index, line);
    for (int i = 0; i < model.total; i++) {
        struct Person* p = &model.persons[i];
        snprintf(line, sizeof(line),
                 "PERSON id=%d location=%s status=%s direction=%s\n", p->id,
                 location_name(p->location), status_name(p->request_state),
                 direction_name(p->direction));
        queue_line(index, line);
    }
}

static void send_command(int index, int person) {
    char line[128];
    const char* action = "BOARD";
    if (phase == PHASE_EXIT) action = "EXIT";
    snprintf(line, sizeof(line), "COMMAND %d %s\n", pending[person], action);
    queue_line(index, line);
}

static void receive_line(int index, const char* line) {
    struct Connection* client = &clients[index];
    if (client->role == ROLE_UNKNOWN) {
        if (strcmp(line, "WATCH") == 0) {
            client->role = ROLE_OBSERVER;
            queue_line(index, "WELCOME observer\n");
            snapshot(index);
            return;
        }
        int id;
        if (strncmp(line, "PERSON ", 7) != 0 ||
            !number(line + 7, 1, 2147483647, &id)) {
            disconnect_client(index);
            return;
        }
        int person = -1;
        for (int i = 0; i < model.total; i++)
            if (model.persons[i].id == id) person = i;
        if (person < 0) {
            disconnect_client(index);
            return;
        }
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].fd >= 0 && clients[i].role == ROLE_PERSON &&
                clients[i].person == person) {
                disconnect_client(index);
                return;
            }
        }
        client->role = ROLE_PERSON;
        client->person = person;
        queue_line(index, "WELCOME person\n");
        event(person, "Person %d connected; location=%s status=%s", id,
              location_name(model.persons[person].location),
              status_name(model.persons[person].request_state));
        if (pending[person] && !acknowledged[person])
            send_command(index, person);
        if (model.persons[person].request_state == REQUEST_DONE)
            queue_line(index, "DONE\n");
        return;
    }
    int value;
    if (client->role == ROLE_PERSON && strncmp(line, "ACK ", 4) == 0 &&
        number(line + 4, 1, 2147483647, &value)) {
        int person = client->person;
        if (pending[person] == value) acknowledged[person] = true;
        return;
    }
    disconnect_client(index);
}

static void accept_clients(void) {
    for (int count = 0; count < MAX_CLIENTS; count++) {
        int fd = accept(listener, NULL, NULL);
        if (fd < 0) return;
        if (fcntl(fd, F_SETFL, O_NONBLOCK) < 0) {
            close(fd);
            continue;
        }
        int slot = -1;
        for (int i = 0; i < MAX_CLIENTS; i++)
            if (clients[i].fd < 0) {
                slot = i;
                break;
            }
        if (slot < 0) {
            close(fd);
            continue;
        }
        memset(&clients[slot], 0, sizeof(clients[slot]));
        clients[slot].fd = fd;
        clients[slot].connected = now_ms();
    }
}

static void network_step(void) {
    struct pollfd items[MAX_CLIENTS + 1];
    items[0] = (struct pollfd){listener, POLLIN, 0};
    for (int i = 0; i < MAX_CLIENTS; i++) {
        short events = POLLIN;
        if (clients[i].queued) events |= POLLOUT;
        items[i + 1] = (struct pollfd){clients[i].fd, events, 0};
    }
    if (poll(items, MAX_CLIENTS + 1, 20) < 0) return;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        struct Connection* c = &clients[i];
        if (c->fd < 0) continue;
        if (c->role == ROLE_UNKNOWN && now_ms() - c->connected > 5000) {
            disconnect_client(i);
            continue;
        }
        short events = items[i + 1].revents;
        if (events & POLLIN) {
            char buffer[256];
            ssize_t size = recv(c->fd, buffer, sizeof(buffer), 0);
            if (size == 0 || (size < 0 && errno != EAGAIN && errno != EINTR)) {
                disconnect_client(i);
                continue;
            }
            for (ssize_t j = 0; j < size && c->fd >= 0; j++) {
                if (buffer[j] == '\n') {
                    c->input[c->used] = '\0';
                    receive_line(i, c->input);
                    c->used = 0;
                }
                else if (buffer[j] == '\0' || c->used == sizeof(c->input) - 1) {
                    disconnect_client(i);
                }
                else
                    c->input[c->used++] = buffer[j];
            }
        }
        if (c->fd >= 0 && (events & POLLOUT) && c->queued) {
            ssize_t sent = send(c->fd, c->output, c->queued, MSG_NOSIGNAL);
            if (sent > 0) {
                c->queued -= (size_t)sent;
                memmove(c->output, c->output + sent, c->queued);
            }
            else if (sent < 0 && errno != EAGAIN && errno != EINTR)
                disconnect_client(i);
        }
        if (c->fd >= 0 && !(events & POLLIN) &&
            (events & (POLLERR | POLLHUP | POLLNVAL)))
            disconnect_client(i);
    }
    if (listener >= 0 && (items[0].revents & POLLIN)) accept_clients();
}

static bool connected(int person) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].fd >= 0 && clients[i].role == ROLE_PERSON &&
            clients[i].person == person)
            return true;
    }
    return false;
}

static bool precedes(int candidate, int current) {
    if (current < 0) return true;
    struct Person* a = &model.persons[candidate];
    struct Person* b = &model.persons[current];
    if (model.priority && a->emergency != b->emergency) return a->emergency;
    if (a->arrival_ms != b->arrival_ms) return a->arrival_ms < b->arrival_ms;
    return a->id < b->id;
}

static void start_group(void) {
    int first = -1;
    int complete = 0;
    for (int i = 0; i < model.total; i++) {
        if (model.persons[i].request_state == REQUEST_DONE) complete++;
        if (model.persons[i].request_state == REQUEST_WAITING && connected(i) &&
            precedes(i, first))
            first = i;
    }
    if (complete == model.total) {
        finished = true;
        return;
    }
    if (first < 0) return;
    direction = model.persons[first].direction;
    int air_needed, energy_needed;
    if (!enough_resources(&model.chamber, direction, &air_needed,
                          &energy_needed)) {
        event(-1, "Insufficient resources: need air=%d energy=%d", air_needed,
              energy_needed);
        failed = true;
        return;
    }
    group_size = 0;
    int limit = 1;
    if (model.group_mode) limit = model.chamber.capacity;
    while (group_size < limit) {
        int selected = -1;
        for (int i = 0; i < model.total; i++) {
            struct Person* p = &model.persons[i];
            if (p->request_state == REQUEST_WAITING &&
                (int)p->direction == direction && connected(i) &&
                precedes(i, selected))
                selected = i;
        }
        if (selected < 0) break;
        group[group_size++] = selected;
        model.persons[selected].request_state = REQUEST_ACTIVE;
        event(selected, "Person %d selected for passage",
              model.persons[selected].id);
    }
    phase = PHASE_PREPARE;
    due = 0;
}

static bool device_action(int kind, int target) {
    struct Airlock* a = &model.chamber;
    if (kind == 2) return change_pressure(a, target);
    if (kind == 0) {
        if (target) return open_inner_door(a);
        return close_inner_door(a);
    }
    if (target) return open_outer_door(a);
    return close_outer_door(a);
}

static void movement(void) {
    bool all = true;
    for (int g = 0; g < group_size; g++) {
        int person = group[g];
        if (!pending[person]) {
            pending[person] = ++sequence;
            acknowledged[person] = false;
            for (int i = 0; i < MAX_CLIENTS; i++) {
                if (clients[i].fd >= 0 && clients[i].role == ROLE_PERSON &&
                    clients[i].person == person)
                    send_command(i, person);
            }
        }
        if (!acknowledged[person]) all = false;
    }
    if (!all) return;
    for (int g = 0; g < group_size; g++) {
        int person = group[g];
        struct Person* p = &model.persons[person];
        bool ok;
        if (phase == PHASE_BOARD) {
            if (direction == DIRECTION_TO_SPACE)
                ok = enter_from_station(&model.chamber, p);
            else
                ok = enter_from_space(&model.chamber, p);
        }
        else {
            if (direction == DIRECTION_TO_SPACE)
                ok = exit_to_space(&model.chamber, p);
            else
                ok = exit_to_station(&model.chamber, p);
        }
        if (!ok) {
            failed = true;
            return;
        }
        pending[person] = 0;
        event(person, "Person %d moved; location=%s inside=%d", p->id,
              location_name(p->location), model.chamber.people_inside);
    }
    phase++;
}

static void simulation_step(void) {
    for (int i = 0; i < model.total; i++) {
        struct Person* p = &model.persons[i];
        if (p->request_state == REQUEST_NOT_ARRIVED && connected(i) &&
            now_ms() - began >= p->arrival_ms) {
            p->request_state = REQUEST_WAITING;
            event(i, "Person %d requested %s emergency=%d", p->id,
                  direction_name(p->direction), p->emergency);
        }
    }
    if (phase == PHASE_IDLE) {
        start_group();
        return;
    }
    if (phase == PHASE_BOARD || phase == PHASE_EXIT) {
        movement();
        return;
    }
    if (phase == PHASE_COMPLETE) {
        for (int g = 0; g < group_size; g++) {
            int person = group[g];
            model.persons[person].request_state = REQUEST_DONE;
            event(person, "Person %d completed passage",
                  model.persons[person].id);
            for (int i = 0; i < MAX_CLIENTS; i++) {
                if (clients[i].role == ROLE_PERSON &&
                    clients[i].person == person)
                    queue_line(i, "DONE\n");
            }
        }
        phase = PHASE_IDLE;
        return;
    }
    int entry_door = 0, exit_door = 1;
    int entry_pressure = PRESSURE_NORMAL, exit_pressure = PRESSURE_LOW;
    if (direction == DIRECTION_TO_STATION) {
        entry_door = 1;
        exit_door = 0;
        entry_pressure = PRESSURE_LOW;
        exit_pressure = PRESSURE_NORMAL;
    }
    int kind = 2, target = entry_pressure;
    if (phase == PHASE_OPEN_ENTRY) {
        kind = entry_door;
        target = 1;
    }
    if (phase == PHASE_CLOSE_ENTRY) {
        kind = entry_door;
        target = 0;
    }
    if (phase == PHASE_CHANGE_PRESSURE) target = exit_pressure;
    if (phase == PHASE_OPEN_EXIT) {
        kind = exit_door;
        target = 1;
    }
    if (phase == PHASE_CLOSE_EXIT) {
        kind = exit_door;
        target = 0;
    }
    int current = model.chamber.pressure.value;
    if (kind == 0) current = model.chamber.inner.open;
    if (kind == 1) current = model.chamber.outer.open;
    if (current == target) {
        phase++;
        return;
    }
    if (!due) {
        int duration = model.chamber.inner.duration;
        if (kind == 2) duration = model.chamber.pressure.duration;
        due = now_ms() + duration;
        event(-1, "%s started; target=%d", device_name(kind), target);
        return;
    }
    if (now_ms() < due) return;
    due = 0;
    if (rand() % 100 < model.failure_rate || !device_action(kind, target)) {
        event(-1, "%s failed", device_name(kind));
        failed = true;
        return;
    }
    event(-1, "%s completed; target=%d air=%d energy=%d", device_name(kind),
          target, model.chamber.air, model.chamber.energy);
    phase++;
}

int main(int argc, char** argv) {
    if (argc != 5) {
        fprintf(stderr, "Usage: %s IP PORT input.txt new.log\n", argv[0]);
        return 1;
    }
    setup_signals();
    if (!read_input(argv[3])) {
        fprintf(stderr, "Invalid input\n");
        return 1;
    }
    model.chamber.pressure.value = PRESSURE_NORMAL;
    listener = open_socket(argv[1], argv[2], true);
    if (listener < 0) {
        perror("Listen");
        return 1;
    }
    log_fd = open(argv[4], O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (log_fd < 0) {
        perror("Log");
        close(listener);
        return 1;
    }
    for (int i = 0; i < MAX_CLIENTS; i++) clients[i].fd = -1;
    srand(model.random_seed);
    began = now_ms();
    printf("Listening on %s:%s\n", argv[1], argv[2]);
    fflush(stdout);
    while (!stopped && !finished && !failed) {
        network_step();
        if (stopped) break;
        simulation_step();
        if (!is_airlock_safe(model.chamber)) failed = true;
    }
    close(listener);
    listener = -1;
    bool closed = close_inner_door(&model.chamber);
    closed = close_outer_door(&model.chamber) && closed;
    if (!closed) failed = true;
    int completed = 0;
    for (int i = 0; i < model.total; i++) {
        if (model.persons[i].request_state == REQUEST_ACTIVE)
            model.persons[i].request_state = REQUEST_INTERRUPTED;
        if (model.persons[i].request_state == REQUEST_DONE) completed++;
        event(i, "Person %d final: location=%s status=%s", model.persons[i].id,
              location_name(model.persons[i].location),
              status_name(model.persons[i].request_state));
    }
    event(-1,
          "Final: completed=%d/%d stopped=%d failed=%d safe=%d inside=%d "
          "doors=%d,%d",
          completed, model.total, stopped != 0, failed,
          is_airlock_safe(model.chamber), model.chamber.people_inside,
          model.chamber.inner.open, model.chamber.outer.open);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].fd < 0) continue;
        if (clients[i].role == ROLE_OBSERVER) snapshot(i);
        queue_line(i, "STOP\n");
    }
    long long deadline = now_ms() + 2000;
    while (now_ms() < deadline) {
        bool queued = false;
        for (int i = 0; i < MAX_CLIENTS; i++)
            if (clients[i].fd >= 0 && clients[i].queued) queued = true;
        if (!queued) break;
        network_step();
    }
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (clients[i].fd >= 0) disconnect_client(i);
    close(log_fd);
    return failed || stopped ? 1 : 0;
}
