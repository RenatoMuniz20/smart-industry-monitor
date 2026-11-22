#!/usr/bin/env python3
"""SIM-09.2: real MQTT reception persists to a temporary SQLite, with delivery waiting for an unavailable Collector."""
from contextlib import closing, contextmanager
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import signal
import socket
import sqlite3
import subprocess
import sys
import tempfile
import threading

from test_cli import GATEWAY, READY, temporary_broker, terminate, wait_for
from test_interscity_gateway import UUID_OK, expected_body, publish, telemetry


def run(args):
    return subprocess.run([GATEWAY, *args], capture_output=True, text=True, timeout=15)


def options(mapping, database, url='http://127.0.0.1:9'):
    return ['--interscity-url', url, '--interscity-map', str(mapping), '--queue-db', str(database)]


def rows(database):
    if not database.exists():
        return []
    with closing(sqlite3.connect(database)) as db:
        db.row_factory = sqlite3.Row
        return [dict(row) for row in db.execute('SELECT * FROM telemetry_queue ORDER BY id')]


def local_tests():
    result = run(['--help'])
    assert result.returncode == 0 and '--queue-db' in result.stdout and result.stderr == ''
    with tempfile.TemporaryDirectory(prefix='sim09-cli-') as directory:
        root = Path(directory)
        mapping, database = root / 'machines.conf', root / 'queue.db'
        mapping.write_text(f'1 {UUID_OK}\n')
        configured = options(mapping, database)
        invalid = [
            ['--queue-db'], ['--queue-db', ''], ['--queue-db', str(database)],
            ['--queue-db', str(database), '--interscity-url', 'http://127.0.0.1:9'],
            ['--queue-db', str(database), '--interscity-map', str(mapping)],
            configured + ['--queue-db', str(database)],
            options(mapping, ''), options(mapping, database, 'ftp://example.org'),
            options(mapping, database, 'http://'), options(mapping, database, 'http://bad host'),
        ]
        for args in invalid:
            result = run(args)
            assert result.returncode != 0 and result.stdout == '', (args, result)
            assert result.stderr.startswith('Argumentos inválidos.\nUso:'), (args, result)
            assert not database.exists()
        # Database opening must fail before any MQTT connection/READY.
        missing = root / 'not-created' / 'queue.db'
        result = run(options(mapping, missing))
        assert result.returncode != 0 and result.stdout == '', result
        assert 'Erro SQLite ao abrir (código ' in result.stderr and 'MQTT' not in result.stderr, result
        assert not missing.parent.exists()
        bad_map = root / 'invalid.conf'
        bad_map.write_text('not a mapping\n')
        result = run(options(bad_map, database))
        assert result.returncode != 0 and 'Erro no mapeamento' in result.stderr, result
        assert not database.exists()
        # If MQTT startup fails after opening SQLite, the process must release DB resources.
        with socket.socket() as unavailable:
            unavailable.bind(('127.0.0.1', 0))
            result = run(options(mapping, database) + ['--port', str(unavailable.getsockname()[1])])
        assert result.returncode != 0 and 'Erro MQTT ao conectar/assinar' in result.stderr, result
        assert 'Fila SQLite habilitada' in result.stdout and READY not in result.stdout, result
        assert rows(database) == []
        assert not Path(str(database) + '-wal').exists()
        assert not Path(str(database) + '-shm').exists()
    print('PASS: fila CLI, combinações, mapa/SQLite antes de MQTT e limpeza na falha de conexão', flush=True)


@contextmanager
def start_gateway(root, label, mapping, database, port, url, http_timeout=1000):
    output, errors = root / f'{label}.out', root / f'{label}.err'
    with output.open('w') as out, errors.open('w') as err:
        process = subprocess.Popen([GATEWAY, '--port', str(port), '--client-id', f'sim09-{label}',
            '--interscity-timeout-ms', str(http_timeout), *options(mapping, database, url)], stdout=out, stderr=err)
        diagnostic = lambda: output.read_text() + errors.read_text()
        try:
            wait_for(process, lambda: READY in output.read_text().splitlines(), diagnostic)
            assert 'Fila SQLite habilitada' in output.read_text(), diagnostic()
            yield process, output, errors, diagnostic
        finally:
            terminate(process)


