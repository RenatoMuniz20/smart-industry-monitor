#ifndef MACHINE_MAP_H
#define MACHINE_MAP_H

#include <stddef.h>

#define MM_UUID_LEN 36

/* Uma linha de config/interscity/machines.conf: "<machine_id> <uuid>". */
typedef struct {
    int machine_id;
    char uuid[MM_UUID_LEN + 1];
} MachineEntry;

/* Lê o arquivo de mapeamento (ignora linhas vazias e comentários com '#').
 * Rejeita uuid inválido, id duplicado e arquivo sem máquinas.
 * Retorna 0 em sucesso; -1 em erro, com a mensagem em `error`. */
int machine_map_load(const char *path, MachineEntry *out, size_t capacity, size_t *count,
                     char *error, size_t error_size);

#endif
