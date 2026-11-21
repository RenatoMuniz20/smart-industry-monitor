#define _POSIX_C_SOURCE 200809L
#include "telemetry_queue.h"

#include <assert.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char uuid[] = "12345678-1234-1234-1234-123456789abc";
static const char body[] = "{\"data\":{\"sim_motor_temperature\":[{\"value\":49.51,\"timestamp\":\"2024-01-01T00:00:00\"}]}}";
static const char t1[] = "2024-01-01T00:00:01";
static const char t2[] = "2024-01-01T00:00:02";
static const char t3[] = "2024-01-01T00:00:03";
static const char t4[] = "2024-01-01T00:00:04";

static sqlite3 *inspect(const char *path)
{
    sqlite3 *db = NULL;
    assert(sqlite3_open(path, &db) == SQLITE_OK);
    return db;
}

static void sql(sqlite3 *db, const char *text)
{
    char *error = NULL;
    int rc = sqlite3_exec(db, text, NULL, NULL, &error);
    if (rc != SQLITE_OK) fprintf(stderr, "SQL de teste: %s\n", error);
    sqlite3_free(error);
    assert(rc == SQLITE_OK);
}

static int64_t scalar(sqlite3 *db, const char *text)
{
    sqlite3_stmt *stmt = NULL;
    assert(sqlite3_prepare_v2(db, text, -1, &stmt, NULL) == SQLITE_OK);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    int64_t value = sqlite3_column_int64(stmt, 0);
    assert(sqlite3_finalize(stmt) == SQLITE_OK);
    return value;
}

static void count_is(TelemetryQueue *q, int64_t expected)
{
    int64_t count = -1;
    assert(telemetry_queue_count(q, &count, NULL) == TELEMETRY_QUEUE_OK);
    assert(count == expected);
}

static int64_t next_id(TelemetryQueue *q, TelemetryQueueStatus status, const char *timestamp,
                        int64_t attempts, const char *last_error)
{
    TelemetryQueueItem item = {0};
    assert(telemetry_queue_peek(q, &item, NULL) == TELEMETRY_QUEUE_OK);
    assert(item.status == status && item.attempt_count == attempts);
    assert(strcmp(item.timestamp, timestamp) == 0 && strcmp(item.uuid, uuid) == 0);
    assert(strcmp(item.payload_json, body) == 0);
    assert(strlen(item.created_at) == 24 && item.created_at[23] == 'Z');
    assert((last_error == NULL && item.last_error == NULL) ||
           (last_error != NULL && item.last_error != NULL && strcmp(item.last_error, last_error) == 0));
    int64_t id = item.id;
    telemetry_queue_item_clear(&item);
    telemetry_queue_item_clear(&item);
    assert(item.payload_json == NULL && item.last_error == NULL);
    return id;
}

static void cursor_is(TelemetryQueue *q, int machine, const char *timestamp)
{
    char out[TELEMETRY_QUEUE_TIMESTAMP_SIZE] = "unchanged";
    int found = -1;
    assert(telemetry_queue_cursor(q, machine, out, &found, NULL) == TELEMETRY_QUEUE_OK);
    assert(found == (timestamp != NULL));
    assert(strcmp(out, timestamp != NULL ? timestamp : "unchanged") == 0);
}

