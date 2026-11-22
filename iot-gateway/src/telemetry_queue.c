#include "telemetry_queue.h"

#include <limits.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct TelemetryQueue {
    sqlite3 *db;
    int poisoned;
};

static TelemetryQueueResult report(TelemetryQueueError *error, int code, const char *message)
{
    if (error != NULL) {
        error->sqlite_code = code;
        snprintf(error->message, sizeof error->message, "%s", message);
    }
    return code == SQLITE_OK ? TELEMETRY_QUEUE_OK : TELEMETRY_QUEUE_ERROR;
}

static TelemetryQueueResult db_error(TelemetryQueue *q, int code, TelemetryQueueError *error)
{
    int extended = sqlite3_extended_errcode(q->db);
    if ((extended & 255) == (code & 255)) code = extended;
    return report(error, code, sqlite3_errmsg(q->db));
}

static int usable(TelemetryQueue *q, TelemetryQueueError *error)
{
    if (q == NULL) {
        report(error, SQLITE_MISUSE, "Contexto da fila nulo");
        return 0;
    }
    if (q->poisoned) {
        report(error, SQLITE_ABORT, "Conexão inválida após falha de rollback; feche o contexto");
        return 0;
    }
    report(error, SQLITE_OK, "");
    return 1;
}

static int valid_timestamp(const char *s)
{
    if (s == NULL || strlen(s) != 19) return 0;
    for (int i = 0; i < 19; ++i) {
        if (i == 4 || i == 7) { if (s[i] != '-') return 0; }
        else if (i == 10) { if (s[i] != 'T') return 0; }
        else if (i == 13 || i == 16) { if (s[i] != ':') return 0; }
        else if (s[i] < '0' || s[i] > '9') return 0;
    }
    int month = (s[5] - '0') * 10 + s[6] - '0';
    int day = (s[8] - '0') * 10 + s[9] - '0';
    int hour = (s[11] - '0') * 10 + s[12] - '0';
    int minute = (s[14] - '0') * 10 + s[15] - '0';
    int second = (s[17] - '0') * 10 + s[18] - '0';
    return month >= 1 && month <= 12 && day >= 1 && day <= 31 &&
           hour <= 23 && minute <= 59 && second <= 59;
}

static int valid_uuid(const char *s)
{
    if (s == NULL || strlen(s) != 36) return 0;
    for (int i = 0; i < 36; ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (s[i] != '-') return 0;
        } else if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) return 0;
    }
    return 1;
}

static int execute(TelemetryQueue *q, const char *sql, TelemetryQueueError *error)
{
    int rc = sqlite3_exec(q->db, sql, NULL, NULL, NULL);
    if (rc != SQLITE_OK) { db_error(q, rc, error); return -1; }
    return 0;
}

static sqlite3_stmt *prepare(TelemetryQueue *q, const char *sql, TelemetryQueueError *error)
{
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(q->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        db_error(q, rc, error);
        sqlite3_finalize(stmt);
        return NULL;
    }
    return stmt;
}

/* Always finalize. Preserve the original error if step/bind already failed. */
static int finish(TelemetryQueue *q, sqlite3_stmt *stmt, int rc, TelemetryQueueError *error)
{
    int failed = rc != SQLITE_DONE && rc != SQLITE_ROW && rc != SQLITE_OK;
    if (failed) db_error(q, rc, error);
    int final_rc = sqlite3_finalize(stmt);
    if (!failed && final_rc != SQLITE_OK) {
        db_error(q, final_rc, error);
        failed = 1;
    }
    return failed ? -1 : 0;
}

static TelemetryQueueResult abort_transaction(TelemetryQueue *q, TelemetryQueueError *error)
{
    if (!sqlite3_get_autocommit(q->db)) {
        int rc = sqlite3_exec(q->db, "ROLLBACK", NULL, NULL, NULL);
        if (rc != SQLITE_OK) {
            q->poisoned = 1;
            if (error != NULL) {
                char previous[sizeof error->message];
                memcpy(previous, error->message, sizeof previous);
                error->sqlite_code = sqlite3_extended_errcode(q->db);
                snprintf(error->message, sizeof error->message, "%.110s; rollback: %.110s",
                         previous, sqlite3_errmsg(q->db));
            }
        }
    }
    return TELEMETRY_QUEUE_ERROR;
}

