#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

#include <stddef.h>
#include <stdint.h>

typedef struct HttpServer HttpServer;
typedef struct HttpRequest HttpRequest;

/* Requisição entregue às rotas. `arg` devolve o valor de um parâmetro da
 * query string (ex.: start em ?start=...) ou NULL se ausente. */
struct HttpRequest {
    const char *method;
    const char *url;
    void *conn;
    const char *(*arg)(const HttpRequest *req, const char *key);
};

/* Função de rota: preenche `body` (terminado em NUL) e `status`.
 * Retorna 0 se tratou a requisição; diferente de 0 vira HTTP 500. */
typedef int (*HttpRouteFn)(void *cls, const HttpRequest *req, char *body,
                           size_t body_size, unsigned int *status);

/* Sobe o servidor HTTP em `bind_addr` (IPv4, ex.: "127.0.0.1") na porta indicada.
 * `web_root` (opcional, NULL desliga) é a pasta do dashboard: GET e HEAD fora de
 * /api/ entregam arquivos dela (SIM-07). Rotas /api/ seguem para `route`. */
int http_server_start(HttpServer **out, const char *bind_addr, uint16_t port,
                      const char *web_root, HttpRouteFn route, void *cls,
                      char *error, size_t error_size);

/* Encerra o servidor e libera a memória. Aceita NULL. */
void http_server_stop(HttpServer *server);

#endif