def stop_gateway(process, output, errors, sig=signal.SIGTERM):
    process.send_signal(sig)
    assert process.wait(timeout=10) == 0, output.read_text() + errors.read_text()
    assert output.read_text().splitlines()[-1] == 'Gateway encerrado por solicitação.'
    assert 'InterSCity:' not in output.read_text()  # no legacy HTTP delivery summary
    assert not Path(f'/proc/{process.pid}').exists()


class HttpTrap(BaseHTTPRequestHandler):
    def do_POST(self):
        target = self.server.queries if self.path.endswith('/collector/resources/data/last') else self.server.observed
        target.append(('POST', self.path))
        self.rfile.read(int(self.headers.get('Content-Length', '0')))
        self.send_response(503)
        self.send_header('Content-Length', '0')
        self.end_headers()

    def do_GET(self):
        self.server.observed.append(('GET', self.path))
        self.send_response(503)
        self.send_header('Content-Length', '0')
        self.end_headers()

    def log_message(self, *_):
        pass


def initialize_database(mapping, database):
    # Initialize through the real module, stopping before a subscription exists.
    with socket.socket() as unavailable:
        unavailable.bind(('127.0.0.1', 0))
        result = run(options(mapping, database) + ['--port', str(unavailable.getsockname()[1])])
    assert result.returncode != 0 and 'Erro MQTT ao conectar/assinar' in result.stderr, result


def reception_rows(database):
    # Machine 99 is a deliberate older ambiguous delivery, not a received sample.
    return [row for row in rows(database) if row['machine_id'] != 99]