TelemetryQueueResult telemetry_queue_open(const char *path, TelemetryQueue **out,
                                           TelemetryQueueError *error)
{
    report(error, SQLITE_OK, "");
    if (path == NULL || *path == '\0' || out == NULL || *out != NULL)
        return report(error, SQLITE_MISUSE, "Caminho ou saída inválida");
    if (sqlite3_threadsafe() == 0)
        return report(error, SQLITE_MISUSE, "SQLite deve suportar conexões independentes em threads");
    TelemetryQueue *q = calloc(1, sizeof *q);
    if (q == NULL) return report(error, SQLITE_NOMEM, "Memória indisponível");
    int rc = sqlite3_open_v2(path, &q->db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX, NULL);
    if (rc != SQLITE_OK) {
        if (q->db != NULL) db_error(q, rc, error);
        else report(error, rc, "Falha ao abrir SQLite");
        goto failed;
    }
    rc = sqlite3_extended_result_codes(q->db, 1);
    if (rc == SQLITE_OK) rc = sqlite3_busy_timeout(q->db, TELEMETRY_QUEUE_BUSY_TIMEOUT_MS);
    if (rc != SQLITE_OK) { db_error(q, rc, error); goto failed; }
    sqlite3_stmt *stmt = prepare(q, "PRAGMA journal_mode=WAL", error);
    if (stmt == NULL) goto failed;
    rc = sqlite3_step(stmt);
    int wal = rc == SQLITE_ROW && sqlite3_column_text(stmt, 0) != NULL &&
              strcmp((const char *)sqlite3_column_text(stmt, 0), "wal") == 0;
    if (finish(q, stmt, rc, error) != 0) goto failed;
    if (!wal) { report(error, SQLITE_ERROR, "SQLite não ativou WAL; use banco em arquivo local"); goto failed; }
    if (execute(q, "PRAGMA synchronous=FULL", error) != 0 ||
        execute(q, "BEGIN IMMEDIATE", error) != 0) goto failed;
    const char *schema =
        "CREATE TABLE IF NOT EXISTS telemetry_queue ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "machine_id INTEGER NOT NULL CHECK(machine_id>0 AND machine_id<=2147483647),"
        "timestamp TEXT NOT NULL CHECK(length(timestamp)=19 AND substr(timestamp,11,1)='T'),"
        "uuid TEXT NOT NULL CHECK(length(uuid)=36),"
        "payload_json TEXT NOT NULL CHECK(json_valid(payload_json) AND json_type(payload_json)='object'),"
        "status TEXT NOT NULL DEFAULT 'pending' CHECK(status IN ('pending','inflight','blocked')),"
        "attempt_count INTEGER NOT NULL DEFAULT 0 CHECK(typeof(attempt_count)='integer' AND attempt_count>=0),"
        "last_error TEXT, created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')),"
        "UNIQUE(machine_id,timestamp));"
        "CREATE TABLE IF NOT EXISTS delivered_cursor ("
        "machine_id INTEGER PRIMARY KEY CHECK(machine_id>0 AND machine_id<=2147483647),"
        "last_timestamp TEXT NOT NULL CHECK(length(last_timestamp)=19 AND substr(last_timestamp,11,1)='T'));";
    if (execute(q, schema, error) != 0 || execute(q, "COMMIT", error) != 0) {
        abort_transaction(q, error);
        goto failed;
    }
    *out = q;
    return TELEMETRY_QUEUE_OK;
failed:
    if (q->db != NULL) sqlite3_close(q->db);
    free(q);
    return TELEMETRY_QUEUE_ERROR;
}

TelemetryQueueResult telemetry_queue_close(TelemetryQueue **queue, TelemetryQueueError *error)
{
    report(error, SQLITE_OK, "");
    if (queue == NULL || *queue == NULL) return TELEMETRY_QUEUE_OK;
    TelemetryQueue *q = *queue;
    int rc = sqlite3_close(q->db);
    if (rc != SQLITE_OK) return db_error(q, rc, error);
    free(q);
    *queue = NULL;
    return TELEMETRY_QUEUE_OK;
}

