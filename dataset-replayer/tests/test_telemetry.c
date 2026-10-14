#include "telemetry.h"

#include <assert.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    DatasetRow row = {
        .timestamp = "2024-01-01 00:21:00",
        .machine_id = 6,
        .machine_type = "Robotic Arm",
        .temperature_motor = 49.7,
        .current_phase_avg = 3.5,
        .operating_mode = "normal",
        .failure_within_24h = 0
    };
    Telemetry out;
    unsigned char previous[sizeof out];
    const char *modes[] = {"idle", "normal", "peak", "unexpected", "", "IDLE"};
    const TelemetryState states[] = {
        TELEMETRY_STATE_IDLE, TELEMETRY_STATE_NORMAL, TELEMETRY_STATE_PEAK,
        TELEMETRY_STATE_UNKNOWN, TELEMETRY_STATE_UNKNOWN, TELEMETRY_STATE_UNKNOWN
    };
    const int invalid_flags[] = {-1, 2, INT_MIN, INT_MAX};

    assert(TELEMETRY_STATE_UNKNOWN == 0);
    assert(telemetry_normalize(&row, &out) == 0);
    assert(out.machine_id == 6);
    assert(strcmp(out.machine_type, "Robotic Arm") == 0);
    assert(strcmp(out.timestamp, "2024-01-01 00:21:00") == 0);
    assert(out.temperature == 49.7);
    assert(out.current == 3.5);
    assert(isnan(out.power));
    assert(out.state == TELEMETRY_STATE_NORMAL);
    assert(out.failure_within_24h == 0);

    for (size_t i = 0; i < sizeof modes / sizeof modes[0]; ++i) {
        strcpy(row.operating_mode, modes[i]);
        for (int flag = 0; flag <= 1; ++flag) {
            row.failure_within_24h = flag;
            assert(telemetry_normalize(&row, &out) == 0);
            assert(out.state == states[i]);
            assert(out.failure_within_24h == flag);
            assert(isnan(out.power));
        }
    }

    row.temperature_motor = NAN;
    assert(telemetry_normalize(&row, &out) == 0);
    assert(isnan(out.temperature));
    assert(out.current == 3.5);
    row.current_phase_avg = NAN;
    assert(telemetry_normalize(&row, &out) == 0);
    assert(isnan(out.temperature));
    assert(isnan(out.current));
    assert(isnan(out.power));

    memset(row.machine_type, 'm', sizeof row.machine_type - 1);
    row.machine_type[sizeof row.machine_type - 1] = '\0';
    memset(row.timestamp, 't', sizeof row.timestamp - 1);
    row.timestamp[sizeof row.timestamp - 1] = '\0';
    assert(telemetry_normalize(&row, &out) == 0);
    assert(strcmp(out.machine_type, row.machine_type) == 0);
    assert(strcmp(out.timestamp, row.timestamp) == 0);
    row.machine_type[0] = 'x';
    row.timestamp[0] = 'x';
    assert(out.machine_type[0] == 'm');
    assert(out.timestamp[0] == 't');
    out.machine_type[1] = 'y';
    out.timestamp[1] = 'y';
    assert(row.machine_type[1] == 'm');
    assert(row.timestamp[1] == 't');

    /* Compare the full object representation, including padding, on error. */
    memcpy(previous, &out, sizeof out);
    for (size_t i = 0; i < sizeof invalid_flags / sizeof invalid_flags[0]; ++i) {
        row.failure_within_24h = invalid_flags[i];
        assert(telemetry_normalize(&row, &out) == -1);
        assert(memcmp(previous, &out, sizeof out) == 0);
    }
    assert(telemetry_normalize(NULL, &out) == -1);
    assert(memcmp(previous, &out, sizeof out) == 0);
    row.failure_within_24h = 0;
    assert(telemetry_normalize(&row, NULL) == -1);
    assert(telemetry_normalize(NULL, NULL) == -1);

    puts("Todos os testes de telemetry passaram.");
    return 0;
}
