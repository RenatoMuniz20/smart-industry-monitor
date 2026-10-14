#define _POSIX_C_SOURCE 200809L
#include "telemetry_json.h"

#include <assert.h>
#include <float.h>
#include <limits.h>
#include <locale.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void expect_error(const Telemetry *t, size_t capacity)
{
    char buffer[2048];
    char previous[sizeof buffer];
    memset(buffer, 0xa5, sizeof buffer);
    memcpy(previous, buffer, sizeof buffer);
    assert(telemetry_to_json(t, buffer, capacity) == -1);
    assert(memcmp(buffer, previous, sizeof buffer) == 0);
}

static void check_number(Telemetry *t, double value)
{
    char json[2048];
    char *end;
    const char *number;
    t->temperature = value;
    assert(telemetry_to_json(t, json, sizeof json) == 0);
    number = strstr(json, "\"temperature\":") + strlen("\"temperature\":");
    double parsed = strtod(number, &end);
    assert(*end == ',');
    assert(parsed == value);
    assert(signbit(parsed) == signbit(value));
}

int main(void)
{
    Telemetry t = {
        .machine_id = 6, .machine_type = "Robotic Arm",
        .timestamp = "2024-01-01 00:21:00", .temperature = 49.5,
        .current = 3.5, .power = 100, .state = TELEMETRY_STATE_NORMAL,
        .failure_within_24h = 1
    };
    char json[2048];
    char reference[2048];
    const char *expected =
        "{\"machine_id\":6,\"machine_type\":\"Robotic Arm\","
        "\"timestamp\":\"2024-01-01 00:21:00\",\"temperature\":49.5,"
        "\"current\":3.5,\"power\":100,\"state\":\"NORMAL\",\"failure_within_24h\":1}";
    assert(telemetry_to_json(&t, json, sizeof json) == 0);
    /* Exact equality verifies all eight keys, types, values and punctuation. */
    assert(strcmp(json, expected) == 0);
    assert(telemetry_to_json(&t, json, strlen(expected) + 1) == 0);
    expect_error(&t, strlen(expected));
    expect_error(&t, 1);
    expect_error(&t, 0);
    expect_error(NULL, sizeof json);
    assert(telemetry_to_json(&t, NULL, sizeof json) == -1);
    assert(telemetry_to_json(NULL, NULL, 0) == -1);
    const int invalid_flags[] = {-1, 2, INT_MIN, INT_MAX};
    for (size_t i = 0; i < sizeof invalid_flags / sizeof invalid_flags[0]; ++i) {
        t.failure_within_24h = invalid_flags[i];
        expect_error(&t, sizeof json);
    }
    t.failure_within_24h = 0;

    const double nonfinite[] = {NAN, INFINITY, -INFINITY};
    for (size_t i = 0; i < 3; ++i) {
        t.temperature = nonfinite[i];
        t.current = nonfinite[(i + 1) % 3];
        t.power = nonfinite[(i + 2) % 3];
        assert(telemetry_to_json(&t, json, sizeof json) == 0);
        assert(strstr(json, "\"temperature\":null,\"current\":null,\"power\":null,") != NULL);
    }
    const char *states[] = {"UNKNOWN", "IDLE", "NORMAL", "PEAK", "UNKNOWN"};
    for (int i = 0; i < 5; ++i) {
        char field[64];
        t.state = (TelemetryState)i;
        snprintf(field, sizeof field, "\"state\":\"%s\"", states[i]);
        assert(telemetry_to_json(&t, json, sizeof json) == 0);
        assert(strstr(json, field) != NULL);
    }

    strcpy(t.machine_type, "\"\\\n\t\r\b\f\001\037");
    strcpy(t.timestamp, "unchanged timestamp +03:00");
    assert(telemetry_to_json(&t, json, sizeof json) == 0);
    assert(strstr(json, "\"machine_type\":\"\\\"\\\\\\u000a\\u0009\\u000d\\u0008\\u000c\\u0001\\u001f\"") != NULL);
    assert(strstr(json, "\"timestamp\":\"unchanged timestamp +03:00\"") != NULL);
    strcpy(t.machine_type, "A\xc2\xa2\xe2\x82\xac\xf0\x9f\x8f\xad");
    assert(telemetry_to_json(&t, json, sizeof json) == 0);
    assert(strstr(json, t.machine_type) != NULL);
    const char *invalid_utf8[] = {
        "\x80", "\xc0\xaf", "\xc1\xbf", "\xc2", "\xe2\x82",
        "\xe0\x80\x80", "\xed\xa0\x80", "\xf0\x80\x80\x80",
        "\xf4\x90\x80\x80", "\xf5\x80\x80\x80", "\xff", "\xe2" "A\x80"
    };
    for (size_t i = 0; i < sizeof invalid_utf8 / sizeof invalid_utf8[0]; ++i) {
        strcpy(t.machine_type, invalid_utf8[i]);
        expect_error(&t, sizeof json);
        strcpy(t.machine_type, "valid");
        strcpy(t.timestamp, invalid_utf8[i]);
        expect_error(&t, sizeof json);
        strcpy(t.timestamp, "valid");
    }
    t.machine_type[0] = t.timestamp[0] = '\0';
    assert(telemetry_to_json(&t, json, sizeof json) == 0);
    assert(strstr(json, "\"machine_type\":\"\",\"timestamp\":\"\"") != NULL);
    memset(t.machine_type, 1, sizeof t.machine_type - 1);
    memset(t.timestamp, 31, sizeof t.timestamp - 1);
    t.machine_type[63] = t.timestamp[63] = '\0';
    assert(telemetry_to_json(&t, reference, sizeof reference) == 0);
    assert(telemetry_to_json(&t, json, strlen(reference) + 1) == 0);
    assert(strcmp(json, reference) == 0);
    expect_error(&t, strlen(reference));
    t.machine_type[63] = 'x';
    expect_error(&t, sizeof json);
    t.machine_type[63] = '\0';
    t.timestamp[63] = 'x';
    expect_error(&t, sizeof json);
    t.timestamp[63] = '\0';

    const double numbers[] = {DBL_MAX, -DBL_MAX, DBL_MIN, DBL_TRUE_MIN,
                              1.2345678901234567, 0.0, -0.0};
    for (size_t i = 0; i < sizeof numbers / sizeof numbers[0]; ++i) {
        check_number(&t, numbers[i]);
    }
    t.machine_id = INT_MIN;
    assert(telemetry_to_json(&t, json, sizeof json) == 0);
    char integer[64];
    snprintf(integer, sizeof integer, "\"machine_id\":%d,", INT_MIN);
    assert(strstr(json, integer) != NULL);
    t.machine_id = INT_MAX;
    snprintf(integer, sizeof integer, "\"machine_id\":%d,", INT_MAX);
    assert(telemetry_to_json(&t, json, sizeof json) == 0);
    assert(strstr(json, integer) != NULL);

    /* Exercise a comma-decimal locale without depending on installed locales. */
    const char *locales[] = {"pt_BR.UTF-8", "pt_BR.utf8", "de_DE.UTF-8", "fr_FR.UTF-8"};
    int tested_locale = 0;
    t.temperature = 1.5;
    assert(telemetry_to_json(&t, reference, sizeof reference) == 0);
    for (size_t i = 0; i < sizeof locales / sizeof locales[0]; ++i) {
        locale_t alternate = newlocale(LC_NUMERIC_MASK, locales[i], (locale_t)0);
        if (alternate == (locale_t)0) {
            continue;
        }
        locale_t previous = uselocale(alternate);
        assert(previous != (locale_t)0);
        assert(telemetry_to_json(&t, json, sizeof json) == 0);
        assert(strcmp(json, reference) == 0);
        assert(uselocale((locale_t)0) == alternate);
        assert(uselocale(previous) != (locale_t)0);
        freelocale(alternate);
        tested_locale = 1;
        break;
    }
    if (!tested_locale) {
        puts("SKIP: locale numérico alternativo indisponível.");
    }

    DatasetRow row = {
        .machine_id = 6, .machine_type = "Robotic Arm",
        .timestamp = "2024-01-01 00:21:00", .temperature_motor = 49.5,
        .current_phase_avg = 3.5, .operating_mode = "normal", .failure_within_24h = 1
    };
    assert(telemetry_normalize(&row, &t) == 0);
    Telemetry original;
    memcpy(&original, &t, sizeof original);
    assert(telemetry_to_json(&t, json, sizeof json) == 0);
    assert(memcmp(&t, &original, sizeof t) == 0);
    assert(strcmp(json,
        "{\"machine_id\":6,\"machine_type\":\"Robotic Arm\","
        "\"timestamp\":\"2024-01-01 00:21:00\",\"temperature\":49.5,"
        "\"current\":3.5,\"power\":null,\"state\":\"NORMAL\",\"failure_within_24h\":1}") == 0);
    puts("Todos os testes de telemetry_json passaram.");
    return 0;
}
