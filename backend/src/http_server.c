#include "http_server.h"

#include <microhttpd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct HttpServer {
    struct MHD_Daemon *daemon;
};

#define HEALTH_PATH "/api/health"

static void set_error(char *error, size_t error_size, const char *message)
{
    if (error != NULL && error_size > 0) {
        snprintf(error, error_size, "%s", message);
    }
}

static enum MHD_Result send_json(struct MHD_Connection *connection,
                                 unsigned int status, const char *body)
{
    struct MHD_Response *response;
    enum MHD_Result ret;

    response = MHD_create_response_from_buffer(strlen(body), (void *)body,
                                               MHD_RESPMEM_MUST_COPY);
    if (response == NULL) {
        return MHD_NO;
    }
    (void)MHD_add_response_header(response, "Content-Type",
                                  "application/json; charset=utf-8");
    ret = MHD_queue_response(connection, status, response);
    MHD_destroy_response(response);
    return ret;
}

static enum MHD_Result handle_request(void *cls, struct MHD_Connection *connection,
                                      const char *url, const char *method,
                                      const char *version, const char *upload_data,
                                      size_t *upload_data_size, void **con_cls)
{
    (void)cls;
    (void)version;
    (void)upload_data;
    (void)upload_data_size;
    (void)con_cls;

    if (strcmp(url, HEALTH_PATH) == 0) {
        if (strcmp(method, "GET") != 0) {
            return send_json(connection, MHD_HTTP_METHOD_NOT_ALLOWED,
                             "{\"error\":\"Method not allowed\"}");
        }
        return send_json(connection, MHD_HTTP_OK, "{\"status\":\"ok\"}");
    }

    return send_json(connection, MHD_HTTP_NOT_FOUND, "{\"error\":\"Not found\"}");
}

int http_server_start(HttpServer **out, uint16_t port, char *error, size_t error_size)
{
    HttpServer *server;
    struct MHD_Daemon *daemon;

    if (out == NULL) {
        set_error(error, error_size, "ponteiro de saída nulo");
        return -1;
    }
    *out = NULL;

    server = calloc(1, sizeof *server);
    if (server == NULL) {
        set_error(error, error_size, "memória insuficiente");
        return -1;
    }

    daemon = MHD_start_daemon(MHD_USE_INTERNAL_POLLING_THREAD, port, NULL, NULL,
                              &handle_request, NULL, MHD_OPTION_END);
    if (daemon == NULL) {
        free(server);
        {
            char message[128];
            snprintf(message, sizeof message, "não foi possível abrir a porta %u",
                     (unsigned int)port);
            set_error(error, error_size, message);
        }
        return -1;
    }

    server->daemon = daemon;
    *out = server;
    return 0;
}

void http_server_stop(HttpServer *server)
{
    if (server == NULL) {
        return;
    }
    if (server->daemon != NULL) {
        MHD_stop_daemon(server->daemon);
    }
    free(server);
}
