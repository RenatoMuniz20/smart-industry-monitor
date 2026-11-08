#ifndef INTERSCITY_PAYLOAD_H
#define INTERSCITY_PAYLOAD_H

#include "gateway_telemetry.h"

#include <stddef.h>

/* Capability names registered in the InterSCity catalog (see
 * docs/sim-05-interscity.md). Never build these names dynamically: the
 * Data Collector silently associates any unknown name with the resource.
 */
#define INTERSCITY_CAP_MOTOR_TEMPERATURE "sim_motor_temperature"
#define INTERSCITY_CAP_CURRENT "sim_current"
#define INTERSCITY_CAP_OPERATING_STATE "sim_operating_state"
#define INTERSCITY_CAP_FAILURE_WITHIN_24H "sim_failure_within_24h"

/* Recommended buffer size; the largest possible payload is far smaller. */
#define INTERSCITY_PAYLOAD_CAPACITY 1024

typedef enum {
    INTERSCITY_PAYLOAD_OK = 0,
    INTERSCITY_PAYLOAD_ARGUMENT,
    INTERSCITY_PAYLOAD_TIMESTAMP,
    INTERSCITY_PAYLOAD_STATE,
    INTERSCITY_PAYLOAD_VALUE,
    INTERSCITY_PAYLOAD_NO_MEMORY,
    INTERSCITY_PAYLOAD_TOO_LARGE
} InterscityPayloadError;

/* Builds the body of POST /adaptor/resources/{uuid}/data for one validated
 * telemetry:
 *   {"data":{"sim_motor_temperature":[{"value":<number>,"timestamp":"..."}],
 *            "sim_current":[...],"sim_operating_state":[...],
 *            "sim_failure_within_24h":[...]}}
 * Policy:
 *   - A missing measurement (present=false, JSON null) omits its capability;
 *     zero is a measurement and is sent. State and prediction are always sent.
 *   - power is not sent: no capability exists for it (always null today).
 *   - timestamp must be "YYYY-MM-DD HH:MM:SS" (or with 'T'), with month 01-12,
 *     day 01-31, hour 00-23 and minute/second 00-59. It is sent as
 *     "YYYY-MM-DDTHH:MM:SS", without time zone, preserving the historical
 *     instant. Calendar validity (e.g. 02-30) is not checked.
 *   - Measurements are written with the shortest decimal text that converts
 *     back to exactly the same double, always with a decimal point (49.51,
 *     5.1, 0.0).
 * Returns 0/-1. On success out holds a NUL-terminated string and
 * *out_length (optional) its length. out and *out_length are unchanged on
 * failure. error is optional. No I/O; json-c objects are freed before return.
 */
int interscity_payload_build(const GatewayTelemetry *telemetry, char *out, size_t out_size,
                             size_t *out_length, InterscityPayloadError *error);

const char *interscity_payload_error_string(InterscityPayloadError error);

#endif
