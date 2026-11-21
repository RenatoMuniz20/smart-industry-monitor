#ifndef DELIVERY_WORKER_H
#define DELIVERY_WORKER_H

typedef struct DeliveryWorker DeliveryWorker;

/* One worker per process. Called by main before starting any threads: creates
 * the exclusive HTTP client (curl global initialization), then starts pthread.
 * The worker opens/uses/closes its own SQLite connection. *out must be NULL.
 * Returns 0 on success, -1 on initialization failure (diagnostic on stderr).
 * db_path/url are copied; the caller need not retain them.
 */
int delivery_worker_start(DeliveryWorker **out, const char *db_path,
                          const char *url, long timeout_ms);
/* Atomic query, NULL-safe. A suspended worker (ambiguous inflight) is NOT fatal.
 * No other worker data is exposed concurrently to main.
 */
int delivery_worker_failed(const DeliveryWorker *worker);
/* Requests stop, joins, reports separate HTTP counters, destroys the HTTP
 * client AFTER join and clears *worker. NULL-safe. On a join error, retains
 * the context rather than freeing resources potentially still in use. Returns -1 on fatal error.
 * An active HTTP request completes/times out and its SQLite transition finishes
 * before exit. Idle/backoff waits notice stop within 50 ms. Call from main only.
 * No new clients/threads may be started concurrently with start/stop (libcurl).
 */
int delivery_worker_stop(DeliveryWorker **worker);

#endif
