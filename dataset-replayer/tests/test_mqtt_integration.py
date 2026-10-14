#!/usr/bin/env python3
"""Local Mosquitto plus controlled protocol peers; no public broker required."""
import concurrent.futures
import json
from pathlib import Path
import shutil
import socket
import subprocess
import tempfile
import time

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
    for scenario in ('ack', 'reject', 'silent-connack', 'silent-puback', 'wrong-ack', 'disconnect'):
        controlled_peer(scenario)
        print(f'PASS: {scenario}', flush=True)
    real_broker()
    print('Todos os testes de integração MQTT passaram.')
