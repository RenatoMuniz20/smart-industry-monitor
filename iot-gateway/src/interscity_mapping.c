#define _POSIX_C_SOURCE 200809L
#include "interscity_mapping.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

/* Up to 254 visible bytes, plus newline and NUL. */
#define LINE_CAPACITY 256

static int finish(InterscityMappingError *error, size_t *error_line,
                  InterscityMappingError value, size_t line)
{
    if (error != NULL) *error = value;
    if (error_line != NULL) *error_line = line;
    return value == INTERSCITY_MAPPING_OK ? 0 : -1;
}

static int is_blank(char c)
{
    return c == ' ' || c == '\t';
}

static int parse_machine_id(const char *text, size_t length, int *out)
{
    long long value = 0;
    if (length == 0) return -1;
    for (size_t i = 0; i < length; ++i) {
        if (text[i] < '0' || text[i] > '9') return -1;
        value = value * 10 + (text[i] - '0');
        if (value > INT_MAX) return -1;
    }
    if (value == 0) return -1;
    *out = (int)value;
    return 0;
}

static int is_canonical_uuid(const char *text, size_t length)
{
    if (length != INTERSCITY_UUID_LENGTH) return 0;
    for (size_t i = 0; i < length; ++i) {
        char c = text[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (c != '-') return 0;
        } else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return 0;
        }
    }
    return 1;
}

int interscity_mapping_load(const char *path, InterscityMapping *out,
                            InterscityMappingError *error, size_t *error_line)
{
    if (path == NULL || out == NULL) {
        return finish(error, error_line, INTERSCITY_MAPPING_ARGUMENT, 0);
    }
    FILE *file = fopen(path, "r");
    if (file == NULL) return finish(error, error_line, INTERSCITY_MAPPING_OPEN, 0);

    InterscityMapping parsed;
    memset(&parsed, 0, sizeof parsed);
    InterscityMappingError failure = INTERSCITY_MAPPING_OK;
    size_t failure_line = 0;
    size_t number = 0;
    char line[LINE_CAPACITY];

    while (failure == INTERSCITY_MAPPING_OK && fgets(line, sizeof line, file) != NULL) {
        ++number;
        size_t length = strlen(line);
        int has_newline = length > 0 && line[length - 1] == '\n';
        if (!has_newline && !feof(file)) {
            failure = INTERSCITY_MAPPING_LINE_TOO_LONG;
            break;
        }
        if (has_newline) line[--length] = '\0';
        if (memchr(line, '\r', length) != NULL) {
            failure = INTERSCITY_MAPPING_CR;
            break;
        }

        size_t i = 0;
        while (i < length && is_blank(line[i])) ++i;
        if (i == length || line[i] == '#') continue;

        size_t id_start = i;
        while (i < length && !is_blank(line[i])) ++i;
        size_t id_length = i - id_start;
        while (i < length && is_blank(line[i])) ++i;
        size_t uuid_start = i;
        while (i < length && !is_blank(line[i])) ++i;
        size_t uuid_length = i - uuid_start;
        while (i < length && is_blank(line[i])) ++i;
        if (uuid_length == 0 || i != length) {
            failure = INTERSCITY_MAPPING_SYNTAX;
            break;
        }

        int machine_id;
        if (parse_machine_id(line + id_start, id_length, &machine_id) != 0) {
            failure = INTERSCITY_MAPPING_MACHINE_ID;
            break;
        }
        if (!is_canonical_uuid(line + uuid_start, uuid_length)) {
            failure = INTERSCITY_MAPPING_UUID;
            break;
        }
        for (size_t k = 0; k < parsed.count; ++k) {
            if (parsed.entries[k].machine_id == machine_id) {
                failure = INTERSCITY_MAPPING_DUPLICATE_ID;
                break;
            }
            if (memcmp(parsed.entries[k].uuid, line + uuid_start, INTERSCITY_UUID_LENGTH) == 0) {
                failure = INTERSCITY_MAPPING_DUPLICATE_UUID;
                break;
            }
        }
        if (failure != INTERSCITY_MAPPING_OK) break;
        if (parsed.count == INTERSCITY_MAPPING_CAPACITY) {
            failure = INTERSCITY_MAPPING_TOO_MANY;
            break;
        }
        InterscityMappingEntry *entry = &parsed.entries[parsed.count++];
        entry->machine_id = machine_id;
        memcpy(entry->uuid, line + uuid_start, INTERSCITY_UUID_LENGTH);
        entry->uuid[INTERSCITY_UUID_LENGTH] = '\0';
    }
    if (failure != INTERSCITY_MAPPING_OK) failure_line = number;

    if (failure == INTERSCITY_MAPPING_OK && ferror(file)) failure = INTERSCITY_MAPPING_READ;
    if (fclose(file) != 0 && failure == INTERSCITY_MAPPING_OK) failure = INTERSCITY_MAPPING_READ;
    if (failure == INTERSCITY_MAPPING_OK && parsed.count == 0) failure = INTERSCITY_MAPPING_EMPTY;
    if (failure != INTERSCITY_MAPPING_OK) return finish(error, error_line, failure, failure_line);

    *out = parsed;
    return finish(error, error_line, INTERSCITY_MAPPING_OK, 0);
}

const char *interscity_mapping_lookup(const InterscityMapping *mapping, int machine_id)
{
    if (mapping == NULL) return NULL;
    for (size_t i = 0; i < mapping->count && i < INTERSCITY_MAPPING_CAPACITY; ++i) {
        if (mapping->entries[i].machine_id == machine_id) return mapping->entries[i].uuid;
    }
    return NULL;
}

const char *interscity_mapping_error_string(InterscityMappingError error)
{
    switch (error) {
    case INTERSCITY_MAPPING_OK: return "Sucesso";
    case INTERSCITY_MAPPING_ARGUMENT: return "Argumento inválido";
    case INTERSCITY_MAPPING_OPEN: return "Não foi possível abrir o arquivo de mapeamento";
    case INTERSCITY_MAPPING_READ: return "Erro de leitura do arquivo de mapeamento";
    case INTERSCITY_MAPPING_CR: return "Quebra de linha CRLF (caractere CR) no mapeamento";
    case INTERSCITY_MAPPING_LINE_TOO_LONG: return "Linha longa demais no mapeamento";
    case INTERSCITY_MAPPING_SYNTAX: return "Linha fora do formato <machine_id> <uuid>";
    case INTERSCITY_MAPPING_MACHINE_ID: return "machine_id inválido no mapeamento";
    case INTERSCITY_MAPPING_UUID: return "UUID fora do formato canônico minúsculo";
    case INTERSCITY_MAPPING_DUPLICATE_ID: return "machine_id repetido no mapeamento";
    case INTERSCITY_MAPPING_DUPLICATE_UUID: return "UUID repetido no mapeamento";
    case INTERSCITY_MAPPING_TOO_MANY: return "Máquinas demais no mapeamento";
    case INTERSCITY_MAPPING_EMPTY: return "Mapeamento sem nenhuma máquina";
    default: return "Erro desconhecido";
    }
}
