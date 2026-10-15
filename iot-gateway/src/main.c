#define _POSIX_C_SOURCE 200809L
#include "gateway_telemetry.h"
#include "mqtt_subscriber.h"

#include <errno.h>
#include <inttypes.h>
#include <json-c/json.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>

static volatile sig_atomic_t stop_requested;

typedef struct {
    int log_failed;
} GatewayContext;

static void request_stop(int signal_number)
{
    (void)signal_number;
    stop_requested = 1;
}

static int parse_positive(const char *text, uintmax_t maximum, int *out)
{
    char *end;
    for (const char *p = text; *p; ++p) {
        if (*p < '0' || *p > '9') return -1;
    }
    errno = 0;
    uintmax_t value = strtoumax(text, &end, 10);
    if (end == text || *end || errno == ERANGE || value == 0 || value > maximum) return -1;
    *out = (int)value;
    return 0;
}

static void usage(FILE *stream)
{
    fprintf(stream, "Uso: iot-gateway [--host <host>] [--port <1..65535>] "
            "[--client-id <id>] [--timeout-ms <1..5000>]\n"
            "     iot-gateway --help\n"
            "Padrões: host=127.0.0.1 port=1883 client-id=smart-industry-gateway timeout-ms=5000\n");
}

/* json-c escapes quotes, backslashes and ASCII controls in untrusted texts.
 * Use the parsed measurements' presence flags to preserve null versus zero.
 */
static int log_telemetry(const GatewayTelemetry *t)
{
    static const char *keys[] = {"machine_id", "machine_type", "timestamp", "temperature",
                                 "current", "power", "state", "failure_within_24h"};
    struct json_object *root = json_object_new_object();
    if (root == NULL) return -1;
    struct json_object *values[] = {
        json_object_new_int(t->machine_id), json_object_new_string(t->machine_type),
        json_object_new_string(t->timestamp),
        t->temperature_present ? json_object_new_double(t->temperature) : NULL,
        t->current_present ? json_object_new_double(t->current) : NULL,
        t->power_present ? json_object_new_double(t->power) : NULL,
        json_object_new_string(t->state), json_object_new_int(t->failure_within_24h)
    };
    const int nullable[] = {0, 0, 0, !t->temperature_present, !t->current_present,
                             !t->power_present, 0, 0};
    int result = -1;
    for (size_t i = 0; i < 8; ++i) {
        if (values[i] == NULL && !nullable[i]) goto done;
        if (json_object_object_add(root, keys[i], values[i]) != 0) goto done;
        values[i] = NULL; /* Ownership transferred to root. */
    }
    const char *json = json_object_to_json_string_ext(root, JSON_C_TO_STRING_PLAIN);
    if (json != NULL && printf("Telemetria válida: %s\n", json) >= 0 && fflush(stdout) == 0) {
        result = 0;
    }
done:
    for (size_t i = 0; i < 8; ++i) json_object_put(values[i]);
    json_object_put(root);
    return result;
}

static void receive_message(const char *topic, const void *payload, size_t length, void *context)
{
    GatewayContext *app = context;
    GatewayTelemetry telemetry;
    GatewayTelemetryError error;
    if (stop_requested || app->log_failed) return;
    if (gateway_telemetry_parse(topic, payload, length, &telemetry, &error) != 0) {
        /* Do not print raw topic/payload: both may contain terminal controls. */
        if (fprintf(stderr, "Telemetria inválida: %s (payload=%zu bytes).\n",
                    gateway_telemetry_error_string(error), length) < 0) app->log_failed = 1;
        return;
    }
    if (log_telemetry(&telemetry) != 0) {
        fprintf(stderr, "Erro ao registrar telemetria válida.\n");
        app->log_failed = 1;
    }
}

int main(int argc, char **argv)
{
    const char *host = "127.0.0.1";
    const char *client_id = "smart-industry-gateway";
    int port = 1883;
    int timeout_ms = 5000;
    unsigned seen = 0;
    GatewayContext app = {0}; /* Callback may execute before connect returns. */
    MqttSubscriber *subscriber = NULL;
    int result = 0;

    if (argc == 2 && strcmp(argv[1], "--help") == 0) {
        usage(stdout);
        return 0;
    }
    for (int i = 1; i < argc; i += 2) {
        unsigned bit;
        if (i + 1 >= argc) goto invalid_arguments;
        if (strcmp(argv[i], "--host") == 0) bit = 1;
        else if (strcmp(argv[i], "--port") == 0) bit = 2;
        else if (strcmp(argv[i], "--client-id") == 0) bit = 4;
        else if (strcmp(argv[i], "--timeout-ms") == 0) bit = 8;
        else goto invalid_arguments;
        if (seen & bit) goto invalid_arguments;
        seen |= bit;
        const char *value = argv[i + 1];
        if (*value == '\0' || strncmp(value, "--", 2) == 0) goto invalid_arguments;
        if (bit == 1) host = value;
        else if (bit == 4) client_id = value;
        else if (bit == 2) {
            if (parse_positive(value, 65535, &port) != 0) goto invalid_arguments;
        } else if (parse_positive(value, 5000, &timeout_ms) != 0) goto invalid_arguments;
    }

    struct sigaction action = {0};
    action.sa_handler = request_stop;
    if (sigemptyset(&action.sa_mask) != 0 || sigaction(SIGINT, &action, NULL) != 0 ||
        sigaction(SIGTERM, &action, NULL) != 0) {
        fprintf(stderr, "Erro ao configurar sinais.\n");
        return 1;
    }
    if (mqtt_subscriber_connect(&subscriber, host, port, client_id, timeout_ms,
                                receive_message, &app) != 0) {
        if (stop_requested) return 0;
        fprintf(stderr, "Erro MQTT ao conectar/assinar: %s.\n",
                mqtt_subscriber_error_string(mqtt_subscriber_last_error()));
        return 1; /* connect already releases partial resources. */
    }
    if (!stop_requested && !app.log_failed) {
        if (puts("Gateway pronto: smart-industry/machines/+/telemetry (QoS 1).") < 0 ||
            fflush(stdout) != 0) app.log_failed = 1;
    }
    while (!stop_requested && !app.log_failed) {
        int rc = mqtt_subscriber_poll(subscriber, 250);
        if (stop_requested) break;
        if (rc != 0) {
            /* Capture before close overwrites the process-local diagnostic. */
            MqttSubscriberError error = mqtt_subscriber_last_error();
            fprintf(stderr, "Erro MQTT ao receber: %s.\n", mqtt_subscriber_error_string(error));
            result = 1;
            break;
        }
    }
    if (app.log_failed) result = 1;
    if (mqtt_subscriber_close(&subscriber) != 0) {
        fprintf(stderr, "Erro MQTT ao encerrar: %s.\n",
                mqtt_subscriber_error_string(mqtt_subscriber_last_error()));
        result = 1;
    }
    if (stop_requested && result == 0) {
        if (puts("Gateway encerrado por solicitação.") < 0 || fflush(stdout) != 0) result = 1;
    }
    return result;

invalid_arguments:
    fprintf(stderr, "Argumentos inválidos.\n");
    usage(stderr);
    return 1;
}