TelemetryQueueResult telemetry_queue_check_duplicate(TelemetryQueue *q, int machine_id,
    const char *timestamp, TelemetryQueueError *error)
{
    if (!usable(q, error)) return TELEMETRY_QUEUE_ERROR;
    if (machine_id <= 0 || !valid_timestamp(timestamp))
        return report(error, SQLITE_MISUSE, "ID ou timestamp normalizado inválido");
    sqlite3_stmt *stmt = prepare(q,
        "SELECT CASE WHEN EXISTS(SELECT 1 FROM delivered_cursor WHERE machine_id=?1 AND last_timestamp>=?2) "
        "THEN 2 WHEN EXISTS(SELECT 1 FROM telemetry_queue WHERE machine_id=?1 AND timestamp=?2) THEN 1 ELSE 0 END", error);
    if (stmt == NULL) return TELEMETRY_QUEUE_ERROR;
    int rc = sqlite3_bind_int(stmt, 1, machine_id);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 2, timestamp, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    int result = rc == SQLITE_ROW ? sqlite3_column_int(stmt, 0) : -1;
    if (finish(q, stmt, rc, error) != 0) return TELEMETRY_QUEUE_ERROR;
    if (result < 0) return report(error, SQLITE_CORRUPT, "Consulta de duplicata sem resultado");
    return (TelemetryQueueResult)result;
}

TelemetryQueueResult telemetry_queue_enqueue(TelemetryQueue *q, int machine_id,
    const char *timestamp, const char *uuid, const char *payload_json, TelemetryQueueError *error)
{
    if (!usable(q, error)) return TELEMETRY_QUEUE_ERROR;
    if (machine_id <= 0 || !valid_timestamp(timestamp) || !valid_uuid(uuid) ||
        payload_json == NULL || *payload_json == '\0')
        return report(error, SQLITE_MISUSE, "Mensagem, UUID ou timestamp inválido");
    if (execute(q, "BEGIN IMMEDIATE", error) != 0) return TELEMETRY_QUEUE_ERROR;
    TelemetryQueueResult duplicate = telemetry_queue_check_duplicate(q, machine_id, timestamp, error);
    if (duplicate == TELEMETRY_QUEUE_ERROR) return abort_transaction(q, error);
    if (duplicate != TELEMETRY_QUEUE_OK) {
        if (execute(q, "COMMIT", error) != 0) return abort_transaction(q, error);
        return duplicate;
    }
    sqlite3_stmt *stmt = prepare(q,
        "INSERT INTO telemetry_queue(machine_id,timestamp,uuid,payload_json) VALUES(?1,?2,?3,?4)", error);
    if (stmt == NULL) return abort_transaction(q, error);
    int rc = sqlite3_bind_int(stmt, 1, machine_id);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 2, timestamp, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 3, uuid, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 4, payload_json, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    if (finish(q, stmt, rc, error) != 0 || execute(q, "COMMIT", error) != 0)
        return abort_transaction(q, error);
    return TELEMETRY_QUEUE_OK;
}

void telemetry_queue_item_clear(TelemetryQueueItem *item)
{
    if (item == NULL) return;
    free(item->payload_json);
    free(item->last_error);
    memset(item, 0, sizeof *item);
}

static int fixed_text(sqlite3_stmt *stmt, int column, char *out, size_t size)
{
    const unsigned char *text = sqlite3_column_text(stmt, column);
    if (text == NULL) return -1;
    int length = sqlite3_column_bytes(stmt, column);
    if (length < 0 || (size_t)length >= size || memchr(text, '\0', (size_t)length) != NULL) return -1;
    memcpy(out, text, (size_t)length);
    out[length] = '\0';
    return 0;
}

static char *allocated_text(sqlite3_stmt *stmt, int column)
{
    const unsigned char *text = sqlite3_column_text(stmt, column);
    if (text == NULL) return NULL;
    size_t length = (size_t)sqlite3_column_bytes(stmt, column);
    char *copy = malloc(length + 1);
    if (copy != NULL) memcpy(copy, text, length + 1);
    return copy;
}

