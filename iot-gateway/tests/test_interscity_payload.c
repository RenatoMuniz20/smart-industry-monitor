#define _POSIX_C_SOURCE 200809L
#include "interscity_payload.h"

#include <assert.h>
#include <float.h>
#include <json-c/json.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

/* First sample of machine 1 in the dataset, the same one sent manually in SIM-05.4. */
static GatewayTelemetry sample(void)
{
    GatewayTelemetry t;
    memset(&t, 0, sizeof t);
    t.machine_id = 1;
    strcpy(t.machine_type, "CNC");
    strcpy(t.timestamp, "2024-01-01 00:00:00");
    strcpy(t.state, "IDLE");
    t.temperature = 49.51;
    t.temperature_present = true;
    t.current = 5.1;
    t.current_present = true;
    t.power = 0.0;
    t.power_present = false;
    t.failure_within_24h = 0;
    return t;
}

static void build_ok(const GatewayTelemetry *t, char *out, size_t size)
{
    InterscityPayloadError error = INTERSCITY_PAYLOAD_ARGUMENT;
    size_t length = 0;
    assert(interscity_payload_build(t, out, size, &length, &error) == 0);
    assert(error == INTERSCITY_PAYLOAD_OK);
    assert(length == strlen(out));
}

/* Returns the single sample object of a capability, or NULL when absent. */
static struct json_object *sample_of(struct json_object *root, const char *capability)
{
    struct json_object *data, *array;
    assert(json_object_object_get_ex(root, "data", &data));
    if (!json_object_object_get_ex(data, capability, &array)) return NULL;
    assert(json_object_get_type(array) == json_type_array);
    assert(json_object_array_length(array) == 1);
    struct json_object *entry = json_object_array_get_idx(array, 0);
    struct json_object *timestamp;
    assert(json_object_object_get_ex(entry, "timestamp", &timestamp));
    assert(strcmp(json_object_get_string(timestamp), "2024-01-01T00:00:00") == 0);
    assert(json_object_object_length(entry) == 2);
    return entry;
}

static int capability_count(struct json_object *root)
{
    struct json_object *data;
    assert(json_object_object_get_ex(root, "data", &data));
    return json_object_object_length(data);
}

static void expect_error(const GatewayTelemetry *t, size_t size, InterscityPayloadError expected)
{
    char out[INTERSCITY_PAYLOAD_CAPACITY];
    memset(out, 'Z', sizeof out);
    size_t length = 12345;
    InterscityPayloadError error = INTERSCITY_PAYLOAD_OK;
    assert(interscity_payload_build(t, out, size, &length, &error) == -1);
    assert(error == expected);
    assert(length == 12345);
    for (size_t i = 0; i < sizeof out; ++i) assert(out[i] == 'Z'); /* out unchanged */
}

static void test_reference_sample(void)
{
    GatewayTelemetry t = sample();
    char out[INTERSCITY_PAYLOAD_CAPACITY];
    build_ok(&t, out, sizeof out);
    const char *expected =
        "{\"data\":{"
        "\"sim_motor_temperature\":[{\"value\":49.51,\"timestamp\":\"2024-01-01T00:00:00\"}],"
        "\"sim_current\":[{\"value\":5.1,\"timestamp\":\"2024-01-01T00:00:00\"}],"
        "\"sim_operating_state\":[{\"value\":\"IDLE\",\"timestamp\":\"2024-01-01T00:00:00\"}],"
        "\"sim_failure_within_24h\":[{\"value\":0,\"timestamp\":\"2024-01-01T00:00:00\"}]}}";
    if (strcmp(out, expected) != 0) {
        fprintf(stderr, "obtido:   %s\nesperado: %s\n", out, expected);
        assert(0);
    }
    /* The text must parse back to the same values and types. */
    struct json_object *root = json_tokener_parse(out);
    assert(root != NULL);
    assert(capability_count(root) == 4);
    struct json_object *value;
    assert(json_object_object_get_ex(sample_of(root, INTERSCITY_CAP_MOTOR_TEMPERATURE), "value", &value));
    assert(json_object_get_type(value) == json_type_double && json_object_get_double(value) == 49.51);
    assert(json_object_object_get_ex(sample_of(root, INTERSCITY_CAP_CURRENT), "value", &value));
    assert(json_object_get_type(value) == json_type_double && json_object_get_double(value) == 5.1);
    assert(json_object_object_get_ex(sample_of(root, INTERSCITY_CAP_OPERATING_STATE), "value", &value));
    assert(json_object_get_type(value) == json_type_string);
    assert(strcmp(json_object_get_string(value), "IDLE") == 0);
    assert(json_object_object_get_ex(sample_of(root, INTERSCITY_CAP_FAILURE_WITHIN_24H), "value", &value));
    assert(json_object_get_type(value) == json_type_int && json_object_get_int(value) == 0);
    json_object_put(root);
}

