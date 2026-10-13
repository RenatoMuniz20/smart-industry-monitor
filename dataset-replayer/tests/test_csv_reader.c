#include "csv_reader.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    size_t calls;
    int action;
} HandlerContext;

static int test_handler(const DatasetRow *row, void *context)
{
    HandlerContext *state = context;

    assert(row->machine_id == 1);
    assert(strcmp(row->machine_type, "CNC") == 0);
    ++state->calls;
    return state->action;
}

int main(void)
{
    DatasetRow row = {0};
    const char *valid =
        "2024-01-01 00:21:00,1,CNC,0.71,49.7,3.5,21.3,862.7,idle,274.15,13.6,60.65,0,none,0";
    char line[256];
    char text[65];
    size_t row_count;
    size_t error_line;
    HandlerContext state = {0};

    assert(csv_parse_row(valid, &row) == 0);
    assert(strcmp(row.timestamp, "2024-01-01 00:21:00") == 0);
    assert(row.machine_id == 1);
    assert(strcmp(row.machine_type, "CNC") == 0);
    assert(row.vibration_rms == 0.71);
    assert(row.temperature_motor == 49.7);
    assert(row.current_phase_avg == 3.5);
    assert(row.pressure_level == 21.3);
    assert(row.rpm == 862.7);
    assert(strcmp(row.operating_mode, "idle") == 0);
    assert(row.hours_since_maintenance == 274.15);
    assert(row.ambient_temp == 13.6);
    assert(row.rul_hours == 60.65);
    assert(row.failure_within_24h == 0);
    assert(strcmp(row.failure_type, "none") == 0);
    assert(row.estimated_repair_cost == 0.0);

    assert(csv_parse_row(
        "2024-01-01 00:21:00,1,CNC,0.71,49.7,,21.3,862.7,idle,274.15,13.6,60.65,0,none,0",
        &row) == 0);
    assert(isnan(row.current_phase_avg));
    assert(row.pressure_level == 21.3);

    assert(csv_parse_row("time,1,CNC", &row) == -1);
    assert(csv_parse_row("time,bad,CNC,1,2,3,4,5,idle,6,7,8,0,none,0", &row) == -1);
    assert(csv_parse_row("time,,CNC,1,2,3,4,5,idle,6,7,8,0,none,0", &row) == -1);
    assert(csv_parse_row("time,1,CNC,1,2,3,4,5,idle,6,7,8,,none,0", &row) == -1);
    assert(csv_parse_row("time,1x,CNC,1,2,3,4,5,idle,6,7,8,0,none,0", &row) == -1);
    assert(csv_parse_row("time,1,CNC,1x,2,3,4,5,idle,6,7,8,0,none,0", &row) == -1);
    assert(csv_parse_row("time,1,CNC,1,2,3,4,5,idle,6,7,8,0,none,0,extra", &row) == -1);
    assert(csv_parse_row("time,999999999999999999999,CNC,1,2,3,4,5,idle,6,7,8,0,none,0", &row) == -1);
    assert(csv_parse_row("time,1,CNC,1e9999,2,3,4,5,idle,6,7,8,0,none,0", &row) == -1);
    assert(csv_parse_row("time,1,CNC,,,,,,idle,,,,0,none,", &row) == 0);
    assert(isnan(row.vibration_rms));
    assert(isnan(row.temperature_motor));
    assert(isnan(row.current_phase_avg));
    assert(isnan(row.pressure_level));
    assert(isnan(row.rpm));
    assert(isnan(row.hours_since_maintenance));
    assert(isnan(row.ambient_temp));
    assert(isnan(row.rul_hours));
    assert(isnan(row.estimated_repair_cost));

    snprintf(line, sizeof line, "%s\n", valid);
    assert(csv_parse_row(line, &row) == 0);
    snprintf(line, sizeof line, "%s\r\n", valid);
    assert(csv_parse_row(line, &row) == 0);

    memset(text, 'a', 63);
    text[63] = '\0';
    snprintf(line, sizeof line, "%s,1,CNC,1,2,3,4,5,idle,6,7,8,0,none,0", text);
    assert(csv_parse_row(line, &row) == 0);
    assert(strlen(row.timestamp) == 63);
    text[63] = 'a';
    text[64] = '\0';
    snprintf(line, sizeof line, "%s,1,CNC,1,2,3,4,5,idle,6,7,8,0,none,0", text);
    assert(csv_parse_row(line, &row) == -1);
    assert(strlen(row.timestamp) == 63);
    assert(csv_parse_row(NULL, &row) == -1);
    assert(csv_parse_row(valid, NULL) == -1);

    assert(csv_read_file("tests/fixtures/valid.csv", &row_count, &error_line, NULL, NULL) == 0);
    assert(row_count == 2);
    assert(error_line == 0);
    assert(csv_read_file("tests/fixtures/nonexistent.csv", &row_count, &error_line, NULL, NULL) == -1);
    assert(row_count == 0);
    assert(error_line == 0);
    assert(csv_read_file("tests/fixtures/invalid-header.csv", &row_count, &error_line, NULL, NULL) == -1);
    assert(row_count == 0);
    assert(error_line == 1);
    assert(csv_read_file("tests/fixtures/invalid-row.csv", &row_count, &error_line, NULL, NULL) == -1);
    assert(row_count == 1);
    assert(error_line == 3);
    assert(csv_read_file(NULL, &row_count, &error_line, NULL, NULL) == -1);
    assert(csv_read_file("tests/fixtures/valid.csv", NULL, &error_line, NULL, NULL) == -1);
    assert(csv_read_file("tests/fixtures/valid.csv", &row_count, NULL, NULL, NULL) == -1);

    assert(csv_read_file("tests/fixtures/valid.csv", &row_count, &error_line,
                         test_handler, &state) == 0);
    assert(state.calls == 2 && row_count == 2 && error_line == 0);
    state.calls = 0;
    state.action = 1;
    assert(csv_read_file("tests/fixtures/valid.csv", &row_count, &error_line,
                         test_handler, &state) == 0);
    assert(state.calls == 1 && row_count == 1 && error_line == 0);
    assert(csv_read_file("tests/fixtures/invalid-row.csv", &row_count, &error_line,
                         test_handler, &state) == 0);
    assert(row_count == 1 && error_line == 0);
    state.calls = 0;
    state.action = -1;
    assert(csv_read_file("tests/fixtures/valid.csv", &row_count, &error_line,
                         test_handler, &state) == -1);
    assert(state.calls == 1 && row_count == 1 && error_line == 2);

    puts("Todos os testes de csv_reader passaram.");
    return 0;
}
