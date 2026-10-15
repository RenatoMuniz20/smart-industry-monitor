CC = gcc
CPPFLAGS = -Iinclude
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic
JSON_CPPFLAGS = $(shell pkg-config --cflags json-c)
JSON_LDLIBS = $(shell pkg-config --libs json-c)
MQTT_CPPFLAGS = $(shell pkg-config --cflags libmosquitto)
MQTT_LDLIBS = $(shell pkg-config --libs libmosquitto)
MQTT_TEST_TARGET = build/test_mqtt_subscriber
TARGET = build/gateway_telemetry.o
TEST_TARGET = build/test_gateway_telemetry

.PHONY: all test test-mqtt clean

all: $(TARGET) build/mqtt_subscriber.o

$(TARGET): src/gateway_telemetry.c include/gateway_telemetry.h Makefile | build
	$(CC) $(CPPFLAGS) $(JSON_CPPFLAGS) $(CFLAGS) -c -o $@ src/gateway_telemetry.c

$(TEST_TARGET): tests/test_gateway_telemetry.c $(TARGET) include/gateway_telemetry.h Makefile | build
	$(CC) $(CPPFLAGS) $(JSON_CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ tests/test_gateway_telemetry.c $(TARGET) $(JSON_LDLIBS) $(LDLIBS)

build/mqtt_subscriber.o: src/mqtt_subscriber.c include/mqtt_subscriber.h Makefile | build
	$(CC) $(CPPFLAGS) $(MQTT_CPPFLAGS) $(CFLAGS) -c -o $@ src/mqtt_subscriber.c

$(MQTT_TEST_TARGET): tests/test_mqtt_subscriber.c build/mqtt_subscriber.o include/mqtt_subscriber.h Makefile | build
	$(CC) $(CPPFLAGS) $(MQTT_CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ tests/test_mqtt_subscriber.c build/mqtt_subscriber.o $(MQTT_LDLIBS) $(LDLIBS)

test-mqtt: $(MQTT_TEST_TARGET)
	python3 -B tests/test_mqtt_integration.py

test: $(TEST_TARGET) $(MQTT_TEST_TARGET)
	./$(TEST_TARGET)
	./$(MQTT_TEST_TARGET)

build:
	mkdir -p $@

clean:
	rm -rf build
