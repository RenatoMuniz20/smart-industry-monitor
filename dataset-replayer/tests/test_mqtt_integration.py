#!/usr/bin/env python3
"""Local Mosquitto plus controlled protocol peers; no public broker required."""
import concurrent.futures
import csv
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import tempfile
import time

from test_ten_machines import ten_machines, unavailable_broker

CLIENT = './build/test_mqtt_publisher'


def listener():
    sock = socket.socket()
    sock.bind(('127.0.0.1', 0))
    sock.listen()
    sock.settimeout(4)
    return sock


def run_client(port, connect=0, publish=0):
    subprocess.run([CLIENT, str(port), str(connect), str(publish)],
                   check=True, timeout=8)


def packet(conn):
    def read(n):
        data = b''
        while len(data) < n:
            part = conn.recv(n - len(data))
            if not part:
                raise EOFError('peer disconnected')
            data += part
        return data
    header = read(1)[0]
    length, multiplier = 0, 1
    for _ in range(4):
        digit = read(1)[0]
        length += (digit & 127) * multiplier
        if digit < 128:
            return header, read(length)
        multiplier *= 128
    raise AssertionError('invalid remaining length')


def controlled_peer(mode):
    with listener() as server:
        port = server.getsockname()[1]

        def serve():
            with server.accept()[0] as conn:
                conn.settimeout(3)
                header, body = packet(conn)
                assert header == 0x10
                assert body[:7] == b'\x00\x04MQTT\x04'  # MQTT 3.1.1
                if mode == 'silent-connack':
                    assert conn.recv(1) == b''  # client times out and closes
                    return
                if mode == 'reject':
                    conn.sendall(b'\x20\x02\x00\x05')
                    return
                conn.sendall(b'\x20\x02\x00\x00')
                if mode == 'disconnect':
                    return
                for _ in range(2 if mode == 'ack' else 1):
                    header, body = packet(conn)
                    assert header == 0x32  # QoS 1, retain false, no DUP
                    n = int.from_bytes(body[:2], 'big')
                    assert body[2:2+n] == b'smart-industry/machines/1/telemetry'
                    mid = body[2+n:4+n]
                    assert json.loads(body[4+n:])['sequence'] in (1, 2)
                    if mode == 'ack':
                        conn.sendall(b'\x40\x02' + mid)
                    elif mode == 'wrong-ack':
                        wrong = (int.from_bytes(mid, 'big') % 65535 + 1).to_bytes(2, 'big')
                        conn.sendall(b'\x40\x02' + wrong)
                        assert conn.recv(1) == b''
                    else:
                        assert conn.recv(1) == b''
                if mode == 'ack':
                    assert packet(conn) == (0xe0, b'')  # graceful DISCONNECT
            if mode == 'ack':
                with server.accept()[0] as reopened:
                    reopened.settimeout(3)
                    assert packet(reopened)[0] == 0x10
                    reopened.sendall(b'\x20\x02\x00\x00')
                    assert packet(reopened) == (0xe0, b'')

        with concurrent.futures.ThreadPoolExecutor(max_workers=1) as pool:
            future = pool.submit(serve)
            start = time.monotonic()
            run_client(port, -1 if mode in ('silent-connack', 'reject') else 0,
                       0 if mode == 'ack' else -1)
            assert time.monotonic() - start < 3
            future.result(timeout=4)


def run_replayer(path, port, machine=6, limit=None, success=True, extra=(),
                 executable='./build/dataset-replayer', env=None):
    command = [executable, str(path), '--publish',
               '--machine-id', str(machine), '--mqtt-host', '127.0.0.1',
               '--mqtt-port', str(port), *extra]
    if limit is not None:
        command += ['--limit', str(limit)]
    result = subprocess.run(command, capture_output=True, text=True, timeout=20, env=env)
    assert (result.returncode == 0) == success, result
    if not success:
        assert result.stdout == '', result
        assert result.stderr, result
    return result


