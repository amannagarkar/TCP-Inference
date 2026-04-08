CC      = gcc
CFLAGS  = -O2 -Wall -Wextra -std=c99
LDFLAGS =

# pthreads only needed for server
SERVER_LDFLAGS = -lpthread
CLIENT_LDFLAGS = -lm

TARGETS = server client

.PHONY: all clean results

all: $(TARGETS)

server: server.c
	$(CC) $(CFLAGS) -o $@ $< $(SERVER_LDFLAGS)

client: client.c
	$(CC) $(CFLAGS) -o $@ $< $(CLIENT_LDFLAGS)

results:
	mkdir -p results/csv results/plots

clean:
	rm -f $(TARGETS)