TelemetryQueueResult telemetry_queue_peek(TelemetryQueue *q, TelemetryQueueItem *out,
                                         TelemetryQueueError *error)
{
    if (!usable(q, error)) return TELEMETRY_QUEUE_ERROR;
    if (out == NULL || out->payload_json != NULL || out->last_error != NULL)
        return report(error, SQLITE_MISUSE, "Saída deve estar inicializada e sem item anterior");
    sqlite3_stmt *stmt = prepare(q,
        "SELECT id,machine_id,timestamp,uuid,payload_json,status,attempt_count,last_error,created_at "
        "FROM telemetry_queue WHERE status IN ('pending','inflight') ORDER BY id LIMIT 1", error);
    if (stmt == NULL) return TELEMETRY_QUEUE_ERROR;
    int rc = sqlite3_step(stmt);
    TelemetryQueueItem item = {0};
    TelemetryQueueResult result = TELEMETRY_QUEUE_ERROR;
    if (rc == SQLITE_DONE) result = TELEMETRY_QUEUE_EMPTY;
    else if (rc == SQLITE_ROW) {
        item.id = sqlite3_column_int64(stmt, 0);
        item.machine_id = sqlite3_column_int(stmt, 1);
        item.attempt_count = sqlite3_column_int64(stmt, 6);
        char status[16];
        if (fixed_text(stmt, 2, item.timestamp, sizeof item.timestamp) != 0 ||
            fixed_text(stmt, 3, item.uuid, sizeof item.uuid) != 0 ||
            fixed_text(stmt, 5, status, sizeof status) != 0 ||
            fixed_text(stmt, 8, item.created_at, sizeof item.created_at) != 0) {
            report(error, SQLITE_CORRUPT, "Texto inválido armazenado na fila");
        } else {
            item.status = strcmp(status, "pending") == 0 ? TELEMETRY_QUEUE_PENDING : TELEMETRY_QUEUE_INFLIGHT;
            item.payload_json = allocated_text(stmt, 4);
            if (sqlite3_column_type(stmt, 7) != SQLITE_NULL) item.last_error = allocated_text(stmt, 7);
            if (item.payload_json == NULL ||
                (sqlite3_column_type(stmt, 7) != SQLITE_NULL && item.last_error == NULL))
                report(error, SQLITE_NOMEM, "Memória indisponível para item da fila");
            else result = TELEMETRY_QUEUE_OK;
        }
    }
    if (finish(q, stmt, rc, error) != 0) result = TELEMETRY_QUEUE_ERROR;
    if (result == TELEMETRY_QUEUE_OK) *out = item;
    else telemetry_queue_item_clear(&item);
    return result;
}

