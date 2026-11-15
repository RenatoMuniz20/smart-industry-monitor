#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

#include <stddef.h>
#include <stdint.h>

typedef struct HttpServer HttpServer;

/* Sobe o servidor HTTP na porta indicada (todas as interfaces).
 * Retorna 0 em sucesso; -1 em erro, com a mensagem em `error`. */
int http_server_start(HttpServer **out, uint16_t port, char *error, size_t error_size);

/* Encerra o servidor e libera a memória. Aceita NULL. */
void http_server_stop(HttpServer *server);

#endif
