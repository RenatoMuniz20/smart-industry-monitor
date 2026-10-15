#include "gateway_telemetry.h"

#include <assert.h>
#include <json-c/json.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const char topic[] = "smart-industry/machines/1/telemetry";
/* Real SIM-03 message, recorded in docs/sim-03-validacao.md. */
static const char fixture[] =
    "{\"machine_id\":1,\"machine_type\":\"CNC\",\"timestamp\":\"2024-01-01 00:00:00\","
    "\"temperature\":49.51,\"current\":5.1,\"power\":null,\"state\":\"IDLE\",\"failure_within_24h\":0}";
static const char *keys[] = {"machine_id", "machine_type", "timestamp", "temperature",
                           "current", "power", "state", "failure_within_24h"};

static void fail(const char *t, const void *payload, size_t length,
                 GatewayTelemetryError expected)
{
    GatewayTelemetry out;
    unsigned char previous[sizeof out];
    GatewayTelemetryError error = GATEWAY_TELEMETRY_OK;
    memset(&out, 0xa5, sizeof out);
    memcpy(previous, &out, sizeof out);
    assert(gateway_telemetry_parse(t, payload, length, &out, &error) == -1);
    assert(error == expected);
    assert(memcmp(&out, previous, sizeof out) == 0);
    assert(gateway_telemetry_error_string(error)[0] != '\0');
}

static GatewayTelemetry success(struct json_object *root)
{
    GatewayTelemetry out;
    GatewayTelemetryError error = GATEWAY_TELEMETRY_INVALID_JSON;
    const char *text = json_object_to_json_string_ext(root, JSON_C_TO_STRING_PLAIN);
    assert(gateway_telemetry_parse(topic, text, strlen(text), &out, &error) == 0);
    assert(error == GATEWAY_TELEMETRY_OK);
    return out;
}

static void fail_object(struct json_object *root, GatewayTelemetryError error)
{
    const char *text = json_object_to_json_string_ext(root, JSON_C_TO_STRING_PLAIN);
    fail(topic, text, strlen(text), error);
}

static int strict_rejection(const char *name, const char *payload)
{
    GatewayTelemetry out;
    int result = gateway_telemetry_parse(topic, payload, strlen(payload), &out, NULL);
    printf("Estrito [%s]: %s\n", name, result == 0 ? "ACEITO INDEVIDAMENTE" : "rejeitado");
    if (result == 0) {
        return 1;
    }
    fail(topic, payload, strlen(payload), GATEWAY_TELEMETRY_INVALID_JSON);
    return 0;
}

static void test_strict_inputs(void)
{
    char payload[1024];
    int accepted = 0;
    const char *utf8[] = {
        "\xc0\xaf", "\xed\xa0\x80", "\xf4\x90\x80\x80", "\xe2\x82",
        "\xe2" "A\x80", "\x80", "\xe0\x80\xaf", "\xf0\x80\x80\xaf"
    };
    const char *names[] = {"UTF-8 overlong C0 AF", "UTF-8 surrogate", "UTF-8 > U+10FFFF",
                           "UTF-8 incompleto", "continuação incorreta", "continuação isolada",
                           "overlong 3 bytes", "overlong 4 bytes"};
    for (size_t i = 0; i < sizeof utf8 / sizeof utf8[0]; ++i) {
        snprintf(payload, sizeof payload,
            "{\"machine_id\":1,\"machine_type\":\"%s\",%s", utf8[i], strstr(fixture, "\"timestamp\""));
        accepted += strict_rejection(names[i], payload);
    }
    snprintf(payload, sizeof payload, "{'machine_id':1,%s", strstr(fixture, "\"machine_type\""));
    accepted += strict_rejection("chave com aspas simples", payload);
    const char *numbers[] = {"1.", "01", "-01", "00", "01.5", "-.5", "1.e2", "1e", "1e+"};
    for (size_t i = 0; i < sizeof numbers / sizeof numbers[0]; ++i) {
        snprintf(payload, sizeof payload,
            "{\"machine_id\":1,\"machine_type\":\"CNC\",\"timestamp\":\"original\","
            "\"temperature\":%s,\"current\":null,\"power\":null,\"state\":\"IDLE\",\"failure_within_24h\":0}", numbers[i]);
        accepted += strict_rejection(numbers[i], payload);
    }
    fflush(stdout);
    assert(accepted == 0);
}

