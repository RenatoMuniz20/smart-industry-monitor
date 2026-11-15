#include "interscity_query.h"

#include <curl/curl.h>
#include <ctype.h>
#include <json-c/json.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LAST_PATH "/collector/resources/data/last"

typedef struct {
    char *data;
    size_t len;
} Buffer;

static void fail(char *error, size_t error_size, const char *fmt, ...)
{
    va_list ap;

    if (error == NULL || error_size == 0) {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(error, error_size, fmt, ap);
    va_end(ap);
}

static void copy_str(char *dst, size_t dst_size, const char *src)
{
    snprintf(dst, dst_size, "%s", src != NULL ? src : "");
}

static int uuid_is_safe(const char *uuid)
{
    size_t n = strlen(uuid);

    if (n != ISQ_UUID_LEN) {
        return 0;
    }
    for (size_t i = 0; i < n; i++) {
        if (!isxdigit((unsigned char)uuid[i]) && uuid[i] != '-') {
            return 0;
        }
    }
    return 1;
}

static char *build_request_body(const char *const *uuids, size_t count)
{
    size_t cap = 32 + count * (ISQ_UUID_LEN + 4);
    size_t used = 0;
    char *body = malloc(cap);

    if (body == NULL) {
        return NULL;
    }
    used += (size_t)snprintf(body, cap, "{\"uuids\":[");
    for (size_t i = 0; i < count; i++) {
        used += (size_t)snprintf(body + used, cap - used, "%s\"%s\"",
                                 i > 0 ? "," : "", uuids[i]);
    }
    snprintf(body + used, cap - used, "]}");
    return body;
}

static size_t write_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    Buffer *buf = userdata;
    size_t n = size * nmemb;
    char *tmp = realloc(buf->data, buf->len + n + 1);

    if (tmp == NULL) {
        return 0;
    }
    buf->data = tmp;
    memcpy(buf->data + buf->len, ptr, n);
    buf->len += n;
    buf->data[buf->len] = '\0';
    return n;
}

static int http_post_json(const char *url, char *body, long timeout_ms,
                          Buffer *resp, long *status, char *error, size_t error_size)
{
    CURL *curl;
    struct curl_slist *headers = NULL;
    CURLcode rc;

    curl = curl_easy_init();
    if (curl == NULL) {
        fail(error, error_size, "falha ao iniciar libcurl");
        return -1;
    }

    headers = curl_slist_append(headers, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(body));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, resp);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    rc = curl_easy_perform(curl);
    if (rc == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, status);
    } else {
        fail(error, error_size, "falha de rede: %s", curl_easy_strerror(rc));
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return rc == CURLE_OK ? 0 : -1;
}

static void parse_sample(json_object *array, IsqSample *out)
{
    json_object *first;
    json_object *value = NULL;
    json_object *date = NULL;

    memset(out, 0, sizeof *out);
    if (array == NULL || !json_object_is_type(array, json_type_array)
        || json_object_array_length(array) == 0) {
        return;
    }
    first = json_object_array_get_idx(array, 0);
    if (first == NULL || !json_object_object_get_ex(first, "value", &value) || value == NULL) {
        return;
    }

    if (json_object_is_type(value, json_type_string)) {
        copy_str(out->text, sizeof out->text, json_object_get_string(value));
    } else if (json_object_is_type(value, json_type_int)
               || json_object_is_type(value, json_type_double)) {
        out->number = json_object_get_double(value);
    } else {
        return;
    }
    out->present = 1;

    if (json_object_object_get_ex(first, "date", &date) && date != NULL) {
        copy_str(out->date, sizeof out->date, json_object_get_string(date));
    }
}

