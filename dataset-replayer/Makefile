CC = gcc
CPPFLAGS = -Iinclude
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic
TARGET = build/dataset-replayer
TEST_TARGET = build/test_csv_reader
TELEMETRY_TEST_TARGET = build/test_telemetry
JSON_TEST_TARGET = build/test_telemetry_json
MQTT_TEST_TARGET = build/test_mqtt_publisher
MQTT_CPPFLAGS = $(shell pkg-config --cflags libmosquitto)
MQTT_LDLIBS = $(shell pkg-config --libs libmosquitto)

.PHONY: all run test test-mqtt clean

all: $(TARGET) build/telemetry_json.o build/mqtt_publisher.o

$(TARGET): src/main.c src/csv_reader.c src/telemetry.c build/telemetry_json.o build/mqtt_publisher.o include/csv_reader.h include/telemetry.h include/telemetry_json.h include/mqtt_publisher.h Makefile | build
	$(CC) $(CPPFLAGS) $(MQTT_CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ src/main.c src/csv_reader.c src/telemetry.c build/telemetry_json.o build/mqtt_publisher.o $(MQTT_LDLIBS) $(LDLIBS)

build/telemetry_json.o: src/telemetry_json.c include/telemetry_json.h include/telemetry.h include/csv_reader.h Makefile | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ src/telemetry_json.c

$(TEST_TARGET): tests/test_csv_reader.c src/csv_reader.c include/csv_reader.h Makefile | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ tests/test_csv_reader.c src/csv_reader.c $(LDLIBS)

$(TELEMETRY_TEST_TARGET): tests/test_telemetry.c src/telemetry.c include/telemetry.h include/csv_reader.h Makefile | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ tests/test_telemetry.c src/telemetry.c $(LDLIBS)

$(JSON_TEST_TARGET): tests/test_telemetry_json.c src/telemetry_json.c src/telemetry.c include/telemetry_json.h include/telemetry.h include/csv_reader.h Makefile | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ tests/test_telemetry_json.c src/telemetry_json.c src/telemetry.c $(LDLIBS)

build/mqtt_publisher.o: src/mqtt_publisher.c include/mqtt_publisher.h Makefile | build
	$(CC) $(CPPFLAGS) $(MQTT_CPPFLAGS) $(CFLAGS) -c -o $@ src/mqtt_publisher.c

$(MQTT_TEST_TARGET): tests/test_mqtt_publisher.c build/mqtt_publisher.o include/mqtt_publisher.h Makefile | build
	$(CC) $(CPPFLAGS) $(MQTT_CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ tests/test_mqtt_publisher.c build/mqtt_publisher.o $(MQTT_LDLIBS) $(LDLIBS)

test-mqtt: $(MQTT_TEST_TARGET) $(TARGET)
	python3 tests/test_mqtt_integration.py

test: $(MQTT_TEST_TARGET) $(TEST_TARGET) $(TELEMETRY_TEST_TARGET) $(JSON_TEST_TARGET) $(TARGET)
	./$(TEST_TARGET)
	./$(TELEMETRY_TEST_TARGET)
	./$(JSON_TEST_TARGET)
	sh tests/test_cli.sh
	./$(MQTT_TEST_TARGET)

build:
	mkdir -p $@

run: $(TARGET)
	./$(TARGET) $(ARGS)

clean:
	rm -rf build
