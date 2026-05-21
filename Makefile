CC ?= musl-gcc
CFLAGS ?= -static -Os -s -Wall -Wextra

TARGET = delayme
SRC = src/delayme.c

VERSION := $(shell cat VERSION)

all:
	$(CC) $(CFLAGS) $(SRC) -DVERSION=\"$(VERSION)\" -o $(TARGET)

debug:
	$(CC) -g -O0 $(SRC) -o $(TARGET)

clean:
	rm -f $(TARGET)