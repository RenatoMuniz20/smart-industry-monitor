#define _POSIX_C_SOURCE 200809L

#include "http_server.h"
#include "machine_map.h"
#include "routes.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DEFAULT_PORT 8080L
#define DEFAULT_INTERSCITY_URL "https://cidadesinteligentes.lsdi.ufma.br/interscity_lh"
#define DEFAULT_TIMEOUT_MS 5000L
#define DEFAULT_MACHINES "../config/interscity/machines.conf"
#define MAX_MACHINES 64

static volatile sig_atomic_t stop_requested = 0;

static void on_signal(int signo)
{
    (void)signo;
    stop_requested = 1;
}

static int parse_number(const char *text, long min, long max, long *value)
{
    char *end = NULL;
    long v;

    if (text == NULL || *text == '\0') {
        return -1;
    }
    v = strtol(text, &end, 10);
    if (*end != '\0' || v < min || v > max) {
        return -1;
    }
    *value = v;
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

static void usage(const char *prog)
{
    printf("Uso: %s [--port N] [--machines ARQUIVO] [--interscity-url URL]"
           " [--interscity-timeout-ms N]\n", prog);
}

int main(int argc, char **argv)
{
    long port = DEFAULT_PORT;
    long timeout_ms = DEFAULT_TIMEOUT_MS;
    const char *machines_path = DEFAULT_MACHINES;
    const char *interscity_url = DEFAULT_INTERSCITY_URL;
    MachineEntry machines[MAX_MACHINES];
    size_t machine_count = 0;
    RouteContext ctx;
    HttpServer *server = NULL;
    char error[256];

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        }
        if (i + 1 >= argc) {
            fprintf(stderr, "faltou valor para %s\n", argv[i]);
            return 2;
        }
        if (strcmp(argv[i], "--port") == 0) {
            if (parse_number(argv[i + 1], 1, 65535, &port) != 0) {
                fprintf(stderr, "porta inválida: %s\n", argv[i + 1]);
                return 2;
            }
        } else if (strcmp(argv[i], "--interscity-timeout-ms") == 0) {
            if (parse_number(argv[i + 1], 100, 60000, &timeout_ms) != 0) {
                fprintf(stderr, "timeout inválido: %s\n", argv[i + 1]);
                return 2;
            }
        } else if (strcmp(argv[i], "--interscity-url") == 0) {
            interscity_url = argv[i + 1];
        } else if (strcmp(argv[i], "--machines") == 0) {
            machines_path = argv[i + 1];
        } else {
            fprintf(stderr, "argumento desconhecido: %s\n", argv[i]);
            usage(argv[0]);
            return 2;
        }
        i++;
    }

    if (machine_map_load(machines_path, machines, MAX_MACHINES, &machine_count,
                         error, sizeof error) != 0) {
        fprintf(stderr, "erro: %s\n", error);
        return 1;
    }

    ctx.interscity_url = interscity_url;
    ctx.interscity_timeout_ms = timeout_ms;
    ctx.machines = machines;
    ctx.machine_count = machine_count;

    if (install_signal_handlers() != 0) {
        fprintf(stderr, "erro: não foi possível instalar tratadores de sinal\n");
        return 1;
    }

    if (http_server_start(&server, (uint16_t)port, routes_handle, &ctx,
                          error, sizeof error) != 0) {
        fprintf(stderr, "erro: %s\n", error);
        return 1;
    }

    printf("backend escutando na porta %u com %zu máquinas\n",
           (unsigned int)port, machine_count);
    fflush(stdout);

    while (!stop_requested) {
        sleep(1);
    }

    puts("encerrando backend...");
    http_server_stop(server);
    return 0;
}