static void lifecycle(const char *path)
{
    TelemetryQueue *q = NULL;
    TelemetryQueueError error;
    assert(telemetry_queue_open(path, &q, &error) == TELEMETRY_QUEUE_OK);
    assert(error.sqlite_code == SQLITE_OK && q != NULL);
    count_is(q, 0);
    cursor_is(q, 1, NULL);
    sqlite3 *db = inspect(path);
    assert(scalar(db, "SELECT count(*) FROM sqlite_master WHERE name IN ('telemetry_queue','delivered_cursor')") == 2);
    assert(scalar(db, "SELECT count(*) FROM pragma_table_info('delivered_cursor')") == 2);
    sqlite3_stmt *stmt = NULL;
    assert(sqlite3_prepare_v2(db, "PRAGMA journal_mode", -1, &stmt, NULL) == SQLITE_OK);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(stmt, 0), "wal") == 0);
    assert(sqlite3_finalize(stmt) == SQLITE_OK);

    TelemetryQueueItem item = {0}, previous;
    item.id = 123;
    memcpy(&previous, &item, sizeof item);
    assert(telemetry_queue_peek(q, &item, &error) == TELEMETRY_QUEUE_EMPTY);
    assert(memcmp(&item, &previous, sizeof item) == 0 && error.sqlite_code == SQLITE_OK);
    assert(telemetry_queue_check_duplicate(q, 1, t2, &error) == TELEMETRY_QUEUE_OK);
    assert(telemetry_queue_enqueue(q, 1, t2, uuid, body, &error) == TELEMETRY_QUEUE_OK);
    assert(telemetry_queue_enqueue(q, 2, t1, uuid, body, &error) == TELEMETRY_QUEUE_OK);
    count_is(q, 2);
    int64_t first = next_id(q, TELEMETRY_QUEUE_PENDING, t2, 0, NULL); /* insertion FIFO, not timestamp */
    assert(telemetry_queue_enqueue(q, 1, t2, uuid, body, &error) == TELEMETRY_QUEUE_DUPLICATE_QUEUED);
    assert(error.sqlite_code == SQLITE_OK);
    assert(telemetry_queue_start_attempt(q, first, &error) == TELEMETRY_QUEUE_OK);
    assert(telemetry_queue_start_attempt(q, first, &error) == TELEMETRY_QUEUE_ERROR);
    assert(error.sqlite_code == SQLITE_CONSTRAINT);
    assert(telemetry_queue_check_duplicate(q, 1, t2, NULL) == TELEMETRY_QUEUE_DUPLICATE_QUEUED);
    assert(telemetry_queue_close(&q, &error) == TELEMETRY_QUEUE_OK && q == NULL);
    assert(telemetry_queue_open(path, &q, &error) == TELEMETRY_QUEUE_OK);
    assert(next_id(q, TELEMETRY_QUEUE_INFLIGHT, t2, 1, NULL) == first);
    assert(telemetry_queue_retry(q, first, "HTTP 503: definitive refusal", &error) == TELEMETRY_QUEUE_OK);
    next_id(q, TELEMETRY_QUEUE_PENDING, t2, 1, "HTTP 503: definitive refusal");
    assert(telemetry_queue_retry(q, first, "again", &error) == TELEMETRY_QUEUE_ERROR);
    assert(telemetry_queue_start_attempt(q, first, &error) == TELEMETRY_QUEUE_OK);
    assert(telemetry_queue_block(q, first, "configuration error", &error) == TELEMETRY_QUEUE_OK);
    assert(scalar(db, "SELECT count(*) FROM telemetry_queue WHERE status='blocked' AND attempt_count=2 AND last_error='configuration error'") == 1);
    assert(telemetry_queue_check_duplicate(q, 1, t2, NULL) == TELEMETRY_QUEUE_DUPLICATE_QUEUED);
    count_is(q, 2); /* blocked still undelivered, but excluded from peek */
    int64_t second = next_id(q, TELEMETRY_QUEUE_PENDING, t1, 0, NULL);
    assert(second > first);
    assert(telemetry_queue_confirm(q, second, &error) == TELEMETRY_QUEUE_ERROR);
    cursor_is(q, 2, NULL);
    assert(telemetry_queue_start_attempt(q, second, &error) == TELEMETRY_QUEUE_OK);
    assert(telemetry_queue_confirm(q, second, &error) == TELEMETRY_QUEUE_OK);
    count_is(q, 1);
    cursor_is(q, 2, t1);
    assert(telemetry_queue_confirm(q, second, &error) == TELEMETRY_QUEUE_ERROR);
    assert(telemetry_queue_enqueue(q, 2, t1, uuid, body, &error) == TELEMETRY_QUEUE_DUPLICATE_DELIVERED);
    assert(telemetry_queue_enqueue(q, 2, "2024-01-01T00:00:00", uuid, body, &error) == TELEMETRY_QUEUE_DUPLICATE_DELIVERED);
    assert(telemetry_queue_enqueue(q, 2, t3, uuid, body, &error) == TELEMETRY_QUEUE_OK);
    int64_t third = next_id(q, TELEMETRY_QUEUE_PENDING, t3, 0, NULL);
    assert(third > second); /* AUTOINCREMENT after removal */
    assert(telemetry_queue_close(&q, &error) == TELEMETRY_QUEUE_OK);
    assert(telemetry_queue_open(path, &q, &error) == TELEMETRY_QUEUE_OK);
    cursor_is(q, 2, t1);
    assert(next_id(q, TELEMETRY_QUEUE_PENDING, t3, 0, NULL) == third);
    assert(telemetry_queue_block(q, third, "blocked before attempting", &error) == TELEMETRY_QUEUE_OK);
    assert(telemetry_queue_peek(q, &item, &error) == TELEMETRY_QUEUE_EMPTY);
    assert(telemetry_queue_start_attempt(q, third, &error) == TELEMETRY_QUEUE_ERROR);
    assert(telemetry_queue_confirm(q, first, &error) == TELEMETRY_QUEUE_ERROR);
    count_is(q, 2);
    assert(sqlite3_close(db) == SQLITE_OK);
    assert(telemetry_queue_close(&q, &error) == TELEMETRY_QUEUE_OK);
    assert(telemetry_queue_close(&q, &error) == TELEMETRY_QUEUE_OK);
}

