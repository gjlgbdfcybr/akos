#include "net.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

volatile sig_atomic_t stopped;

static void stop_handler(int signal_number) {
    (void)signal_number;
    stopped = 1;
}

void setup_signals(void) {
    struct sigaction action = {0};
    action.sa_handler = stop_handler;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);
    signal(SIGPIPE, SIG_IGN);
}

long long now_ms(void) {
    struct timespec time;
    clock_gettime(CLOCK_MONOTONIC, &time);
    return (long long)time.tv_sec * 1000 + time.tv_nsec / 1000000;
}

bool number(const char* text, int minimum, int maximum, int* value) {
    char* end;
    errno = 0;
    long result = strtol(text, &end, 10);
    if (errno || text == end || *end || result < minimum || result > maximum)
        return false;
    *value = (int)result;
    return true;
}

int open_socket(const char* ip, const char* port, bool server) {
    int port_number;
    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    if (!number(port, 1, 65535, &port_number) ||
        inet_pton(AF_INET, ip, &address.sin_addr) != 1) {
        errno = EINVAL;
        return -1;
    }
    address.sin_port = htons((unsigned short)port_number);
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    if (fcntl(fd, F_SETFL, O_NONBLOCK) < 0) {
        close(fd);
        return -1;
    }
    if (server) {
        int enabled = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
        if (bind(fd, (struct sockaddr*)&address, sizeof(address)) == 0 &&
            listen(fd, 128) == 0)
            return fd;
    }
    else {
        int result = connect(fd, (struct sockaddr*)&address, sizeof(address));
        if (result == 0) return fd;
        if (errno == EINPROGRESS) {
            long long deadline = now_ms() + 5000;
            while (!stopped && now_ms() < deadline) {
                struct pollfd item = {fd, POLLOUT, 0};
                if (poll(&item, 1, 100) <= 0) continue;
                int error = 0;
                socklen_t size = sizeof(error);
                if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) < 0)
                    break;
                if (error == 0) return fd;
                errno = error;
                break;
            }
            if (stopped)
                errno = EINTR;
            else if (now_ms() >= deadline)
                errno = ETIMEDOUT;
        }
    }
    close(fd);
    return -1;
}

bool send_line(int fd, const char* line) {
    size_t offset = 0;
    size_t size = strlen(line);
    long long deadline = now_ms() + 2000;
    while (offset < size && !stopped && now_ms() < deadline) {
        ssize_t sent = send(fd, line + offset, size - offset, MSG_NOSIGNAL);
        if (sent > 0)
            offset += (size_t)sent;
        else if (sent < 0 &&
                 (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            struct pollfd item = {fd, POLLOUT, 0};
            poll(&item, 1, 100);
        }
        else
            return false;
    }
    return offset == size;
}
