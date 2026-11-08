#!/usr/bin/env python3
"""SIM-05.5.4: Gateway com envio ao InterSCity.

Sem argumentos: opções e mapeamento, sem broker e sem rede.
Com --mqtt: Mosquitto temporário + InterSCity falso local + Gateway real.
Nenhum caso acessa a instância real do LSDi.
"""
from http.server import ThreadingHTTPServer
import json
import os
from pathlib import Path
import re
import signal
import socket
import subprocess
import sys
import tempfile
import threading

from test_cli import GATEWAY, PREFIX as VALID, READY, temporary_broker, terminate, wait_for
import test_interscity_client as mock

REPO = Path(__file__).resolve().parents[2]
UUID_OK = '00000000-0000-4000-8000-000000000201'
UUID_500 = '00000000-0000-4000-8000-000000000500'
REAL_URL = 'https://cidadesinteligentes.lsdi.ufma.br/interscity_lh'  # validada, nunca contatada
FAKE_URL = 'http://127.0.0.1:9/interscity_lh'


def run(args):
    return subprocess.run([GATEWAY, *args], capture_output=True, text=True, timeout=15)


def local_tests():
    help_result = run(['--help'])
    assert help_result.returncode == 0 and help_result.stderr == '', help_result
    assert '--interscity-url' in help_result.stdout and 'interscity-timeout-ms=5000' in help_result.stdout

    with tempfile.TemporaryDirectory(prefix='sim05-gateway-cli-') as directory:
        root = Path(directory)
        good = root / 'machines.conf'
        good.write_text(f'1 {UUID_OK}\n')
        enabled = ['--interscity-url', FAKE_URL, '--interscity-map', str(good)]
        invalid = [
            ['--interscity-url', FAKE_URL],                 # sem mapeamento
            ['--interscity-map', str(good)],                # sem URL
            ['--interscity-timeout-ms', '1000'],            # timeout sozinho
            ['--interscity-url'], ['--interscity-map'], ['--interscity-timeout-ms'],
            enabled + ['--interscity-url', FAKE_URL],       # duplicada
            enabled + ['--interscity-map', str(good)],
            enabled + ['--interscity-timeout-ms', '1', '--interscity-timeout-ms', '1'],
            ['--interscity-url', '', '--interscity-map', str(good)],
            ['--interscity-url', '--help', '--interscity-map', str(good)],
            ['--interscity-url', 'ftp://example.org', '--interscity-map', str(good)],
            ['--interscity-url', 'http://exa mple.org', '--interscity-map', str(good)],
            ['--interscity-url', 'http://', '--interscity-map', str(good)],
        ] + [enabled + ['--interscity-timeout-ms', value]
             for value in ('0', '-1', '+1', '1.5', '1x', ' 1', '60001', '9' * 100)]
        for args in invalid:
            result = run(args)
            assert result.returncode != 0 and result.stdout == '', (args, result)
            assert result.stderr.startswith('Argumentos inválidos.\nUso:'), (args, result)

        crlf = root / 'crlf.conf'
        crlf.write_bytes(f'# ok\n1 {UUID_OK}\r\n'.encode())
        duplicated = root / 'duplicated.conf'
        duplicated.write_text(f'1 {UUID_OK}\n1 {UUID_500}\n')
        empty = root / 'empty.conf'
        empty.write_text('# nenhuma máquina\n')
        cases = [
            (root / 'missing.conf', 'Erro no mapeamento InterSCity: Não foi possível abrir o arquivo de mapeamento.\n'),
            (crlf, 'Erro no mapeamento InterSCity: Quebra de linha CRLF (caractere CR) no mapeamento (linha 2).\n'),
            (duplicated, 'Erro no mapeamento InterSCity: machine_id repetido no mapeamento (linha 2).\n'),
            (empty, 'Erro no mapeamento InterSCity: Mapeamento sem nenhuma máquina.\n'),
        ]
        for path, message in cases:
            # The mapping is checked before MQTT: no broker is needed for these errors.
            result = run(['--interscity-url', FAKE_URL, '--interscity-map', str(path)])
            assert result.returncode == 1 and result.stdout == '', (path, result)
            assert result.stderr == message, (path, result)

    # Real mapping and real URL: both accepted; no HTTP request is made before
    # telemetry arrives, so the closed MQTT port ends the run before any send.
    with socket.socket() as unavailable:
        unavailable.bind(('127.0.0.1', 0))
        result = run(['--port', str(unavailable.getsockname()[1]),
                      '--interscity-url', REAL_URL,
                      '--interscity-map', str(REPO / 'config/interscity/machines.conf')])
    assert result.returncode == 1, result
    assert result.stdout == (f'InterSCity habilitado: {REAL_URL} '
                             '(10 máquinas mapeadas, timeout 5000 ms).\n'), result
    assert result.stderr.startswith('Erro MQTT ao conectar/assinar:'), result
    print('PASS: opções InterSCity, mapeamento inválido e mapeamento real, sem broker', flush=True)


def publish(port, machine, payload):
    subprocess.run(['mosquitto_pub', '-h', '127.0.0.1', '-p', str(port), '-V', 'mqttv311', '-q', '1',
                    '-t', f'smart-industry/machines/{machine}/telemetry', '-s'],
                   input=payload, check=True, timeout=10)


def telemetry(machine, **changes):
    sample = {'machine_id': machine, 'machine_type': 'CNC', 'timestamp': '2024-01-01 00:00:00',
              'temperature': 49.51, 'current': 5.1, 'power': None, 'state': 'IDLE',
              'failure_within_24h': 0}
    sample.update(changes)
    return json.dumps(sample).encode()


