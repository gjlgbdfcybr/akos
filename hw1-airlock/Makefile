CC = gcc
CPPFLAGS = -D_POSIX_C_SOURCE=200809L
CFLAGS = -std=c17 -Wall -Wextra -Wpedantic -O2

airlock: main.c airlock.c dispatcher.c output.c person.h airlock.h dispatcher.h output.h
	$(CC) $(CPPFLAGS) $(CFLAGS) main.c airlock.c dispatcher.c output.c -o airlock

.PHONY: clean
clean:
	rm -f airlock
