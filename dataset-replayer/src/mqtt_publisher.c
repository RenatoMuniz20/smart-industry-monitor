#define _POSIX_C_SOURCE 200809L
#include "mqtt_publisher.h"

#include <limits.h>
#include <mosquitto.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct MqttPublisher {
    struct mosquitto *client;
    int timeout_ms;
    int connected;
    int failed;
    int pending_mid;
    int acknowledged;
};

static int active;

static int64_t milliseconds(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return -1;
    }
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static void on_connect(struct mosquitto *client, void *context, int code)
{
    MqttPublisher *p = context;
    (void)client;
    p->connected = code == 0;
    p->failed = code != 0;
}

static void on_disconnect(struct mosquitto *client, void *context, int code)
{
    MqttPublisher *p = context;
    (void)client;
    (void)code;
    p->connected = 0;
    p->failed = 1;
}

static void on_publish(struct mosquitto *client, void *context, int mid)
{
    MqttPublisher *p = context;
    (void)client;
    if (mid == p->pending_mid) {
        p->acknowledged = 1;
    }
}

static int wait_for(MqttPublisher *p, const int *done, int64_t deadline)
{
    while (!*done && !p->failed) {
        int64_t now = milliseconds();
        if (now < 0 || now >= deadline) {
            break;
        }
        int remaining = (int)(deadline - now);
        /* Blocking event processing, with bounded slices; no retry/reconnect. */
        if (mosquitto_loop(p->client, remaining < 100 ? remaining : 100, 1)
            != MOSQ_ERR_SUCCESS) {
            p->failed = 1;
        }
    }
    if (!*done || p->failed) {
        p->failed = 1;
        return -1;
    }
    return 0;
}

int mqtt_publisher_topic(int machine_id, char *buffer, size_t capacity)
{
    char topic[64];
    if (machine_id <= 0 || buffer == NULL) {
        return -1;
    }
    int length = snprintf(topic, sizeof topic,
                          "smart-industry/machines/%d/telemetry", machine_id);
    if (length < 0 || (size_t)length >= sizeof topic || (size_t)length >= capacity) {
        return -1;
    }
    memcpy(buffer, topic, (size_t)length + 1);
    return 0;
}

int mqtt_publisher_close(MqttPublisher **publisher)
{
    int result = 0;
    if (publisher == NULL || *publisher == NULL) {
        return 0;
    }
    MqttPublisher *p = *publisher;
    if (p->connected && !p->failed) {
        if (mosquitto_disconnect(p->client) != MOSQ_ERR_SUCCESS) {
            result = -1;
        } else {
            /* Flush queued DISCONNECT without waiting for a peer response. */
            int rc = mosquitto_loop(p->client, 0, 1);
            if (rc != MOSQ_ERR_SUCCESS && rc != MOSQ_ERR_NO_CONN) {
                result = -1;
            }
        }
    }
    mosquitto_destroy(p->client);
    free(p);
    *publisher = NULL;
    if (mosquitto_lib_cleanup() != MOSQ_ERR_SUCCESS) {
        result = -1;
    }
    active = 0;
    return result;
}

int mqtt_publisher_connect(MqttPublisher **out, const char *host, int port,
                           const char *client_id, int timeout_ms)
{
    if (out == NULL || *out != NULL || active || host == NULL || *host == '\0' ||
        client_id == NULL || *client_id == '\0' || port < 1 || port > 65535 ||
        timeout_ms < 1 || timeout_ms > 5000) {
        return -1;
    }
    int64_t start = milliseconds();
    if (start < 0 || mosquitto_lib_init() != MOSQ_ERR_SUCCESS) {
        return -1;
    }
    MqttPublisher *p = calloc(1, sizeof *p);
    if (p == NULL) {
        mosquitto_lib_cleanup();
        return -1;
    }
    active = 1;
    p->timeout_ms = timeout_ms;
    p->client = mosquitto_new(client_id, true, p);
    if (p->client == NULL) {
        free(p);
        active = 0;
        mosquitto_lib_cleanup();
        return -1;
    }
    mosquitto_connect_callback_set(p->client, on_connect);
    mosquitto_disconnect_callback_set(p->client, on_disconnect);
    mosquitto_publish_callback_set(p->client, on_publish);
    if (mosquitto_int_option(p->client, MOSQ_OPT_PROTOCOL_VERSION, MQTT_PROTOCOL_V311)
            != MOSQ_ERR_SUCCESS ||
        mosquitto_connect(p->client, host, port, 60) != MOSQ_ERR_SUCCESS ||
        wait_for(p, &p->connected, start + timeout_ms) != 0) {
        mqtt_publisher_close(&p);
        return -1;
    }
    *out = p;
    return 0;
}

int mqtt_publisher_publish(MqttPublisher *p, int machine_id, const char *json)
{
    char topic[64];
    if (p == NULL || !p->connected || p->failed || json == NULL || *json == '\0' ||
        mqtt_publisher_topic(machine_id, topic, sizeof topic) != 0) {
        return -1;
    }
    size_t length = strlen(json);
    if (length > INT_MAX) {
        return -1;
    }
    int64_t start = milliseconds();
    if (start < 0) {
        return -1;
    }
    p->acknowledged = 0;
    p->pending_mid = 0;
    if (mosquitto_publish(p->client, &p->pending_mid, topic, (int)length,
                          json, 1, false) != MOSQ_ERR_SUCCESS) {
        p->failed = 1;
        return -1;
    }
    return wait_for(p, &p->acknowledged, start + p->timeout_ms);
}
