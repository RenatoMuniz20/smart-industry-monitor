#include <stdio.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "csv_reader.h"
#include "telemetry.h"

typedef struct {
    int machine_id;
    size_t limit; /* Zero means unlimited. */
    size_t selected;
} ReplayerContext;

static int parse_positive(const char *text, uintmax_t maximum, uintmax_t *value)
{
    char *end;
    uintmax_t parsed;

    for (const char *p = text; *p; ++p) {
        if (*p < '0' || *p > '9') {
            return -1;
        }
    }
    errno = 0;
    parsed = strtoumax(text, &end, 10);
    if (end == text || *end || errno == ERANGE || parsed == 0 || parsed > maximum) {
        return -1;
    }
    *value = parsed;
    return 0;
}

static void format_double(char *buffer, size_t capacity, double value)
{
    if (isnan(value)) {
        snprintf(buffer, capacity, "N/A");
    } else {
        snprintf(buffer, capacity, "%.10g", value);
    }
}

static const char *format_state(TelemetryState state)
{
    switch (state) {
    case TELEMETRY_STATE_IDLE:
        return "IDLE";
    case TELEMETRY_STATE_NORMAL:
        return "NORMAL";
    case TELEMETRY_STATE_PEAK:
        return "PEAK";
    default:
        return "UNKNOWN";
    }
}

static int select_row(const DatasetRow *row, void *context)
{
    ReplayerContext *state = context;
    Telemetry telemetry;
    char temperature[64];
    char current[64];
    char power[64];

    if (row->machine_id != state->machine_id) {
        return 0;
    }
    if (telemetry_normalize(row, &telemetry) != 0) {
        return -1;
    }
    format_double(temperature, sizeof temperature, telemetry.temperature);
    format_double(current, sizeof current, telemetry.current);
    format_double(power, sizeof power, telemetry.power);
    if (printf("timestamp=%s machine_id=%d machine_type=%s temperature=%s current=%s "
               "power=%s state=%s failure_within_24h=%d\n",
               telemetry.timestamp, telemetry.machine_id, telemetry.machine_type,
               temperature, current, power, format_state(telemetry.state),
               telemetry.failure_within_24h) < 0) {
        return -1;
    }
    ++state->selected;
    return state->limit != 0 && state->selected >= state->limit ? 1 : 0;
}

int main(int argc, char *argv[])
{
    size_t row_count;
    size_t error_line;
    ReplayerContext context = {0};
    int has_machine = 0;
    int has_limit = 0;

    if (argc < 4) {
        goto usage;
    }
    for (int i = 2; i < argc; i += 2) {
        uintmax_t value;

        if (i + 1 >= argc) {
            goto usage;
        }
        if (strcmp(argv[i], "--machine-id") == 0 && !has_machine) {
            if (parse_positive(argv[i + 1], INT_MAX, &value) != 0) {
                goto usage;
            }
            context.machine_id = (int)value;
            has_machine = 1;
        } else if (strcmp(argv[i], "--limit") == 0 && !has_limit) {
            if (parse_positive(argv[i + 1], SIZE_MAX, &value) != 0) {
                goto usage;
            }
            context.limit = (size_t)value;
            has_limit = 1;
        } else {
            goto usage;
        }
    }
    if (!has_machine) {
        goto usage;
    }
    if (csv_read_file(argv[1], &row_count, &error_line, select_row, &context) != 0) {
        if (error_line != 0) {
            fprintf(stderr, "Erro ao processar o CSV na linha %zu.\n", error_line);
        } else {
            fprintf(stderr, "Erro ao ler o arquivo CSV: %s\n", argv[1]);
        }
        return 1;
    }
    if (context.selected == 0) {
        printf("Nenhum registro encontrado para machine_id %d.\n", context.machine_id);
    }
    printf("Registros selecionados: %zu\n", context.selected);
    return 0;

usage:
    fprintf(stderr, "Uso: %s <csv> --machine-id <id> [--limit <quantidade>]\n", argv[0]);
    return 1;
}
