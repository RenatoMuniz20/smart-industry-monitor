#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#include "csv_reader.h"
#include "telemetry.h"
#include "telemetry_json.h"
#include "mqtt_publisher.h"

typedef struct {
    int machine_id;
    size_t limit; /* Zero means unlimited. */
    size_t selected;
    MqttPublisher *publisher;
    int interval_ms;
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

static int wait_interval(int interval_ms)
{
    struct timespec remaining = {
        .tv_sec = interval_ms / 1000,
        .tv_nsec = (interval_ms % 1000) * 1000000L
    };

    while (nanosleep(&remaining, &remaining) != 0) {
        if (errno != EINTR) {
            return -1;
        }
    }
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
        if (state->publisher != NULL) {
            fprintf(stderr, "Erro ao normalizar telemetria.\n");
        }
        return -1;
    }
    if (state->publisher != NULL) {
        char json[2048];
        if (telemetry_to_json(&telemetry, json, sizeof json) != 0) {
            fprintf(stderr, "Erro ao serializar telemetria JSON.\n");
            return -1;
        }
        /* Sleep only when there is another selected, serializable record.
         * selected counts PUBACKs, so the first publication never waits.
         */
        if (state->selected != 0 && wait_interval(state->interval_ms) != 0) {
            fprintf(stderr, "Erro ao aguardar intervalo entre publicações.\n");
            return -1;
        }
        if (mqtt_publisher_publish(state->publisher, telemetry.machine_id, json) != 0) {
            fprintf(stderr, "Erro na publicação MQTT ou na confirmação PUBACK.\n");
            return -1;
        }
        ++state->selected; /* mqtt_publisher_publish waits for matching PUBACK. */
        return state->limit != 0 && state->selected >= state->limit ? 1 : 0;
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
    int publish = 0;
    const char *mqtt_host = "127.0.0.1";
    int mqtt_port = 1883;
    int interval_ms = 1000;
    int has_host = 0;
    int has_port = 0;
    int has_interval = 0;

    if (argc < 4) {
        goto usage;
    }
    for (int i = 2; i < argc; ++i) {
        uintmax_t value;

        if (strcmp(argv[i], "--publish") == 0) {
            if (publish) {
                goto usage;
            }
            publish = 1;
            continue;
        }
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
        } else if (strcmp(argv[i], "--mqtt-host") == 0 && !has_host) {
            if (argv[i + 1][0] == '\0' || strncmp(argv[i + 1], "--", 2) == 0) {
                goto usage;
            }
            mqtt_host = argv[i + 1];
            has_host = 1;
        } else if (strcmp(argv[i], "--mqtt-port") == 0 && !has_port) {
            if (parse_positive(argv[i + 1], 65535, &value) != 0) {
                goto usage;
            }
            mqtt_port = (int)value;
            has_port = 1;
        } else if (strcmp(argv[i], "--interval-ms") == 0 && !has_interval) {
            if (parse_positive(argv[i + 1], 60000, &value) != 0) {
                goto usage;
            }
            interval_ms = (int)value;
            has_interval = 1;
        } else {
            goto usage;
        }
        ++i; /* All options except --publish consume a value. */
    }
    if (!has_machine || (!publish && (has_host || has_port || has_interval))) {
        goto usage;
    }
    if (publish) {
        char client_id[64];
        int length = snprintf(client_id, sizeof client_id, "sim-%d-%jd",
                              context.machine_id, (intmax_t)getpid());
        if (length < 0 || (size_t)length >= sizeof client_id) {
            fprintf(stderr, "Erro ao gerar Client ID MQTT.\n");
            return 1;
        }
        context.interval_ms = interval_ms;
        if (mqtt_publisher_connect(&context.publisher, mqtt_host, mqtt_port,
                                   client_id, 5000) != 0) {
            fprintf(stderr, "Erro ao conectar ao broker MQTT %s:%d.\n",
                    mqtt_host, mqtt_port);
            return 1;
        }
    }
    if (csv_read_file(argv[1], &row_count, &error_line, select_row, &context) != 0) {
        if (error_line != 0) {
            fprintf(stderr, "Erro ao processar o CSV na linha %zu.\n", error_line);
        } else {
            fprintf(stderr, "Erro ao ler o arquivo CSV: %s\n", argv[1]);
        }
        if (publish) {
            if (mqtt_publisher_close(&context.publisher) != 0) {
                fprintf(stderr, "Erro ao encerrar conexão MQTT.\n");
            }
            fprintf(stderr, "Execução interrompida; publicações confirmadas antes do erro: %zu.\n",
                    context.selected);
        }
        return 1;
    }
    if (publish) {
        if (mqtt_publisher_close(&context.publisher) != 0) {
            fprintf(stderr, "Erro ao encerrar conexão MQTT; publicações confirmadas: %zu.\n",
                    context.selected);
            return 1;
        }
        printf("Publicações confirmadas: %zu\n", context.selected);
        return 0;
    }
    if (context.selected == 0) {
        printf("Nenhum registro encontrado para machine_id %d.\n", context.machine_id);
    }
    printf("Registros selecionados: %zu\n", context.selected);
    return 0;

usage:
    fprintf(stderr, "Uso: %s <csv> --machine-id <id> [--limit <quantidade>] "
            "[--publish [--mqtt-host <host>] [--mqtt-port <1..65535>] "
            "[--interval-ms <1..60000>]]\n", argv[0]);
    return 1;
}
