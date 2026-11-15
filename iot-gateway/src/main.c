#define _POSIX_C_SOURCE 200809L
#include "gateway_telemetry.h"
#include "interscity_client.h"
#include "interscity_mapping.h"
#include "interscity_payload.h"
#include "mqtt_subscriber.h"

#include <errno.h>
#include <inttypes.h>
#include <json-c/json.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define TIMESTAMP_TEXT INTERSCITY_CLIENT_TIMESTAMP_CAPACITY /* AAAA-MM-DDTHH:MM:SS + NUL */
#define RECONNECT_FIRST_MS 1000u   /* first pause after losing the broker */
#define RECONNECT_MAX_MS 30000u    /* pause never grows beyond this */
#define CONSULT_WAIT_MS 2000u      /* time for the Adaptor -> Data Collector path to store */

static volatile sig_atomic_t stop_requested;

/* Last timestamp delivered per machine in this run (dedupe is in memory only). */
typedef struct {
    int machine_id;
    char last[TIMESTAMP_TEXT];
} DeliveredEntry;

typedef struct {
    int log_failed;
    InterscityClient *interscity;      /* NULL: no forwarding, SIM-04 behavior */
    const InterscityMapping *mapping;
    unsigned long sent;                /* confirmed by HTTP 201 or by the Data Collector */
    unsigned long failed;
    unsigned long unmapped;
    unsigned long duplicates;          /* skipped: timestamp already delivered */
    unsigned long recovered;           /* ambiguous failure, but the Data Collector had it */
    unsigned long resent;              /* ambiguous failure, not stored: sent once more */
    unsigned long reconnects;
    DeliveredEntry *delivered;
    size_t delivered_count;
    size_t delivered_capacity;
} GatewayContext;

/* Static because of its size (about 45 KB); loaded once before MQTT starts. */
static InterscityMapping interscity_machines;

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
            "                  [--interscity-url <url> --interscity-map <arquivo> "
            "[--interscity-timeout-ms <1..60000>]]\n"
            "     iot-gateway --help\n"
            "Padrões: host=127.0.0.1 port=1883 client-id=smart-industry-gateway timeout-ms=5000\n"
            "InterSCity (opcional): sem --interscity-url e --interscity-map nada é enviado; "
            "interscity-timeout-ms=5000\n");
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

/* Pause that stops early when a stop is requested (checked every 50 ms). */
static void sleep_ms(unsigned milliseconds)
{
    while (milliseconds > 0 && !stop_requested) {
        unsigned step = milliseconds < 50 ? milliseconds : 50;
        struct timespec nap = {0, (long)step * 1000000L};
        nanosleep(&nap, NULL);
        milliseconds -= step;
    }
}

/* "AAAA-MM-DD HH:MM:SS" or "AAAA-MM-DDTHH:MM:SS", already validated by the
 * payload builder, becomes "AAAA-MM-DDTHH:MM:SS" so that strcmp orders samples.
 */
static void normalize_timestamp(const char *in, char out[TIMESTAMP_TEXT])
{
    memcpy(out, in, 19);
    out[10] = 'T';
    out[19] = '\0';
}

static const char *last_delivered(const GatewayContext *app, int machine_id)
{
    for (size_t i = 0; i < app->delivered_count; ++i) {
        if (app->delivered[i].machine_id == machine_id) return app->delivered[i].last;
    }
    return NULL;
}

static int remember_delivered(GatewayContext *app, int machine_id, const char *timestamp)
{
    for (size_t i = 0; i < app->delivered_count; ++i) {
        if (app->delivered[i].machine_id == machine_id) {
            memcpy(app->delivered[i].last, timestamp, TIMESTAMP_TEXT);
            return 0;
        }
    }
    if (app->delivered_count == app->delivered_capacity) {
        size_t capacity = app->delivered_capacity != 0 ? app->delivered_capacity * 2 : 16;
        DeliveredEntry *grown = realloc(app->delivered, capacity * sizeof *grown);
        if (grown == NULL) return -1;
        app->delivered = grown;
        app->delivered_capacity = capacity;
    }
    DeliveredEntry *entry = &app->delivered[app->delivered_count++];
    entry->machine_id = machine_id;
    memcpy(entry->last, timestamp, TIMESTAMP_TEXT);
    return 0;
}