static void test_valid_lexemes(void)
{
    char payload[1024];
    GatewayTelemetry out;
    const char *numbers[] = {"0", "-0", "10", "-10", "0.5", "-0.5",
                              "1e2", "1E+2", "1e-2", "1.25e+2"};
    const double expected[] = {0, 0, 10, -10, .5, -.5, 100, 100, .01, 125};
    for (size_t i = 0; i < sizeof numbers / sizeof numbers[0]; ++i) {
        snprintf(payload, sizeof payload,
            "{\"machine_id\":1,\"machine_type\":\"Pump's 01 1. -01\",\"timestamp\":\"original\","
            "\"temperature\":%s,\"current\":null,\"power\":null,\"state\":\"IDLE\",\"failure_within_24h\":0}", numbers[i]);
        assert(gateway_telemetry_parse(topic, payload, strlen(payload), &out, NULL) == 0);
        assert(out.temperature_present && out.temperature == expected[i]);
        assert(strcmp(out.machine_type, "Pump's 01 1. -01") == 0);
    }
    const char *unicode[] = {"\xc2\x80", "\xdf\xbf", "\xe0\xa0\x80", "\xed\x9f\xbf",
                             "\xee\x80\x80", "\xf0\x90\x80\x80", "\xf4\x8f\xbf\xbf"};
    for (size_t i = 0; i < sizeof unicode / sizeof unicode[0]; ++i) {
        snprintf(payload, sizeof payload, "{\"machine_id\":1,\"machine_type\":\"%s\",%s",
                 unicode[i], strstr(fixture, "\"timestamp\""));
        assert(gateway_telemetry_parse(topic, payload, strlen(payload), &out, NULL) == 0);
        assert(strcmp(out.machine_type, unicode[i]) == 0);
    }
    struct json_object *root = json_tokener_parse(fixture);
    const char *escaped = "quote \" 1. apostrophe ' slash \\ 01\n";
    json_object_object_add(root, "machine_type", json_object_new_string(escaped));
    out = success(root);
    assert(strcmp(out.machine_type, escaped) == 0);
    json_object_put(root);
    snprintf(payload, sizeof payload, "{\"machine_\\u0069d\":1,%s",
             strstr(fixture, "\"machine_type\""));
    assert(gateway_telemetry_parse(topic, payload, strlen(payload), &out, NULL) == 0);
}

