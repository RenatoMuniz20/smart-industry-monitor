#!/usr/bin/env python3
"""SIM-09.3/09.4: real Gateway + isolated Mosquitto/SQLite + event-controlled HTTP."""
from contextlib import closing, contextmanager
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import signal
import socket
import sqlite3
import tempfile
import threading
import time

from test_cli import temporary_broker, wait_for
from test_durable_queue import (initialize_database, rows, start_gateway, stop_gateway)
from test_interscity_gateway import UUID_OK, expected_body, publish, telemetry


class HttpHandler(BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        with self.server.lock:
            if self.path == '/collector/resources/data/last':
                assert body == {'uuids': [UUID_OK]}, body
                index = len(self.server.queries)
                status, gate, response = self.server.collector(index, body)
                self.server.queries.append((self.path, body, time.monotonic(), status))
            else:
                assert self.path == f'/adaptor/resources/{UUID_OK}/data', self.path
                index = len(self.server.requests)
                status, gate = self.server.plan(index, body)
                self.server.requests.append((self.path, body, time.monotonic(), status))
                response = b''
        if status == 0:  # Real connection loss, not an HTTP refusal.
            self.close_connection = True
            self.connection.shutdown(socket.SHUT_RDWR)
            self.connection.close()
            return
        if not isinstance(response, bytes):
            response = json.dumps(response).encode()
        if gate is not None and not gate.wait(15):
            self.server.errors.append('HTTP test gate timed out')
        try:
            self.send_response(status)
            self.send_header('Content-Length', str(len(response)))
            self.send_header('Content-Type', 'application/json')
            self.end_headers()
            self.wfile.write(response)
        except (BrokenPipeError, ConnectionResetError):
            pass  # Expected when testing a curl timeout.

    def log_message(self, *_):
        pass


@contextmanager
def http_server(plan, collector=None):
    server = ThreadingHTTPServer(('127.0.0.1', 0), HttpHandler)
    server.daemon_threads = True
    server.lock = threading.Lock()
    server.requests, server.queries, server.errors = [], [], []
    server.collector = collector or (lambda *_: (503, None, {}))
    server.plan = plan
    thread = threading.Thread(target=server.serve_forever)
    thread.start()
    gates = []
    def snapshot():
        with server.lock:
            return list(server.requests)
    def queries():
        with server.lock:
            return list(server.queries)
    try:
        yield f'http://127.0.0.1:{server.server_port}', snapshot, queries, gates
    finally:
        for gate in gates:
            gate.set()
        server.shutdown()
        server.server_close()
        thread.join(5)
        assert not thread.is_alive() and not server.errors, server.errors


def execute(database, sql):
    with closing(sqlite3.connect(database)) as db:
        db.executescript(sql)


def cursor(database):
    with closing(sqlite3.connect(database)) as db:
        return db.execute('SELECT machine_id,last_timestamp FROM delivered_cursor ORDER BY machine_id').fetchall()


def send(port, minute):
    publish(port, 1, telemetry(1, timestamp=f'2024-01-01 00:{minute:02d}:00'))


def collected(timestamp='2024-01-01T00:00:00'):
    return {'resources': [{'id': UUID_OK, 'capabilities': {
        'sim_operating_state': [{'value': 'IDLE', 'date': timestamp + '.000Z'}]}}]}


def seed_inflight(mapping, db, attempts=3):
    initialize_database(mapping, db)
    with closing(sqlite3.connect(db)) as connection:
        connection.execute("INSERT INTO telemetry_queue(machine_id,timestamp,uuid,payload_json,status,attempt_count) "
                           "VALUES(1,'2024-01-01T00:00:00',?,?,'inflight',?)",
                           (UUID_OK, json.dumps(expected_body()), attempts))
        connection.commit()


def recovery_tests(root, mapping, port):
    # A: timeout after storing T0; equality recovers it without a second POST.
    db = root / 'recover-stored.db'
    first = threading.Event()
    with http_server(lambda i, _: (201, first if i == 0 else None),
                     lambda *_: (200, None, collected())) as (url, posts, queries, gates):
        gates.append(first)
        with start_gateway(root, 'recover-stored', mapping, db, port, url, http_timeout=300) as (p, out, err, diag):
            send(port, 0)
            send(port, 1)
            wait_for(p, lambda: len(posts()) == 2 and len(queries()) == 1 and not rows(db), diag)
            assert [r[1] for r in posts()] == [expected_body(), expected_body('2024-01-01T00:01:00')]
            assert queries()[0][2] - posts()[0][2] >= 2.15  # 300 ms timeout + initial 2 s window
            assert cursor(db) == [(1, '2024-01-01T00:01:00')]
            stop_gateway(p, out, err)
            assert '1 entregues' in out.read_text() and '1 recuperadas pelo Data Collector' in out.read_text()
    print('PASS recuperação A: timeout armazenado, 1 consulta, sem POST duplicado; fila prossegue', flush=True)

    # B/C: absence and an older timestamp both need TWO consultations, and the
    # retried row must have attempt_count=2 before its second POST can complete.
    for label, answer in [('missing', (404, None, {'error': 'Resource not found'})),
                          ('older', (200, None, collected('2023-12-31T23:59:00')))]:
        db = root / f'recover-{label}.db'
        first, second = threading.Event(), threading.Event()
        with http_server(lambda i, _: (201, first if i == 0 else second),
                         lambda *_: answer) as (url, posts, queries, gates):
            gates.extend([first, second])
            with start_gateway(root, label, mapping, db, port, url, http_timeout=1000) as (p, out, err, diag):
                send(port, 0)
                wait_for(p, lambda: len(posts()) == 2, diag)
                assert len(queries()) == 2 and queries()[1][2] - queries()[0][2] >= 1.95
                sample = rows(db)
                assert len(sample) == 1 and sample[0]['status'] == 'inflight' and sample[0]['attempt_count'] == 2
                assert cursor(db) == [] and [r[1] for r in posts()] == [expected_body()] * 2
                second.set()
                wait_for(p, lambda: not rows(db), diag)
                stop_gateway(p, out, err)
                assert 'nova tentativa autorizada' in err.read_text()
                assert '1 entregues' in out.read_text() and '0 recuperadas pelo Data Collector' in out.read_text()
        print(f'PASS recuperação B ({label}): duas consultas espaçadas, tentativa 2, HTTP 201 remove', flush=True)

    # C: connection loss at the Adaptor and Collector unavailable for three
    # queries. MQTT keeps persisting. Recovery needs NO Gateway restart.
    db = root / 'outage.db'
    restored = threading.Event()
    with http_server(lambda *_: (201 if restored.is_set() else 0, None),
                     lambda *_: (200, None, {'resources': []}) if restored.is_set() else (503, None, {})) as (url, posts, queries, _):
        with start_gateway(root, 'outage', mapping, db, port, url) as (p, out, err, diag):
            pid = p.pid
            send(port, 0)
            wait_for(p, lambda: 'resultado ambíguo' in err.read_text(), diag)
            for i in range(1, 5):
                send(port, i)
            wait_for(p, lambda: len(rows(db)) == 5 and len(queries()) >= 3, diag)
            assert p.poll() is None and len(posts()) == 1 and cursor(db) == []
            assert [r['status'] for r in rows(db)] == ['inflight'] + ['pending'] * 4
            failures = queries()
            assert [q[3] for q in failures] == [503] * len(failures)
            assert failures[1][2] - failures[0][2] >= .95 and failures[2][2] - failures[1][2] >= 1.95
            restored.set()
            wait_for(p, lambda: len(posts()) == 6 and not rows(db), diag)
            assert p.pid == pid and p.poll() is None
            assert [r[1] for r in posts()] == [expected_body()] + [expected_body(f'2024-01-01T00:{i:02d}:00') for i in range(5)]
            assert cursor(db) == [(1, '2024-01-01T00:04:00')]
            stop_gateway(p, out, err)
            assert '5 entregues' in out.read_text()
            print(f'EVIDÊNCIA indisponibilidade: PID={pid} mantido; 5 registros persistidos durante falha; '
                  f'{len(failures)} consultas HTTP 503; recuperação com 5 entregas e fila vazia.', flush=True)
    print('PASS recuperação C: perda real de conexão, backoff e retomada automática sem reiniciar', flush=True)

    # D: create inflight by a real timeout, shut down, and reopen the same DB.
    db = root / 'restart-reconcile.db'
    first = threading.Event()
    with http_server(lambda i, _: (201, first if i == 0 else None),
                     lambda *_: (200, None, collected())) as (url, posts, queries, gates):
        gates.append(first)
        with start_gateway(root, 'before-restart', mapping, db, port, url, http_timeout=300) as (p, out, err, diag):
            send(port, 0)
            wait_for(p, lambda: 'resultado ambíguo' in err.read_text(), diag)
            stop_gateway(p, out, err)
            assert rows(db)[0]['status'] == 'inflight' and rows(db)[0]['attempt_count'] == 1
            assert len(posts()) == 1 and queries() == []
        with start_gateway(root, 'after-restart', mapping, db, port, url) as (p, out, err, diag):
            send(port, 1)
            wait_for(p, lambda: len(queries()) == 1 and len(posts()) == 2 and not rows(db), diag)
            assert queries()[0][2] < posts()[1][2]
            assert [r[1] for r in posts()] == [expected_body(), expected_body('2024-01-01T00:01:00')]
            stop_gateway(p, out, err)
            assert '1 recuperadas pelo Data Collector' in out.read_text()
    print('PASS recuperação D: reinício preserva inflight/tentativa; consulta antes de qualquer novo POST', flush=True)

    # E: a newer timestamp MUST NOT confirm/delete T0. Hold the retry response
    # so that state, attempts, and lack of delivered cursor can be inspected.
    db = root / 'newer.db'
    seed_inflight(mapping, db)
    gate = threading.Event()
    with http_server(lambda *_: (201, gate),
                     lambda *_: (200, None, collected('2024-01-01T00:02:00'))) as (url, posts, queries, gates):
        gates.append(gate)
        with start_gateway(root, 'newer', mapping, db, port, url, http_timeout=3000) as (p, out, err, diag):
            wait_for(p, lambda: len(posts()) == 1, diag)
            assert len(queries()) == 1 and rows(db)[0]['attempt_count'] == 4
            assert rows(db)[0]['status'] == 'inflight' and cursor(db) == []
            assert posts()[0][1] == expected_body() and 'risco de duplicação' in err.read_text()
            assert 'recuperada pelo Data Collector' not in out.read_text()
            gate.set()
            wait_for(p, lambda: not rows(db), diag)
            stop_gateway(p, out, err)
            assert '1 entregues' in out.read_text() and '0 recuperadas pelo Data Collector' in out.read_text()
    print('PASS recuperação E: timestamp posterior não confirma; reenvio explícito com risco de duplicação', flush=True)

    # F: Collector 503 never authorizes POST; restore a matching timestamp.
    db = root / 'collector-outage.db'
    seed_inflight(mapping, db)
    restored = threading.Event()
    with http_server(lambda *_: (201, None),
                     lambda *_: (200, None, collected()) if restored.is_set() else (503, None, {})) as (url, posts, queries, _):
        with start_gateway(root, 'collector-outage', mapping, db, port, url) as (p, out, err, diag):
            send(port, 1)
            wait_for(p, lambda: len(queries()) >= 2, diag)
            assert posts() == [] and p.poll() is None
            assert [r['attempt_count'] for r in rows(db)] == [3, 0]
            assert 'reconciliação indisponível' in err.read_text()
            restored.set()
            wait_for(p, lambda: not rows(db) and len(posts()) == 1, diag)
            stop_gateway(p, out, err)
            assert '1 recuperadas pelo Data Collector' in out.read_text()
    print('PASS recuperação F: consultas 503 mantêm inflight e MQTT; recuperação automática sem reenvio do confirmado', flush=True)

    # G: malformed and incompatible Collector responses are NOT absence.
    invalid_date = collected()
    invalid_date['resources'][0]['capabilities']['sim_current'] = [{'date': 42}]
    invalid_series = collected()
    invalid_series['resources'][0]['capabilities']['sim_current'] = {}
    answers = [(200, b'{'), (200, {}), (200, invalid_date), (200, invalid_series),
               (200, json.dumps(collected()).encode() + b'\x00'),
               (404, b'not JSON: Resource not found')]
    wrong_uuid = collected()
    wrong_uuid['resources'][0]['id'] = '00000000-0000-4000-8000-000000000400'
    missing_id = collected()
    del missing_id['resources'][0]['id']
    invalid_id = collected()
    invalid_id['resources'][0]['id'] = 42
    invalid_suffix = collected()
    invalid_suffix['resources'][0]['capabilities']['sim_operating_state'][0]['date'] = '2024-01-01T00:00:00XINVALID'
    answers.extend((200, body) for body in (wrong_uuid, missing_id, invalid_id, invalid_suffix))
    for index, (status, body) in enumerate(answers):
        db = root / f'invalid-collector-{index}.db'
        seed_inflight(mapping, db)
        restored = threading.Event()
        valid_response = collected()
        if index % 2 == 0:  # Also exercise UTC without fractions in the worker.
            valid_response['resources'][0]['capabilities']['sim_operating_state'][0]['date'] = '2024-01-01T00:00:00Z'
        with http_server(lambda *_: (201, None),
                         lambda *_: (200, None, valid_response) if restored.is_set() else (status, None, body)) as (url, posts, queries, _):
            with start_gateway(root, f'invalid-collector-{index}', mapping, db, port, url) as (p, out, err, diag):
                wait_for(p, lambda: 'reconciliação indisponível' in err.read_text(), diag)
                assert rows(db)[0]['status'] == 'inflight' and rows(db)[0]['attempt_count'] == 3
                assert posts() == [] and cursor(db) == []
                restored.set()
                wait_for(p, lambda: not rows(db), diag)
                stop_gateway(p, out, err)
                assert posts() == [] and len(queries()) >= 2
                assert cursor(db) == [(1, '2024-01-01T00:00:00')]
                assert '1 recuperadas pelo Data Collector' in out.read_text()
    print(f'PASS recuperação G: {len(answers)} respostas malformadas/incompatíveis preservam inflight, sem reenvio; recuperação posterior', flush=True)

    # Eventual consistency: fail between two absences, then match. The failed
    # query must reset absence evidence, avoiding an unauthorized second POST.
    db = root / 'eventual.db'
    seed_inflight(mapping, db)
    answers = [(200, None, {'resources': []}), (503, None, {}),
               (200, None, {'resources': []}), (200, None, collected())]
    with http_server(lambda *_: (201, None),
                     lambda i, _: answers[min(i, len(answers) - 1)]) as (url, posts, queries, _):
        with start_gateway(root, 'eventual', mapping, db, port, url) as (p, out, err, diag):
            wait_for(p, lambda: not rows(db), diag)
            assert len(queries()) == 4 and posts() == []
            stop_gateway(p, out, err)
            assert '1 recuperadas pelo Data Collector' in out.read_text()
    print('PASS consistência: falha entre ausências reinicia evidência; confirmação posterior evita POST', flush=True)

    # H: shutdown during a slow Collector query, no retry/POST after stop.
    db = root / 'term-reconcile.db'
    seed_inflight(mapping, db)
    gate = threading.Event()
    with http_server(lambda *_: (201, None),
                     lambda *_: (200, gate, collected())) as (url, posts, queries, gates):
        gates.append(gate)
        with start_gateway(root, 'term-reconcile', mapping, db, port, url, http_timeout=700) as (p, out, err, diag):
            wait_for(p, lambda: len(queries()) == 1, diag)
            stop_gateway(p, out, err)
            assert posts() == [] and len(queries()) == 1
            assert rows(db)[0]['status'] == 'inflight' and rows(db)[0]['attempt_count'] == 3
            assert cursor(db) == []
    print('PASS recuperação H: SIGTERM durante consulta lenta, timeout/join sem deadlock, inflight preservado', flush=True)

    # SQLite rollback after recovery is just as fatal as after HTTP201.
    db = root / 'confirm-recovery-failure.db'
    seed_inflight(mapping, db)
    execute(db, "CREATE TRIGGER fail_recovery BEFORE DELETE ON telemetry_queue "
                "BEGIN SELECT RAISE(ABORT,'injected recovery confirmation failure'); END;")
    with http_server(lambda *_: (201, None), lambda *_: (200, None, collected())) as (url, posts, queries, _):
        with start_gateway(root, 'confirm-recovery-failure', mapping, db, port, url) as (p, out, err, diag):
            assert p.wait(10) != 0, diag()
            assert rows(db)[0]['status'] == 'inflight' and cursor(db) == [] and posts() == []
            assert 'erro fatal ao confirmar SQLite após consulta' in err.read_text()
            assert 'recuperada pelo Data Collector (timestamp igual;' not in out.read_text()
            assert '0 recuperadas pelo Data Collector' in out.read_text()
    print('PASS rollback: falha SQLite após consulta é fatal; sem cursor/remoção/recuperação falsos', flush=True)


def main():
    with tempfile.TemporaryDirectory(prefix='sim09-worker-') as directory:
        root = Path(directory)
        mapping = root / 'machines.conf'
        mapping.write_text(f'1 {UUID_OK}\n')
        with temporary_broker(root) as (_, port):
            # A: stored payload and UUID, FIFO, persistent confirmation/cursor.
            db = root / 'success.db'
            with http_server(lambda *_: (201, None)) as (url, requests, queries, _):
                with start_gateway(root, 'success', mapping, db, port, url) as (p, out, err, diag):
                    for i in range(3):
                        send(port, i)
                    wait_for(p, lambda: len(requests()) == 3 and not rows(db) and bool(cursor(db)), diag)
                    observed = requests()
                    assert [r[0] for r in observed] == [f'/adaptor/resources/{UUID_OK}/data'] * 3
                    assert [r[1] for r in observed] == [expected_body(f'2024-01-01T00:{i:02d}:00') for i in range(3)]
                    assert cursor(db) == [(1, '2024-01-01T00:02:00')]
                    # Delivered dedupe, distinct from pending duplicate coverage.
                    send(port, 0)
                    wait_for(p, lambda: 'já confirmada' in out.read_text(), diag)
                    stop_gateway(p, out, err)
                    assert len(requests()) == 3 and '3 entregues' in out.read_text()
                    assert err.read_text() == '', diag()
            print('PASS A: 3 POSTs FIFO/payload/UUID, fila vazia, cursor e deduplicação confirmada', flush=True)

            # B: two transient failures (503 and 429), 1s/2s backoff; success resets it.
            db = root / 'retry.db'
            statuses = [503, 429, 201, 503, 201]
            with http_server(lambda i, _: (statuses[i] if i < len(statuses) else 400, None)) as (url, requests, queries, _):
                with start_gateway(root, 'retry', mapping, db, port, url) as (p, out, err, diag):
                    send(port, 0)
                    wait_for(p, lambda: 'nova tentativa em 1000 ms' in err.read_text(), diag)
                    sample = rows(db)
                    assert len(sample) == 1 and sample[0]['status'] == 'pending' and sample[0]['attempt_count'] == 1
                    assert sample[0]['last_error'] == 'HTTP 503'
                    wait_for(p, lambda: len(requests()) == 3 and not rows(db), diag)
                    send(port, 1)
                    wait_for(p, lambda: len(requests()) == 5 and not rows(db), diag)
                    observed = requests()
                    assert observed[1][2] - observed[0][2] >= .95
                    assert observed[2][2] - observed[1][2] >= 1.95
                    assert observed[4][2] - observed[3][2] >= .95
                    assert err.read_text().count('nova tentativa em 1000 ms') == 2, diag()
                    assert err.read_text().count('nova tentativa em 2000 ms') == 1, diag()
                    assert [r[1] for r in observed[:3]] == [expected_body()] * 3
                    stop_gateway(p, out, err)
                    assert '2 entregues, 3 retentativas' in out.read_text()
            print('PASS B: 503/429 preservam pending, backoff 1/2s e reset após entrega', flush=True)

            # C: definitive refusal stays blocked; later eligible record is sent.
            db = root / 'blocked.db'
            with http_server(lambda i, _: (400 if i == 0 else 201, None)) as (url, requests, queries, _):
                with start_gateway(root, 'blocked', mapping, db, port, url) as (p, out, err, diag):
                    send(port, 0)
                    send(port, 1)
                    wait_for(p, lambda: len(requests()) == 2 and len(rows(db)) == 1 and bool(cursor(db)), diag)
                    sample = rows(db)[0]
                    assert sample['status'] == 'blocked' and sample['last_error'] == 'HTTP 400' and sample['attempt_count'] == 1
                    assert cursor(db) == [(1, '2024-01-01T00:01:00')]
                    stop_gateway(p, out, err)
                    assert '1 entregues, 0 retentativas, 1 bloqueadas' in out.read_text()
            print('PASS C: HTTP 400 bloqueado e preservado; próxima mensagem entregue', flush=True)

            # D/F: HTTP held by an event; reception persists independently. A
            # timeout suspends delivery and leaves all later messages pending.
            db = root / 'ambiguous.db'
            gate = threading.Event()
            with http_server(lambda *_: (201, gate)) as (url, requests, queries, gates):
                gates.append(gate)
                with start_gateway(root, 'ambiguous', mapping, db, port, url, http_timeout=3000) as (p, out, err, diag):
                    send(port, 0)
                    wait_for(p, lambda: len(requests()) == 1, diag)
                    for i in range(1, 4):
                        send(port, i)
                    wait_for(p, lambda: len(rows(db)) == 4, diag)
                    assert not gate.is_set() and 'aguarda reconciliação' not in err.read_text(), diag()
                    assert [r['status'] for r in rows(db)] == ['inflight', 'pending', 'pending', 'pending']
                    wait_for(p, lambda: 'resultado ambíguo' in err.read_text(), diag)
                    send(port, 4)
                    wait_for(p, lambda: len(rows(db)) == 5, diag)
                    assert [r['attempt_count'] for r in rows(db)] == [1, 0, 0, 0, 0]
                    assert cursor(db) == [] and len(requests()) == 1
                    stop_gateway(p, out, err)
                    assert len(requests()) == 1 and '0 entregues' in out.read_text()
            print('PASS D/F: MQTT persiste durante HTTP lento; timeout mantém inflight sem reenvio', flush=True)

            # E: reopening the same ambiguous DB does not POST any row, but MQTT
            # continues; collector is unavailable and must not permit a POST.
            with http_server(lambda *_: (201, None)) as (url, requests, queries, _):
                with start_gateway(root, 'inflight', mapping, db, port, url) as (p, out, err, diag):
                    wait_for(p, lambda: 'inflight; início da reconciliação' in err.read_text(), diag)
                    wait_for(p, lambda: len(queries()) >= 1, diag)
                    send(port, 5)
                    wait_for(p, lambda: len(rows(db)) == 6, diag)
                    stop_gateway(p, out, err)
                    assert requests() == [] and err.read_text().count('início da reconciliação') == 1
                    assert [r['status'] for r in rows(db)] == ['inflight'] + ['pending'] * 5
            print('PASS E: reinicialização consulta Collector indisponível; sem POST; MQTT continua', flush=True)

            # G1: SIGTERM during HTTP201 waits for the response, then confirms.
            db = root / 'term-success.db'
            gate = threading.Event()
            with http_server(lambda *_: (201, gate)) as (url, requests, queries, gates):
                gates.append(gate)
                with start_gateway(root, 'term-success', mapping, db, port, url, http_timeout=2000) as (p, out, err, diag):
                    send(port, 0)
                    wait_for(p, lambda: len(requests()) == 1, diag)
                    p.send_signal(signal.SIGTERM)
                    gate.set()
                    assert p.wait(5) == 0, diag()
                    assert rows(db) == [] and cursor(db) == [(1, '2024-01-01T00:00:00')]
                    assert '1 entregues' in out.read_text() and 'Gateway encerrado por solicitação.' in out.read_text()
            # G2: SIGTERM during an uncompleted HTTP waits only for its timeout.
            db = root / 'term-timeout.db'
            gate = threading.Event()
            with http_server(lambda *_: (201, gate)) as (url, requests, queries, gates):
                gates.append(gate)
                with start_gateway(root, 'term-timeout', mapping, db, port, url, http_timeout=1000) as (p, out, err, diag):
                    send(port, 0)
                    wait_for(p, lambda: len(requests()) == 1, diag)
                    send(port, 1)
                    wait_for(p, lambda: len(rows(db)) == 2, diag)
                    assert 'resultado ambíguo' not in err.read_text(), diag()
                    stop_gateway(p, out, err)
                    assert len(requests()) == 1 and cursor(db) == []
                    assert [r['status'] for r in rows(db)] == ['inflight', 'pending']
            # Backoff is interruptible rather than waiting until its deadline.
            db = root / 'term-backoff.db'
            with http_server(lambda *_: (503, None)) as (url, requests, queries, _):
                with start_gateway(root, 'term-backoff', mapping, db, port, url) as (p, out, err, diag):
                    send(port, 0)
                    wait_for(p, lambda: 'nova tentativa em 2000 ms' in err.read_text(), diag)
                    stop_gateway(p, out, err)
                    assert rows(db)[0]['status'] == 'pending' and len(requests()) == 2
            print('PASS G: SIGTERM durante 201, timeout e backoff; join e estados consistentes', flush=True)

            # H: trigger aborts DELETE after cursor upsert; transaction must roll
            # back both, leave inflight and stop the process with no false delivery.
            db = root / 'confirm-failure.db'
            with http_server(lambda *_: (201, None)) as (url, requests, queries, _):
                with start_gateway(root, 'confirm-failure', mapping, db, port, url) as (p, out, err, diag):
                    execute(db, "CREATE TRIGGER fail_confirm BEFORE DELETE ON telemetry_queue "
                                "BEGIN SELECT RAISE(ABORT,'injected confirmation failure'); END;")
                    send(port, 0)
                    assert p.wait(10) != 0, diag()
                    sample = rows(db)
                    assert len(sample) == 1 and sample[0]['status'] == 'inflight' and sample[0]['attempt_count'] == 1
                    assert cursor(db) == [] and len(requests()) == 1
                    assert 'erro fatal ao confirmar SQLite após HTTP 201' in err.read_text()
                    assert 'injected confirmation failure' in err.read_text()
                    assert 'SQLite confirmado' not in out.read_text() and '0 entregues' in out.read_text()
            print('PASS H: falha de confirmação reverte cursor/delete, preserva inflight e encerra com erro', flush=True)
            recovery_tests(root, mapping, port)
        for suffix in ('*-wal', '*-shm'):
            assert not list(root.glob(suffix)), list(root.glob(suffix))
    print('Todos os testes do worker HTTP passaram.', flush=True)


if __name__ == '__main__':
    main()
