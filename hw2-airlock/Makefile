CC = gcc
CFLAGS = -D_POSIX_C_SOURCE=200809L -std=c17 -Wall -Wextra -Wpedantic -O2 -pthread

airlock: main.c airlock.c dispatcher.c config.c output.c person.h airlock.h dispatcher.h
	$(CC) $(CFLAGS) main.c airlock.c dispatcher.c config.c output.c -o airlock

.PHONY: clean
clean:
	rm -f airlock