static void transactions(const char *path)
{
    TelemetryQueue *q = NULL;
    TelemetryQueueError error;
    assert(telemetry_queue_open(path, &q, &error) == TELEMETRY_QUEUE_OK);
    sqlite3 *db = inspect(path);
    assert(telemetry_queue_enqueue(q, 1, t1, uuid, body, NULL) == TELEMETRY_QUEUE_OK);
    assert(telemetry_queue_enqueue(q, 1, t3, uuid, body, NULL) == TELEMETRY_QUEUE_OK);
    int64_t old = next_id(q, TELEMETRY_QUEUE_PENDING, t1, 0, NULL);
    int64_t newer = scalar(db, "SELECT id FROM telemetry_queue WHERE timestamp='2024-01-01T00:00:03'");
    assert(telemetry_queue_start_attempt(q, newer, &error) == TELEMETRY_QUEUE_OK);
    assert(telemetry_queue_confirm(q, newer, &error) == TELEMETRY_QUEUE_OK);
    cursor_is(q, 1, t3);
    assert(telemetry_queue_start_attempt(q, old, &error) == TELEMETRY_QUEUE_OK);
    assert(telemetry_queue_confirm(q, old, &error) == TELEMETRY_QUEUE_OK);
    cursor_is(q, 1, t3); /* an older confirmation never moves the cursor back */
    assert(telemetry_queue_enqueue(q, 1, t4, uuid, body, &error) == TELEMETRY_QUEUE_OK);
    int64_t id = next_id(q, TELEMETRY_QUEUE_PENDING, t4, 0, NULL);
    assert(telemetry_queue_start_attempt(q, id, NULL) == TELEMETRY_QUEUE_OK);
    sql(db, "CREATE TRIGGER reject_delete BEFORE DELETE ON telemetry_queue BEGIN SELECT RAISE(ABORT,'injected delete failure'); END");
    assert(telemetry_queue_confirm(q, id, &error) == TELEMETRY_QUEUE_ERROR);
    assert(error.sqlite_code == SQLITE_CONSTRAINT_TRIGGER && strstr(error.message, "injected delete failure") != NULL);
    cursor_is(q, 1, t3); /* cursor update rolled back */
    assert(next_id(q, TELEMETRY_QUEUE_INFLIGHT, t4, 1, NULL) == id);
    count_is(q, 1);
    assert(telemetry_queue_enqueue(q, 3, t1, uuid, body, NULL) == TELEMETRY_QUEUE_OK);
    int64_t third = scalar(db, "SELECT id FROM telemetry_queue WHERE machine_id=3");
    assert(telemetry_queue_start_attempt(q, third, NULL) == TELEMETRY_QUEUE_OK);
    assert(telemetry_queue_confirm(q, third, NULL) == TELEMETRY_QUEUE_ERROR);
    cursor_is(q, 3, NULL); /* cursor insertion also rolled back, even without diagnostics */
    sql(db, "DROP TRIGGER reject_delete");
    assert(telemetry_queue_confirm(q, id, &error) == TELEMETRY_QUEUE_OK);
    cursor_is(q, 1, t4);
    sql(db, "CREATE TRIGGER reject_cursor BEFORE INSERT ON delivered_cursor BEGIN SELECT RAISE(ABORT,'injected cursor failure'); END");
    assert(telemetry_queue_confirm(q, third, &error) == TELEMETRY_QUEUE_ERROR);
    assert(error.sqlite_code == SQLITE_CONSTRAINT_TRIGGER);
    assert(next_id(q, TELEMETRY_QUEUE_INFLIGHT, t1, 1, NULL) == third);
    cursor_is(q, 3, NULL);
    sql(db, "DROP TRIGGER reject_cursor");
    assert(telemetry_queue_confirm(q, third, NULL) == TELEMETRY_QUEUE_OK);
    count_is(q, 0);
    assert(sqlite3_close(db) == SQLITE_OK);
    assert(telemetry_queue_close(&q, NULL) == TELEMETRY_QUEUE_OK);
}

