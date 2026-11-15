#define _POSIX_C_SOURCE 200809L
#include "interscity_client.h"

#include <curl/curl.h>
#include <json-c/json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define URL_CAPACITY 512
#define DATA_PATH_LENGTH 61   /* "/adaptor/resources/" + 36-byte UUID + "/data" + NUL */
#define REQUEST_CAPACITY 96   /* {"uuids":["<36 bytes>"]} + NUL, with room to spare */
#define RESPONSE_LIMIT 65536  /* consultation responses are small; larger ones are refused */

struct InterscityClient {
    CURL *curl;
    struct curl_slist *headers;
    char base_url[URL_CAPACITY];
};

struct Collected {
    char *data;
    size_t length;
};

static int finish(InterscityClientError *error, InterscityClientError value)
{
    if (error != NULL) *error = value;
    return value == INTERSCITY_CLIENT_OK ? 0 : -1;
}

/* The response body of a POST to the Adaptor is not needed: only the status is used. */
static size_t discard_body(char *data, size_t size, size_t count, void *context)
{
    (void)data;
    (void)context;
    return size * count;
}

/* Keeps a consultation response; returning less than received aborts the transfer. */
static size_t collect_body(char *data, size_t size, size_t count, void *context)
{
    struct Collected *collected = context;
    size_t n = size * count;
    if (n > RESPONSE_LIMIT - collected->length) return 0;
    char *grown = realloc(collected->data, collected->length + n + 1);
    if (grown == NULL) return 0;
    collected->data = grown;
    memcpy(collected->data + collected->length, data, n);
    collected->length += n;
    collected->data[collected->length] = '\0';
    return n;
}

static int is_canonical_uuid(const char *text)
{
    if (strnlen(text, 37) != 36) return 0;
    for (size_t i = 0; i < 36; ++i) {
        char c = text[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (c != '-') return 0;
        } else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return 0;
        }
    }
    return 1;
}

static size_t valid_base_length(const char *url)
{
    size_t scheme;
    if (strncmp(url, "http://", 7) == 0) scheme = 7;
    else if (strncmp(url, "https://", 8) == 0) scheme = 8;
    else return 0;
    size_t length = strnlen(url, URL_CAPACITY);
    if (length == URL_CAPACITY) return 0;
    for (size_t i = 0; i < length; ++i) {
        unsigned char c = (unsigned char)url[i];
        if (c <= 0x20 || c == 0x7f) return 0;
    }
    while (length > scheme && url[length - 1] == '/') --length;
    if (length == scheme || length + DATA_PATH_LENGTH >= URL_CAPACITY) return 0;
    return length;
}

static void release(InterscityClient *client)
{
    if (client->curl != NULL) curl_easy_cleanup(client->curl);
    curl_slist_free_all(client->headers);
    free(client);
    curl_global_cleanup();
}

int interscity_client_create(InterscityClient **out, const char *base_url, long timeout_ms,
                             InterscityClientError *error)
{
    if (out == NULL || *out != NULL || base_url == NULL || timeout_ms < 1 ||
        timeout_ms > INTERSCITY_CLIENT_MAX_TIMEOUT_MS) {
        return finish(error, INTERSCITY_CLIENT_ARGUMENT);
    }
    size_t length = valid_base_length(base_url);
    if (length == 0) return finish(error, INTERSCITY_CLIENT_ARGUMENT);

    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        return finish(error, INTERSCITY_CLIENT_LIBRARY);
    }
    InterscityClient *client = calloc(1, sizeof *client);
    if (client == NULL) {
        curl_global_cleanup();
        return finish(error, INTERSCITY_CLIENT_MEMORY);
    }
    memcpy(client->base_url, base_url, length);
    client->base_url[length] = '\0';

    client->curl = curl_easy_init();
    if (client->curl == NULL) {
        release(client);
        return finish(error, INTERSCITY_CLIENT_LIBRARY);
    }
    /* "Expect:" disables the 100-continue round trip for request bodies. */
    static const char *header_lines[] = {"Content-Type: application/json", "Expect:"};
    for (size_t i = 0; i < sizeof header_lines / sizeof header_lines[0]; ++i) {
        struct curl_slist *extended = curl_slist_append(client->headers, header_lines[i]);
        if (extended == NULL) {
            release(client);
            return finish(error, INTERSCITY_CLIENT_MEMORY);
        }
        client->headers = extended;
    }
    CURL *curl = client->curl;
    if (curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, discard_body) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, client->headers) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_POST, 1L) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, timeout_ms) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "smart-industry-gateway") != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https") != CURLE_OK) {
        release(client);
        return finish(error, INTERSCITY_CLIENT_LIBRARY);
    }
    *out = client;
    return finish(error, INTERSCITY_CLIENT_OK);
}

