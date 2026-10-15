#include "gateway_telemetry.h"

#include <json-c/json.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

/* Validate Unicode scalar UTF-8, including shortest encodings. json-c 0.19's
 * UTF-8 flag only checks the shape of continuation sequences.
 */
static int valid_utf8(const unsigned char *text, size_t length)
{
    for (size_t i = 0; i < length;) {
        unsigned char c = text[i];
        if (c < 0x80) {
            ++i;
            continue;
        }
        size_t count;
        if (c >= 0xc2 && c <= 0xdf) count = 2;
        else if (c >= 0xe0 && c <= 0xef) count = 3;
        else if (c >= 0xf0 && c <= 0xf4) count = 4;
        else return 0;
        if (length - i < count) return 0;
        for (size_t j = 1; j < count; ++j) {
            if (text[i + j] < 0x80 || text[i + j] > 0xbf) return 0;
        }
        unsigned char second = text[i + 1];
        if ((c == 0xe0 && second < 0xa0) || (c == 0xed && second >= 0xa0) ||
            (c == 0xf0 && second < 0x90) || (c == 0xf4 && second >= 0x90)) {
            return 0;
        }
        i += count;
    }
    return 1;
}

static int digit(unsigned char c)
{
    return c >= '0' && c <= '9';
}

/* Complement json-c after successful parsing: inspect number tokens and reject
 * single quotes outside double-quoted strings. Structural syntax, escapes,
 * object construction and types remain the responsibility of json-c.
 */
static int valid_lexemes(const unsigned char *text, size_t length)
{
    size_t i = 0;
    while (i < length) {
        if (text[i] == '"') {
            ++i;
            while (i < length && text[i] != '"') {
                if (text[i] == '\\') {
                    ++i; /* Skip an escaped quote/backslash (or escape prefix). */
                }
                if (i < length) ++i;
            }
            if (i == length) return 0;
            ++i;
        } else if (text[i] == '\'' || text[i] == '+') {
            return 0;
        } else if (text[i] == '-' || digit(text[i])) {
            /* JSON number: -?(0|[1-9][0-9]*)(.[0-9]+)?([eE][+-]?[0-9]+)? */
            if (text[i] == '-') ++i;
            if (i == length) return 0;
            if (text[i] == '0') {
                ++i;
            } else {
                if (text[i] < '1' || text[i] > '9') return 0;
                do { ++i; } while (i < length && digit(text[i]));
            }
            if (i < length && text[i] == '.') {
                ++i;
                if (i == length || !digit(text[i])) return 0;
                do { ++i; } while (i < length && digit(text[i]));
            }
            if (i < length && (text[i] == 'e' || text[i] == 'E')) {
                ++i;
                if (i < length && (text[i] == '+' || text[i] == '-')) ++i;
                if (i == length || !digit(text[i])) return 0;
                do { ++i; } while (i < length && digit(text[i]));
            }
            if (i < length && text[i] != ',' && text[i] != '}' && text[i] != ']' &&
                text[i] != ' ' && text[i] != '\t' && text[i] != '\r' && text[i] != '\n') {
                return 0;
            }
        } else {
            ++i;
        }
    }
    return 1;
}

static int topic_id(const char *topic, int *out)
{
    static const char prefix[] = "smart-industry/machines/";
    int value = 0;
    if (strncmp(topic, prefix, sizeof prefix - 1) != 0) {
        return -1;
    }
    const char *p = topic + sizeof prefix - 1;
    if (*p < '0' || *p > '9') {
        return -1;
    }
    while (*p >= '0' && *p <= '9') {
        int digit = *p++ - '0';
        if (value > (INT_MAX - digit) / 10) {
            return -1;
        }
        value = value * 10 + digit;
    }
    if (value == 0 || strcmp(p, "/telemetry") != 0) {
        return -1;
    }
    *out = value;
    return 0;
}

