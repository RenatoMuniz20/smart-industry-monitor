#include "http_server.h"

#include <arpa/inet.h>
#include <microhttpd.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

/* 1 MiB: o histórico de um período pode ser grande. */
#define BODY_SIZE (1024 * 1024)

struct HttpServer {
    struct MHD_Daemon *daemon;
    HttpRouteFn route;
    void *cls;
};

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

static const char *request_arg(const HttpRequest *req, const char *key)
{
    return MHD_lookup_connection_value((struct MHD_Connection *)req->conn,
                                       MHD_GET_ARGUMENT_KIND, key);
}

static enum MHD_Result handle_request(void *cls, struct MHD_Connection *connection,
                                      const char *url, const char *method,
                                      const char *version, const char *upload_data,
                                      size_t *upload_data_size, void **con_cls)
{
    HttpServer *server = cls;
    HttpRequest req;
    unsigned int status = 500;
    char *body;
    enum MHD_Result ret;

    (void)version;
    (void)upload_data;
    (void)upload_data_size;
    (void)con_cls;

    body = malloc(BODY_SIZE);
    if (body == NULL) {
        return send_json(connection, 500, "{\"error\":\"memória insuficiente\"}");
    }
    body[0] = '\0';

    req.method = method;
    req.url = url;
    req.conn = connection;
    req.arg = request_arg;

    if (server->route(server->cls, &req, body, BODY_SIZE, &status) != 0) {
        status = 500;
        snprintf(body, BODY_SIZE, "%s", "{\"error\":\"erro interno\"}");
    }
    body[BODY_SIZE - 1] = '\0';

    ret = send_json(connection, status, body);
    free(body);
    return ret;
}

int http_server_start(HttpServer **out, const char *bind_addr, uint16_t port,
                      HttpRouteFn route, void *cls, char *error, size_t error_size)
{
    HttpServer *server;
    struct MHD_Daemon *daemon;
    struct sockaddr_in addr;
    char message[256];

    if (out == NULL || route == NULL || bind_addr == NULL) {
        set_error(error, error_size, "argumentos inválidos");
        return -1;
    }
    *out = NULL;

    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, bind_addr, &addr.sin_addr) != 1) {
        snprintf(message, sizeof message, "endereço inválido: %s", bind_addr);
        set_error(error, error_size, message);
        return -1;
    }

    server = calloc(1, sizeof *server);
    if (server == NULL) {
        set_error(error, error_size, "memória insuficiente");
        return -1;
    }
    server->route = route;
    server->cls = cls;

    daemon = MHD_start_daemon(MHD_USE_INTERNAL_POLLING_THREAD, port, NULL, NULL,
                              &handle_request, server,
                              MHD_OPTION_SOCK_ADDR, (struct sockaddr *)&addr,
                              MHD_OPTION_END);
    if (daemon == NULL) {
        free(server);
        snprintf(message, sizeof message, "não foi possível abrir %s:%u",
                 bind_addr, (unsigned int)port);
        set_error(error, error_size, message);
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