def replayer_cases(root, port):
    dataset = Path('../data/predictive_maintenance_v3.csv')
    with dataset.open(newline='') as source:
        reader = csv.DictReader(source)
        fields = reader.fieldnames
        rows = []
        for row in reader:
            if row['machine_id'] == '6':
                rows.append(row)
                if len(rows) == 3:
                    break
    assert len(rows) == 3

    def receive(path, machine, count, limit=None, extra=()):
        output = root / 'replayer-messages'
        debug_path = root / 'replayer-debug'
        with output.open('w') as messages, debug_path.open('w') as debug:
            sub = subprocess.Popen([
                'stdbuf', '-oL', '-eL', 'mosquitto_sub', '-h', '127.0.0.1',
                '-p', str(port), '-V', 'mqttv311', '-q', '1', '-t',
                'smart-industry/machines/+/telemetry', '-C', str(count),
                '-W', '5', '-d', '-v'], stdout=messages, stderr=debug)
            try:
                deadline = time.monotonic() + 4
                while 'received SUBACK' not in output.read_text() + debug_path.read_text():
                    assert sub.poll() is None and time.monotonic() < deadline
                    time.sleep(.05)
                result = run_replayer(path, port, machine, limit, extra=extra)
                assert result.stdout == f'Publicações confirmadas: {count}\n', result
                assert result.stderr == '', result
                assert sub.wait(timeout=6) == 0
                received = []
                for line in output.read_text().splitlines():
                    if line.startswith('smart-industry/'):
                        topic, payload = line.split(' ', 1)
                        assert topic == f'smart-industry/machines/{machine}/telemetry'
                        data = json.loads(payload)
                        assert set(data) == {'machine_id', 'machine_type', 'timestamp',
                                            'temperature', 'current', 'power', 'state',
                                            'failure_within_24h'}
                        assert data['machine_id'] == machine
                        assert data['power'] is None
                        received.append(data)
                assert len(received) == count
                return received
            finally:
                if sub.poll() is None:
                    sub.terminate()
                sub.wait(timeout=5)

    # A single publication never waits, even with the largest interval.
    received = receive(dataset, 6, 1, 1, ('--interval-ms', '60000'))
    assert received[0]['timestamp'] == rows[0]['timestamp']
    assert received[0]['machine_type'] == rows[0]['machine_type']
    assert received[0]['temperature'] == float(rows[0]['temperature_motor'])
    assert received[0]['current'] == float(rows[0]['current_phase_avg'])
    assert received[0]['failure_within_24h'] == int(rows[0]['failure_within_24h'])
    assert received[0]['state'] == rows[0]['operating_mode'].upper()
    received = receive(dataset, 6, 3, 3)
    assert [r['timestamp'] for r in received] == [r['timestamp'] for r in rows]

    fixture = root / 'replayer.csv'
    def write_rows(selected):
        with fixture.open('w', newline='') as target:
            writer = csv.DictWriter(target, fieldnames=fields, lineterminator='\n')
            writer.writeheader()
            writer.writerows(selected)

    other = dict(rows[0], machine_id='7', failure_within_24h='2')
    missing = dict(rows[1], temperature_motor='', current_phase_avg='')
    invalid = dict(rows[2], failure_within_24h='2')
    write_rows([other, rows[0], missing, invalid])
    received = receive(fixture, 6, 2, 2)
    assert received[1]['temperature'] is None and received[1]['current'] is None
    # A selected invalid row aborts after two confirmations, without success output.
    result = run_replayer(fixture, port, success=False)
    assert 'Erro ao normalizar' in result.stderr
    assert 'linha 5' in result.stderr
    assert 'antes do erro: 2.' in result.stderr
    write_rows([other, rows[0], missing])
    receive(fixture, 6, 2)  # no --limit: end of CSV
    result = run_replayer(fixture, port, machine=99)
    assert result.stdout == 'Publicações confirmadas: 0\n'
    result = run_replayer(root / 'missing.csv', port, success=False)
    assert 'Erro ao ler' in result.stderr and 'antes do erro: 0.' in result.stderr
    # CSV accepts these bytes; the JSON module must reject invalid UTF-8 safely.
    write_rows([rows[0]])
    fixture.write_bytes(fixture.read_bytes().replace(rows[0]['machine_type'].encode(), b'\xff'))
    result = run_replayer(fixture, port, success=False)
    assert 'Erro ao serializar' in result.stderr and 'antes do erro: 0.' in result.stderr
    print('PASS: CSV → Telemetry → JSON → MQTT, filtros, limites e erros', flush=True)


