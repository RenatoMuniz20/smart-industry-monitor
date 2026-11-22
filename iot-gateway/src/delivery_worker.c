#define _POSIX_C_SOURCE 200809L
#include "delivery_worker.h"
#include "interscity_client.h"
#include "telemetry_queue.h"

#include <errno.h>
#include <inttypes.h>
#include <json-c/json.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Both waits are interruptible and hold no SQLite transaction. */
#define RECONCILE_INITIAL_MS 2000u
#define CONSISTENCY_WINDOW_MS 2000u

struct DeliveryWorker {
    pthread_t thread;
    atomic_bool stop;
    atomic_bool failed;
    atomic_bool ready;
    char *path;
    InterscityClient *client; /* Created before pthread_create; used only by run. */
    unsigned long delivered, retried, blocked, recovered; /* Read only after join. */
};

static void fatal(DeliveryWorker *w, const char *operation, const TelemetryQueueError *error)
{
    struct json_object *text = json_object_new_string(error->message);
    const char *escaped = text ? json_object_to_json_string_ext(text, JSON_C_TO_STRING_PLAIN) : NULL;
    fprintf(stderr, "Worker HTTP: erro fatal %s (código %d): %s.\n", operation,
            error->sqlite_code, escaped ? escaped : "diagnóstico indisponível");
    json_object_put(text);
    atomic_store(&w->failed, true);
}

static int pause_ms(DeliveryWorker *w, unsigned milliseconds)
{
    while (milliseconds && !atomic_load(&w->stop)) {
        unsigned step = milliseconds < 50 ? milliseconds : 50;
        struct timespec remaining = {0, (long)step * 1000000L};
        while (nanosleep(&remaining, &remaining) != 0) {
            if (errno != EINTR) {
                fprintf(stderr, "Worker HTTP: erro fatal na espera.\n");
                atomic_store(&w->failed, true);
                return -1;
            }
            if (atomic_load(&w->stop)) return 0;
        }
        milliseconds -= step;
    }
    return 0;
}

static unsigned next_delay(unsigned delay_ms)
{
    return delay_ms >= 16000 ? 30000 : delay_ms * 2;
}

/* Resolve only the FIFO head. Failure to consult never means absence. Two
 * consecutive absent/older answers are required before authorizing a retry;
 * a failed consultation resets that evidence. /last cannot prove contents. */
static void reconcile(DeliveryWorker *w, TelemetryQueue *queue,
                      const TelemetryQueueItem *item, unsigned *delay_ms)
{
    int absence_seen = 0;
    fprintf(stderr, "Worker HTTP: id=%" PRId64 " inflight; início da reconciliação.\n", item->id);
    if (pause_ms(w, RECONCILE_INITIAL_MS) != 0) return;
    while (!atomic_load(&w->stop) && !atomic_load(&w->failed)) {
        int found = 0;
        char stored[INTERSCITY_CLIENT_TIMESTAMP_CAPACITY];
        InterscityClientError query_error = INTERSCITY_CLIENT_OK;
        if (interscity_client_last_timestamp(w->client, item->uuid, stored, sizeof stored,
                                             &found, &query_error) != 0) {
            absence_seen = 0;
            fprintf(stderr, "Worker HTTP: id=%" PRId64 " reconciliação indisponível: %s (código %d); "
                    "preservado inflight; nova consulta em %u ms.\n", item->id,
                    interscity_client_error_string(query_error), (int)query_error, *delay_ms);
            if (pause_ms(w, *delay_ms) != 0) return;
            *delay_ms = next_delay(*delay_ms);
            continue;
        }
        TelemetryQueueError error;
        int comparison = found ? strcmp(stored, item->timestamp) : -1;
        if (found && comparison == 0) {
            if (telemetry_queue_confirm(queue, item->id, &error) != TELEMETRY_QUEUE_OK) {
                fatal(w, "ao confirmar SQLite após consulta", &error);
            } else {
                w->recovered++;
                *delay_ms = 1000;
                if (printf("Worker HTTP: id=%" PRId64 " recuperada pelo Data Collector "
                           "(timestamp igual; SQLite confirmado); retomando fila.\n", item->id) < 0 ||
                    fflush(stdout) != 0) atomic_store(&w->failed, true);
            }
            return;
        }
        if (atomic_load(&w->stop)) return; /* Do not authorize a new send on stop. */
        if (comparison < 0 && !absence_seen) {
            absence_seen = 1;
            fprintf(stderr, "Worker HTTP: id=%" PRId64 " amostra ausente ou anterior; "
                    "aguardando %u ms de consistência antes da segunda consulta.\n",
                    item->id, CONSISTENCY_WINDOW_MS);
            if (pause_ms(w, CONSISTENCY_WINDOW_MS) != 0) return;
            continue;
        }
        const char *reason = comparison > 0 ?
            "Data Collector posterior: amostra atual não comprovada; risco de duplicação" :
            "Data Collector sem amostra atual em duas consultas; reenvio autorizado";
        if (telemetry_queue_retry(queue, item->id, reason, &error) != TELEMETRY_QUEUE_OK) {
            fatal(w, "ao autorizar nova tentativa SQLite", &error);
        } else {
            w->retried++;
            fprintf(stderr, "Worker HTTP: id=%" PRId64 " nova tentativa autorizada: %s; "
                    "pending; retomando fila.\n", item->id, reason);
        }
        return;
    }
}

