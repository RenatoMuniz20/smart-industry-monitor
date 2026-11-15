#define _POSIX_C_SOURCE 200809L

#include "http_server.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DEFAULT_PORT 8080

static volatile sig_atomic_t stop_requested = 0;

static void on_signal(int signo)
{
    (void)signo;
    stop_requested = 1;
}

static int parse_port(const char *text, uint16_t *port)
{
    char *end = NULL;
    unsigned long value;

    if (text == NULL || *text == '\0') {
        return -1;
    }
    value = strtoul(text, &end, 10);
    if (*end != '\0' || value == 0 || value > 65535UL) {
        return -1;
    }
    *port = (uint16_t)value;
    return 0;
}

static int install_signal_handlers(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGINT, &sa, NULL) != 0 || sigaction(SIGTERM, &sa, NULL) != 0) {
        return -1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    uint16_t port = DEFAULT_PORT;
    HttpServer *server = NULL;
    char error[256];

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            i++;
            if (parse_port(argv[i], &port) != 0) {
                fprintf(stderr, "porta inválida: %s\n", argv[i]);
                return 2;
            }
        } else if (strcmp(argv[i], "--help") == 0) {
            printf("Uso: %s [--port N]   (padrão: %d)\n", argv[0], DEFAULT_PORT);
            return 0;
        } else {
            fprintf(stderr, "argumento desconhecido: %s\nUso: %s [--port N]\n",
                    argv[i], argv[0]);
            return 2;
        }
    }

    if (install_signal_handlers() != 0) {
        fprintf(stderr, "erro: não foi possível instalar tratadores de sinal\n");
        return 1;
    }

    if (http_server_start(&server, port, error, sizeof error) != 0) {
        fprintf(stderr, "erro: %s\n", error);
        return 1;
    }

    printf("backend escutando na porta %u (GET %s)\n",
           (unsigned int)port, "/api/health");
    fflush(stdout);

    while (!stop_requested) {
        sleep(1);
    }

    puts("encerrando backend...");
    http_server_stop(server);
    return 0;
}