def replayer_publication_failure():
    # One ACK followed by a dropped connection: no count for the failed row,
    # no third publication, and no automatic reconnect.
    with listener() as server:
        def serve():
            with server.accept()[0] as conn:
                conn.settimeout(3)
                _, body = packet(conn)
                n = int.from_bytes(body[10:12], 'big')
                client_id = body[12:12+n].decode()
                assert client_id.startswith('sim-6-')
                conn.sendall(b'\x20\x02\x00\x00')
                header, body = packet(conn)
                assert header == 0x32
                n = int.from_bytes(body[:2], 'big')
                conn.sendall(b'\x40\x02' + body[2+n:4+n])
                assert packet(conn)[0] == 0x32
        with concurrent.futures.ThreadPoolExecutor(max_workers=1) as pool:
            future = pool.submit(serve)
            result = run_replayer('../data/predictive_maintenance_v3.csv',
                                  server.getsockname()[1], limit=3, success=False)
            assert 'PUBACK' in result.stderr and 'antes do erro: 1.' in result.stderr
            future.result(timeout=4)
        server.settimeout(.1)
        try:
            unexpected, _ = server.accept()
        except socket.timeout:
            pass
        else:
            unexpected.close()
            raise AssertionError('Unexpected reconnect')
    print('PASS: falha após PUBACK parcial, sem sucesso nem reconexão', flush=True)


def replayer_timing():
    with tempfile.TemporaryDirectory(prefix='sim-timing-') as directory:
        path = Path(directory) / 'timing.csv'
        with Path('../data/predictive_maintenance_v3.csv').open(newline='') as source:
            reader = csv.DictReader(source)
            fields = reader.fieldnames
            rows = []
            for row in reader:
                if row['machine_id'] == '6':
                    rows.append(row)
                    if len(rows) == 3:
                        break
        # Interleaved and trailing records must not add sleeps.
        other = dict(rows[0], machine_id='7')
        with path.open('w', newline='') as target:
            writer = csv.DictWriter(target, fieldnames=fields, lineterminator='\n')
            writer.writeheader()
            writer.writerows([other, rows[0], other, rows[1], other, rows[2], other])

        for interval, count, mode, limit in (
                (60000, 1, None, 1), (100, 3, None, 3),
                (None, 3, None, None), (100, 3, 'eintr', 3),
                (100, 1, 'fail', 3)):
            with listener() as server:
                def serve():
                    with server.accept()[0] as conn:
                        conn.settimeout(15)  # generous deadlock guard, not a cadence assertion
                        assert packet(conn)[0] == 0x10
                        conn.sendall(b'\x20\x02\x00\x00')
                        last_ack = None
                        for i in range(count):
                            header, body = packet(conn)
                            arrival = time.monotonic()
                            assert header == 0x32  # QoS 1, retain false
                            n = int.from_bytes(body[:2], 'big')
                            assert body[2:2+n] == b'smart-industry/machines/6/telemetry'
                            payload = json.loads(body[4+n:])
                            assert payload['timestamp'] == rows[i]['timestamp']
                            if last_ack is not None:
                                seconds = (1000 if interval is None else interval) / 1000
                                assert arrival - last_ack >= seconds * .9
                            # Delayed PUBACK ensures pacing starts after confirmation.
                            if i == 0 and count > 1:
                                time.sleep(.15)
                            last_ack = time.monotonic()
                            conn.sendall(b'\x40\x02' + body[2+n:4+n])
                        assert packet(conn) == (0xe0, b'')  # no extra publication
                with concurrent.futures.ThreadPoolExecutor(max_workers=1) as pool:
                    future = pool.submit(serve)
                    env = dict(os.environ)
                    env.pop('SIM_TEST_SLEEP', None)
                    if mode:
                        env['SIM_TEST_SLEEP'] = mode
                    extra = () if interval is None else ('--interval-ms', str(interval))
                    result = run_replayer(path, server.getsockname()[1], limit=limit,
                                          success=mode != 'fail', extra=extra,
                                          executable='./build/test_replayer_timing', env=env)
                    future.result(timeout=15)
                calls = [line for line in result.stderr.splitlines()
                         if line.startswith('TEST nanosleep ')]
                expected = 1 if mode == 'fail' else count - 1 + (mode == 'eintr')
                assert len(calls) == expected, (calls, expected)
                if mode == 'fail':
                    assert 'Erro ao aguardar intervalo' in result.stderr
                    assert 'antes do erro: 1.' in result.stderr
                else:
                    assert result.stdout == f'Publicações confirmadas: {count}\n'
                    assert len(result.stderr.splitlines()) == len(calls)
                if interval is None:
                    assert calls == ['TEST nanosleep 1 0'] * 2
                if interval == 100:
                    assert calls == ['TEST nanosleep 0 100000000'] * expected
                print(f'PASS: temporização intervalo={interval}, modo={mode}, esperas={len(calls)}',
                      flush=True)

        # Instrument the traditional path too: no nanosleep calls at all.
        result = subprocess.run(['./build/test_replayer_timing', str(path),
                                 '--machine-id', '6'], capture_output=True, text=True, timeout=20)
        assert result.returncode == 0 and result.stderr == ''
        assert result.stdout.endswith('Registros selecionados: 3\n')


