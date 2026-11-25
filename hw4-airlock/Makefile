CC = gcc
CPPFLAGS = -D_POSIX_C_SOURCE=200809L
CFLAGS = -std=c17 -Wall -Wextra -Wpedantic -O2

.PHONY: all clean
all: server client observer

server: server.c config.c airlock.c net.c model.h airlock.h person.h net.h
	$(CC) $(CPPFLAGS) $(CFLAGS) server.c config.c airlock.c net.c -o server

client: client.c net.c net.h
	$(CC) $(CPPFLAGS) $(CFLAGS) client.c net.c -o client

observer: client.c net.c net.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -DOBSERVER=1 client.c net.c -o observer

clean:
	rm -f server client observer
