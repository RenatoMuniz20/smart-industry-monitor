CC = gcc
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -Iinclude
LDLIBS = -lmicrohttpd

BUILD = build
BIN = $(BUILD)/backend
SRCS = src/main.c src/http_server.c

.PHONY: all test clean

all: $(BIN)

$(BIN): $(SRCS) include/http_server.h
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(LDLIBS)

test: $(BIN)
	python3 -B tests/test_backend.py $(BIN)

clean:
	rm -rf $(BUILD)
