CC      = gcc
CFLAGS  = -O2 -Wall -Wextra -std=gnu11
LDFLAGS = -lpthread -lm
TARGET  = edge_collector

$(TARGET): edge_collector.c
	$(CC) $(CFLAGS) -o $(TARGET) edge_collector.c $(LDFLAGS)

clean:
	rm -f $(TARGET)

.PHONY: clean
