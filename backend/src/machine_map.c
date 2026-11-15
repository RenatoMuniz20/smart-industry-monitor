#include "machine_map.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_LINE 256

static void set_error(char *error, size_t error_size, const char *fmt, ...)
{
    va_list ap;

    if (error == NULL || error_size == 0) {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(error, error_size, fmt, ap);
    va_end(ap);
}

static int uuid_is_valid(const char *s)
{
    size_t n = strlen(s);

    if (n != MM_UUID_LEN) {
        return 0;
    }
    for (size_t i = 0; i < n; i++) {
        if (!isxdigit((unsigned char)s[i]) && s[i] != '-') {
            return 0;
        }
    }
    return 1;
}

int machine_map_load(const char *path, MachineEntry *out, size_t capacity, size_t *count,
                     char *error, size_t error_size)
{
    FILE *f;
    char line[MAX_LINE];
    size_t n = 0;
    int lineno = 0;
    int rc = 0;

    if (path == NULL || out == NULL || count == NULL) {
        set_error(error, error_size, "argumentos inválidos");
        return -1;
    }
    *count = 0;

    f = fopen(path, "r");
    if (f == NULL) {
        set_error(error, error_size, "não foi possível abrir o arquivo de máquinas: %s", path);
        return -1;
    }

    while (rc == 0 && fgets(line, sizeof line, f) != NULL) {
        char *p = line;
        char *end;
        char *uuid;
        long id;
        int dup = 0;

        lineno++;
        if (strchr(line, '\n') == NULL && !feof(f)) {
            set_error(error, error_size, "linha %d muito longa", lineno);
            rc = -1;
            break;
        }

        while (isspace((unsigned char)*p)) {
            p++;
        }
        if (*p == '\0' || *p == '#') {
            continue;
        }

        id = strtol(p, &end, 10);
        if (end == p || id < 1 || id > 1000000L) {
            set_error(error, error_size, "linha %d: id de máquina inválido", lineno);
            rc = -1;
            break;
        }

        p = end;
        while (isspace((unsigned char)*p)) {
            p++;
        }
        uuid = p;
        while (*p != '\0' && !isspace((unsigned char)*p)) {
            p++;
        }
        *p = '\0';

        if (!uuid_is_valid(uuid)) {
            set_error(error, error_size, "linha %d: uuid inválido", lineno);
            rc = -1;
            break;
        }
        if (n >= capacity) {
            set_error(error, error_size, "mais de %zu máquinas no arquivo", capacity);
            rc = -1;
            break;
        }
        for (size_t j = 0; j < n; j++) {
            if (out[j].machine_id == (int)id || strcmp(out[j].uuid, uuid) == 0) {
                dup = 1;
                break;
            }
        }
        if (dup) {
            set_error(error, error_size, "linha %d: id ou uuid duplicado", lineno);
            rc = -1;
            break;
        }

        out[n].machine_id = (int)id;
        snprintf(out[n].uuid, sizeof out[n].uuid, "%s", uuid);
        n++;
    }

    fclose(f);
    if (rc != 0) {
        return -1;
    }
    if (n == 0) {
        set_error(error, error_size, "arquivo de máquinas sem entradas");
        return -1;
    }
    *count = n;
    return 0;
}