static void *run(void *context)
{
    DeliveryWorker *w = context;
    TelemetryQueue *queue = NULL;
    TelemetryQueueError error;
    unsigned delay_ms = 1000;
    if (telemetry_queue_open(w->path, &queue, &error) != TELEMETRY_QUEUE_OK) {
        fatal(w, "ao abrir SQLite", &error);
        atomic_store(&w->ready, true);
        return NULL;
    }
    atomic_store(&w->ready, true);
    while (!atomic_load(&w->stop) && !atomic_load(&w->failed)) {
        TelemetryQueueItem item = {0};
        TelemetryQueueResult peek = telemetry_queue_peek(queue, &item, &error);
        if (peek == TELEMETRY_QUEUE_EMPTY) {
            if (pause_ms(w, 250) != 0) break;
            continue;
        }
        if (peek != TELEMETRY_QUEUE_OK) {
            fatal(w, "ao consultar SQLite", &error);
            break;
        }
        if (item.status == TELEMETRY_QUEUE_INFLIGHT) {
            reconcile(w, queue, &item, &delay_ms);
            telemetry_queue_item_clear(&item);
            continue;
        }
        if (atomic_load(&w->stop)) {
            telemetry_queue_item_clear(&item);
            break;
        }
        if (telemetry_queue_start_attempt(queue, item.id, &error) != TELEMETRY_QUEUE_OK) {
            fatal(w, "ao iniciar tentativa SQLite", &error);
            telemetry_queue_item_clear(&item);
            break;
        }
        if (atomic_load(&w->stop)) {
            telemetry_queue_item_clear(&item); /* Keep inflight for next startup. */
            break;
        }
        /* No SQLite transaction survives start_attempt. Finish this attempt's
         * transition even if main requests stop during the HTTP call. */
        InterscityClientResult response = {0};
        InterscityClientError http_error = INTERSCITY_CLIENT_OK;
        int posted = interscity_client_post_data(w->client, item.uuid, item.payload_json,
                                                strlen(item.payload_json), &response, &http_error);
        int transient = 0;
        if (posted == 0) {
            if (telemetry_queue_confirm(queue, item.id, &error) != TELEMETRY_QUEUE_OK) {
                fatal(w, "ao confirmar SQLite após HTTP 201", &error);
            } else {
                w->delivered++;
                delay_ms = 1000;
                if (printf("Worker HTTP: id=%" PRId64 " machine_id=%d entregue (HTTP 201; SQLite confirmado).\n",
                           item.id, item.machine_id) < 0 || fflush(stdout) != 0) atomic_store(&w->failed, true);
            }
        } else if (interscity_client_is_ambiguous(http_error)) {
            fprintf(stderr, "Worker HTTP: id=%" PRId64 " resultado ambíguo (%s); preservado inflight; reconciliação automática após janela inicial.\n",
                    item.id, interscity_client_error_string(http_error));
        } else if (http_error == INTERSCITY_CLIENT_HTTP_STATUS) {
            char reason[96];
            int length = snprintf(reason, sizeof reason, "HTTP %ld", response.http_status);
            if (length < 0 || (size_t)length >= sizeof reason) {
                atomic_store(&w->failed, true);
            } else if (response.http_status == 429 || (response.http_status >= 500 && response.http_status <= 599)) {
                if (telemetry_queue_retry(queue, item.id, reason, &error) != TELEMETRY_QUEUE_OK) {
                    fatal(w, "ao reagendar SQLite", &error);
                } else {
                    w->retried++;
                    transient = 1;
                    fprintf(stderr, "Worker HTTP: id=%" PRId64 " %s; pending; nova tentativa em %u ms.\n",
                            item.id, reason, delay_ms);
                }
            } else if (telemetry_queue_block(queue, item.id, reason, &error) != TELEMETRY_QUEUE_OK) {
                fatal(w, "ao bloquear SQLite", &error);
            } else {
                w->blocked++;
                fprintf(stderr, "Worker HTTP: id=%" PRId64 " blocked (%s).\n", item.id, reason);
            }
        } else {
            fprintf(stderr, "Worker HTTP: erro fatal local no POST: %s; preservado inflight.\n",
                    interscity_client_error_string(http_error));
            atomic_store(&w->failed, true);
        }
        telemetry_queue_item_clear(&item);
        if (atomic_load(&w->failed)) break;
        if (transient) {
            if (pause_ms(w, delay_ms) != 0) break;
            delay_ms = next_delay(delay_ms);
        }
    }
    if (telemetry_queue_close(&queue, &error) != TELEMETRY_QUEUE_OK) fatal(w, "ao fechar SQLite", &error);
    return NULL;
}