def integration_tests():
    http = ThreadingHTTPServer(('127.0.0.1', 0), HttpTrap)
    http.daemon_threads = True
    http.observed = []
    http.queries = []
    thread = threading.Thread(target=http.serve_forever, daemon=True)
    thread.start()
    url = f'http://127.0.0.1:{http.server_address[1]}/interscity_lh'
    try:
        with tempfile.TemporaryDirectory(prefix='sim09-mqtt-queue-') as directory:
            root = Path(directory)
            mapping, database = root / 'machines.conf', root / 'queue.db'
            mapping.write_text(f'1 {UUID_OK}\n')
            initialize_database(mapping, database)
            with closing(sqlite3.connect(database)) as db:
                db.execute("INSERT INTO telemetry_queue(machine_id,timestamp,uuid,payload_json,status,attempt_count) "
                           "VALUES(99,'2023-01-01T00:00:00',?,?,'inflight',1)",
                           (UUID_OK, json.dumps(expected_body('2023-01-01T00:00:00'))))
                db.commit()
            with temporary_broker(root) as (_, port):
                with start_gateway(root, 'first', mapping, database, port, url) as (process, out, err, diagnostic):
                    timestamps = ['2024-01-01 00:00:00', '2024-01-01 00:00:30', '2024-01-01 00:01:00']
                    for i, timestamp in enumerate(timestamps):
                        publish(port, 1, telemetry(1, timestamp=timestamp, current=None if i == 1 else 5.1))
                    wait_for(process, lambda: len(reception_rows(database)) == 3, diagnostic)
                    stored = reception_rows(database)
                    for i, row in enumerate(stored):
                        timestamp = timestamps[i].replace(' ', 'T')
                        assert row['machine_id'] == 1 and row['timestamp'] == timestamp and row['uuid'] == UUID_OK, row
                        assert row['status'] == 'pending' and row['attempt_count'] == 0 and row['last_error'] is None, row
                        assert json.loads(row['payload_json']) == expected_body(timestamp, current=i != 1), row
                    wait_for(process, lambda: len(http.queries) >= 1, diagnostic)
                    assert http.observed == [], http.observed
                    print('PASS A/D: 3 pending, UUID/timestamp/payload corretos, attempt_count=0; reconciliação indisponível; nenhum POST ao Adaptor', flush=True)
                    publish(port, 1, telemetry(1))
                    wait_for(process, lambda: 'já enfileirada' in out.read_text(), diagnostic)
                    assert len(reception_rows(database)) == 3
                    print('PASS B: duplicata pendente ignorada', flush=True)
                    publish(port, 1, b'{}')
                    wait_for(process, lambda: 'Telemetria inválida:' in err.read_text(), diagnostic)
                    publish(port, 1, telemetry(1, timestamp='2024-01-01 00:02:00'))
                    wait_for(process, lambda: len(reception_rows(database)) == 4, diagnostic)
                    inserted = reception_rows(database)[-1]
                    assert inserted['timestamp'] == '2024-01-01T00:02:00' and inserted['machine_id'] == 1
                    assert json.loads(inserted['payload_json']) == expected_body('2024-01-01T00:02:00')
                    publish(port, 2, telemetry(2))
                    wait_for(process, lambda: 'machine_id=2 sem UUID' in err.read_text(), diagnostic)
                    publish(port, 1, telemetry(1, timestamp='not-a-timestamp'))
                    wait_for(process, lambda: 'machine_id=1 não persistida:' in err.read_text(), diagnostic)
                    assert len(reception_rows(database)) == 4 and all(row['machine_id'] == 1 for row in reception_rows(database))
                    assert http.observed == []
                    stop_gateway(process, out, err)
                    assert '4 persistidas, 1 duplicatas ignoradas, 1 sem mapeamento, 1 não preparadas.' in out.read_text()
                    snapshot = reception_rows(database)
                    print('PASS E/F: JSON inválido, ID sem mapa e falha de payload descartados; válida seguinte persistida', flush=True)

                with socket.socket() as closed_http:
                    closed_http.bind(('127.0.0.1', 0))
                    unavailable_url = f'http://127.0.0.1:{closed_http.getsockname()[1]}'
                    with start_gateway(root, 'restart', mapping, database, port, unavailable_url) as (process, out, err, diagnostic):
                        assert reception_rows(database) == snapshot
                        publish(port, 1, telemetry(1))
                        wait_for(process, lambda: 'já enfileirada' in out.read_text(), diagnostic)
                        publish(port, 1, telemetry(1, timestamp='2024-01-01T00:03:00'))
                        wait_for(process, lambda: len(reception_rows(database)) == 5, diagnostic)
                        assert 'inflight; início da reconciliação' in err.read_text() and http.observed == []
                        stop_gateway(process, out, err, signal.SIGINT)
                print('PASS C/D: reinicialização preserva fila/deduplicação; nova amostra sem InterSCity disponível', flush=True)

                # Seed a confirmed cursor as a test fixture, not via HTTP/worker.
                with closing(sqlite3.connect(database)) as db:
                    db.execute("INSERT INTO delivered_cursor VALUES(1,'2024-01-01T00:00:00')")
                    db.execute("DELETE FROM telemetry_queue WHERE timestamp='2024-01-01T00:00:00'")
                    db.commit()
                with start_gateway(root, 'failure', mapping, database, port, url) as (process, out, err, diagnostic):
                    publish(port, 1, telemetry(1))
                    wait_for(process, lambda: 'já confirmada' in out.read_text(), diagnostic)
                    assert len(reception_rows(database)) == 4
                    with closing(sqlite3.connect(database)) as db:
                        db.execute("CREATE TRIGGER reject_queue BEFORE INSERT ON telemetry_queue "
                                   "BEGIN SELECT RAISE(ABORT,'injected persistence failure'); END")
                        db.commit()
                    publish(port, 1, telemetry(1, timestamp='2024-01-01 00:04:00'))
                    assert process.wait(timeout=10) != 0, diagnostic()
                    assert 'Erro SQLite ao persistir (código ' in err.read_text() and 'injected persistence failure' in err.read_text()
                    assert 'ts 2024-01-01T00:04:00, pending' not in out.read_text()
                    assert 'Gateway encerrado por solicitação.' not in out.read_text()
                    assert 'InterSCity:' not in out.read_text()
                    assert len(reception_rows(database)) == 4
                    assert all(row['status'] == 'pending' and row['attempt_count'] == 0 for row in reception_rows(database))
                    assert not Path(f'/proc/{process.pid}').exists()
                assert http.observed == [], http.observed
                barrier = [row for row in rows(database) if row['machine_id'] == 99]
                assert len(barrier) == 1 and barrier[0]['status'] == 'inflight' and barrier[0]['attempt_count'] == 1
                print('PASS: duplicata confirmada; falha SQLite fatal sem falso sucesso, sem POST durante falha de consulta', flush=True)
            assert not Path(str(database) + '-wal').exists()
            assert not Path(str(database) + '-shm').exists()
    finally:
        http.shutdown()
        http.server_close()
        thread.join(timeout=5)
        assert not thread.is_alive()
    print('Todos os testes MQTT da fila durável passaram.')


if __name__ == '__main__':
    if sys.argv[1:] == ['--mqtt']:
        integration_tests()
    else:
        assert len(sys.argv) == 1
        local_tests()