int main(void)
{
    test_strict_inputs();
    test_valid_lexemes();
    GatewayTelemetry out;
    assert(gateway_telemetry_parse(topic, fixture, sizeof fixture - 1, &out, NULL) == 0);
    assert(out.machine_id == 1 && strcmp(out.machine_type, "CNC") == 0);
    assert(strcmp(out.timestamp, "2024-01-01 00:00:00") == 0);
    assert(strcmp(out.state, "IDLE") == 0 && out.failure_within_24h == 0);
    assert(out.temperature_present && out.temperature == 49.51);
    assert(out.current_present && out.current == 5.1);
    assert(!out.power_present && out.power == 0);
    char unterminated[sizeof fixture - 1];
    memcpy(unterminated, fixture, sizeof unterminated);
    assert(gateway_telemetry_parse(topic, unterminated, sizeof unterminated, &out, NULL) == 0);

    const char *states[] = {"UNKNOWN", "IDLE", "NORMAL", "PEAK"};
    for (size_t i = 0; i < 4; ++i) {
        struct json_object *root = json_tokener_parse(fixture);
        json_object_object_add(root, "state", json_object_new_string(states[i]));
        json_object_object_add(root, "failure_within_24h", json_object_new_int(i % 2));
        out = success(root);
        assert(strcmp(out.state, states[i]) == 0 && out.failure_within_24h == (int)(i % 2));
        json_object_put(root);
    }
    for (size_t i = 0; i < 8; ++i) {
        struct json_object *root = json_tokener_parse(fixture);
        json_object_object_del(root, keys[i]);
        fail_object(root, GATEWAY_TELEMETRY_INVALID_FIELDS);
        json_object_put(root);
        root = json_tokener_parse(fixture);
        json_object_object_add(root, keys[i], json_object_new_boolean(1));
        fail_object(root, GATEWAY_TELEMETRY_INVALID_TYPE);
        json_object_object_add(root, keys[i], json_object_new_array());
        fail_object(root, GATEWAY_TELEMETRY_INVALID_TYPE);
        json_object_object_add(root, keys[i], json_object_new_object());
        fail_object(root, GATEWAY_TELEMETRY_INVALID_TYPE);
        json_object_object_add(root, keys[i], NULL);
        if (i >= 3 && i <= 5) success(root);
        else fail_object(root, GATEWAY_TELEMETRY_INVALID_TYPE);
        json_object_put(root);
    }
    struct json_object *root = json_tokener_parse(fixture);
    json_object_object_add(root, "extra", NULL);
    fail_object(root, GATEWAY_TELEMETRY_INVALID_FIELDS);
    json_object_object_del(root, "extra");
    json_object_object_add(root, "machine_id", json_object_new_int64((int64_t)INT_MAX + 1));
    fail_object(root, GATEWAY_TELEMETRY_INVALID_VALUE);
    json_object_object_add(root, "machine_id", json_object_new_int(-1));
    fail_object(root, GATEWAY_TELEMETRY_INVALID_VALUE);
    json_object_object_add(root, "machine_id", json_object_new_int(0));
    fail_object(root, GATEWAY_TELEMETRY_INVALID_VALUE);
    json_object_object_add(root, "machine_id", json_object_new_int(2));
    fail_object(root, GATEWAY_TELEMETRY_ID_MISMATCH);
    json_object_object_add(root, "machine_id", json_object_new_double(1));
    fail_object(root, GATEWAY_TELEMETRY_INVALID_TYPE);
    json_object_object_add(root, "machine_id", json_object_new_int(1));
    for (int flag = -1; flag <= 2; flag += 3) {
        json_object_object_add(root, "failure_within_24h", json_object_new_int(flag));
        fail_object(root, GATEWAY_TELEMETRY_INVALID_VALUE);
    }
    json_object_object_add(root, "failure_within_24h", json_object_new_string("1"));
    fail_object(root, GATEWAY_TELEMETRY_INVALID_TYPE);
    json_object_object_add(root, "failure_within_24h", json_object_new_int(1));
    for (size_t i = 3; i <= 5; ++i) {
        json_object_object_add(root, keys[i], json_object_new_string("12.5"));
        fail_object(root, GATEWAY_TELEMETRY_INVALID_TYPE);
        json_object_object_add(root, keys[i], json_object_new_double(-12.5));
        out = success(root);
        const double readings[] = {out.temperature, out.current, out.power};
        const bool present[] = {out.temperature_present, out.current_present, out.power_present};
        assert(readings[i - 3] == -12.5 && present[i - 3]);
        json_object_object_add(root, keys[i], json_object_new_int(0));
        out = success(root);
        const double zeros[] = {out.temperature, out.current, out.power};
        const bool zero_present[] = {out.temperature_present, out.current_present, out.power_present};
        assert(zeros[i - 3] == 0 && zero_present[i - 3]);
        json_object_object_add(root, keys[i], NULL);
    }
    out = success(root);
    assert(!out.temperature_present && !out.current_present && !out.power_present);
    assert(out.temperature == 0 && out.current == 0 && out.power == 0);
    const char *text_keys[] = {"machine_type", "timestamp"};
    char text[65];
    memset(text, 'a', 64);
    text[64] = '\0';
    for (size_t i = 0; i < 2; ++i) {
        json_object_object_add(root, text_keys[i], json_object_new_string(text));
        fail_object(root, GATEWAY_TELEMETRY_INVALID_TEXT);
        json_object_object_add(root, text_keys[i], json_object_new_string_len(text, 63));
        out = success(root);
        assert(strlen(i == 0 ? out.machine_type : out.timestamp) == 63);
        json_object_object_add(root, text_keys[i], json_object_new_string(""));
        fail_object(root, GATEWAY_TELEMETRY_INVALID_TEXT);
        json_object_object_add(root, text_keys[i], json_object_new_string_len("a\0b", 3));
        fail_object(root, GATEWAY_TELEMETRY_INVALID_TEXT);
        json_object_object_add(root, text_keys[i], json_object_new_string("\"\\\n\xc3\xa7"));
        out = success(root);
        assert(strcmp(i == 0 ? out.machine_type : out.timestamp, "\"\\\n\xc3\xa7") == 0);
    }
    json_object_object_add(root, "state", json_object_new_string("idle"));
    fail_object(root, GATEWAY_TELEMETRY_INVALID_VALUE);
    json_object_put(root);

    const char *topics[] = {"", "smart-industry/machines/0/telemetry", "smart-industry/machines/-1/telemetry",
        "smart-industry/machines/+1/telemetry", "smart-industry/machines/1.0/telemetry",
        "smart-industry/machines/99999999999999999999/telemetry", "smart-industry/machines/1/telemetry/x",
        "smart-industry/machines//telemetry", "other/machines/1/telemetry", "smart-industry/machines/1/status"};
    for (size_t i = 0; i < sizeof topics / sizeof topics[0]; ++i)
        fail(topics[i], fixture, strlen(fixture), GATEWAY_TELEMETRY_INVALID_TOPIC);
    fail(NULL, fixture, strlen(fixture), GATEWAY_TELEMETRY_INVALID_ARGUMENT);
    fail(topic, NULL, 1, GATEWAY_TELEMETRY_INVALID_ARGUMENT);
    fail(topic, fixture, 0, GATEWAY_TELEMETRY_INVALID_ARGUMENT);
    fail(topic, fixture, (size_t)INT_MAX + 1, GATEWAY_TELEMETRY_INVALID_ARGUMENT);
    assert(gateway_telemetry_parse(topic, fixture, strlen(fixture), NULL, NULL) == -1);
    const char *bad_json[] = {"{", "[]", "null", "true", "{\"x\":}", "/*comment*/{}", "{\"x\":1,}"};
    for (size_t i = 0; i < sizeof bad_json / sizeof bad_json[0]; ++i) {
        GatewayTelemetryError error;
        assert(gateway_telemetry_parse(topic, bad_json[i], strlen(bad_json[i]), &out, &error) == -1);
    }
    fail(topic, fixture, strlen(fixture) - 1, GATEWAY_TELEMETRY_INVALID_JSON);
    fail(topic, fixture, sizeof fixture, GATEWAY_TELEMETRY_INVALID_JSON);
    char buffer[1024];
    snprintf(buffer, sizeof buffer, "%s {}", fixture);
    fail(topic, buffer, strlen(buffer), GATEWAY_TELEMETRY_INVALID_JSON);
    snprintf(buffer, sizeof buffer, "%s garbage", fixture);
    fail(topic, buffer, strlen(buffer), GATEWAY_TELEMETRY_INVALID_JSON);
    snprintf(buffer, sizeof buffer, "%s \t\r\n", fixture);
    assert(gateway_telemetry_parse(topic, buffer, strlen(buffer), &out, NULL) == 0);
    memcpy(buffer, fixture, sizeof fixture);
    char *type = strstr(buffer, "CNC");
    *type = (char)0xff;
    fail(topic, buffer, strlen(buffer), GATEWAY_TELEMETRY_INVALID_JSON);
    memcpy(buffer, fixture, sizeof fixture);
    buffer[20] = '\0';
    fail(topic, buffer, sizeof fixture - 1, GATEWAY_TELEMETRY_INVALID_JSON);
    const char *nonfinite[] = {"NaN", "Infinity", "-Infinity", "1e9999"};
    for (size_t i = 0; i < 4; ++i) {
        snprintf(buffer, sizeof buffer,
            "{\"machine_id\":1,\"machine_type\":\"CNC\",\"timestamp\":\"original\","
            "\"temperature\":%s,\"current\":null,\"power\":null,\"state\":\"IDLE\",\"failure_within_24h\":0}", nonfinite[i]);
        GatewayTelemetryError error;
        assert(gateway_telemetry_parse(topic, buffer, strlen(buffer), &out, &error) == -1);
    }
    /* json-c exposes last-value-wins rather than duplicate-key rejection. */
    snprintf(buffer, sizeof buffer, "{\"machine_id\":2,%s", fixture + 1);
    assert(gateway_telemetry_parse(topic, buffer, strlen(buffer), &out, NULL) == 0);
    assert(out.machine_id == 1);
    snprintf(buffer, sizeof buffer, "{\"machine_id\\u0000suffix\":1,%s",
             strstr(fixture, "\"machine_type\""));
    fail(topic, buffer, strlen(buffer), GATEWAY_TELEMETRY_INVALID_TEXT);
    root = json_tokener_parse(fixture);
    json_object_object_add(root, "timestamp", json_object_new_string("literal \\u0000"));
    out = success(root);
    assert(strcmp(out.timestamp, "literal \\u0000") == 0);
    json_object_put(root);
    root = json_tokener_parse(fixture);
    json_object_object_add(root, "machine_id", json_object_new_int(INT_MAX));
    char maximum_topic[64];
    snprintf(maximum_topic, sizeof maximum_topic, "smart-industry/machines/%d/telemetry", INT_MAX);
    const char *maximum_payload = json_object_to_json_string(root);
    assert(gateway_telemetry_parse(maximum_topic, maximum_payload, strlen(maximum_payload), &out, NULL) == 0);
    assert(out.machine_id == INT_MAX);
    json_object_put(root);
    /* Reusable after failures; no restriction to demonstration IDs. */
    root = json_tokener_parse(fixture);
    json_object_object_add(root, "machine_id", json_object_new_int(42));
    const char *encoded = json_object_to_json_string(root);
    assert(gateway_telemetry_parse("smart-industry/machines/42/telemetry", encoded, strlen(encoded), &out, NULL) == 0);
    json_object_put(root);
    assert(gateway_telemetry_parse(topic, fixture, strlen(fixture), &out, NULL) == 0);
    assert(strcmp(gateway_telemetry_error_string((GatewayTelemetryError)-1), "Erro desconhecido") == 0);
    puts("Todos os testes de gateway_telemetry passaram.");
    return 0;
}
