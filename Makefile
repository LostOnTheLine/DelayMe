CC ?= musl-gcc

CFLAGS ?= -Os -s -Wall -Wextra
LDFLAGS ?= -static

TARGET = delayme
SRC = src/delayme.c

VERSION := $(shell cat VERSION)

export SOURCE_DATE_EPOCH := 0

all:
	$(CC) $(CFLAGS) $(LDFLAGS) \
	$(SRC) \
	-DVERSION=\"$(VERSION)\" \
	-o $(TARGET)

debug:
	$(CC) -g -O0 $(SRC) -o $(TARGET)

clean:
	rm -f $(TARGET)