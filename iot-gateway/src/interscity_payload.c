#define _POSIX_C_SOURCE 200809L
#include "interscity_payload.h"

#include <json-c/json.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TIMESTAMP_LENGTH 19 /* YYYY-MM-DDTHH:MM:SS */
#define NUMBER_CAPACITY 40

static int finish(InterscityPayloadError *error, InterscityPayloadError value)
{
    if (error != NULL) *error = value;
    return value == INTERSCITY_PAYLOAD_OK ? 0 : -1;
}

static int digits_in_range(const char *text, size_t start, size_t count, int minimum, int maximum)
{
    int value = 0;
    for (size_t i = 0; i < count; ++i) {
        char c = text[start + i];
        if (c < '0' || c > '9') return 0;
        value = value * 10 + (c - '0');
    }
    return value >= minimum && value <= maximum;
}

static int convert_timestamp(const char *in, size_t in_capacity, char out[TIMESTAMP_LENGTH + 1])
{
    if (strnlen(in, in_capacity) != TIMESTAMP_LENGTH) return -1;
    if (in[4] != '-' || in[7] != '-' || (in[10] != ' ' && in[10] != 'T') ||
        in[13] != ':' || in[16] != ':') return -1;
    if (!digits_in_range(in, 0, 4, 0, 9999) || !digits_in_range(in, 5, 2, 1, 12) ||
        !digits_in_range(in, 8, 2, 1, 31) || !digits_in_range(in, 11, 2, 0, 23) ||
        !digits_in_range(in, 14, 2, 0, 59) || !digits_in_range(in, 17, 2, 0, 59)) return -1;
    memcpy(out, in, TIMESTAMP_LENGTH + 1);
    out[10] = 'T';
    return 0;
}

static int is_known_state(const char *state, size_t capacity)
{
    static const char *states[] = {"UNKNOWN", "IDLE", "NORMAL", "PEAK"};
    if (strnlen(state, capacity) == capacity) return 0;
    for (size_t i = 0; i < sizeof states / sizeof states[0]; ++i) {
        if (strcmp(state, states[i]) == 0) return 1;
    }
    return 0;
}

static int render(double value, int precision, char text[NUMBER_CAPACITY])
{
    int written = snprintf(text, NUMBER_CAPACITY, "%.*g", precision, value);
    return written > 0 && written < NUMBER_CAPACITY - 2 ? 0 : -1; /* room for ".0" */
}

/* Shortest "%.Ng" text that converts back to exactly the same double, with a
 * decimal point so the value stays a JSON float (49.51, 5.1, 0.0, 50.0).
 * Precision is raised to the number of integer digits (below 1e17) so common
 * values are not written in exponent form (50.0 instead of 5e+01). The result
 * is re-checked and falls back to 17 significant digits, which always round-trip.
 * The gateway never calls setlocale, so the "C" locale and '.' are used.
 */
static int format_double(double value, char text[NUMBER_CAPACITY])
{
    if (!isfinite(value)) return -1;
    int precision = 17;
    for (int p = 1; p <= 17; ++p) {
        if (render(value, p, text) != 0) return -1;
        if (strtod(text, NULL) == value) {
            precision = p;
            break;
        }
    }
    double magnitude = value < 0 ? -value : value;
    if (magnitude >= 1.0 && magnitude < 1e17) {
        char integer_part[NUMBER_CAPACITY];
        int digits = snprintf(integer_part, sizeof integer_part, "%.0f", magnitude);
        if (digits > precision && digits <= 17) precision = digits;
    }
    if (render(value, precision, text) != 0) return -1;
    if (strtod(text, NULL) != value && render(value, 17, text) != 0) return -1;
    if (strpbrk(text, ".eE") == NULL) strcat(text, ".0");
    return 0;
}

/* Adds capability: [{"value": value, "timestamp": timestamp}] to data.
 * Takes ownership of value in every case (it may be NULL on allocation failure).
 */
static int add_sample(struct json_object *data, const char *capability,
                      struct json_object *value, const char *timestamp)
{
    struct json_object *array = json_object_new_array();
    if (array == NULL || json_object_object_add(data, capability, array) != 0) {
        json_object_put(array);
        json_object_put(value);
        return -1;
    }
    struct json_object *entry = json_object_new_object(); /* array is owned by data now */
    if (entry == NULL || json_object_array_add(array, entry) != 0) {
        json_object_put(entry);
        json_object_put(value);
        return -1;
    }
    if (value == NULL || json_object_object_add(entry, "value", value) != 0) {
        json_object_put(value);
        return -1;
    }
    struct json_object *text = json_object_new_string(timestamp);
    if (text == NULL || json_object_object_add(entry, "timestamp", text) != 0) {
        json_object_put(text);
        return -1;
    }
    return 0;
}