static void separate_connections(const char *path)
{
    TelemetryQueue *a = NULL, *b = NULL;
    TelemetryQueueError error;
    assert(telemetry_queue_open(path, &a, NULL) == TELEMETRY_QUEUE_OK);
    assert(telemetry_queue_open(path, &b, NULL) == TELEMETRY_QUEUE_OK);
    assert(telemetry_queue_enqueue(a, 1, t1, uuid, body, NULL) == TELEMETRY_QUEUE_OK);
    assert(telemetry_queue_enqueue(b, 1, t1, uuid, body, NULL) == TELEMETRY_QUEUE_DUPLICATE_QUEUED);
    int64_t id = next_id(b, TELEMETRY_QUEUE_PENDING, t1, 0, NULL);
    assert(telemetry_queue_start_attempt(b, id, NULL) == TELEMETRY_QUEUE_OK);
    assert(telemetry_queue_start_attempt(a, id, &error) == TELEMETRY_QUEUE_ERROR);
    assert(next_id(a, TELEMETRY_QUEUE_INFLIGHT, t1, 1, NULL) == id);
    sqlite3 *db = inspect(path);
    sql(db, "BEGIN IMMEDIATE");
    assert(telemetry_queue_enqueue(a, 2, t2, uuid, body, &error) == TELEMETRY_QUEUE_ERROR);
    assert((error.sqlite_code & 255) == SQLITE_BUSY);
    sql(db, "ROLLBACK");
    count_is(b, 1);
    assert(telemetry_queue_enqueue(b, 2, t2, uuid, body, NULL) == TELEMETRY_QUEUE_OK);
    assert(sqlite3_close(db) == SQLITE_OK);
    assert(telemetry_queue_close(&a, NULL) == TELEMETRY_QUEUE_OK);
    assert(telemetry_queue_close(&b, NULL) == TELEMETRY_QUEUE_OK);
}

