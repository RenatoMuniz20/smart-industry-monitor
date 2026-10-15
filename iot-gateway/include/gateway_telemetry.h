#ifndef GATEWAY_TELEMETRY_H
#define GATEWAY_TELEMETRY_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    int machine_id;
    char machine_type[64];
    char timestamp[64];
    char state[64];
    double temperature;
    bool temperature_present;
    double current;
    bool current_present;
    double power;
    bool power_present;
    int failure_within_24h;
} GatewayTelemetry;

typedef enum {
    GATEWAY_TELEMETRY_OK = 0,
    GATEWAY_TELEMETRY_INVALID_ARGUMENT,
    GATEWAY_TELEMETRY_INVALID_TOPIC,
    GATEWAY_TELEMETRY_INVALID_JSON,
    GATEWAY_TELEMETRY_INVALID_FIELDS,
    GATEWAY_TELEMETRY_INVALID_TYPE,
    GATEWAY_TELEMETRY_INVALID_VALUE,
    GATEWAY_TELEMETRY_INVALID_TEXT,
    GATEWAY_TELEMETRY_ID_MISMATCH,
    GATEWAY_TELEMETRY_NO_MEMORY
} GatewayTelemetryError;

/* Returns 0/-1. out is unchanged on failure. error is optional and is set
 * to OK on success. topic must be NUL-terminated; payload need not be.
 * All eight fields are required. Missing measurements have present=false
 * and value=0. No I/O. json-c resources are released before returning.
 * Duplicate JSON keys follow json-c's last-value-wins behavior (see README).
 */
int gateway_telemetry_parse(const char *topic, const void *payload,
                            size_t payload_length, GatewayTelemetry *out,
                            GatewayTelemetryError *error);

const char *gateway_telemetry_error_string(GatewayTelemetryError error);

#endif
