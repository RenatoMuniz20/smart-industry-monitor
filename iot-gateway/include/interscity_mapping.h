#ifndef INTERSCITY_MAPPING_H
#define INTERSCITY_MAPPING_H

#include <stddef.h>

#define INTERSCITY_UUID_LENGTH 36
#define INTERSCITY_MAPPING_CAPACITY 1024

typedef struct {
    int machine_id;
    char uuid[INTERSCITY_UUID_LENGTH + 1];
} InterscityMappingEntry;

typedef struct {
    size_t count;
    InterscityMappingEntry entries[INTERSCITY_MAPPING_CAPACITY];
} InterscityMapping;

typedef enum {
    INTERSCITY_MAPPING_OK = 0,
    INTERSCITY_MAPPING_ARGUMENT,
    INTERSCITY_MAPPING_OPEN,
    INTERSCITY_MAPPING_READ,
    INTERSCITY_MAPPING_CR,
    INTERSCITY_MAPPING_LINE_TOO_LONG,
    INTERSCITY_MAPPING_SYNTAX,
    INTERSCITY_MAPPING_MACHINE_ID,
    INTERSCITY_MAPPING_UUID,
    INTERSCITY_MAPPING_DUPLICATE_ID,
    INTERSCITY_MAPPING_DUPLICATE_UUID,
    INTERSCITY_MAPPING_TOO_MANY,
    INTERSCITY_MAPPING_EMPTY
} InterscityMappingError;

/* Loads config/interscity/machines.conf style files: one "<machine_id> <uuid>"
 * pair per line, separated by spaces or tabs. Empty lines and lines whose
 * first non-blank character is '#' are ignored. machine_id is a positive
 * decimal int; uuid is canonical lowercase (8-4-4-4-12 hex digits). CR bytes
 * (CRLF files), extra fields, duplicated IDs or UUIDs, lines longer than 254
 * bytes, more than INTERSCITY_MAPPING_CAPACITY pairs or no pairs at all are
 * rejected. Returns 0/-1. out is unchanged on failure. error and error_line
 * are optional; error_line is the 1-based line of a line-level error, else 0.
 * No logging and no dynamic allocation besides the stdio FILE.
 */
int interscity_mapping_load(const char *path, InterscityMapping *out,
                            InterscityMappingError *error, size_t *error_line);

/* Returns the UUID mapped to machine_id, or NULL when absent. The pointer is
 * owned by mapping and valid while mapping is unchanged.
 */
const char *interscity_mapping_lookup(const InterscityMapping *mapping, int machine_id);

const char *interscity_mapping_error_string(InterscityMappingError error);

#endif
