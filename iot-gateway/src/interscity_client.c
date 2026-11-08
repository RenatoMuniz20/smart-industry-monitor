#define _POSIX_C_SOURCE 200809L
#include "interscity_client.h"

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define URL_CAPACITY 512
#define DATA_PATH_LENGTH 61 /* "/adaptor/resources/" + 36-byte UUID + "/data" */

struct InterscityClient {
    CURL *curl;
    struct curl_slist *headers;
    char base_url[URL_CAPACITY];
};

static int finish(InterscityClientError *error, InterscityClientError value)
{
    if (error != NULL) *error = value;
    return value == INTERSCITY_CLIENT_OK ? 0 : -1;
}

/* The response body is not needed: only the status code is used. */
static size_t discard_body(char *data, size_t size, size_t count, void *context)
{
    (void)data;
    (void)context;
    return size * count;
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

int interscity_client_post_data(InterscityClient *client, const char *uuid,
                                const char *body, size_t body_length,
                                InterscityClientResult *result, InterscityClientError *error)
{
    if (result != NULL) {
        result->http_status = 0;
        result->elapsed_ms = 0;
        result->new_connection = 0;
    }
    if (client == NULL || uuid == NULL || !is_canonical_uuid(uuid) || body == NULL ||
        body_length == 0) {
        return finish(error, INTERSCITY_CLIENT_ARGUMENT);
    }
    char url[URL_CAPACITY];
    int written = snprintf(url, sizeof url, "%s/adaptor/resources/%s/data", client->base_url, uuid);
    if (written < 0 || (size_t)written >= sizeof url) return finish(error, INTERSCITY_CLIENT_ARGUMENT);

    CURL *curl = client->curl;
    if (curl_easy_setopt(curl, CURLOPT_URL, url) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)body_length) != CURLE_OK) {
        return finish(error, INTERSCITY_CLIENT_LIBRARY);
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
    if (result != NULL) {
        result->http_status = status;
        result->elapsed_ms = (long)(total_us / 1000);
        result->new_connection = connects > 0;
    }

    if (code == CURLE_OPERATION_TIMEDOUT) return finish(error, INTERSCITY_CLIENT_TIMEOUT);
    if (code != CURLE_OK) return finish(error, INTERSCITY_CLIENT_NETWORK);
    if (status != 201) return finish(error, INTERSCITY_CLIENT_HTTP_STATUS);
    return finish(error, INTERSCITY_CLIENT_OK);
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
    default: return "Erro desconhecido";
    }
}
