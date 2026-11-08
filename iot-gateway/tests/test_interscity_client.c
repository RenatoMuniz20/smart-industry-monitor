#define _POSIX_C_SOURCE 200809L
#include "interscity_client.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The mock server (tests/test_interscity_client.py) answers by UUID suffix:
 * ...0201 -> 201, ...0400 -> 400, ...0500 -> 500, ...0999 -> sleeps 2 s.
 * It records every request; the Python script checks them after this program.
 */
#define UUID_201 "00000000-0000-4000-8000-000000000201"
#define UUID_400 "00000000-0000-4000-8000-000000000400"
#define UUID_500 "00000000-0000-4000-8000-000000000500"
#define UUID_SLOW "00000000-0000-4000-8000-000000000999"

static const char *require_env(const char *name)
{
    const char *value = getenv(name);
    if (value == NULL || *value == '\0') {
        fprintf(stderr, "Defina %s (execute por tests/test_interscity_client.py).\n", name);
        exit(2);
    }
    return value;
}

static void post(InterscityClient *client, const char *uuid, const char *body,
                 InterscityClientError expected, long expected_status, InterscityClientResult *result)
{
    InterscityClientError error = INTERSCITY_CLIENT_OK;
    int rc = interscity_client_post_data(client, uuid, body, strlen(body), result, &error);
    if (error != expected || result->http_status != expected_status) {
        fprintf(stderr, "uuid=%s: erro=%s status=%ld (esperado erro=%s status=%ld)\n", uuid,
                interscity_client_error_string(error), result->http_status,
                interscity_client_error_string(expected), expected_status);
        assert(0);
    }
    assert(rc == (expected == INTERSCITY_CLIENT_OK ? 0 : -1));
}

static void test_arguments(void)
{
    InterscityClient *client = NULL;
    InterscityClientError error;
    static const char *bad_urls[] = {
        "", "ftp://example.org", "http://", "https://", "http:///", "example.org",
        "http://exa mple.org", "http://example.org/\n", "HTTP://example.org"
    };
    for (size_t i = 0; i < sizeof bad_urls / sizeof bad_urls[0]; ++i) {
        assert(interscity_client_create(&client, bad_urls[i], 1000, &error) == -1);
        assert(error == INTERSCITY_CLIENT_ARGUMENT && client == NULL);
    }
    char long_url[600] = "http://";
    memset(long_url + 7, 'a', 500);
    long_url[507] = '\0';
    assert(interscity_client_create(&client, long_url, 1000, &error) == -1);
    assert(interscity_client_create(&client, NULL, 1000, &error) == -1);
    assert(interscity_client_create(&client, "http://127.0.0.1", 0, &error) == -1);
    assert(interscity_client_create(&client, "http://127.0.0.1", INTERSCITY_CLIENT_MAX_TIMEOUT_MS + 1,
                                    &error) == -1);
    assert(interscity_client_create(NULL, "http://127.0.0.1", 1000, &error) == -1);
    assert(error == INTERSCITY_CLIENT_ARGUMENT);

    assert(interscity_client_create(&client, "https://example.org/prefix//", 1000, &error) == 0);
    assert(error == INTERSCITY_CLIENT_OK && client != NULL);
    InterscityClient *other = client;
    assert(interscity_client_create(&other, "http://127.0.0.1", 1000, &error) == -1); /* *out != NULL */
    assert(error == INTERSCITY_CLIENT_ARGUMENT && other == client);

    InterscityClientResult result = {123, 456, 1};
    static const char *bad_uuids[] = {
        "", "abc", "00000000-0000-4000-8000-00000000020", "00000000-0000-4000-8000-0000000002011",
        "00000000-0000-4000-8000-00000000020A", "000000000000-4000-8000-0000000002011",
        "00000000-0000-4000-8000-00000000020/"
    };
    for (size_t i = 0; i < sizeof bad_uuids / sizeof bad_uuids[0]; ++i) {
        assert(interscity_client_post_data(client, bad_uuids[i], "{}", 2, &result, &error) == -1);
        assert(error == INTERSCITY_CLIENT_ARGUMENT);
        assert(result.http_status == 0 && result.elapsed_ms == 0 && result.new_connection == 0);
    }
    assert(interscity_client_post_data(client, NULL, "{}", 2, NULL, &error) == -1);
    assert(interscity_client_post_data(client, UUID_201, NULL, 2, NULL, &error) == -1);
    assert(interscity_client_post_data(client, UUID_201, "{}", 0, NULL, &error) == -1);
    assert(interscity_client_post_data(NULL, UUID_201, "{}", 2, NULL, &error) == -1);
    assert(error == INTERSCITY_CLIENT_ARGUMENT);

    interscity_client_destroy(&client);
    assert(client == NULL);
    interscity_client_destroy(&client); /* NULL-safe */
    interscity_client_destroy(NULL);
}

