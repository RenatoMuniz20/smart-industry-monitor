CC = gcc
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -Iinclude -I/usr/include/json-c
LDLIBS = -lmicrohttpd -lcurl -ljson-c

BUILD = build
BIN = $(BUILD)/backend
TEST_QUERY = $(BUILD)/test_interscity_query
SRCS = src/main.c src/http_server.c src/routes.c src/machine_map.c src/interscity_query.c
QUERY_SRCS = src/interscity_query.c
HDRS = include/http_server.h include/routes.h include/machine_map.h include/interscity_query.h

.PHONY: all test clean

all: $(BIN)

$(BIN): $(SRCS) $(HDRS)
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(LDLIBS)

$(TEST_QUERY): tests/test_interscity_query.c $(QUERY_SRCS) include/interscity_query.h
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ tests/test_interscity_query.c $(QUERY_SRCS) $(LDLIBS)

test: $(BIN) $(TEST_QUERY)
	python3 -B tests/test_backend.py $(BIN)
	python3 -B tests/test_interscity_query.py $(TEST_QUERY)
	python3 -B tests/test_machines_latest.py $(BIN)

clean:
	rm -rf $(BUILD)
