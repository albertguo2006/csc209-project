PORT ?= 4242

CC      = gcc
CFLAGS  = -Wall -Wextra -g -DPORT=$(PORT)

.PHONY: all clean

all: server client

server: server.c game.c game.h protocol.h
	$(CC) $(CFLAGS) -o server server.c game.c -lm

client: client.c protocol.h
	$(CC) $(CFLAGS) -o client client.c

clean:
	rm -f server client