static int parse_response(const char *text, IsqMachine *out, size_t out_capacity,
                          size_t *out_count, char *error, size_t error_size)
{
    json_object *root;
    json_object *resources = NULL;
    size_t n;

    root = json_tokener_parse(text);
    if (root == NULL) {
        fail(error, error_size, "resposta não é JSON válido");
        return -1;
    }
    if (!json_object_object_get_ex(root, "resources", &resources) || resources == NULL
        || !json_object_is_type(resources, json_type_array)) {
        json_object_put(root);
        fail(error, error_size, "resposta sem a lista 'resources'");
        return -1;
    }

    n = json_object_array_length(resources);
    if (n > out_capacity) {
        json_object_put(root);
        fail(error, error_size, "espaço insuficiente para %zu máquinas", n);
        return -1;
    }

    for (size_t i = 0; i < n; i++) {
        json_object *res = json_object_array_get_idx(resources, i);
        json_object *uuid = NULL;
        json_object *caps = NULL;
        json_object *arr = NULL;
        IsqMachine *m = &out[i];

        memset(m, 0, sizeof *m);
        if (res == NULL || !json_object_object_get_ex(res, "uuid", &uuid) || uuid == NULL
            || !json_object_object_get_ex(res, "capabilities", &caps) || caps == NULL) {
            json_object_put(root);
            fail(error, error_size, "recurso sem 'uuid' ou 'capabilities' na posição %zu", i);
            return -1;
        }
        copy_str(m->uuid, sizeof m->uuid, json_object_get_string(uuid));

        if (json_object_object_get_ex(caps, "sim_operating_state", &arr)) {
            parse_sample(arr, &m->operating_state);
        }
        if (json_object_object_get_ex(caps, "sim_motor_temperature", &arr)) {
            parse_sample(arr, &m->motor_temperature);
        }
        if (json_object_object_get_ex(caps, "sim_current", &arr)) {
            parse_sample(arr, &m->current);
        }
        if (json_object_object_get_ex(caps, "sim_failure_within_24h", &arr)) {
            parse_sample(arr, &m->failure_within_24h);
        }
    }

    *out_count = n;
    json_object_put(root);
    return 0;
}

int interscity_query_last(const char *base_url, long timeout_ms,
                          const char *const *uuids, size_t uuid_count,
                          IsqMachine *out, size_t out_capacity, size_t *out_count,
                          char *error, size_t error_size)
{
    Buffer resp = {NULL, 0};
    char *body = NULL;
    char url[512];
    long status = 0;
    int rc = -1;

    if (base_url == NULL || uuids == NULL || uuid_count == 0 || out == NULL || out_count == NULL) {
        fail(error, error_size, "argumentos inválidos");
        return -1;
    }
    *out_count = 0;

    for (size_t i = 0; i < uuid_count; i++) {
        if (uuids[i] == NULL || !uuid_is_safe(uuids[i])) {
            fail(error, error_size, "uuid inválido: %s", uuids[i] ? uuids[i] : "(nulo)");
            return -1;
        }
    }
    if (uuid_count > out_capacity) {
        fail(error, error_size, "espaço insuficiente para %zu máquinas", uuid_count);
        return -1;
    }
    if (strlen(base_url) + strlen(LAST_PATH) >= sizeof url) {
        fail(error, error_size, "URL base muito longa");
        return -1;
    }
    snprintf(url, sizeof url, "%s%s", base_url, LAST_PATH);

    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        fail(error, error_size, "falha ao iniciar libcurl");
        return -1;
    }

    body = build_request_body(uuids, uuid_count);
    if (body == NULL) {
        fail(error, error_size, "memória insuficiente");
        return -1;
    }

    if (http_post_json(url, body, timeout_ms, &resp, &status, error, error_size) != 0) {
        goto cleanup;
    }
    if (status != 200) {
        fail(error, error_size, "InterSCity respondeu HTTP %ld", status);
        goto cleanup;
    }

    rc = parse_response(resp.data != NULL ? resp.data : "", out, out_capacity,
                        out_count, error, error_size);

cleanup:
    free(body);
    free(resp.data);
    return rc;
}
