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

.PHONY: all test test-mqtt test-e2e clean

all: build/iot-gateway

build/iot-gateway: src/main.c $(TARGET) build/mqtt_subscriber.o include/gateway_telemetry.h include/mqtt_subscriber.h Makefile | build
	$(CC) $(CPPFLAGS) $(JSON_CPPFLAGS) $(MQTT_CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ src/main.c $(TARGET) build/mqtt_subscriber.o $(JSON_LDLIBS) $(MQTT_LDLIBS) $(LDLIBS)

$(TARGET): src/gateway_telemetry.c include/gateway_telemetry.h Makefile | build
	$(CC) $(CPPFLAGS) $(JSON_CPPFLAGS) $(CFLAGS) -c -o $@ src/gateway_telemetry.c

$(TEST_TARGET): tests/test_gateway_telemetry.c $(TARGET) include/gateway_telemetry.h Makefile | build
	$(CC) $(CPPFLAGS) $(JSON_CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ tests/test_gateway_telemetry.c $(TARGET) $(JSON_LDLIBS) $(LDLIBS)

build/mqtt_subscriber.o: src/mqtt_subscriber.c include/mqtt_subscriber.h Makefile | build
	$(CC) $(CPPFLAGS) $(MQTT_CPPFLAGS) $(CFLAGS) -c -o $@ src/mqtt_subscriber.c

$(MQTT_TEST_TARGET): tests/test_mqtt_subscriber.c build/mqtt_subscriber.o include/mqtt_subscriber.h Makefile | build
	$(CC) $(CPPFLAGS) $(MQTT_CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ tests/test_mqtt_subscriber.c build/mqtt_subscriber.o $(MQTT_LDLIBS) $(LDLIBS)

test-mqtt: $(MQTT_TEST_TARGET) build/iot-gateway
	python3 -B tests/test_mqtt_integration.py
	python3 -B tests/test_cli.py --mqtt

test-e2e: build/iot-gateway
	$(MAKE) -C ../dataset-replayer all
	python3 -B tests/test_end_to_end.py

test: $(TEST_TARGET) $(MQTT_TEST_TARGET) build/iot-gateway
	./$(TEST_TARGET)
	./$(MQTT_TEST_TARGET)
	python3 -B tests/test_cli.py

build:
	mkdir -p $@

clean:
	rm -rf build
