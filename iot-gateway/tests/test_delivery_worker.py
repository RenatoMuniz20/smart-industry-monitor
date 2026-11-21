#!/usr/bin/env python3
"""SIM-09.3: real Gateway + isolated Mosquitto/SQLite + event-controlled HTTP."""
from contextlib import closing, contextmanager
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import signal
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
            index = len(self.server.requests)
            status, gate = self.server.plan(index, body)
            self.server.requests.append((self.path, body, time.monotonic(), status))
        if gate is not None and not gate.wait(15):
            self.server.errors.append('HTTP test gate timed out')
        try:
            self.send_response(status)
            self.send_header('Content-Length', '0')
            self.end_headers()
        except (BrokenPipeError, ConnectionResetError):
            pass  # Expected when testing a curl timeout.

    def log_message(self, *_):
        pass


@contextmanager
def http_server(plan):
    server = ThreadingHTTPServer(('127.0.0.1', 0), HttpHandler)
    server.daemon_threads = True
    server.lock = threading.Lock()
    server.requests, server.errors = [], []
    server.plan = plan
    thread = threading.Thread(target=server.serve_forever)
    thread.start()
    gates = []
    def snapshot():
        with server.lock:
            return list(server.requests)
    try:
        yield f'http://127.0.0.1:{server.server_port}', snapshot, gates
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


def main():
    with tempfile.TemporaryDirectory(prefix='sim09-worker-') as directory:
        root = Path(directory)
        mapping = root / 'machines.conf'
        mapping.write_text(f'1 {UUID_OK}\n')
        with temporary_broker(root) as (_, port):
            # A: stored payload and UUID, FIFO, persistent confirmation/cursor.
            db = root / 'success.db'
            with http_server(lambda *_: (201, None)) as (url, requests, _):
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
            with http_server(lambda i, _: (statuses[i] if i < len(statuses) else 400, None)) as (url, requests, _):
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
            with http_server(lambda i, _: (400 if i == 0 else 201, None)) as (url, requests, _):
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
            with http_server(lambda *_: (201, gate)) as (url, requests, gates):
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
            # continues; no collector query is implemented in this substep.
            with http_server(lambda *_: (201, None)) as (url, requests, _):
                with start_gateway(root, 'inflight', mapping, db, port, url) as (p, out, err, diag):
                    wait_for(p, lambda: 'inflight; aguarda reconciliação' in err.read_text(), diag)
                    send(port, 5)
                    wait_for(p, lambda: len(rows(db)) == 6, diag)
                    stop_gateway(p, out, err)
                    assert requests() == [] and err.read_text().count('envios suspensos') == 1
                    assert [r['status'] for r in rows(db)] == ['inflight'] + ['pending'] * 5
            print('PASS E: reinicialização inflight suspende POST, mas novas mensagens são persistidas', flush=True)

            # G1: SIGTERM during HTTP201 waits for the response, then confirms.
            db = root / 'term-success.db'
            gate = threading.Event()
            with http_server(lambda *_: (201, gate)) as (url, requests, gates):
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
            with http_server(lambda *_: (201, gate)) as (url, requests, gates):
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
            with http_server(lambda *_: (503, None)) as (url, requests, _):
                with start_gateway(root, 'term-backoff', mapping, db, port, url) as (p, out, err, diag):
                    send(port, 0)
                    wait_for(p, lambda: 'nova tentativa em 2000 ms' in err.read_text(), diag)
                    stop_gateway(p, out, err)
                    assert rows(db)[0]['status'] == 'pending' and len(requests()) == 2
            print('PASS G: SIGTERM durante 201, timeout e backoff; join e estados consistentes', flush=True)

            # H: trigger aborts DELETE after cursor upsert; transaction must roll
            # back both, leave inflight and stop the process with no false delivery.
            db = root / 'confirm-failure.db'
            with http_server(lambda *_: (201, None)) as (url, requests, _):
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
        for suffix in ('*-wal', '*-shm'):
            assert not list(root.glob(suffix)), list(root.glob(suffix))
    print('Todos os testes do worker HTTP passaram.', flush=True)


if __name__ == '__main__':
    main()
