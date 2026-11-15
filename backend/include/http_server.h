#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

#include <stddef.h>
#include <stdint.h>

typedef struct HttpServer HttpServer;

/* Função de rota: recebe método e caminho, preenche `body` (terminado em NUL)
 * e `status`. Retorna 0 se tratou a requisição; diferente de 0 vira HTTP 500. */
typedef int (*HttpRouteFn)(void *cls, const char *method, const char *url,
                           char *body, size_t body_size, unsigned int *status);

/* Sobe o servidor HTTP na porta indicada (todas as interfaces). */
int http_server_start(HttpServer **out, uint16_t port, HttpRouteFn route, void *cls,
                      char *error, size_t error_size);

/* Encerra o servidor e libera a memória. Aceita NULL. */
void http_server_stop(HttpServer *server);

#endif