static TelemetryQueueResult transition(TelemetryQueue *q, int64_t id, const char *message,
                                        const char *sql, TelemetryQueueError *error)
{
    if (!usable(q, error)) return TELEMETRY_QUEUE_ERROR;
    if (id <= 0) return report(error, SQLITE_MISUSE, "ID da fila inválido");
    sqlite3_stmt *stmt = prepare(q, sql, error);
    if (stmt == NULL) return TELEMETRY_QUEUE_ERROR;
    int rc = sqlite3_bind_int64(stmt, 1, id);
    if (message != NULL && rc == SQLITE_OK) rc = sqlite3_bind_text(stmt, 2, message, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    int changed = sqlite3_changes(q->db);
    if (finish(q, stmt, rc, error) != 0) return TELEMETRY_QUEUE_ERROR;
    if (changed != 1) return report(error, SQLITE_CONSTRAINT, "Item ausente, estado inválido ou contador esgotado");
    return TELEMETRY_QUEUE_OK;
}

TelemetryQueueResult telemetry_queue_start_attempt(TelemetryQueue *q, int64_t id, TelemetryQueueError *error)
{
    return transition(q, id, NULL,
        "UPDATE telemetry_queue SET status='inflight',attempt_count=attempt_count+1 "
        "WHERE id=?1 AND status='pending' AND attempt_count<9223372036854775807", error);
}

TelemetryQueueResult telemetry_queue_retry(TelemetryQueue *q, int64_t id,
    const char *last_error, TelemetryQueueError *error)
{
    if (last_error == NULL || *last_error == '\0') return report(error, SQLITE_MISUSE, "Erro transitório obrigatório");
    return transition(q, id, last_error,
        "UPDATE telemetry_queue SET status='pending',last_error=?2 WHERE id=?1 AND status='inflight'", error);
}

TelemetryQueueResult telemetry_queue_block(TelemetryQueue *q, int64_t id,
    const char *last_error, TelemetryQueueError *error)
{
    if (last_error == NULL || *last_error == '\0') return report(error, SQLITE_MISUSE, "Motivo do bloqueio obrigatório");
    return transition(q, id, last_error,
        "UPDATE telemetry_queue SET status='blocked',last_error=?2 WHERE id=?1 AND status IN ('pending','inflight')", error);
}

TelemetryQueueResult telemetry_queue_confirm(TelemetryQueue *q, int64_t id, TelemetryQueueError *error)
{
    if (!usable(q, error)) return TELEMETRY_QUEUE_ERROR;
    if (id <= 0) return report(error, SQLITE_MISUSE, "ID da fila inválido");
    if (execute(q, "BEGIN IMMEDIATE", error) != 0) return TELEMETRY_QUEUE_ERROR;
    sqlite3_stmt *stmt = prepare(q,
        "INSERT INTO delivered_cursor(machine_id,last_timestamp) "
        "SELECT machine_id,timestamp FROM telemetry_queue WHERE id=?1 AND status='inflight' "
        "ON CONFLICT(machine_id) DO UPDATE SET last_timestamp=excluded.last_timestamp "
        "WHERE excluded.last_timestamp>delivered_cursor.last_timestamp", error);
    if (stmt == NULL) return abort_transaction(q, error);
    int rc = sqlite3_bind_int64(stmt, 1, id);
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    if (finish(q, stmt, rc, error) != 0) return abort_transaction(q, error);
    stmt = prepare(q, "DELETE FROM telemetry_queue WHERE id=?1 AND status='inflight'", error);
    if (stmt == NULL) return abort_transaction(q, error);
    rc = sqlite3_bind_int64(stmt, 1, id);
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    int changed = sqlite3_changes(q->db);
    if (finish(q, stmt, rc, error) != 0) return abort_transaction(q, error);
    if (changed != 1) {
        report(error, SQLITE_CONSTRAINT, "Confirmação exige item inflight existente");
        return abort_transaction(q, error);
    }
    if (execute(q, "COMMIT", error) != 0) return abort_transaction(q, error);
    return TELEMETRY_QUEUE_OK;
}

TelemetryQueueResult telemetry_queue_count(TelemetryQueue *q, int64_t *out, TelemetryQueueError *error)
{
    if (!usable(q, error)) return TELEMETRY_QUEUE_ERROR;
    if (out == NULL) return report(error, SQLITE_MISUSE, "Saída de contagem nula");
    sqlite3_stmt *stmt = prepare(q, "SELECT count(*) FROM telemetry_queue", error);
    if (stmt == NULL) return TELEMETRY_QUEUE_ERROR;
    int rc = sqlite3_step(stmt);
    int64_t count = rc == SQLITE_ROW ? sqlite3_column_int64(stmt, 0) : -1;
    if (finish(q, stmt, rc, error) != 0) return TELEMETRY_QUEUE_ERROR;
    if (count < 0) return report(error, SQLITE_CORRUPT, "Contagem sem resultado");
    *out = count;
    return TELEMETRY_QUEUE_OK;
}

TelemetryQueueResult telemetry_queue_cursor(TelemetryQueue *q, int machine_id,
    char out[TELEMETRY_QUEUE_TIMESTAMP_SIZE], int *found, TelemetryQueueError *error)
{
    if (!usable(q, error)) return TELEMETRY_QUEUE_ERROR;
    if (machine_id <= 0 || out == NULL || found == NULL)
        return report(error, SQLITE_MISUSE, "Argumentos de cursor inválidos");
    sqlite3_stmt *stmt = prepare(q, "SELECT last_timestamp FROM delivered_cursor WHERE machine_id=?1", error);
    if (stmt == NULL) return TELEMETRY_QUEUE_ERROR;
    int rc = sqlite3_bind_int(stmt, 1, machine_id);
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    char timestamp[TELEMETRY_QUEUE_TIMESTAMP_SIZE] = {0};
    int exists = rc == SQLITE_ROW;
    int valid = !exists || fixed_text(stmt, 0, timestamp, sizeof timestamp) == 0;
    if (finish(q, stmt, rc, error) != 0) return TELEMETRY_QUEUE_ERROR;
    if (!valid) return report(error, SQLITE_CORRUPT, "Timestamp de cursor inválido");
    if (exists) memcpy(out, timestamp, sizeof timestamp);
    *found = exists;
    return TELEMETRY_QUEUE_OK;
}
