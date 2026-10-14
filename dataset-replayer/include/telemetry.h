#ifndef TELEMETRY_H
#define TELEMETRY_H

#include "csv_reader.h"

typedef enum {
    TELEMETRY_STATE_UNKNOWN = 0,
    TELEMETRY_STATE_IDLE,
    TELEMETRY_STATE_NORMAL,
    TELEMETRY_STATE_PEAK
} TelemetryState;

typedef struct Telemetry {
    int machine_id;
    char machine_type[64];
    char timestamp[64];
    double temperature;
    double current;
    double power;
    TelemetryState state;
    int failure_within_24h;
} Telemetry;

/* Returns 0 on success, -1 for NULL arguments or a non-binary failure flag.
 * out is unchanged on error. Text fields follow DatasetRow's string contract.
 * failure_within_24h is predictive, not an indication of current failure.
 */
int telemetry_normalize(const DatasetRow *row, Telemetry *out);

#endif
