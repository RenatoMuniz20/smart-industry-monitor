#!/usr/bin/env python3
"""Temporary local broker and minimal handshake fault fixtures, no JSON layer."""
import concurrent.futures
import os
from pathlib import Path
import shutil
import socket
import subprocess
import tempfile
import time

CLIENT = os.environ.get('SIM_SUBSCRIBER_TEST', './build/test_mqtt_subscriber')
FILTER = b'smart-industry/machines/+/telemetry'


def listener():
    sock = socket.socket()
    sock.bind(('127.0.0.1', 0))
    sock.listen()
    sock.settimeout(5)
    return sock


def packet(conn):
    def read(size):
        data = b''
        while len(data) < size:
            chunk = conn.recv(size - len(data))
            if not chunk:
                raise EOFError('Unexpected disconnect')
            data += chunk
        return data
    header = read(1)[0]
    length, multiplier = 0, 1
    for _ in range(4):
        digit = read(1)[0]
        length += (digit & 127) * multiplier
        if digit < 128:
            return header, read(length)
        multiplier *= 128
    raise AssertionError('Invalid MQTT length')


def negative_peer(mode):
    # Only exchange CONNECT/SUBSCRIBE and inject one fault. Not a broker.
    with listener() as server:
        def serve():
            for _ in range(2):
                with server.accept()[0] as conn:
                    conn.settimeout(3)
                    header, body = packet(conn)
                    assert header == 0x10 and body[:7] == b'\x00\x04MQTT\x04'
                    assert body[7] == 2  # clean session, no auth/will
                    if mode == 'silent-connack':
                        assert conn.recv(1) == b''
                        continue
                    if mode == 'reject':
                        conn.sendall(b'\x20\x02\x00\x05')
                        continue
                    conn.sendall(b'\x20\x02\x00\x00')
                    header, body = packet(conn)
                    assert header == 0x82
                    mid = int.from_bytes(body[:2], 'big')
                    size = int.from_bytes(body[2:4], 'big')
                    assert body[4:4+size] == FILTER and body[4+size:] == b'\x01'
                    if mode == 'silent-suback':
                        assert conn.recv(1) == b''
                        continue
                    if mode == 'wrong-mid':
                        mid = mid % 65535 + 1
                    qos = {'qos0': b'\x00', 'qos2': b'\x02', 'rejected-suback': b'\x80',
                           'wrong-count': b'\x01\x01'}.get(mode, b'\x01')
                    response = mid.to_bytes(2, 'big') + qos
                    conn.sendall(bytes([0x90, len(response)]) + response)
        expected = 'reject' if mode == 'reject' else 'timeout' if mode.startswith('silent') else 'suback'
        with concurrent.futures.ThreadPoolExecutor(max_workers=1) as pool:
            future = pool.submit(serve)
            result = subprocess.run([CLIENT, str(server.getsockname()[1]), expected],
                                    capture_output=True, text=True, timeout=10)
            future.result(timeout=5)
            assert result.returncode == 0, result
            assert 'EXPECTED_ERROR' in result.stdout, result
    print(f'PASS: {mode}, incluindo reabertura após falha', flush=True)


def terminate(process):
    if process is not None:
        if process.poll() is None:
            process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5)


def wait_ready(process, path):
    deadline = time.monotonic() + 8
    while 'READY\n' not in path.read_text():
        assert process.poll() is None and time.monotonic() < deadline, path.read_text()
        time.sleep(.02)


def broker_cases():
    for tool in ('mosquitto', 'mosquitto_pub'):
        if not shutil.which(tool):
            raise RuntimeError(f'Dependência indisponível: {tool}')
    with tempfile.TemporaryDirectory(prefix='sim04-subscriber-') as directory:
        root = Path(directory)
        with listener() as reserved:
            port = reserved.getsockname()[1]
        config = root / 'mosquitto.conf'
        config.write_text(f'listener {port} 127.0.0.1\nallow_anonymous true\npersistence false\n')
        broker = subscriber = None
        try:
            with (root / 'broker.log').open('w') as log:
                broker = subprocess.Popen(['mosquitto', '-c', str(config)], stdout=log, stderr=log)
                deadline = time.monotonic() + 8
                while True:
                    assert broker.poll() is None, (root / 'broker.log').read_text()
                    try:
                        with socket.create_connection(('127.0.0.1', port), timeout=.1):
                            break
                    except OSError:
                        assert time.monotonic() < deadline
                        time.sleep(.02)
                output = root / 'receive.log'
                cases = [('smart-industry/machines/1/telemetry', b'not JSON'),
                         ('smart-industry/machines/42/telemetry', b'\x00\xffA\x00B'),
                         ('smart-industry/machines/7/telemetry', b'')]
                with output.open('w') as result_log:
                    subscriber = subprocess.Popen([CLIENT, str(port), 'receive'],
                                                   stdout=result_log, stderr=result_log)
                    wait_ready(subscriber, output)  # CONNACK + SUBACK, not just TCP
                    for topic, payload in cases:
                        command = ['mosquitto_pub', '-h', '127.0.0.1', '-p', str(port),
                                   '-V', 'mqttv311', '-q', '1', '-t', topic]
                        command += ['-s'] if payload else ['-n']
                        subprocess.run(command, input=payload, check=True, timeout=5)
                    assert subscriber.wait(timeout=15) == 0, output.read_text()
                lines = output.read_text().splitlines()
                assert [line for line in lines if line.startswith('MSG ')] == [
                    f'MSG {topic} {len(payload)} {payload.hex()}' for topic, payload in cases]
                assert lines[-1] == 'DONE', lines
                print('PASS: broker real, 3 mensagens, tópico/bytes/comprimento, vazio, reabertura', flush=True)
                output = root / 'disconnect.log'
                with output.open('w') as result_log:
                    subscriber = subprocess.Popen([CLIENT, str(port), 'disconnect'],
                                                   stdout=result_log, stderr=result_log)
                    wait_ready(subscriber, output)
                    terminate(broker)
                    assert subscriber.wait(timeout=15) == 0, output.read_text()
                assert output.read_text().splitlines()[-1] == 'DONE'
                print('PASS: desconexão detectada, contexto fechado sem reconectar', flush=True)
        finally:
            terminate(subscriber)
            terminate(broker)


if __name__ == '__main__':
    with socket.socket() as unavailable:
        unavailable.bind(('127.0.0.1', 0))  # reserved, not listening
        subprocess.run([CLIENT, str(unavailable.getsockname()[1]), 'unavailable'],
                       check=True, timeout=10)
    print('PASS: broker indisponível', flush=True)
    for mode in ('reject', 'silent-connack', 'silent-suback', 'wrong-mid',
                 'qos0', 'qos2', 'rejected-suback', 'wrong-count'):
        negative_peer(mode)
    broker_cases()
    print('Todos os testes de integração MQTT do Gateway passaram.')
