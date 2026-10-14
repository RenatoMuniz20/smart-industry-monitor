#define _POSIX_C_SOURCE 200809L

#include "csv_reader.h"

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

static int parse_int(const char *field, int *value)
{
    char *end;
    long parsed;

    errno = 0;
    parsed = strtol(field, &end, 10);
    if (end == field || *end != '\0' || errno == ERANGE ||
        parsed < INT_MIN || parsed > INT_MAX) {
        return -1;
    }
    *value = (int)parsed;
    return 0;
}

static int parse_double(const char *field, double *value)
{
    char *end;
    double parsed;

    if (*field == '\0') {
        *value = NAN;
        return 0;
    }
    errno = 0;
    parsed = strtod(field, &end);
    if (end == field || *end != '\0' || errno == ERANGE) {
        return -1;
    }
    *value = parsed;
    return 0;
}

static int copy_text(char *destination, size_t capacity, const char *field)
{
    size_t length = strlen(field);

    if (length >= capacity) {
        return -1;
    }
    memcpy(destination, field, length + 1);
    return 0;
}

int csv_parse_row(const char *line, DatasetRow *row)
{
    char *buffer;
    char *fields[15];
    size_t count = 1;
    size_t length;
    DatasetRow parsed = {0};
    int result = -1;

    if (line == NULL || row == NULL) {
        return -1;
    }
    length = strlen(line);
    if (length > 0 && line[length - 1] == '\n') {
        --length;
        if (length > 0 && line[length - 1] == '\r') {
            --length;
        }
    }
    buffer = malloc(length + 1);
    if (buffer == NULL) {
        return -1;
    }
    memcpy(buffer, line, length);
    buffer[length] = '\0';
    fields[0] = buffer;
    for (size_t i = 0; i < length; ++i) {
        /* Quoted fields and embedded line breaks are outside this format. */
        if (buffer[i] == '"' || buffer[i] == '\n' || buffer[i] == '\r') {
            goto done;
        }
        if (buffer[i] == ',') {
            if (count == 15) {
                goto done;
            }
            buffer[i] = '\0';
            fields[count++] = buffer + i + 1;
        }
    }
    if (count != 15) {
        goto done;
    }
    if (copy_text(parsed.timestamp, sizeof parsed.timestamp, fields[0]) != 0 ||
        parse_int(fields[1], &parsed.machine_id) != 0 ||
        copy_text(parsed.machine_type, sizeof parsed.machine_type, fields[2]) != 0 ||
        parse_double(fields[3], &parsed.vibration_rms) != 0 ||
        parse_double(fields[4], &parsed.temperature_motor) != 0 ||
        parse_double(fields[5], &parsed.current_phase_avg) != 0 ||
        parse_double(fields[6], &parsed.pressure_level) != 0 ||
        parse_double(fields[7], &parsed.rpm) != 0 ||
        copy_text(parsed.operating_mode, sizeof parsed.operating_mode, fields[8]) != 0 ||
        parse_double(fields[9], &parsed.hours_since_maintenance) != 0 ||
        parse_double(fields[10], &parsed.ambient_temp) != 0 ||
        parse_double(fields[11], &parsed.rul_hours) != 0 ||
        parse_int(fields[12], &parsed.failure_within_24h) != 0 ||
        copy_text(parsed.failure_type, sizeof parsed.failure_type, fields[13]) != 0 ||
        parse_double(fields[14], &parsed.estimated_repair_cost) != 0) {
        goto done;
    }
    *row = parsed;
    result = 0;

done:
    free(buffer);
    return result;
}

int csv_read_file(const char *path, size_t *row_count, size_t *error_line,
                  CsvRowHandler handler, void *context)
{
    static const char header[] =
        "timestamp,machine_id,machine_type,vibration_rms,temperature_motor,"
        "current_phase_avg,pressure_level,rpm,operating_mode,hours_since_maintenance,"
        "ambient_temp,rul_hours,failure_within_24h,failure_type,estimated_repair_cost";
    FILE *file;
    char *line = NULL;
    size_t capacity = 0;
    size_t physical_line = 0;
    ssize_t length;
    int result = -1;
    DatasetRow row;

    if (row_count != NULL) {
        *row_count = 0;
    }
    if (error_line != NULL) {
        *error_line = 0;
    }
    if (path == NULL || row_count == NULL || error_line == NULL) {
        return -1;
    }
    file = fopen(path, "r");
    if (file == NULL) {
        return -1;
    }

    for (;;) {
        errno = 0;
        length = getline(&line, &capacity, file);
        if (length < 0) {
            if (ferror(file) || !feof(file) || errno != 0) {
                *error_line = physical_line + 1;
                goto close_file;
            }
            break;
        }
        ++physical_line;
        /* Reject embedded NUL bytes rather than validating a truncated row. */
        if (memchr(line, '\0', (size_t)length) != NULL) {
            *error_line = physical_line;
            goto close_file;
        }
        if (physical_line == 1) {
            if (length > 0 && line[length - 1] == '\n') {
                line[--length] = '\0';
                if (length > 0 && line[length - 1] == '\r') {
                    line[--length] = '\0';
                }
            }
            if (strcmp(line, header) != 0) {
                *error_line = 1;
                goto close_file;
            }
        } else {
            if (csv_parse_row(line, &row) != 0) {
                *error_line = physical_line;
                goto close_file;
            }
            ++*row_count;
            if (handler != NULL) {
                int action = handler(&row, context);

                if (action == 1) {
                    result = 0;
                    goto close_file;
                }
                if (action != 0) {
                    *error_line = physical_line;
                    goto close_file;
                }
            }
        }
    }
    if (physical_line == 0) {
        *error_line = 1;
        goto close_file;
    }
    result = 0;

close_file:
    free(line);
    if (fclose(file) != 0) {
        result = -1;
    }
    return result;
}
