CC = gcc
CPPFLAGS = -Iinclude
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic
TARGET = build/dataset-replayer
TEST_TARGET = build/test_csv_reader
TELEMETRY_TEST_TARGET = build/test_telemetry
JSON_TEST_TARGET = build/test_telemetry_json

.PHONY: all run test clean

all: $(TARGET) build/telemetry_json.o

$(TARGET): src/main.c src/csv_reader.c src/telemetry.c include/csv_reader.h include/telemetry.h Makefile | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ src/main.c src/csv_reader.c src/telemetry.c $(LDLIBS)

build/telemetry_json.o: src/telemetry_json.c include/telemetry_json.h include/telemetry.h include/csv_reader.h Makefile | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ src/telemetry_json.c

$(TEST_TARGET): tests/test_csv_reader.c src/csv_reader.c include/csv_reader.h Makefile | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ tests/test_csv_reader.c src/csv_reader.c $(LDLIBS)

$(TELEMETRY_TEST_TARGET): tests/test_telemetry.c src/telemetry.c include/telemetry.h include/csv_reader.h Makefile | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ tests/test_telemetry.c src/telemetry.c $(LDLIBS)

$(JSON_TEST_TARGET): tests/test_telemetry_json.c src/telemetry_json.c src/telemetry.c include/telemetry_json.h include/telemetry.h include/csv_reader.h Makefile | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ tests/test_telemetry_json.c src/telemetry_json.c src/telemetry.c $(LDLIBS)

test: $(TEST_TARGET) $(TELEMETRY_TEST_TARGET) $(JSON_TEST_TARGET) $(TARGET)
	./$(TEST_TARGET)
	./$(TELEMETRY_TEST_TARGET)
	./$(JSON_TEST_TARGET)
	sh tests/test_cli.sh

build:
	mkdir -p $@

run: $(TARGET)
	./$(TARGET) $(ARGS)

clean:
	rm -rf build
