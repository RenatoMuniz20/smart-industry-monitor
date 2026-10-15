CC = gcc
CPPFLAGS = -Iinclude
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic
JSON_CPPFLAGS = $(shell pkg-config --cflags json-c)
JSON_LDLIBS = $(shell pkg-config --libs json-c)
TARGET = build/gateway_telemetry.o
TEST_TARGET = build/test_gateway_telemetry

.PHONY: all test clean

all: $(TARGET)

$(TARGET): src/gateway_telemetry.c include/gateway_telemetry.h Makefile | build
	$(CC) $(CPPFLAGS) $(JSON_CPPFLAGS) $(CFLAGS) -c -o $@ src/gateway_telemetry.c

$(TEST_TARGET): tests/test_gateway_telemetry.c $(TARGET) include/gateway_telemetry.h Makefile | build
	$(CC) $(CPPFLAGS) $(JSON_CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ tests/test_gateway_telemetry.c $(TARGET) $(JSON_LDLIBS) $(LDLIBS)

test: $(TEST_TARGET)
	./$(TEST_TARGET)

build:
	mkdir -p $@

clean:
	rm -rf build
