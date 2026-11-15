#!/usr/bin/env python3
"""CLI checks without broker; --mqtt adds isolated communication and signals."""
from contextlib import contextmanager
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import sys
import tempfile
import time

from test_mqtt_integration import listener, terminate

GATEWAY = os.environ.get('SIM_GATEWAY_TEST', './build/iot-gateway')
READY = 'Gateway pronto: smart-industry/machines/+/telemetry (QoS 1).'
RECONNECTED = 'MQTT reconectado: assinatura renovada.'
PREFIX = 'Telemetria válida: '


def local_tests():
    result = subprocess.run([GATEWAY, '--help'], capture_output=True, text=True, timeout=10)
    assert result.returncode == 0 and result.stderr == ''
    assert 'Uso:' in result.stdout and 'timeout-ms=5000' in result.stdout
    cases = [['--unknown'], ['positional'], ['--help', '--port', '1883']]
    for option in ('--host', '--port', '--client-id', '--timeout-ms'):
        cases += [[option], [option, ''], [option, '--help']]
        value = 'host' if option == '--host' else 'id' if option == '--client-id' else '1'
        cases.append([option, value, option, value])
    for option, maximum in (('--port', 65535), ('--timeout-ms', 5000)):
        cases += [[option, value] for value in
                  ('0', '-1', '+1', '1.5', '1x', ' 1', str(maximum + 1), '9' * 100)]
    for args in cases:
        result = subprocess.run([GATEWAY, *args], capture_output=True, text=True, timeout=10)
        assert result.returncode != 0 and result.stdout == '', (args, result)
        assert result.stderr.startswith('Argumentos inválidos.\nUso:'), (args, result)
    print('PASS: CLI --help e argumentos inválidos, sem broker', flush=True)


def wait_for(process, condition, diagnostic):
    deadline = time.monotonic() + 10
    while not condition():
        assert process.poll() is None and time.monotonic() < deadline, diagnostic()
        time.sleep(.02)


def wait_for_port(broker, port, root):
    deadline = time.monotonic() + 10
    while True:
        assert broker.poll() is None, (root / 'broker.log').read_text()
        try:
            with socket.create_connection(('127.0.0.1', port), timeout=.1):
                return
        except OSError:
            assert time.monotonic() < deadline
            time.sleep(.02)


@contextmanager
def temporary_broker(root):
    with listener() as reserved:
        port = reserved.getsockname()[1]
    config = root / 'mosquitto.conf'
    config.write_text(f'listener {port} 127.0.0.1\nallow_anonymous true\npersistence false\n')
    with (root / 'broker.log').open('w') as log:
        broker = subprocess.Popen(['mosquitto', '-c', str(config)], stdout=log, stderr=log)
        try:
            wait_for_port(broker, port, root)
            yield broker, port
        finally:
            terminate(broker)