static void report_delivery(GatewayContext *app, const GatewayTelemetry *t,
                            const char *timestamp, const InterscityClientResult *result)
{
    app->sent++;
    if (remember_delivered(app, t->machine_id, timestamp) != 0) app->log_failed = 1;
    if (printf("InterSCity: machine_id=%d enviada (HTTP %ld, %ld ms, conexão %s).\n",
               t->machine_id, result->http_status, result->elapsed_ms,
               result->new_connection ? "nova" : "reutilizada") < 0 || fflush(stdout) != 0) {
        app->log_failed = 1;
    }
}

static void report_post_failure(GatewayContext *app, const GatewayTelemetry *t,
                                InterscityClientError error, const InterscityClientResult *result)
{
    if (fprintf(stderr, "InterSCity: machine_id=%d falhou: %s (HTTP %ld, %ld ms).\n",
                t->machine_id, interscity_client_error_string(error), result->http_status,
                result->elapsed_ms) < 0) app->log_failed = 1;
}

/* Sends one validated telemetry to the InterSCity Resource Adaptor.
 *
 * - HTTP 201: delivered.
 * - Other HTTP status: definite refusal, logged, not resent.
 * - Timeout or network error: ambiguous. The Adaptor may have accepted the
 *   sample, so the Data Collector is consulted after a pause. Only if it does
 *   not have the sample is the POST sent once more. If the consultation itself
 *   fails, nothing is resent.
 *
 * A sample already delivered in this run (same machine, same or older
 * timestamp) is skipped. Failures never stop the Gateway.
 */
static void forward_to_interscity(GatewayContext *app, const GatewayTelemetry *t)
{
    const char *uuid = interscity_mapping_lookup(app->mapping, t->machine_id);
    if (uuid == NULL) {
        app->unmapped++;
        if (fprintf(stderr, "InterSCity: machine_id=%d sem UUID no mapeamento; não enviada.\n",
                    t->machine_id) < 0) app->log_failed = 1;
        return;
    }
    char body[INTERSCITY_PAYLOAD_CAPACITY];
    size_t length = 0;
    InterscityPayloadError payload_error = INTERSCITY_PAYLOAD_OK;
    if (interscity_payload_build(t, body, sizeof body, &length, &payload_error) != 0) {
        app->failed++;
        if (fprintf(stderr, "InterSCity: machine_id=%d não enviada: %s.\n", t->machine_id,
                    interscity_payload_error_string(payload_error)) < 0) app->log_failed = 1;
        return;
    }
    char timestamp[TIMESTAMP_TEXT];
    normalize_timestamp(t->timestamp, timestamp);
    const char *last = last_delivered(app, t->machine_id);
    if (last != NULL && strcmp(timestamp, last) <= 0) {
        app->duplicates++;
        if (printf("InterSCity: machine_id=%d ignorada (ts %s já enviado).\n", t->machine_id,
                   timestamp) < 0 || fflush(stdout) != 0) app->log_failed = 1;
        return;
    }

    InterscityClientResult result = {0, 0, 0};
    InterscityClientError error = INTERSCITY_CLIENT_OK;
    if (interscity_client_post_data(app->interscity, uuid, body, length, &result, &error) == 0) {
        report_delivery(app, t, timestamp, &result);
        return;
    }
    if (!interscity_client_is_ambiguous(error)) {
        app->failed++;
        report_post_failure(app, t, error, &result);
        return;
    }

    /* Ambiguous: the Adaptor may already have published this sample. */
    if (fprintf(stderr, "InterSCity: machine_id=%d sem resposta (%s); consultando o Data "
                "Collector antes de qualquer reenvio.\n", t->machine_id,
                interscity_client_error_string(error)) < 0) app->log_failed = 1;
    sleep_ms(CONSULT_WAIT_MS);

    int found = 0;
    char stored[TIMESTAMP_TEXT];
    InterscityClientError consult_error = INTERSCITY_CLIENT_OK;
    if (interscity_client_last_timestamp(app->interscity, uuid, stored, sizeof stored,
                                         &found, &consult_error) != 0) {
        app->failed++;
        if (fprintf(stderr, "InterSCity: machine_id=%d não confirmada (consulta falhou: %s); "
                    "não reenviada.\n", t->machine_id,
                    interscity_client_error_string(consult_error)) < 0) app->log_failed = 1;
        return;
    }
    if (found && strcmp(stored, timestamp) >= 0) {
        /* No HTTP response was received for the POST, so no "enviada" line here. */
        app->sent++;
        app->recovered++;
        if (remember_delivered(app, t->machine_id, timestamp) != 0) app->log_failed = 1;
        if (printf("InterSCity: machine_id=%d confirmada pelo Data Collector; não reenviada.\n",
                   t->machine_id) < 0 || fflush(stdout) != 0) app->log_failed = 1;
        return;
    }

    app->resent++;
    InterscityClientResult again = {0, 0, 0};
    if (interscity_client_post_data(app->interscity, uuid, body, length, &again, &error) == 0) {
        report_delivery(app, t, timestamp, &again);
        return;
    }
    app->failed++;
    report_post_failure(app, t, error, &again);
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
        return;
    }
    if (app->interscity != NULL) forward_to_interscity(app, &telemetry);
}

