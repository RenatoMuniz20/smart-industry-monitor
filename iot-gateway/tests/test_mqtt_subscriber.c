#define _POSIX_C_SOURCE 200809L
#include "mqtt_subscriber.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static void message(const char *topic, const void *payload, size_t length, void *context)
{
    int *count = context;
    const unsigned char *bytes = payload;
    assert(topic != NULL && (payload != NULL || length == 0));
    ++*count;
    printf("MSG %s %zu ", topic, length);
    for (size_t i = 0; i < length; ++i) printf("%02x", bytes[i]);
    putchar('\n');
    fflush(stdout);
}

static double monotonic(void)
{
    struct timespec now;
    assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
    return now.tv_sec + now.tv_nsec / 1e9;
}

static void local_tests(void)
{
    MqttSubscriber *s = NULL;
    int count = 0;
    assert(mqtt_subscriber_connect(NULL, "127.0.0.1", 1883, "test", 500, message, &count) == -1);
    assert(mqtt_subscriber_connect(&s, NULL, 1883, "test", 500, message, &count) == -1);
    assert(mqtt_subscriber_connect(&s, "", 1883, "test", 500, message, &count) == -1);
    assert(mqtt_subscriber_connect(&s, "127.0.0.1", 0, "test", 500, message, &count) == -1);
    assert(mqtt_subscriber_connect(&s, "127.0.0.1", 65536, "test", 500, message, &count) == -1);
    assert(mqtt_subscriber_connect(&s, "127.0.0.1", 1883, NULL, 500, message, &count) == -1);
    assert(mqtt_subscriber_connect(&s, "127.0.0.1", 1883, "", 500, message, &count) == -1);
    assert(mqtt_subscriber_connect(&s, "127.0.0.1", 1883, "test", 0, message, &count) == -1);
    assert(mqtt_subscriber_connect(&s, "127.0.0.1", 1883, "test", 5001, message, &count) == -1);
    assert(mqtt_subscriber_connect(&s, "127.0.0.1", 1883, "test", 500, NULL, &count) == -1);
    assert(mqtt_subscriber_last_error() == MQTT_SUBSCRIBER_ARGUMENT && s == NULL);
    assert(mqtt_subscriber_poll(NULL, 100) == -1);
    assert(mqtt_subscriber_close(NULL) == 0);
    assert(mqtt_subscriber_close(&s) == 0);
    assert(strcmp(mqtt_subscriber_error_string((MqttSubscriberError)-1), "Erro desconhecido") == 0);
    for (int i = MQTT_SUBSCRIBER_OK; i <= MQTT_SUBSCRIBER_CLOSE; ++i)
        assert(mqtt_subscriber_error_string((MqttSubscriberError)i)[0] != '\0');
    assert(count == 0);
}

int main(int argc, char **argv)
{
    local_tests();
    if (argc == 1) {
        puts("Todos os testes locais de mqtt_subscriber passaram.");
        return 0;
    }
    assert(argc == 3);
    int port = atoi(argv[1]);
    const char *mode = argv[2];
    MqttSubscriber *s = NULL;
    int count = 0;
    char id[64];
    snprintf(id, sizeof id, "sim-subscriber-test-%jd", (intmax_t)getpid());
    MqttSubscriberError expected = MQTT_SUBSCRIBER_OK;
    if (strcmp(mode, "unavailable") == 0) expected = MQTT_SUBSCRIBER_CONNECT;
    else if (strcmp(mode, "reject") == 0) expected = MQTT_SUBSCRIBER_CONNACK;
    else if (strcmp(mode, "timeout") == 0) expected = MQTT_SUBSCRIBER_TIMEOUT;
    else if (strcmp(mode, "suback") == 0) expected = MQTT_SUBSCRIBER_SUBACK;
    if (expected != MQTT_SUBSCRIBER_OK) {
        /* Retry explicitly after a failed open: proves active/library cleanup. */
        for (int i = 0; i < 2; ++i) {
            assert(mqtt_subscriber_connect(&s, "127.0.0.1", port, id, 500,
                                           message, &count) == -1);
            assert(s == NULL && mqtt_subscriber_last_error() == expected);
        }
        assert(count == 0);
        puts("EXPECTED_ERROR");
        return 0;
    }
    assert(mqtt_subscriber_connect(&s, "127.0.0.1", port, id, 1000, message, &count) == 0);
    MqttSubscriber *other = NULL;
    assert(mqtt_subscriber_connect(&other, "127.0.0.1", port, "second", 500, message, &count) == -1);
    assert(other == NULL && mqtt_subscriber_last_error() == MQTT_SUBSCRIBER_ACTIVE);
    assert(mqtt_subscriber_connect(&s, "127.0.0.1", port, id, 500, message, &count) == -1);
    assert(mqtt_subscriber_last_error() == MQTT_SUBSCRIBER_ARGUMENT);
    assert(mqtt_subscriber_poll(s, -1) == -1);
    assert(mqtt_subscriber_poll(s, 5001) == -1);
    assert(mqtt_subscriber_poll(s, 20) == 0); /* idle polling is successful */
    puts("READY");
    fflush(stdout);
    double deadline = monotonic() + 10;
    int rc = 0;
    while (monotonic() < deadline) {
        rc = mqtt_subscriber_poll(s, 100);
        if (rc != 0 || (strcmp(mode, "receive") == 0 && count == 3)) break;
    }
    if (strcmp(mode, "disconnect") == 0) {
        assert(rc == -1 && count == 0);
        assert(mqtt_subscriber_last_error() == MQTT_SUBSCRIBER_DISCONNECTED ||
               mqtt_subscriber_last_error() == MQTT_SUBSCRIBER_NETWORK);
        assert(mqtt_subscriber_poll(s, 0) == -1);
    } else {
        assert(strcmp(mode, "receive") == 0 && rc == 0 && count == 3);
    }
    assert(mqtt_subscriber_close(&s) == 0 && s == NULL);
    assert(mqtt_subscriber_close(&s) == 0);
    if (strcmp(mode, "receive") == 0) {
        assert(mqtt_subscriber_connect(&s, "127.0.0.1", port, id, 1000, message, &count) == 0);
        assert(mqtt_subscriber_close(&s) == 0 && s == NULL);
    }
    puts("DONE");
    return 0;
}