static GatewayTelemetryError copy_text(struct json_object *value, char out[64])
{
    if (!json_object_is_type(value, json_type_string)) {
        return GATEWAY_TELEMETRY_INVALID_TYPE;
    }
    int length = json_object_get_string_len(value);
    const char *text = json_object_get_string(value);
    if (length < 1 || length >= 64 || memchr(text, '\0', (size_t)length) != NULL) {
        return GATEWAY_TELEMETRY_INVALID_TEXT;
    }
    memcpy(out, text, (size_t)length);
    out[length] = '\0';
    return GATEWAY_TELEMETRY_OK;
}

static GatewayTelemetryError measurement(struct json_object *value,
                                          double *out, bool *present)
{
    if (value == NULL) {
        *out = 0;
        *present = false;
        return GATEWAY_TELEMETRY_OK;
    }
    if (!json_object_is_type(value, json_type_int) &&
        !json_object_is_type(value, json_type_double)) {
        return GATEWAY_TELEMETRY_INVALID_TYPE;
    }
    double number = json_object_get_double(value);
    if (!isfinite(number)) {
        return GATEWAY_TELEMETRY_INVALID_VALUE;
    }
    *out = number;
    *present = true;
    return GATEWAY_TELEMETRY_OK;
}

int gateway_telemetry_parse(const char *topic, const void *payload,
                            size_t payload_length, GatewayTelemetry *out,
                            GatewayTelemetryError *error)
{
    static const char *keys[] = {
        "machine_id", "machine_type", "timestamp", "temperature", "current",
        "power", "state", "failure_within_24h"
    };
    GatewayTelemetry parsed = {0};
    GatewayTelemetryError status = GATEWAY_TELEMETRY_INVALID_ARGUMENT;
    struct json_tokener *tok = NULL;
    struct json_object *root = NULL;
    struct json_object *values[8];
    int id;

    if (topic == NULL || payload == NULL || out == NULL ||
        payload_length == 0 || payload_length > INT_MAX) {
        goto done;
    }
    status = GATEWAY_TELEMETRY_INVALID_TOPIC;
    if (topic_id(topic, &id) != 0) {
        goto done;
    }
    status = GATEWAY_TELEMETRY_INVALID_JSON;
    if (memchr(payload, '\0', payload_length) != NULL ||
        !valid_utf8(payload, payload_length)) {
        goto done;
    }
    tok = json_tokener_new();
    if (tok == NULL) {
        status = GATEWAY_TELEMETRY_NO_MEMORY;
        goto done;
    }
    json_tokener_set_flags(tok, JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
    root = json_tokener_parse_ex(tok, payload, (int)payload_length);
    if (json_tokener_get_error(tok) != json_tokener_success) {
        if (json_tokener_get_error(tok) == json_tokener_error_memory) {
            status = GATEWAY_TELEMETRY_NO_MEMORY;
        }
        goto done;
    }
    const unsigned char *bytes = payload;
    for (size_t i = json_tokener_get_parse_end(tok); i < payload_length; ++i) {
        if (bytes[i] != ' ' && bytes[i] != '\t' && bytes[i] != '\r' && bytes[i] != '\n') {
            goto done;
        }
    }
    if (!valid_lexemes(bytes, payload_length)) {
        goto done;
    }
    /* json-c stores object keys as C strings. Reject active NUL escapes in
     * keys as well as values, before a truncated key could match the schema.
     * Skip escaped backslashes so a literal "\\u0000" remains valid text.
     * JSON syntax itself has already been validated by json-c.
     */
    for (size_t i = 0; i < payload_length; ++i) {
        if (bytes[i] == '\\') {
            if (payload_length - i >= 6 && memcmp(bytes + i, "\\u0000", 6) == 0) {
                status = GATEWAY_TELEMETRY_INVALID_TEXT;
                goto done;
            }
            ++i;
        }
    }
    status = GATEWAY_TELEMETRY_INVALID_FIELDS;
    if (!json_object_is_type(root, json_type_object) || json_object_object_length(root) != 8) {
        goto done;
    }
    for (size_t i = 0; i < 8; ++i) {
        /* get_ex distinguishes an absent key from a present JSON null. */
        if (!json_object_object_get_ex(root, keys[i], &values[i])) {
            goto done;
        }
    }
    status = GATEWAY_TELEMETRY_INVALID_TYPE;
    if (!json_object_is_type(values[0], json_type_int) ||
        !json_object_is_type(values[7], json_type_int)) {
        goto done;
    }
    int64_t machine = json_object_get_int64(values[0]);
    int64_t flag = json_object_get_int64(values[7]);
    status = GATEWAY_TELEMETRY_INVALID_VALUE;
    if (machine <= 0 || machine > INT_MAX || (flag != 0 && flag != 1)) {
        goto done;
    }
    parsed.machine_id = (int)machine;
    parsed.failure_within_24h = (int)flag;
    status = GATEWAY_TELEMETRY_ID_MISMATCH;
    if (parsed.machine_id != id) {
        goto done;
    }
    status = copy_text(values[1], parsed.machine_type);
    if (status != GATEWAY_TELEMETRY_OK) {
        goto done;
    }
    status = copy_text(values[2], parsed.timestamp);
    if (status != GATEWAY_TELEMETRY_OK) {
        goto done;
    }
    status = copy_text(values[6], parsed.state);
    if (status != GATEWAY_TELEMETRY_OK) {
        goto done;
    }
    status = GATEWAY_TELEMETRY_INVALID_VALUE;
    if (strcmp(parsed.state, "UNKNOWN") != 0 && strcmp(parsed.state, "IDLE") != 0 &&
        strcmp(parsed.state, "NORMAL") != 0 && strcmp(parsed.state, "PEAK") != 0) {
        goto done;
    }
    status = measurement(values[3], &parsed.temperature, &parsed.temperature_present);
    if (status != GATEWAY_TELEMETRY_OK) {
        goto done;
    }
    status = measurement(values[4], &parsed.current, &parsed.current_present);
    if (status != GATEWAY_TELEMETRY_OK) {
        goto done;
    }
    status = measurement(values[5], &parsed.power, &parsed.power_present);
    if (status != GATEWAY_TELEMETRY_OK) {
        goto done;
    }
    *out = parsed;

done:
    json_object_put(root);
    if (tok != NULL) json_tokener_free(tok);
    if (error != NULL) *error = status;
    return status == GATEWAY_TELEMETRY_OK ? 0 : -1;
}

const char *gateway_telemetry_error_string(GatewayTelemetryError error)
{
    switch (error) {
    case GATEWAY_TELEMETRY_OK: return "Sucesso";
    case GATEWAY_TELEMETRY_INVALID_ARGUMENT: return "Argumento ou comprimento inválido";
    case GATEWAY_TELEMETRY_INVALID_TOPIC: return "Tópico MQTT inválido";
    case GATEWAY_TELEMETRY_INVALID_JSON: return "JSON inválido, incompleto ou UTF-8 inválido";
    case GATEWAY_TELEMETRY_INVALID_FIELDS: return "Esperadas exatamente as oito propriedades obrigatórias";
    case GATEWAY_TELEMETRY_INVALID_TYPE: return "Tipo JSON incompatível";
    case GATEWAY_TELEMETRY_INVALID_VALUE: return "Valor numérico ou estado inválido";
    case GATEWAY_TELEMETRY_INVALID_TEXT: return "Texto vazio, excessivo ou com NUL embutido";
    case GATEWAY_TELEMETRY_ID_MISMATCH: return "ID do tópico diverge do payload";
    case GATEWAY_TELEMETRY_NO_MEMORY: return "Memória indisponível para parsing JSON";
    default: return "Erro desconhecido";
    }
}
