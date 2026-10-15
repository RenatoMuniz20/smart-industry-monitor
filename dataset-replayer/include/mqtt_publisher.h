#ifndef MQTT_PUBLISHER_H
#define MQTT_PUBLISHER_H

#include <stddef.h>

typedef struct MqttPublisher MqttPublisher;

/* One active context per process, single-threaded use only. The module owns
 * libmosquitto init/cleanup; do not mix with other library users in the process.
 * *out must be NULL. On error it stays NULL. Client ID must be nonempty and
 * unique among broker clients (e.g. machine ID + PID, supplied by caller).
 * timeout_ms must be 1..5000. Waits for accepted CONNACK before success.
 * mosquitto_connect uses synchronous DNS/TCP setup, subject to OS timeouts;
 * the monotonic deadline bounds network event processing, not that setup.
 */
int mqtt_publisher_connect(MqttPublisher **out, const char *host, int port,
                           const char *client_id, int timeout_ms);

/* QoS 1, retain false. Returns 0 only after matching PUBACK, otherwise -1.
 * json must be a nonempty NUL-terminated JSON string; JSON validation belongs
 * to telemetry_to_json. Topic is constructed from a strictly positive ID.
 * Network failure/timeout invalidates the connection: close it before retry.
 * Invalid arguments alone do not invalidate the connection.
 */
int mqtt_publisher_publish(MqttPublisher *publisher, int machine_id,
                           const char *json);

/* NULL-safe, releases all resources and clears *publisher. Returns 0 or -1
 * if a graceful DISCONNECT fails; resources are released in either case.
 */
int mqtt_publisher_close(MqttPublisher **publisher);

/* Returns 0/-1; destination is unchanged on error. Includes NUL in capacity. */
int mqtt_publisher_topic(int machine_id, char *buffer, size_t capacity);

#endif
