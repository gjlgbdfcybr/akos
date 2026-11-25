#ifndef NET_H
#define NET_H
#include <signal.h>
#include <stdbool.h>
extern volatile sig_atomic_t stopped;
void setup_signals(void);
long long now_ms(void);
bool number(const char* text, int minimum, int maximum, int* value);
int open_socket(const char* ip, const char* port, bool server);
bool send_line(int fd, const char* line);
#endif
