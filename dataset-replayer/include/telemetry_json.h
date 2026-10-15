#ifndef TELEMETRY_JSON_H
#define TELEMETRY_JSON_H

#include <stddef.h>
#include "telemetry.h"

/* Returns 0 on success, -1 on invalid arguments/text/flag, insufficient
 * capacity (including NUL), or formatting/locale error. buffer is unchanged
 * on error. Text arrays must be NUL-terminated UTF-8. Nonfinite doubles become
 * null; unrecognized states become UNKNOWN. No network or file I/O.
 */
int telemetry_to_json(const Telemetry *telemetry, char *buffer, size_t capacity);

#endif