/* Reconnects with exponential pause (1 s, 2 s, ... up to 30 s) until the broker
 * accepts the subscription again, a stop is requested, or logging fails. The
 * broker does not keep messages for a client that was away (clean session), so
 * samples published during the outage are lost; this is documented.
 */
static int reconnect_mqtt(GatewayContext *app, MqttSubscriber **subscriber, const char *host,
                          int port, const char *client_id, int timeout_ms)
{
    unsigned delay_ms = RECONNECT_FIRST_MS;
    while (!stop_requested && !app->log_failed) {
        sleep_ms(delay_ms);
        if (stop_requested) return 0;
        if (mqtt_subscriber_connect(subscriber, host, port, client_id, timeout_ms,
                                    receive_message, app) == 0) {
            app->reconnects++;
            if (puts("MQTT reconectado: assinatura renovada.") < 0 || fflush(stdout) != 0) {
                app->log_failed = 1;
                return 0;
            }
            return 1;
        }
        unsigned next_ms = delay_ms * 2 > RECONNECT_MAX_MS ? RECONNECT_MAX_MS : delay_ms * 2;
        if (fprintf(stderr, "Erro MQTT ao reconectar: %s. Nova tentativa em %u ms.\n",
                    mqtt_subscriber_error_string(mqtt_subscriber_last_error()), next_ms) < 0) {
            app->log_failed = 1;
        }
        delay_ms = next_ms;
    }
    return 0;
}

