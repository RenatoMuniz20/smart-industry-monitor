#include "routes.h"

#include "interscity_query.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HEALTH_PATH "/api/health"
#define LATEST_PATH "/api/machines/latest"

/* Escritor com controle de estouro: nunca escreve além de `cap`. */
typedef struct {
    char *buf;
    size_t cap;
    size_t len;
    int overflow;
} Writer;

static void w_raw(Writer *w, const char *s)
{
    size_t n;

    if (w->overflow) {
        return;
    }
    n = strlen(s);
    if (w->len + n + 1 > w->cap) {
        w->overflow = 1;
        return;
    }
    memcpy(w->buf + w->len, s, n);
    w->len += n;
    w->buf[w->len] = '\0';
}

static void w_char(Writer *w, char c)
{
    if (w->overflow) {
        return;
    }
    if (w->len + 2 > w->cap) {
        w->overflow = 1;
        return;
    }
    w->buf[w->len++] = c;
    w->buf[w->len] = '\0';
}

static void w_printf(Writer *w, const char *fmt, ...)
{
    char tmp[256];
    va_list ap;
    int n;

    if (w->overflow) {
        return;
    }
    va_start(ap, fmt);
    n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof tmp) {
        w->overflow = 1;
        return;
    }
    w_raw(w, tmp);
}

static void w_json_string(Writer *w, const char *s)
{
    const unsigned char *p = (const unsigned char *)s;

    w_char(w, '"');
    for (; *p != '\0'; p++) {
        if (*p == '"' || *p == '\\') {
            w_char(w, '\\');
            w_char(w, (char)*p);
        } else if (*p < 0x20) {
            w_printf(w, "\\u%04x", (unsigned int)*p);
        } else {
            w_char(w, (char)*p);
        }
    }
    w_char(w, '"');
}

/* Número no menor formato que reconstrói o mesmo double (49.51, não 49.509999999999998). */
static void w_number(Writer *w, double v)
{
    char tmp[32];
    int prec;

    for (prec = 1; prec <= 17; prec++) {
        snprintf(tmp, sizeof tmp, "%.*g", prec, v);
        if (strtod(tmp, NULL) == v) {
            break;
        }
    }
    w_raw(w, tmp);
}

static void w_sample(Writer *w, const char *name, const IsqSample *s, int textual)
{
    w_printf(w, "\"%s\":", name);
    if (!s->present) {
        w_raw(w, "null");
        return;
    }
    w_raw(w, "{\"value\":");
    if (textual) {
        w_json_string(w, s->text);
    } else {
        w_number(w, s->number);
    }
    w_raw(w, ",\"timestamp\":");
    w_json_string(w, s->date);
    w_char(w, '}');
}

static void fail_body(char *body, size_t body_size, unsigned int *status,
                      unsigned int code, const char *message)
{
    *status = code;
    snprintf(body, body_size, "{\"error\":\"%s\"}", message);
}

static void build_machines(Writer *w, const RouteContext *ctx,
                           const IsqMachine *results, size_t count)
{
    w_raw(w, "{\"machines\":[");
    for (size_t i = 0; i < ctx->machine_count; i++) {
        const MachineEntry *m = &ctx->machines[i];
        const IsqMachine *r = NULL;

        if (i > 0) {
            w_char(w, ',');
        }
        for (size_t j = 0; j < count; j++) {
            if (strcmp(results[j].uuid, m->uuid) == 0) {
                r = &results[j];
                break;
            }
        }

        w_printf(w, "{\"machine_id\":%d,\"uuid\":", m->machine_id);
        w_json_string(w, m->uuid);
        w_char(w, ',');
        if (r != NULL) {
            w_sample(w, "operating_state", &r->operating_state, 1);
            w_char(w, ',');
            w_sample(w, "motor_temperature", &r->motor_temperature, 0);
            w_char(w, ',');
            w_sample(w, "current", &r->current, 0);
            w_char(w, ',');
            w_sample(w, "failure_within_24h", &r->failure_within_24h, 0);
        } else {
            w_raw(w, "\"operating_state\":null,\"motor_temperature\":null,"
                     "\"current\":null,\"failure_within_24h\":null");
        }
        w_char(w, '}');
    }
    w_raw(w, "]}");
}

static void handle_latest(const RouteContext *ctx, char *body, size_t body_size,
                          unsigned int *status)
{
    const char **uuids = NULL;
    IsqMachine *results = NULL;
    size_t count = 0;
    size_t n = ctx->machine_count;
    char err[256];
    Writer w;

    *status = 200;
    if (n == 0) {
        snprintf(body, body_size, "%s", "{\"machines\":[]}");
        return;
    }

    uuids = malloc(n * sizeof *uuids);
    results = malloc(n * sizeof *results);
    if (uuids == NULL || results == NULL) {
        free(uuids);
        free(results);
        fail_body(body, body_size, status, 500, "memória insuficiente");
        return;
    }
    for (size_t i = 0; i < n; i++) {
        uuids[i] = ctx->machines[i].uuid;
    }

    if (interscity_query_last(ctx->interscity_url, ctx->interscity_timeout_ms,
                              uuids, n, results, n, &count, err, sizeof err) != 0) {
        fprintf(stderr, "erro ao consultar o InterSCity: %s\n", err);
        fail_body(body, body_size, status, 502, "falha ao consultar o InterSCity");
    } else {
        w.buf = body;
        w.cap = body_size;
        w.len = 0;
        w.overflow = 0;
        body[0] = '\0';

        build_machines(&w, ctx, results, count);
        if (w.overflow) {
            fail_body(body, body_size, status, 500, "resposta grande demais");
        }
    }

    free(uuids);
    free(results);
}

int routes_handle(void *cls, const char *method, const char *url,
                  char *body, size_t body_size, unsigned int *status)
{
    const RouteContext *ctx = cls;

    if (ctx == NULL || body == NULL || body_size == 0 || status == NULL) {
        return -1;
    }

    if (strcmp(url, HEALTH_PATH) == 0) {
        if (strcmp(method, "GET") != 0) {
            fail_body(body, body_size, status, 405, "Method not allowed");
        } else {
            *status = 200;
            snprintf(body, body_size, "%s", "{\"status\":\"ok\"}");
        }
        return 0;
    }

    if (strcmp(url, LATEST_PATH) == 0) {
        if (strcmp(method, "GET") != 0) {
            fail_body(body, body_size, status, 405, "Method not allowed");
        } else {
            handle_latest(ctx, body, body_size, status);
        }
        return 0;
    }

    fail_body(body, body_size, status, 404, "Not found");
    return 0;
}