static void invalid_arguments(const char *path, const char *directory)
{
    TelemetryQueue *q = NULL;
    TelemetryQueueError error;
    char missing[512];
    snprintf(missing, sizeof missing, "%s/missing/queue.db", directory);
    assert(telemetry_queue_open(missing, &q, &error) == TELEMETRY_QUEUE_ERROR);
    assert(q == NULL && (error.sqlite_code & 255) == SQLITE_CANTOPEN && error.message[0]);
    assert(telemetry_queue_open(":memory:", &q, &error) == TELEMETRY_QUEUE_ERROR && q == NULL);
    assert(telemetry_queue_open(NULL, &q, &error) == TELEMETRY_QUEUE_ERROR);
    assert(telemetry_queue_open("", &q, &error) == TELEMETRY_QUEUE_ERROR);
    assert(telemetry_queue_open(path, NULL, &error) == TELEMETRY_QUEUE_ERROR);
    assert(telemetry_queue_close(NULL, NULL) == TELEMETRY_QUEUE_OK);
    assert(telemetry_queue_open(path, &q, NULL) == TELEMETRY_QUEUE_OK);
    assert(telemetry_queue_open(path, &q, &error) == TELEMETRY_QUEUE_ERROR);
    assert(telemetry_queue_enqueue(q, 0, t1, uuid, body, &error) == TELEMETRY_QUEUE_ERROR);
    assert(telemetry_queue_enqueue(q, 1, "2024-01-01 00:00:00", uuid, body, &error) == TELEMETRY_QUEUE_ERROR);
    assert(telemetry_queue_enqueue(q, 1, "2024-13-01T00:00:00", uuid, body, &error) == TELEMETRY_QUEUE_ERROR);
    assert(telemetry_queue_enqueue(q, 1, t1, "bad-uuid", body, &error) == TELEMETRY_QUEUE_ERROR);
    assert(telemetry_queue_enqueue(q, 1, t1, uuid, NULL, &error) == TELEMETRY_QUEUE_ERROR);
    assert(telemetry_queue_enqueue(q, 1, t1, uuid, "invalid JSON", &error) == TELEMETRY_QUEUE_ERROR);
    assert(error.sqlite_code != SQLITE_OK && error.message[0]);
    assert(telemetry_queue_enqueue(q, 1, t1, uuid, "[]", &error) == TELEMETRY_QUEUE_ERROR);
    count_is(q, 0);
    assert(telemetry_queue_enqueue(q, 1, t1, uuid, body, NULL) == TELEMETRY_QUEUE_OK);
    int64_t id = next_id(q, TELEMETRY_QUEUE_PENDING, t1, 0, NULL);
    assert(telemetry_queue_start_attempt(q, -1, &error) == TELEMETRY_QUEUE_ERROR);
    assert(telemetry_queue_start_attempt(q, id + 1000, &error) == TELEMETRY_QUEUE_ERROR);
    assert(telemetry_queue_retry(q, id, NULL, &error) == TELEMETRY_QUEUE_ERROR);
    assert(telemetry_queue_block(q, id, "", &error) == TELEMETRY_QUEUE_ERROR);
    int64_t sentinel = 123;
    assert(telemetry_queue_count(NULL, &sentinel, &error) == TELEMETRY_QUEUE_ERROR && sentinel == 123);
    assert(telemetry_queue_count(q, NULL, &error) == TELEMETRY_QUEUE_ERROR);
    TelemetryQueueItem item = {0};
    assert(telemetry_queue_peek(q, &item, NULL) == TELEMETRY_QUEUE_OK);
    assert(telemetry_queue_peek(q, &item, &error) == TELEMETRY_QUEUE_ERROR);
    telemetry_queue_item_clear(&item);
    assert(telemetry_queue_peek(q, NULL, &error) == TELEMETRY_QUEUE_ERROR);
    char cursor[20] = "unchanged";
    int found = 123;
    assert(telemetry_queue_cursor(q, 0, cursor, &found, &error) == TELEMETRY_QUEUE_ERROR);
    assert(found == 123 && strcmp(cursor, "unchanged") == 0);
    /* Prepared statements do not hide SQLite failures or partially write out. */
    sqlite3 *db = inspect(path);
    sql(db, "DROP TABLE telemetry_queue");
    item.id = 789;
    TelemetryQueueItem saved;
    memcpy(&saved, &item, sizeof item);
    assert(telemetry_queue_peek(q, &item, &error) == TELEMETRY_QUEUE_ERROR);
    assert(error.sqlite_code == SQLITE_ERROR && error.message[0]);
    assert(memcmp(&item, &saved, sizeof item) == 0);
    sentinel = 456;
    assert(telemetry_queue_count(q, &sentinel, &error) == TELEMETRY_QUEUE_ERROR && sentinel == 456);
    assert(sqlite3_close(db) == SQLITE_OK);
    assert(telemetry_queue_close(&q, NULL) == TELEMETRY_QUEUE_OK);
}

int main(void)
{
    char directory[] = "/tmp/sim09-queue-XXXXXX";
    assert(mkdtemp(directory) != NULL);
    const char *names[] = {"lifecycle", "transactions", "connections", "invalid"};
    char path[512];
    for (size_t i = 0; i < 4; ++i) {
        snprintf(path, sizeof path, "%s/%s.db", directory, names[i]);
        if (i == 0) lifecycle(path);
        else if (i == 1) transactions(path);
        else if (i == 2) separate_connections(path);
        else invalid_arguments(path, directory);
        assert(unlink(path) == 0);
        char sidecar[520];
        snprintf(sidecar, sizeof sidecar, "%s-wal", path);
        assert(access(sidecar, F_OK) != 0);
        snprintf(sidecar, sizeof sidecar, "%s-shm", path);
        assert(access(sidecar, F_OK) != 0);
    }
    assert(rmdir(directory) == 0);
    puts("Todos os testes de telemetry_queue passaram (WAL, estados, cursor, rollback e conexões separadas).");
    return 0;
}
