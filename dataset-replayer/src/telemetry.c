#include "telemetry.h"

#include <math.h>
#include <string.h>

int telemetry_normalize(const DatasetRow *row, Telemetry *out)
{
    Telemetry normalized = {0};

    if (row == NULL || out == NULL ||
        (row->failure_within_24h != 0 && row->failure_within_24h != 1)) {
        return -1;
    }

    normalized.machine_id = row->machine_id;
    memcpy(normalized.machine_type, row->machine_type, sizeof normalized.machine_type);
    memcpy(normalized.timestamp, row->timestamp, sizeof normalized.timestamp);
    normalized.temperature = row->temperature_motor;
    normalized.current = row->current_phase_avg;
    normalized.power = NAN;
    normalized.state = TELEMETRY_STATE_UNKNOWN;
    if (strcmp(row->operating_mode, "idle") == 0) {
        normalized.state = TELEMETRY_STATE_IDLE;
    } else if (strcmp(row->operating_mode, "normal") == 0) {
        normalized.state = TELEMETRY_STATE_NORMAL;
    } else if (strcmp(row->operating_mode, "peak") == 0) {
        normalized.state = TELEMETRY_STATE_PEAK;
    }
    normalized.failure_within_24h = row->failure_within_24h;

    *out = normalized;
    return 0;
}
