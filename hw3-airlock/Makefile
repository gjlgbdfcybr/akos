CC = gcc
CPPFLAGS = -D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700
CFLAGS = -std=c17 -Wall -Wextra -Wpedantic -O2
LDLIBS = -pthread -lrt
SOURCES = main.c airlock.c dispatcher.c config.c output.c ipc.c lifecycle.c
HEADERS = shared.h airlock.h person.h

.PHONY: all clean
all: airlock_unnamed airlock_named controller worker

airlock_unnamed: $(SOURCES) $(HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) -DMODE=0 $(SOURCES) -o $@ $(LDLIBS)
airlock_named: $(SOURCES) $(HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) -DMODE=1 $(SOURCES) -o $@ $(LDLIBS)
controller: $(SOURCES) $(HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) -DMODE=2 $(SOURCES) -o $@ $(LDLIBS)
worker: $(SOURCES) $(HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) -DMODE=3 $(SOURCES) -o $@ $(LDLIBS)

clean:
	rm -f airlock_unnamed airlock_named controller worker
