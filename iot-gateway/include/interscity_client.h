#ifndef INTERSCITY_CLIENT_H
#define INTERSCITY_CLIENT_H

#include <stddef.h>

#define INTERSCITY_CLIENT_MAX_TIMEOUT_MS 60000L

typedef struct InterscityClient InterscityClient;

typedef enum {
    INTERSCITY_CLIENT_OK = 0,
    INTERSCITY_CLIENT_ARGUMENT,
    INTERSCITY_CLIENT_MEMORY,
    INTERSCITY_CLIENT_LIBRARY,
    INTERSCITY_CLIENT_TIMEOUT,
    INTERSCITY_CLIENT_NETWORK,
    INTERSCITY_CLIENT_HTTP_STATUS
} InterscityClientError;

typedef struct {
    long http_status;   /* 0 when no HTTP response was received */
    long elapsed_ms;    /* total duration of the request */
    int new_connection; /* 1 if a TCP/TLS connection was opened, 0 if one was reused */
} InterscityClientResult;

/* base_url: "http://host[:port][/prefix]" or "https://...", e.g.
 * https://cidadesinteligentes.lsdi.ufma.br/interscity_lh; trailing '/' are
 * ignored. Spaces and control characters are rejected. timeout_ms:
 * 1..INTERSCITY_CLIENT_MAX_TIMEOUT_MS, the limit for each whole request,
 * connection included. TLS certificates are always verified.
 * *out must be NULL and stays NULL on failure. Calls curl_global_init (the
 * matching cleanup happens in destroy); create clients before starting threads.
 * One client keeps one reusable connection: use it from a single thread.
 */
int interscity_client_create(InterscityClient **out, const char *base_url, long timeout_ms,
                             InterscityClientError *error);

/* POST {base_url}/adaptor/resources/{uuid}/data with body (JSON, need not be
 * NUL-terminated). uuid must be canonical lowercase. Success means HTTP 201
 * only: any other status is INTERSCITY_CLIENT_HTTP_STATUS; no complete response
 * within timeout_ms is INTERSCITY_CLIENT_TIMEOUT; DNS/TCP/TLS failures are
 * INTERSCITY_CLIENT_NETWORK. Never retries, because the POST is not idempotent:
 * after a timeout the data may or may not have been accepted. A 201 means the
 * Adaptor published the data, not that the Data Collector stored it.
 * The response body is discarded. result (optional) is filled with whatever
 * was observed, also on failures. Blocks for at most about timeout_ms.
 */
int interscity_client_post_data(InterscityClient *client, const char *uuid,
                                const char *body, size_t body_length,
                                InterscityClientResult *result, InterscityClientError *error);

/* NULL-safe; releases the connection and clears *client. */
void interscity_client_destroy(InterscityClient **client);

const char *interscity_client_error_string(InterscityClientError error);

#endif
