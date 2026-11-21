#ifndef TELEMETRY_QUEUE_H
#define TELEMETRY_QUEUE_H

#include <stddef.h>
#include <stdint.h>

#define TELEMETRY_QUEUE_TIMESTAMP_SIZE 20
#define TELEMETRY_QUEUE_BUSY_TIMEOUT_MS 1000

typedef struct TelemetryQueue TelemetryQueue;
typedef enum {
    TELEMETRY_QUEUE_ERROR = -1,
    TELEMETRY_QUEUE_OK = 0,
    TELEMETRY_QUEUE_DUPLICATE_QUEUED = 1, /* pending, inflight or blocked */
    TELEMETRY_QUEUE_DUPLICATE_DELIVERED = 2, /* timestamp <= delivered cursor */
    TELEMETRY_QUEUE_EMPTY = 3
} TelemetryQueueResult;
typedef enum {
    TELEMETRY_QUEUE_PENDING = 0,
    TELEMETRY_QUEUE_INFLIGHT,
    TELEMETRY_QUEUE_BLOCKED
} TelemetryQueueStatus;
typedef struct {
    int sqlite_code; /* SQLite extended result code; 0 on success/duplicate/empty. */
    char message[256];
} TelemetryQueueError;
typedef struct {
    int64_t id;
    int machine_id;
    char timestamp[TELEMETRY_QUEUE_TIMESTAMP_SIZE];
    char uuid[37];
    char *payload_json;
    TelemetryQueueStatus status;
    int64_t attempt_count;
    char *last_error; /* NULL when no error was recorded. */
    char created_at[32]; /* UTC insertion time, distinct from sample timestamp. */
} TelemetryQueueItem;

/* File-backed SQLite database, automatically initialized with WAL, FULL sync
 * and a 1000 ms busy timeout. *out must be NULL; unchanged on error. One handle
 * per thread: never share a connection between threads. Separate connections
 * can use the same path. No network/HTTP dependencies. error is always optional.
 * SQLite must provide built-in JSON functions (3.38+ standard builds).
 */
TelemetryQueueResult telemetry_queue_open(const char *path, TelemetryQueue **out,
                                           TelemetryQueueError *error);
/* Clears *queue on success; retains it if sqlite3_close fails so caller can
 * inspect the error/retry. NULL-safe. No statements escape this module.
 */
TelemetryQueueResult telemetry_queue_close(TelemetryQueue **queue, TelemetryQueueError *error);

/* Positive machine_id; timestamp exactly YYYY-MM-DDTHH:MM:SS, with component
 * ranges as in SIM-08 (no full calendar check); canonical lowercase UUID;
 * payload_json a NUL-terminated JSON object. Strings are copied into SQLite.
 * Insert + cursor/uniqueness check is atomic. Duplicate returns distinguish
 * queued from confirmed samples; confirmed takes precedence. Cursor policy
 * ignores timestamps <= last confirmed: out-of-order delivery is NOT supported.
 */
TelemetryQueueResult telemetry_queue_enqueue(TelemetryQueue *queue, int machine_id,
    const char *timestamp, const char *uuid, const char *payload_json, TelemetryQueueError *error);
/* Advisory check only; concurrent insertion/delivery can change its result.
 * enqueue repeats the check inside a write transaction.
 */
TelemetryQueueResult telemetry_queue_check_duplicate(TelemetryQueue *queue, int machine_id,
    const char *timestamp, TelemetryQueueError *error);

/* Oldest non-blocked row by insertion id, pending OR inflight. Does not claim
 * the row or hold a transaction after returning. Initialize *out to {0}; clear
 * a previous item before reuse. out is unchanged on ERROR/EMPTY. Item owns its
 * payload_json/last_error (malloc); clear after use. Inflight rows survive reopen
 * unchanged: ambiguous sends must be reconciled by SIM-09.4, never reset here.
 */
TelemetryQueueResult telemetry_queue_peek(TelemetryQueue *queue, TelemetryQueueItem *out,
                                         TelemetryQueueError *error);
void telemetry_queue_item_clear(TelemetryQueueItem *item);

/* Guarded transitions: pending -> inflight increments attempt_count exactly
 * once; inflight -> pending requires a definitive transient failure; block
 * accepts pending/inflight. Errors must be nonempty strings and are copied.
 * Missing IDs and illegal transitions return ERROR, never a silent success.
 */
TelemetryQueueResult telemetry_queue_start_attempt(TelemetryQueue *queue, int64_t id,
                                                  TelemetryQueueError *error);
TelemetryQueueResult telemetry_queue_retry(TelemetryQueue *queue, int64_t id,
    const char *last_error, TelemetryQueueError *error);
TelemetryQueueResult telemetry_queue_block(TelemetryQueue *queue, int64_t id,
    const char *last_error, TelemetryQueueError *error);
/* Inflight only. Cursor advances monotonically and row is deleted in ONE
 * transaction; any failure rolls back both operations. No HTTP call belongs
 * inside this function/transaction. A rollback failure invalidates the handle
 * for further operations until close; the diagnostic includes that failure.
 */
TelemetryQueueResult telemetry_queue_confirm(TelemetryQueue *queue, int64_t id,
                                            TelemetryQueueError *error);
/* Count all undelivered rows, including blocked/inflight. out unchanged on error. */
TelemetryQueueResult telemetry_queue_count(TelemetryQueue *queue, int64_t *out,
                                          TelemetryQueueError *error);
/* found=0 on successful missing cursor (out unchanged); otherwise copies 20
 * bytes including NUL. Both outputs unchanged on error.
 */
TelemetryQueueResult telemetry_queue_cursor(TelemetryQueue *queue, int machine_id,
    char out[TELEMETRY_QUEUE_TIMESTAMP_SIZE], int *found, TelemetryQueueError *error);

#endif