static void test_missing_measurements(void)
{
    char out[INTERSCITY_PAYLOAD_CAPACITY];
    GatewayTelemetry t = sample();
    t.temperature_present = false;
    build_ok(&t, out, sizeof out);
    struct json_object *root = json_tokener_parse(out);
    assert(capability_count(root) == 3);
    assert(sample_of(root, INTERSCITY_CAP_MOTOR_TEMPERATURE) == NULL);
    assert(sample_of(root, INTERSCITY_CAP_CURRENT) != NULL);
    json_object_put(root);

    t.current_present = false;
    build_ok(&t, out, sizeof out);
    root = json_tokener_parse(out);
    assert(capability_count(root) == 2); /* state and prediction are always sent */
    assert(sample_of(root, INTERSCITY_CAP_OPERATING_STATE) != NULL);
    assert(sample_of(root, INTERSCITY_CAP_FAILURE_WITHIN_24H) != NULL);
    json_object_put(root);
}

static void test_zero_is_a_measurement_and_power_is_ignored(void)
{
    char out[INTERSCITY_PAYLOAD_CAPACITY];
    GatewayTelemetry t = sample();
    t.current = 0.0;
    t.power = 123.0;
    t.power_present = true;
    t.failure_within_24h = 1;
    strcpy(t.state, "PEAK");
    build_ok(&t, out, sizeof out);
    assert(strstr(out, "\"sim_current\":[{\"value\":0.0,") != NULL);
    assert(strstr(out, "power") == NULL);
    assert(strstr(out, "\"sim_failure_within_24h\":[{\"value\":1,") != NULL);
    assert(strstr(out, "\"value\":\"PEAK\"") != NULL);
}

static void test_number_round_trip(void)
{
    const double values[] = {0.1, 1.0 / 3.0, 50.0, -12.5, 123456.789, 1e-7, 1e20, -0.0, DBL_MAX, DBL_MIN};
    char out[INTERSCITY_PAYLOAD_CAPACITY];
    for (size_t i = 0; i < sizeof values / sizeof values[0]; ++i) {
        GatewayTelemetry t = sample();
        t.temperature = values[i];
        build_ok(&t, out, sizeof out);
        struct json_object *root = json_tokener_parse(out);
        assert(root != NULL);
        struct json_object *value;
        assert(json_object_object_get_ex(sample_of(root, INTERSCITY_CAP_MOTOR_TEMPERATURE), "value", &value));
        assert(json_object_get_type(value) == json_type_double);
        assert(json_object_get_double(value) == values[i]);
        json_object_put(root);
    }
    static const struct { double value; const char *text; } readable[] = {
        {50.0, "50.0"}, {100.0, "100.0"}, {-12.5, "-12.5"}, {0.0, "0.0"},
        {123456.789, "123456.789"}, {0.1, "0.1"}, {1e20, "1e+20"}, {1e-7, "1e-07"}
    };
    for (size_t i = 0; i < sizeof readable / sizeof readable[0]; ++i) {
        GatewayTelemetry t = sample();
        t.temperature = readable[i].value;
        build_ok(&t, out, sizeof out);
        char expected[64];
        snprintf(expected, sizeof expected, "\"value\":%s,", readable[i].text);
        if (strstr(out, expected) == NULL) {
            fprintf(stderr, "esperado %s em %s\n", expected, out);
            assert(0);
        }
    }
}