/* One POST with the given sink for the response body. Fills observed with what
 * curl reported, even on failure. Does not interpret the HTTP status. */
static InterscityClientError perform_post(InterscityClient *client, const char *url,
                                          const char *body, size_t body_length,
                                          curl_write_callback sink, void *sink_context,
                                          InterscityClientResult *observed)
{
    CURL *curl = client->curl;
    if (curl_easy_setopt(curl, CURLOPT_URL, url) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, sink) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, sink_context) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)body_length) != CURLE_OK) {
        return INTERSCITY_CLIENT_LIBRARY;
    }
    CURLcode code = curl_easy_perform(curl);

    long status = 0;
    long connects = 0;
    curl_off_t total_us = 0;
    if (curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status) != CURLE_OK) status = 0;
    if (curl_easy_getinfo(curl, CURLINFO_NUM_CONNECTS, &connects) != CURLE_OK) connects = 0;
    if (curl_easy_getinfo(curl, CURLINFO_TOTAL_TIME_T, &total_us) != CURLE_OK) total_us = 0;
    /* Do not keep a pointer to the caller's buffer after returning. */
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, NULL);

    observed->http_status = status;
    observed->elapsed_ms = (long)(total_us / 1000);
    observed->new_connection = connects > 0;

    if (code == CURLE_OPERATION_TIMEDOUT) return INTERSCITY_CLIENT_TIMEOUT;
    if (code != CURLE_OK) return INTERSCITY_CLIENT_NETWORK;
    return INTERSCITY_CLIENT_OK;
}

int interscity_client_post_data(InterscityClient *client, const char *uuid,
                                const char *body, size_t body_length,
                                InterscityClientResult *result, InterscityClientError *error)
{
    InterscityClientResult observed = {0, 0, 0};
    if (result != NULL) *result = observed;
    if (client == NULL || uuid == NULL || !is_canonical_uuid(uuid) || body == NULL ||
        body_length == 0) {
        return finish(error, INTERSCITY_CLIENT_ARGUMENT);
    }
    char url[URL_CAPACITY];
    int written = snprintf(url, sizeof url, "%s/adaptor/resources/%s/data", client->base_url, uuid);
    if (written < 0 || (size_t)written >= sizeof url) return finish(error, INTERSCITY_CLIENT_ARGUMENT);

    InterscityClientError code = perform_post(client, url, body, body_length,
                                              discard_body, NULL, &observed);
    if (result != NULL) *result = observed;
    if (code != INTERSCITY_CLIENT_OK) return finish(error, code);
    if (observed.http_status != 201) return finish(error, INTERSCITY_CLIENT_HTTP_STATUS);
    return finish(error, INTERSCITY_CLIENT_OK);
}

int interscity_client_is_ambiguous(InterscityClientError error)
{
    return error == INTERSCITY_CLIENT_TIMEOUT || error == INTERSCITY_CLIENT_NETWORK;
}

/* Reads resources[0].capabilities.<each capability>[0].date and keeps the most
 * recent one as its first 19 characters. "found" stays 0 when no series has data. */