static void test_mock_server(const char *base_url)
{
    InterscityClient *client = NULL;
    InterscityClientResult result;
    char url[512];
    snprintf(url, sizeof url, "%s/", base_url); /* trailing '/' must be ignored */
    assert(interscity_client_create(&client, url, 1500, NULL) == 0);

    post(client, UUID_201, "{\"data\":{\"n\":[{\"value\":1}]}}", INTERSCITY_CLIENT_OK, 201, &result);
    assert(result.new_connection == 1);
    post(client, UUID_201, "{\"data\":{\"n\":[{\"value\":2}]}}", INTERSCITY_CLIENT_OK, 201, &result);
    assert(result.new_connection == 0); /* the connection was reused */
    post(client, UUID_400, "{\"bad\":true}", INTERSCITY_CLIENT_HTTP_STATUS, 400, &result);
    post(client, UUID_500, "{\"data\":{}}", INTERSCITY_CLIENT_HTTP_STATUS, 500, &result);
    post(client, UUID_201, "{\"data\":{\"n\":[{\"value\":3}]}}", INTERSCITY_CLIENT_OK, 201, &result);
    interscity_client_destroy(&client);

    /* Timeout: the server sleeps 2 s; the client gives up after 300 ms. */
    assert(interscity_client_create(&client, base_url, 300, NULL) == 0);
    post(client, UUID_SLOW, "{\"data\":{\"slow\":[{\"value\":1}]}}", INTERSCITY_CLIENT_TIMEOUT, 0, &result);
    assert(result.elapsed_ms >= 250 && result.elapsed_ms < 1500);
    /* The client remains usable after a timeout. */
    post(client, UUID_201, "{\"data\":{\"n\":[{\"value\":4}]}}", INTERSCITY_CLIENT_OK, 201, &result);
    interscity_client_destroy(&client);
}

static void test_closed_port(const char *closed_url)
{
    InterscityClient *client = NULL;
    InterscityClientResult result;
    assert(interscity_client_create(&client, closed_url, 1000, NULL) == 0);
    post(client, UUID_201, "{}", INTERSCITY_CLIENT_NETWORK, 0, &result);
    interscity_client_destroy(&client);
}

static void test_error_strings(void)
{
    for (int e = INTERSCITY_CLIENT_OK; e <= INTERSCITY_CLIENT_HTTP_STATUS; ++e) {
        const char *text = interscity_client_error_string((InterscityClientError)e);
        assert(text != NULL && *text != '\0' && strcmp(text, "Erro desconhecido") != 0);
    }
    assert(strcmp(interscity_client_error_string((InterscityClientError)999), "Erro desconhecido") == 0);
}

int main(void)
{
    const char *base_url = require_env("SIM_INTERSCITY_MOCK_URL");
    const char *closed_url = require_env("SIM_INTERSCITY_CLOSED_URL");
    test_arguments();
    test_mock_server(base_url);
    test_closed_port(closed_url);
    test_error_strings();
    puts("test_interscity_client: todos os testes C passaram.");
    return 0;
}
