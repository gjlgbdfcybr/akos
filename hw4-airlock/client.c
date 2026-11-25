#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "net.h"

#ifndef OBSERVER
#define OBSERVER 0
#endif

static int process_line(int fd, const char* line) {
    printf("%s\n", line);
    fflush(stdout);
    if (strcmp(line, "STOP") == 0 || strcmp(line, "DONE") == 0) return 1;
    if (!OBSERVER && strncmp(line, "COMMAND ", 8) == 0) {
        int sequence;
        char action[16], extra;
        if (sscanf(line + 8, "%d %15s %c", &sequence, action, &extra) != 2 ||
            sequence < 1)
            return -1;
        if (strcmp(action, "BOARD") != 0 && strcmp(action, "EXIT") != 0)
            return -1;
        char reply[64];
        snprintf(reply, sizeof(reply), "ACK %d\n", sequence);
        if (!send_line(fd, reply)) return -1;
    }
    return 0;
}

int main(int argc, char** argv) {
    int expected = 4;
    if (OBSERVER) expected = 3;
    if (argc != expected) {
        fprintf(stderr, "Usage: %s IP PORT", argv[0]);
        if (!OBSERVER) fprintf(stderr, " PERSON_ID");
        fprintf(stderr, "\n");
        return 1;
    }
    char hello[64];
    if (OBSERVER)
        strcpy(hello, "WATCH\n");
    else {
        int id;
        if (!number(argv[3], 1, 2147483647, &id)) return 1;
        snprintf(hello, sizeof(hello), "PERSON %d\n", id);
    }
    setup_signals();
    int fd = open_socket(argv[1], argv[2], false);
    if (fd < 0) {
        perror("Connect");
        return 1;
    }
    if (!send_line(fd, hello)) {
        close(fd);
        return 1;
    }
    char line[2048];
    size_t used = 0;
    int result = 0;
    bool ended = false;
    while (!stopped && !ended) {
        struct pollfd item = {fd, POLLIN, 0};
        if (poll(&item, 1, 100) < 0) {
            if (errno == EINTR) continue;
            result = 1;
            break;
        }
        if (item.revents & POLLIN) {
            char buffer[1024];
            ssize_t size = recv(fd, buffer, sizeof(buffer), 0);
            if (size == 0) {
                fprintf(stderr, "Server disconnected\n");
                result = 1;
                break;
            }
            if (size < 0) {
                if (errno == EAGAIN || errno == EINTR) continue;
                result = 1;
                break;
            }
            for (ssize_t i = 0; i < size && !ended; i++) {
                if (buffer[i] == '\n') {
                    line[used] = '\0';
                    int status = process_line(fd, line);
                    used = 0;
                    if (status) {
                        ended = true;
                        result = status < 0;
                    }
                }
                else if (used == sizeof(line) - 1 || buffer[i] == '\0') {
                    ended = true;
                    result = 1;
                }
                else
                    line[used++] = buffer[i];
            }
        }
        if (!ended && !(item.revents & POLLIN) &&
            (item.revents & (POLLERR | POLLHUP | POLLNVAL))) {
            ended = true;
            result = 1;
        }
    }
    close(fd);
    return result;
}