def real_broker():
    for command in ('mosquitto', 'mosquitto_sub', 'stdbuf'):
        if not shutil.which(command):
            raise RuntimeError(f'Missing dependency: {command}')
    with tempfile.TemporaryDirectory(prefix='sim-mqtt-') as directory:
        root = Path(directory)
        with listener() as reserve:
            port = reserve.getsockname()[1]
        config = root / 'mosquitto.conf'
        config.write_text(f'listener {port} 127.0.0.1\nallow_anonymous true\npersistence false\n')
        broker = subprocess.Popen(['mosquitto', '-c', str(config)],
                                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        subscriber = None
        try:
            deadline = time.monotonic() + 5
            while True:
                if broker.poll() is not None:
                    raise RuntimeError('Temporary broker failed to start')
                try:
                    with socket.create_connection(('127.0.0.1', port), timeout=.1):
                        break
                except OSError:
                    if time.monotonic() >= deadline:
                        raise
                    time.sleep(.05)
            with (root / 'messages').open('w+') as messages, (root / 'debug').open('w+') as debug:
                subscriber = subprocess.Popen([
                    'stdbuf', '-oL', '-eL', 'mosquitto_sub', '-h', '127.0.0.1', '-p', str(port),
                    '-V', 'mqttv311', '-q', '1', '-t', 'smart-industry/machines/1/telemetry',
                    '-C', '2', '-W', '5', '-d'], stdout=messages, stderr=debug)
                # Debug output may be on stdout or stderr depending on version.
                deadline = time.monotonic() + 4
                while True:
                    logs = (root / 'messages').read_text() + (root / 'debug').read_text()
                    if 'received SUBACK' in logs:
                        break
                    if subscriber.poll() is not None or time.monotonic() >= deadline:
                        raise RuntimeError(f'Subscriber not ready: {logs}')
                    time.sleep(.05)
                run_client(port)
                assert subscriber.wait(timeout=6) == 0
                received = [json.loads(line) for line in (root / 'messages').read_text().splitlines()
                            if line.startswith('{')]
                assert received == [{'sequence': 1}, {'sequence': 2}], received
            replayer_cases(root, port)
            ten_machines(root, port)
        finally:
            for process in (subscriber, broker):
                if process is not None:
                    if process.poll() is None:
                        process.terminate()
                    process.wait(timeout=5)


if __name__ == '__main__':
    # Reserve but do not listen: deterministic connection refused, no broker.
    with socket.socket() as unavailable:
        unavailable.bind(('127.0.0.1', 0))
        run_client(unavailable.getsockname()[1], -1, -1)
        result = run_replayer('../data/predictive_maintenance_v3.csv',
                              unavailable.getsockname()[1], success=False)
        assert 'Erro ao conectar' in result.stderr
        unavailable_broker(unavailable.getsockname()[1])
    for scenario in ('ack', 'reject', 'silent-connack', 'silent-puback', 'wrong-ack', 'disconnect'):
        controlled_peer(scenario)
        print(f'PASS: {scenario}', flush=True)
    replayer_publication_failure()
    replayer_timing()
    real_broker()
    print('Todos os testes de integração MQTT passaram.')