def expected_body(timestamp='2024-01-01T00:00:00', current=True, state='IDLE', failure=0):
    data = {'sim_motor_temperature': [{'value': 49.51, 'timestamp': timestamp}]}
    if current:
        data['sim_current'] = [{'value': 5.1, 'timestamp': timestamp}]
    data['sim_operating_state'] = [{'value': state, 'timestamp': timestamp}]
    data['sim_failure_within_24h'] = [{'value': failure, 'timestamp': timestamp}]
    return {'data': data}


def integration_tests():
    server = ThreadingHTTPServer(('127.0.0.1', 0), mock.MockAdaptor)
    server.daemon_threads = True
    threading.Thread(target=server.serve_forever, daemon=True).start()
    base_url = f'http://127.0.0.1:{server.server_address[1]}{mock.PREFIX}'
    try:
        with tempfile.TemporaryDirectory(prefix='sim05-gateway-') as directory:
            root = Path(directory)
            mapping = root / 'machines.conf'
            mapping.write_text(f'# teste\n1 {UUID_OK}\n5 {UUID_500}\n')
            with temporary_broker(root) as (broker, port):
                output, errors = root / 'stdout.log', root / 'stderr.log'
                with output.open('w') as out, errors.open('w') as err:
                    process = subprocess.Popen(
                        [GATEWAY, '--port', str(port), '--client-id', f'sim05-gateway-{os.getpid()}',
                         '--interscity-url', base_url, '--interscity-map', str(mapping),
                         '--interscity-timeout-ms', '2000'], stdout=out, stderr=err)
                    try:
                        diagnostic = lambda: output.read_text() + errors.read_text()
                        lines = lambda: output.read_text().splitlines()
                        sent = lambda: [line for line in lines() if line.startswith('InterSCity: machine_id=')]
                        wait_for(process, lambda: READY in lines(), diagnostic)
                        assert lines()[0] == (f'InterSCity habilitado: {base_url} '
                                              '(2 máquinas mapeadas, timeout 2000 ms).'), diagnostic()
                        assert errors.read_text() == '', diagnostic()

                        publish(port, 1, telemetry(1))                       # amostra de referência
                        wait_for(process, lambda: len(sent()) == 1, diagnostic)
                        publish(port, 1, telemetry(1, current=None, state='PEAK', failure_within_24h=1))
                        wait_for(process, lambda: len(sent()) == 2, diagnostic)
                        publish(port, 2, telemetry(2))                       # sem mapeamento
                        wait_for(process, lambda: 'machine_id=2 sem UUID' in errors.read_text(), diagnostic)
                        publish(port, 5, telemetry(5, machine_type='Pump'))  # InterSCity responde 500
                        wait_for(process, lambda: 'machine_id=5 falhou' in errors.read_text(), diagnostic)
                        publish(port, 1, b'{}')                              # inválida
                        wait_for(process, lambda: 'Telemetria inválida:' in errors.read_text(), diagnostic)
                        publish(port, 1, telemetry(1, timestamp='2024-01-01 00:01:00'))
                        wait_for(process, lambda: len(sent()) == 3, diagnostic)
                        assert process.poll() is None, diagnostic()          # nenhuma falha derrubou o Gateway

                        pattern = r'InterSCity: machine_id=1 enviada \(HTTP 201, \d+ ms, conexão (nova|reutilizada)\)\.'
                        matches = [re.fullmatch(pattern, line) for line in sent()]
                        assert all(matches), diagnostic()
                        assert matches[0].group(1) == 'nova' and matches[1].group(1) == 'reutilizada', diagnostic()
                        error_lines = errors.read_text().splitlines()
                        assert len(error_lines) == 3, diagnostic()
                        assert error_lines[0] == 'InterSCity: machine_id=2 sem UUID no mapeamento; não enviada.'
                        assert re.fullmatch(r'InterSCity: machine_id=5 falhou: Resposta HTTP diferente de 201 '
                                            r'\(HTTP 500, \d+ ms\)\.', error_lines[1]), diagnostic()
                        assert error_lines[2].startswith('Telemetria inválida:'), diagnostic()

                        process.send_signal(signal.SIGTERM)
                        assert process.wait(timeout=10) == 0, diagnostic()
                        final = lines()
                        assert sum(line.startswith(VALID) for line in final) == 5, diagnostic()
                        assert final[-2] == 'InterSCity: 3 enviadas, 1 falhas, 1 sem mapeamento.', diagnostic()
                        assert final[-1] == 'Gateway encerrado por solicitação.', diagnostic()
                        assert not Path(f'/proc/{process.pid}').exists()
                    finally:
                        terminate(process)
    finally:
        server.shutdown()
        server.server_close()

    def path(uuid):
        return f'{mock.PREFIX}/adaptor/resources/{uuid}/data'

    with mock.LOCK:
        received = list(mock.RECEIVED)
    expected = [
        (path(UUID_OK), expected_body()),
        (path(UUID_OK), expected_body(current=False, state='PEAK', failure=1)),
        (path(UUID_500), expected_body()),
        (path(UUID_OK), expected_body(timestamp='2024-01-01T00:01:00')),
    ]
    observed = [(request['path'], json.loads(request['body'])) for request in received]
    assert observed == expected, json.dumps(received, indent=2, ensure_ascii=False)
    assert all(request['content_type'] == 'application/json' for request in received), received
    print('PASS: Gateway → InterSCity falso: 4 envios conferidos (201, medição omitida, 500), '
          'máquina sem mapeamento, inválida, SIGTERM=0', flush=True)
    print('CORPO ENVIADO:', received[0]['body'], flush=True)


if __name__ == '__main__':
    if sys.argv[1:] == ['--mqtt']:
        integration_tests()
    else:
        assert len(sys.argv) == 1
        local_tests()
