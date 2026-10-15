#ifndef MQTT_SUBSCRIBER_H
#define MQTT_SUBSCRIBER_H

#include <stddef.h>

typedef struct MqttSubscriber MqttSubscriber;
typedef void (*MqttMessageHandler)(const char *topic, const void *payload,
                                   size_t payload_length, void *context);

typedef enum {
    MQTT_SUBSCRIBER_OK = 0,
    MQTT_SUBSCRIBER_ARGUMENT,
    MQTT_SUBSCRIBER_ACTIVE,
    MQTT_SUBSCRIBER_MEMORY,
    MQTT_SUBSCRIBER_LIBRARY,
    MQTT_SUBSCRIBER_CLOCK,
    MQTT_SUBSCRIBER_CONNECT,
    MQTT_SUBSCRIBER_CONNACK,
    MQTT_SUBSCRIBER_SUBSCRIBE,
    MQTT_SUBSCRIBER_SUBACK,
    MQTT_SUBSCRIBER_TIMEOUT,
    MQTT_SUBSCRIBER_NETWORK,
    MQTT_SUBSCRIBER_DISCONNECTED,
    MQTT_SUBSCRIBER_CLOSE
} MqttSubscriberError;

/* One active context per process, sequential use only. Owns libmosquitto
 * init/cleanup: do not mix with independent users of that library.
 * *out must be NULL; stays NULL on failure. handler must be non-NULL.
 * timeout_ms: 1..5000 for CONNACK + SUBACK together (monotonic deadline).
 * Synchronous DNS/TCP setup may exceed that deadline, as in the publisher.
 * Success requires accepted CONNACK and matching SUBACK granting exactly QoS 1.
 * The caller supplies a nonempty unique client ID. Fixed subscription:
 * smart-industry/machines/+/telemetry, MQTT 3.1.1, clean session.
 */
int mqtt_subscriber_connect(MqttSubscriber **out, const char *host, int port,
                            const char *client_id, int timeout_ms,
                            MqttMessageHandler handler, void *context);

/* One synchronous network-loop iteration, timeout_ms: 0..5000 (0 nonblocking).
 * Returns 0 even if no message arrived, -1 on error; no automatic reconnect.
 * Call regularly to service keepalive (60s). Callback arguments are borrowed
 * and valid only until callback return. Payload is binary, not NUL-terminated.
 * Callbacks may also run during connect if data arrives before SUBACK/return.
 * Do not call lifecycle/poll functions from inside the callback (no reentry).
 */
int mqtt_subscriber_poll(MqttSubscriber *subscriber, int timeout_ms);

/* NULL-safe, clears *subscriber and releases resources even if shutdown fails.
 * Does not wait for a broker response to DISCONNECT. Returns 0/-1.
 */
int mqtt_subscriber_close(MqttSubscriber **subscriber);

/* Process-local diagnostic for the last operation, also available after a
 * failed connect. No logging. Query before another API call overwrites it.
 */
MqttSubscriberError mqtt_subscriber_last_error(void);
const char *mqtt_subscriber_error_string(MqttSubscriberError error);

#endif