int delivery_worker_failed(const DeliveryWorker *w)
{
    return w != NULL && atomic_load(&w->failed);
}

int delivery_worker_start(DeliveryWorker **out, const char *path, const char *url, long timeout_ms)
{
    if (!out || *out || !path || !*path || !url) {
        fprintf(stderr, "Worker HTTP: argumentos inválidos.\n");
        return -1;
    }
    DeliveryWorker *w = calloc(1, sizeof *w);
    if (!w) {
        fprintf(stderr, "Worker HTTP: memória insuficiente.\n");
        return -1;
    }
    atomic_init(&w->stop, false);
    atomic_init(&w->failed, false);
    atomic_init(&w->ready, false);
    w->path = strdup(path);
    if (!w->path) {
        fprintf(stderr, "Worker HTTP: memória insuficiente.\n");
        free(w);
        return -1;
    }
    InterscityClientError error = INTERSCITY_CLIENT_OK;
    if (interscity_client_create(&w->client, url, timeout_ms, &error) != 0) {
        fprintf(stderr, "Worker HTTP: erro ao inicializar cliente: %s.\n", interscity_client_error_string(error));
        free(w->path);
        free(w);
        return -1;
    }
    int rc = pthread_create(&w->thread, NULL, run, w);
    if (rc != 0) {
        fprintf(stderr, "Worker HTTP: pthread_create falhou (código %d).\n", rc);
        interscity_client_destroy(&w->client);
        free(w->path);
        free(w);
        return -1;
    }
    *out = w;
    /* Startup handshake: do not consume MQTT if the worker DB could not open.
     * The client is already initialized; main never uses its easy handle. */
    while (!atomic_load(&w->ready)) {
        struct timespec nap = {0, 10000000L};
        nanosleep(&nap, NULL);
    }
    if (delivery_worker_failed(w)) {
        delivery_worker_stop(out);
        return -1;
    }
    return 0;
}

int delivery_worker_stop(DeliveryWorker **worker)
{
    if (!worker || !*worker) return 0;
    DeliveryWorker *w = *worker;
    atomic_store(&w->stop, true);
    int rc = pthread_join(w->thread, NULL);
    if (rc != 0) {
        /* Do not free resources that a thread might still be using. */
        fprintf(stderr, "Worker HTTP: pthread_join falhou (código %d).\n", rc);
        return -1;
    }
    int result = delivery_worker_failed(w) ? -1 : 0;
    if (printf("Worker HTTP: %lu entregues, %lu retentativas, %lu bloqueadas, %lu recuperadas pelo Data Collector.\n",
               w->delivered, w->retried, w->blocked, w->recovered) < 0 || fflush(stdout) != 0) result = -1;
    interscity_client_destroy(&w->client);
    free(w->path);
    free(w);
    *worker = NULL;
    return result;
}