static void test_timestamps(void)
{
    char out[INTERSCITY_PAYLOAD_CAPACITY];
    GatewayTelemetry t = sample();
    strcpy(t.timestamp, "2024-01-01T00:00:00");
    build_ok(&t, out, sizeof out);

    static const char *invalid[] = {
        "", "2024-01-01", "2024-01-01 00:00", "2024-01-01 00:00:00Z", "2024-01-01  00:00:00",
        "2024/01/01 00:00:00", "2024-00-01 00:00:00", "2024-13-01 00:00:00", "2024-01-00 00:00:00",
        "2024-01-32 00:00:00", "2024-01-01 24:00:00", "2024-01-01 00:60:00", "2024-01-01 00:00:60",
        "2024-01-01x00:00:00", "2O24-01-01 00:00:00", "2024-1-01 00:00:00 "
    };
    for (size_t i = 0; i < sizeof invalid / sizeof invalid[0]; ++i) {
        t = sample();
        strcpy(t.timestamp, invalid[i]);
        expect_error(&t, INTERSCITY_PAYLOAD_CAPACITY, INTERSCITY_PAYLOAD_TIMESTAMP);
    }
    t = sample();
    memset(t.timestamp, '1', sizeof t.timestamp); /* not NUL-terminated */
    expect_error(&t, INTERSCITY_PAYLOAD_CAPACITY, INTERSCITY_PAYLOAD_TIMESTAMP);
}

static void test_invalid_values(void)
{
    GatewayTelemetry t = sample();
    strcpy(t.state, "BROKEN");
    expect_error(&t, INTERSCITY_PAYLOAD_CAPACITY, INTERSCITY_PAYLOAD_STATE);
    t = sample();
    t.state[0] = '\0';
    expect_error(&t, INTERSCITY_PAYLOAD_CAPACITY, INTERSCITY_PAYLOAD_STATE);

    t = sample();
    t.temperature = NAN;
    expect_error(&t, INTERSCITY_PAYLOAD_CAPACITY, INTERSCITY_PAYLOAD_VALUE);
    t = sample();
    t.current = INFINITY;
    expect_error(&t, INTERSCITY_PAYLOAD_CAPACITY, INTERSCITY_PAYLOAD_VALUE);
    t = sample();
    t.failure_within_24h = 2;
    expect_error(&t, INTERSCITY_PAYLOAD_CAPACITY, INTERSCITY_PAYLOAD_VALUE);

    /* An absent measurement is not formatted, so its stored value is irrelevant. */
    char out[INTERSCITY_PAYLOAD_CAPACITY];
    t = sample();
    t.temperature = NAN;
    t.temperature_present = false;
    build_ok(&t, out, sizeof out);
}

static void test_buffer_and_arguments(void)
{
    GatewayTelemetry t = sample();
    char out[INTERSCITY_PAYLOAD_CAPACITY];
    size_t length;
    build_ok(&t, out, sizeof out);
    length = strlen(out);
    expect_error(&t, length, INTERSCITY_PAYLOAD_TOO_LARGE); /* no room for NUL */
    build_ok(&t, out, length + 1);
    assert(interscity_payload_build(&t, out, sizeof out, NULL, NULL) == 0);

    InterscityPayloadError error;
    assert(interscity_payload_build(NULL, out, sizeof out, NULL, &error) == -1);
    assert(error == INTERSCITY_PAYLOAD_ARGUMENT);
    assert(interscity_payload_build(&t, NULL, sizeof out, NULL, &error) == -1);
    assert(error == INTERSCITY_PAYLOAD_ARGUMENT);
    expect_error(&t, 0, INTERSCITY_PAYLOAD_ARGUMENT);
}

static void test_error_strings(void)
{
    for (int e = INTERSCITY_PAYLOAD_OK; e <= INTERSCITY_PAYLOAD_TOO_LARGE; ++e) {
        const char *text = interscity_payload_error_string((InterscityPayloadError)e);
        assert(text != NULL && *text != '\0' && strcmp(text, "Erro desconhecido") != 0);
    }
    assert(strcmp(interscity_payload_error_string((InterscityPayloadError)999), "Erro desconhecido") == 0);
}

int main(void)
{
    test_reference_sample();
    test_missing_measurements();
    test_zero_is_a_measurement_and_power_is_ignored();
    test_number_round_trip();
    test_timestamps();
    test_invalid_values();
    test_buffer_and_arguments();
    test_error_strings();
    puts("test_interscity_payload: todos os testes passaram.");
    return 0;
}
