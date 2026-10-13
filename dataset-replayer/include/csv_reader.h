#ifndef CSV_READER_H
#define CSV_READER_H

#include <stddef.h>

typedef struct DatasetRow {
    char timestamp[64];
    int machine_id;
    char machine_type[64];
    double vibration_rms;
    double temperature_motor;
    double current_phase_avg;
    double pressure_level;
    double rpm;
    char operating_mode[64];
    double hours_since_maintenance;
    double ambient_temp;
    double rul_hours;
    int failure_within_24h;
    char failure_type[64];
    double estimated_repair_cost;
} DatasetRow;

/* Returns 0 on success, -1 on invalid input; row is unchanged on error. */
int csv_parse_row(const char *line, DatasetRow *row);

/* Return 0 to continue, 1 to stop successfully, negative on error.
 * row is valid only during the callback.
 */
typedef int (*CsvRowHandler)(const DatasetRow *row, void *context);

/* Counts valid rows before the first error. Physical lines start at 1.
 * error_line is 0 on success or on errors unrelated to a specific line.
 * path, row_count and error_line must be non-NULL; handler is optional.
 * The callback row is counted, including on stop/error.
 * Lines after an early stop are not validated.
 */
int csv_read_file(const char *path, size_t *row_count, size_t *error_line,
                  CsvRowHandler handler, void *context);

#endif