int main(int argc, char **argv)
{
    const char *host = "127.0.0.1";
    const char *client_id = "smart-industry-gateway";
    int port = 1883;
    int timeout_ms = 5000;
    const char *interscity_url = NULL;
    const char *interscity_map = NULL;
    int interscity_timeout_ms = 5000;
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
        else if (strcmp(argv[i], "--interscity-url") == 0) bit = 16;
        else if (strcmp(argv[i], "--interscity-map") == 0) bit = 32;
        else if (strcmp(argv[i], "--interscity-timeout-ms") == 0) bit = 64;
        else goto invalid_arguments;
        if (seen & bit) goto invalid_arguments;
        seen |= bit;
        const char *value = argv[i + 1];
        if (*value == '\0' || strncmp(value, "--", 2) == 0) goto invalid_arguments;
        if (bit == 1) host = value;
        else if (bit == 4) client_id = value;
        else if (bit == 16) interscity_url = value;
        else if (bit == 32) interscity_map = value;
        else if (bit == 2) {
            if (parse_positive(value, 65535, &port) != 0) goto invalid_arguments;
        } else if (bit == 64) {
            if (parse_positive(value, 60000, &interscity_timeout_ms) != 0) goto invalid_arguments;
        } else if (parse_positive(value, 5000, &timeout_ms) != 0) goto invalid_arguments;
    }
    /* Forwarding needs both URL and mapping; its timeout alone is meaningless. */
    if ((interscity_url == NULL) != (interscity_map == NULL)) goto invalid_arguments;
    if (interscity_url == NULL && (seen & 64)) goto invalid_arguments;

    struct sigaction action = {0};
    action.sa_handler = request_stop;
    if (sigemptyset(&action.sa_mask) != 0 || sigaction(SIGINT, &action, NULL) != 0 ||
        sigaction(SIGTERM, &action, NULL) != 0) {
        fprintf(stderr, "Erro ao configurar sinais.\n");
        return 1;
    }
    if (interscity_url != NULL) {
        /* Before MQTT: callbacks may run while connect is still in progress. */
        InterscityClientError client_error = INTERSCITY_CLIENT_OK;
        if (interscity_client_create(&app.interscity, interscity_url, interscity_timeout_ms,
                                     &client_error) != 0) {
            if (client_error == INTERSCITY_CLIENT_ARGUMENT) goto invalid_arguments;
            fprintf(stderr, "Erro InterSCity ao inicializar: %s.\n",
                    interscity_client_error_string(client_error));
            return 1;
        }
        InterscityMappingError map_error = INTERSCITY_MAPPING_OK;
        size_t line = 0;
        if (interscity_mapping_load(interscity_map, &interscity_machines, &map_error, &line) != 0) {
            if (line > 0) {
                fprintf(stderr, "Erro no mapeamento InterSCity: %s (linha %zu).\n",
                        interscity_mapping_error_string(map_error), line);
            } else {
                fprintf(stderr, "Erro no mapeamento InterSCity: %s.\n",
                        interscity_mapping_error_string(map_error));
            }
            interscity_client_destroy(&app.interscity);
            return 1;
        }
        app.mapping = &interscity_machines;
        /* The URL was validated by the client: no spaces or control characters. */
        if (printf("InterSCity habilitado: %s (%zu máquinas mapeadas, timeout %d ms).\n",
                   interscity_url, interscity_machines.count, interscity_timeout_ms) < 0 ||
            fflush(stdout) != 0) {
            interscity_client_destroy(&app.interscity);
            return 1;
        }
    }
    if (mqtt_subscriber_connect(&subscriber, host, port, client_id, timeout_ms,
                                receive_message, &app) != 0) {
        int code = stop_requested ? 0 : 1;
        if (!stop_requested) {
            fprintf(stderr, "Erro MQTT ao conectar/assinar: %s.\n",
                    mqtt_subscriber_error_string(mqtt_subscriber_last_error()));
        }
        interscity_client_destroy(&app.interscity);
        return code; /* connect already releases partial resources. */
    }
    if (!stop_requested && !app.log_failed) {
        if (puts("Gateway pronto: smart-industry/machines/+/telemetry (QoS 1).") < 0 ||
            fflush(stdout) != 0) app.log_failed = 1;
    }
    while (!stop_requested && !app.log_failed) {
        int rc = mqtt_subscriber_poll(subscriber, 250);
        if (stop_requested) break;
        if (rc == 0) continue;
        /* Capture before close overwrites the process-local diagnostic. */
        MqttSubscriberError error = mqtt_subscriber_last_error();
        if (fprintf(stderr, "Erro MQTT ao receber: %s. Reconectando.\n",
                    mqtt_subscriber_error_string(error)) < 0) app.log_failed = 1;
        mqtt_subscriber_close(&subscriber); /* the broken session is discarded */
        if (!reconnect_mqtt(&app, &subscriber, host, port, client_id, timeout_ms)) break;
    }
    if (app.log_failed) result = 1;
    if (mqtt_subscriber_close(&subscriber) != 0) {
        fprintf(stderr, "Erro MQTT ao encerrar: %s.\n",
                mqtt_subscriber_error_string(mqtt_subscriber_last_error()));
        result = 1;
    }
    if (app.interscity != NULL) {
        if (printf("InterSCity: %lu enviadas, %lu falhas, %lu sem mapeamento.\n",
                   app.sent, app.failed, app.unmapped) < 0 ||
            printf("InterSCity: %lu duplicadas ignoradas, %lu confirmadas pelo Data Collector, "
                   "%lu reenviadas.\n", app.duplicates, app.recovered, app.resent) < 0 ||
            fflush(stdout) != 0) result = 1;
        interscity_client_destroy(&app.interscity);
    }
    free(app.delivered);
    if (stop_requested && result == 0) {
        if (puts("Gateway encerrado por solicitação.") < 0 || fflush(stdout) != 0) result = 1;
    }
    return result;

invalid_arguments:
    fprintf(stderr, "Argumentos inválidos.\n");
    usage(stderr);
    return 1;
}
