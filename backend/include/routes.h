#ifndef ROUTES_H
#define ROUTES_H

#include <stddef.h>

#include "machine_map.h"

typedef struct {
    const char *interscity_url;
    long interscity_timeout_ms;
    const MachineEntry *machines;
    size_t machine_count;
} RouteContext;

/* Compatível com HttpRouteFn: `cls` é um RouteContext *. */
int routes_handle(void *cls, const char *method, const char *url,
                  char *body, size_t body_size, unsigned int *status);

#endif
