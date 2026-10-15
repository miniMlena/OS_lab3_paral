CC = gcc
CFLAGS = -O2 -Wall -Wextra -Wpedantic -std=c11
LDFLAGS = -pthread

TARGET = main

.PHONY: all clean

all: $(TARGET)

$(TARGET): main.c
	$(CC) $(CFLAGS) main.c -o $(TARGET) $(LDFLAGS)

clean:
	rm -f $(TARGET)