def start_broker(root, port):
    """Starts a new broker with the same configuration (same port) after a stop."""
    broker = subprocess.Popen(['mosquitto', '-c', str(root / 'mosquitto.conf')],
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    wait_for_port(broker, port, root)
    return broker


def publish(port, payload):
    subprocess.run(['mosquitto_pub', '-h', '127.0.0.1', '-p', str(port), '-V', 'mqttv311',
                    '-q', '1', '-t', 'smart-industry/machines/1/telemetry', '-s'],
                   input=payload, check=True, timeout=10)


def integration_tests():
    with socket.socket() as unavailable:
        unavailable.bind(('127.0.0.1', 0))
        result = subprocess.run([GATEWAY, '--port', str(unavailable.getsockname()[1])],
                                capture_output=True, text=True, timeout=10)
        assert result.returncode != 0 and result.stdout == ''
        assert result.stderr.startswith('Erro MQTT ao conectar/assinar:')
    print('PASS: CLI broker indisponível', flush=True)

    with tempfile.TemporaryDirectory(prefix='sim04-gateway-cli-') as directory:
        root = Path(directory)
        with temporary_broker(root) as (broker, port):
            for mode in ('sigint-messages', 'sigterm', 'network-error'):
                output, errors = root / 'stdout.log', root / 'stderr.log'
                with output.open('w') as out, errors.open('w') as err:
                    # First invocation also exercises default host, ID and timeout.
                    command = [GATEWAY, '--port', str(port)]
                    if mode != 'sigint-messages':
                        command += ['--host', '127.0.0.1', '--client-id', 'sim-cli-test',
                                    '--timeout-ms', '1000']
                    process = subprocess.Popen(command, stdout=out, stderr=err)
                    try:
                        diagnostic = lambda: output.read_text() + errors.read_text()
                        wait_for(process, lambda: READY in output.read_text().splitlines(), diagnostic)
                        assert errors.read_text() == ''
                        if mode == 'sigint-messages':
                            first = {'machine_id': 1, 'machine_type': 'CNC\n\x1b[31m"\\',
                                     'timestamp': '2024-01-01 00:00:00', 'temperature': 49.51,
                                     'current': 5.1, 'power': None, 'state': 'IDLE', 'failure_within_24h': 0}
                            second = dict(first, machine_type='CNC', temperature=0,
                                          current=None, timestamp='original sample', state='PEAK', failure_within_24h=1)
                            publish(port, json.dumps(first).encode())
                            publish(port, b'invalid JSON\n\x1b[31m')
                            publish(port, json.dumps(second).encode())
                            wait_for(process, lambda: sum(line.startswith(PREFIX) for line in
                                     output.read_text().splitlines()) == 2 and 'Telemetria inválida:' in errors.read_text(), diagnostic)
                            lines = output.read_text().splitlines()
                            assert len(lines) == 3 and lines[0] == READY, lines
                            records = [json.loads(line[len(PREFIX):]) for line in lines[1:]]
                            assert records == [first, second], records
                            assert '\x1b' not in output.read_text() and '\x1b' not in errors.read_text()
                            assert len(errors.read_text().splitlines()) == 1
                            assert process.poll() is None  # Invalid JSON is not a transport failure.
                            print('LOG OBSERVADO:', lines[2], flush=True)
                            print('ERRO OBSERVADO:', errors.read_text().strip(), flush=True)
                            process.send_signal(signal.SIGINT)
                        elif mode == 'sigterm':
                            process.send_signal(signal.SIGTERM)
                        else:
                            # Broker goes away: the Gateway stays alive and reconnects.
                            terminate(broker)
                            wait_for(process, lambda: 'Erro MQTT ao receber:' in errors.read_text(), diagnostic)
                            assert process.poll() is None, diagnostic()
                            restarted = start_broker(root, port)
                            try:
                                wait_for(process, lambda: RECONNECTED in output.read_text(), diagnostic)
                                after = {'machine_id': 1, 'machine_type': 'CNC',
                                         'timestamp': '2024-01-01 00:05:00', 'temperature': 50.0,
                                         'current': 5.0, 'power': None, 'state': 'NORMAL',
                                         'failure_within_24h': 0}
                                publish(port, json.dumps(after).encode())
                                wait_for(process, lambda: any(line.startswith(PREFIX) for line in
                                         output.read_text().splitlines()), diagnostic)
                                logged = [line for line in output.read_text().splitlines()
                                          if line.startswith(PREFIX)]
                                assert json.loads(logged[-1][len(PREFIX):]) == after, diagnostic()
                            finally:
                                terminate(restarted)
                            process.send_signal(signal.SIGTERM)
                        code = process.wait(timeout=10)
                        assert code == 0, diagnostic()
                        assert output.read_text().splitlines()[-1] == 'Gateway encerrado por solicitação.'
                        if mode == 'network-error':
                            assert 'Erro MQTT ao receber:' in errors.read_text(), diagnostic()
                        if mode == 'sigterm':
                            assert errors.read_text() == ''
                        assert not Path(f'/proc/{process.pid}').exists()
                    finally:
                        terminate(process)
                print(f'PASS: CLI {mode}, processo coletado', flush=True)
    print('Todos os testes de integração da CLI passaram.')


if __name__ == '__main__':
    if sys.argv[1:] == ['--mqtt']:
        integration_tests()
    else:
        assert len(sys.argv) == 1
        local_tests()