int interscity_payload_build(const GatewayTelemetry *telemetry, char *out, size_t out_size,
                             size_t *out_length, InterscityPayloadError *error)
{
    if (telemetry == NULL || out == NULL || out_size == 0) {
        return finish(error, INTERSCITY_PAYLOAD_ARGUMENT);
    }
    char timestamp[TIMESTAMP_LENGTH + 1];
    if (convert_timestamp(telemetry->timestamp, sizeof telemetry->timestamp, timestamp) != 0) {
        return finish(error, INTERSCITY_PAYLOAD_TIMESTAMP);
    }
    if (!is_known_state(telemetry->state, sizeof telemetry->state)) {
        return finish(error, INTERSCITY_PAYLOAD_STATE);
    }
    char temperature[NUMBER_CAPACITY];
    char current[NUMBER_CAPACITY];
    if ((telemetry->temperature_present && format_double(telemetry->temperature, temperature) != 0) ||
        (telemetry->current_present && format_double(telemetry->current, current) != 0) ||
        (telemetry->failure_within_24h != 0 && telemetry->failure_within_24h != 1)) {
        return finish(error, INTERSCITY_PAYLOAD_VALUE);
    }

    struct json_object *root = json_object_new_object();
    struct json_object *data = json_object_new_object();
    if (root == NULL || data == NULL || json_object_object_add(root, "data", data) != 0) {
        json_object_put(data);
        json_object_put(root);
        return finish(error, INTERSCITY_PAYLOAD_NO_MEMORY);
    }
    int failed = 0;
    if (telemetry->temperature_present) {
        failed = add_sample(data, INTERSCITY_CAP_MOTOR_TEMPERATURE,
                            json_object_new_double_s(telemetry->temperature, temperature),
                            timestamp) != 0;
    }
    if (!failed && telemetry->current_present) {
        failed = add_sample(data, INTERSCITY_CAP_CURRENT,
                            json_object_new_double_s(telemetry->current, current), timestamp) != 0;
    }
    if (!failed) {
        failed = add_sample(data, INTERSCITY_CAP_OPERATING_STATE,
                            json_object_new_string(telemetry->state), timestamp) != 0;
    }
    if (!failed) {
        failed = add_sample(data, INTERSCITY_CAP_FAILURE_WITHIN_24H,
                            json_object_new_int(telemetry->failure_within_24h), timestamp) != 0;
    }

    InterscityPayloadError result = failed ? INTERSCITY_PAYLOAD_NO_MEMORY : INTERSCITY_PAYLOAD_OK;
    if (!failed) {
        const char *json = json_object_to_json_string_ext(root, JSON_C_TO_STRING_PLAIN);
        if (json == NULL) {
            result = INTERSCITY_PAYLOAD_NO_MEMORY;
        } else {
            size_t length = strlen(json);
            if (length >= out_size) {
                result = INTERSCITY_PAYLOAD_TOO_LARGE;
            } else {
                memcpy(out, json, length + 1);
                if (out_length != NULL) *out_length = length;
            }
        }
    }
    json_object_put(root);
    return finish(error, result);
}

const char *interscity_payload_error_string(InterscityPayloadError error)
{
    switch (error) {
    case INTERSCITY_PAYLOAD_OK: return "Sucesso";
    case INTERSCITY_PAYLOAD_ARGUMENT: return "Argumento inválido";
    case INTERSCITY_PAYLOAD_TIMESTAMP: return "Timestamp fora do formato AAAA-MM-DD HH:MM:SS";
    case INTERSCITY_PAYLOAD_STATE: return "Estado operacional desconhecido";
    case INTERSCITY_PAYLOAD_VALUE: return "Valor de medição ou predição inválido";
    case INTERSCITY_PAYLOAD_NO_MEMORY: return "Memória indisponível ao montar o JSON";
    case INTERSCITY_PAYLOAD_TOO_LARGE: return "JSON maior que o buffer de saída";
    default: return "Erro desconhecido";
    }
}