static InterscityClientError parse_last(const char *text, char *out, int *found)
{
    static const char *capabilities[] = {"sim_operating_state", "sim_motor_temperature",
                                         "sim_current", "sim_failure_within_24h"};
    char best[INTERSCITY_CLIENT_TIMESTAMP_CAPACITY] = "";
    int any = 0;
    json_object *resources = NULL;
    json_object *resource = NULL;
    json_object *caps = NULL;

    if (text == NULL) return INTERSCITY_CLIENT_RESPONSE;
    json_object *root = json_tokener_parse(text);
    if (root == NULL) return INTERSCITY_CLIENT_RESPONSE;
    if (!json_object_object_get_ex(root, "resources", &resources) || resources == NULL ||
        !json_object_is_type(resources, json_type_array)) {
        json_object_put(root);
        return INTERSCITY_CLIENT_RESPONSE;
    }
    if (json_object_array_length(resources) == 0) {
        json_object_put(root);
        return INTERSCITY_CLIENT_OK; /* nothing stored */
    }
    resource = json_object_array_get_idx(resources, 0);
    if (resource == NULL || !json_object_object_get_ex(resource, "capabilities", &caps) ||
        caps == NULL || !json_object_is_type(caps, json_type_object)) {
        json_object_put(root);
        return INTERSCITY_CLIENT_RESPONSE;
    }
    for (size_t i = 0; i < sizeof capabilities / sizeof capabilities[0]; ++i) {
        json_object *series = NULL;
        json_object *sample = NULL;
        json_object *date = NULL;
        if (!json_object_object_get_ex(caps, capabilities[i], &series) || series == NULL ||
            !json_object_is_type(series, json_type_array) ||
            json_object_array_length(series) == 0) {
            continue;
        }
        sample = json_object_array_get_idx(series, 0);
        if (sample == NULL || !json_object_object_get_ex(sample, "date", &date) || date == NULL) continue;
        const char *text_date = json_object_get_string(date);
        if (text_date == NULL || strlen(text_date) < 19) continue;
        if (!any || strncmp(text_date, best, 19) > 0) {
            memcpy(best, text_date, 19);
            best[19] = '\0';
            any = 1;
        }
    }
    json_object_put(root);
    if (any) {
        memcpy(out, best, INTERSCITY_CLIENT_TIMESTAMP_CAPACITY);
        *found = 1;
    }
    return INTERSCITY_CLIENT_OK;
}

int interscity_client_last_timestamp(InterscityClient *client, const char *uuid,
                                     char *out, size_t out_size, int *found,
                                     InterscityClientError *error)
{
    if (found != NULL) *found = 0;
    if (client == NULL || uuid == NULL || !is_canonical_uuid(uuid) || out == NULL ||
        out_size < INTERSCITY_CLIENT_TIMESTAMP_CAPACITY || found == NULL) {
        return finish(error, INTERSCITY_CLIENT_ARGUMENT);
    }
    char url[URL_CAPACITY];
    char request[REQUEST_CAPACITY];
    int url_written = snprintf(url, sizeof url, "%s/collector/resources/data/last", client->base_url);
    int body_written = snprintf(request, sizeof request, "{\"uuids\":[\"%s\"]}", uuid);
    if (url_written < 0 || (size_t)url_written >= sizeof url ||
        body_written < 0 || (size_t)body_written >= sizeof request) {
        return finish(error, INTERSCITY_CLIENT_ARGUMENT);
    }

    struct Collected collected = {NULL, 0};
    InterscityClientResult observed = {0, 0, 0};
    InterscityClientError outcome = perform_post(client, url, request, (size_t)body_written,
                                                 collect_body, &collected, &observed);
    if (outcome == INTERSCITY_CLIENT_OK) {
        if (observed.http_status == 200) {
            outcome = parse_last(collected.data, out, found);
        } else if (observed.http_status == 404 && collected.data != NULL &&
                   strstr(collected.data, "Resource not found") != NULL) {
            outcome = INTERSCITY_CLIENT_OK; /* machine without data: found stays 0 */
        } else {
            outcome = INTERSCITY_CLIENT_HTTP_STATUS;
        }
    }
    free(collected.data);
    return finish(error, outcome);
}

void interscity_client_destroy(InterscityClient **client)
{
    if (client == NULL || *client == NULL) return;
    release(*client);
    *client = NULL;
}

const char *interscity_client_error_string(InterscityClientError error)
{
    switch (error) {
    case INTERSCITY_CLIENT_OK: return "Sucesso";
    case INTERSCITY_CLIENT_ARGUMENT: return "Argumento inválido";
    case INTERSCITY_CLIENT_MEMORY: return "Memória indisponível";
    case INTERSCITY_CLIENT_LIBRARY: return "Erro ao inicializar/configurar libcurl";
    case INTERSCITY_CLIENT_TIMEOUT: return "Tempo esgotado aguardando o InterSCity";
    case INTERSCITY_CLIENT_NETWORK: return "Falha de rede (DNS, TCP ou TLS)";
    case INTERSCITY_CLIENT_HTTP_STATUS: return "Resposta HTTP diferente de 201";
    case INTERSCITY_CLIENT_RESPONSE: return "Resposta do Data Collector fora do formato esperado";
    default: return "Erro desconhecido";
    }
}
