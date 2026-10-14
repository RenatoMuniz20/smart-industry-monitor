#include "mqtt_publisher.h"

#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    MqttPublisher *p = NULL;
    char topic[64];
    char previous[sizeof topic];
    memset(topic, 'x', sizeof topic);
    memcpy(previous, topic, sizeof topic);
    assert(mqtt_publisher_topic(0, topic, sizeof topic) == -1);
    assert(mqtt_publisher_topic(-1, topic, sizeof topic) == -1);
    assert(mqtt_publisher_topic(1, NULL, sizeof topic) == -1);
    assert(mqtt_publisher_topic(1, topic, 0) == -1);
    assert(memcmp(topic, previous, sizeof topic) == 0);
    assert(mqtt_publisher_topic(1, topic, sizeof topic) == 0);
    assert(strcmp(topic, "smart-industry/machines/1/telemetry") == 0);
    size_t length = strlen(topic);
    assert(mqtt_publisher_topic(1, topic, length + 1) == 0);
    memcpy(previous, topic, sizeof topic);
    assert(mqtt_publisher_topic(1, topic, length) == -1);
    assert(memcmp(topic, previous, sizeof topic) == 0);
    assert(mqtt_publisher_topic(INT_MAX, topic, sizeof topic) == 0);
    assert(mqtt_publisher_connect(NULL, "127.0.0.1", 1883, "test", 5000) == -1);
    assert(mqtt_publisher_connect(&p, NULL, 1883, "test", 5000) == -1);
    assert(mqtt_publisher_connect(&p, "", 1883, "test", 5000) == -1);
    assert(mqtt_publisher_connect(&p, "127.0.0.1", 0, "test", 5000) == -1);
    assert(mqtt_publisher_connect(&p, "127.0.0.1", 65536, "test", 5000) == -1);
    assert(mqtt_publisher_connect(&p, "127.0.0.1", 1883, NULL, 5000) == -1);
    assert(mqtt_publisher_connect(&p, "127.0.0.1", 1883, "", 5000) == -1);
    assert(mqtt_publisher_connect(&p, "127.0.0.1", 1883, "test", 0) == -1);
    assert(mqtt_publisher_connect(&p, "127.0.0.1", 1883, "test", 5001) == -1);
    assert(p == NULL);
    assert(mqtt_publisher_publish(NULL, 1, "{}") == -1);
    assert(mqtt_publisher_close(NULL) == 0);
    assert(mqtt_publisher_close(&p) == 0);
    if (argc == 1) {
        puts("Todos os testes locais de mqtt_publisher passaram.");
        return 0;
    }
    assert(argc == 4);
    int port = atoi(argv[1]);
    int expected_connect = atoi(argv[2]);
    int expected_publish = atoi(argv[3]);
    /* Each harness invocation is a distinct process with a distinct ID. */
    char client_id[64];
    snprintf(client_id, sizeof client_id, "sim-test-%s", argv[1]);
    assert(mqtt_publisher_connect(&p, "127.0.0.1", port, client_id, 500)
           == expected_connect);
    if (expected_connect == 0) {
        MqttPublisher *second = NULL;
        assert(mqtt_publisher_connect(&second, "127.0.0.1", port, "second", 500) == -1);
        assert(mqtt_publisher_connect(&p, "127.0.0.1", port, "again", 500) == -1);
        assert(mqtt_publisher_publish(p, 0, "{}") == -1);
        assert(mqtt_publisher_publish(p, 1, NULL) == -1);
        assert(mqtt_publisher_publish(p, 1, "") == -1);
        assert(mqtt_publisher_publish(p, 1, "{\"sequence\":1}") == expected_publish);
        assert(mqtt_publisher_publish(p, 1, "{\"sequence\":2}") == expected_publish);
    } else {
        assert(p == NULL);
    }
    assert(mqtt_publisher_close(&p) == 0);
    assert(p == NULL);
    assert(mqtt_publisher_close(&p) == 0);
    if (expected_connect == 0 && expected_publish == 0) {
        /* Explicitly reopen after cleanup, without any automatic reconnect. */
        assert(mqtt_publisher_connect(&p, "127.0.0.1", port, client_id, 500) == 0);
        assert(mqtt_publisher_close(&p) == 0);
        assert(p == NULL);
    }
    puts("Teste MQTT concluído.");
    return 0;
}
