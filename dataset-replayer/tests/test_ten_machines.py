#!/usr/bin/env python3
"""Launcher checks; MQTT cases reuse the existing temporary broker harness."""
import csv
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import time

SCRIPT = Path('scripts/run-ten-machines.sh').resolve()
IDS = {1, 2, 3, 6, 7, 8, 9, 11, 12, 13}


def environment(**values):
    env = dict(os.environ)
    for key in ('LIMIT', 'INTERVAL_MS', 'MQTT_HOST', 'MQTT_PORT', 'CSV_PATH'):
        env.pop(key, None)
    env.update({key: str(value) for key, value in values.items()})
    return env


def launched(log):
    return {int(machine): int(pid) for machine, pid in
            re.findall(r'Máquina (\d+) iniciada: PID (\d+)', log)}


def assert_reaped(log):
    for pid in launched(log).values():
        assert not Path(f'/proc/{pid}').exists(), f'Child still exists: {pid}'


def local_checks():
    for values in ({'LIMIT': 0}, {'INTERVAL_MS': 60001}, {'MQTT_PORT': 65536},
                   {'MQTT_HOST': ''}, {'LIMIT': 'abc'}, {'LIMIT': '9' * 100},
                   {'INTERVAL_MS': '-1'}, {'CSV_PATH': '/nonexistent/sim.csv'}):
        result = subprocess.run([str(SCRIPT)], env=environment(**values),
                                capture_output=True, text=True, timeout=10)
        assert result.returncode != 0 and not launched(result.stderr), result
    print('Todos os testes locais do launcher passaram.', flush=True)


def unavailable_broker(port):
    result = subprocess.run([str(SCRIPT)], env=environment(MQTT_PORT=port),
                            capture_output=True, text=True, timeout=20)
    assert result.returncode != 0 and 'concluídas com sucesso' not in result.stdout
    assert 'Erro ao conectar' in result.stderr
    assert_reaped(result.stderr)
    print('PASS: dez máquinas com broker indisponível', flush=True)


def ten_machines(root, port):
    # Resolve expected types and original record order directly from the dataset.
    expected = {machine: [] for machine in IDS}
    with Path('../data/predictive_maintenance_v3.csv').open() as source:
        for row in csv.DictReader(source):
            machine = int(row['machine_id'])
            if machine in expected and len(expected[machine]) < 3:
                expected[machine].append(row)
    for machine, rows in expected.items():
        kind = 'CNC' if machine < 4 else 'Pump' if machine < 10 else 'Compressor'
        assert len(rows) == 3 and all(row['machine_type'] == kind for row in rows)

    messages = root / 'ten-messages'
    debug = root / 'ten-debug'
    output = root / 'ten-output'
    errors = root / 'ten-errors'
    with messages.open('w') as received, debug.open('w') as details:
        sub = subprocess.Popen([
            'stdbuf', '-oL', '-eL', 'mosquitto_sub', '-h', '127.0.0.1', '-p', str(port),
            '-V', 'mqttv311', '-q', '1', '-t', 'smart-industry/machines/+/telemetry',
            '-C', '30', '-W', '20', '-d', '-v'], stdout=received, stderr=details)
        launcher = None
        try:
            deadline = time.monotonic() + 10
            while 'received SUBACK' not in messages.read_text() + debug.read_text():
                assert sub.poll() is None and time.monotonic() < deadline
                time.sleep(.02)
            with output.open('w') as out, errors.open('w') as err:
                started = time.monotonic()
                launcher = subprocess.Popen([str(SCRIPT)], cwd='/tmp',
                    env=environment(LIMIT=3, INTERVAL_MS=500, MQTT_PORT=port),
                    stdout=out, stderr=err)
                pids = wait_for_children(launcher, errors)
                assert len(set(pids.values())) == 10
                # Evidence of ten simultaneous independent real C processes.
                for machine, pid in pids.items():
                    args = Path(f'/proc/{pid}/cmdline').read_bytes().split(b'\0')
                    assert b'--publish' in args
                    assert args[args.index(b'--machine-id') + 1] == str(machine).encode()
                    assert args[args.index(b'--interval-ms') + 1] == b'500'
                    assert args[args.index(b'--limit') + 1] == b'3'
                assert launcher.wait(timeout=20) == 0
                assert time.monotonic() - started >= .9  # two 500 ms waits, with tolerance
            assert sub.wait(timeout=20) == 0
            assert_reaped(errors.read_text())
            assert output.read_text().count('Publicações confirmadas: 3') == 10
            assert 'Dez máquinas concluídas com sucesso.' in output.read_text()
            grouped = {machine: [] for machine in IDS}
            for line in messages.read_text().splitlines():
                if line.startswith('smart-industry/'):
                    topic, payload = line.split(' ', 1)
                    data = json.loads(payload)
                    machine = data['machine_id']
                    assert machine in IDS
                    assert topic == f'smart-industry/machines/{machine}/telemetry'
                    assert data['power'] is None
                    grouped[machine].append(data)
            for machine, records in grouped.items():
                assert len(records) == 3
                assert [r['timestamp'] for r in records] == [r['timestamp'] for r in expected[machine]]
                assert all(r['machine_type'] == expected[machine][0]['machine_type'] for r in records)
        finally:
            for process in (launcher, sub):
                if process is not None:
                    if process.poll() is None:
                        process.terminate()
                    process.wait(timeout=20)
    print('PASS: dez processos simultâneos, IDs, tipos, tópicos e limites', flush=True)

    for mode in (signal.SIGINT, signal.SIGTERM, 'child-failure'):
        with output.open('w') as out, errors.open('w') as err:
            launcher = subprocess.Popen([str(SCRIPT)], env=environment(MQTT_PORT=port),
                                        stdout=out, stderr=err)
            try:
                pids = wait_for_children(launcher, errors)
                for pid in pids.values():
                    args = Path(f'/proc/{pid}/cmdline').read_bytes().split(b'\0')
                    assert args[args.index(b'--interval-ms') + 1] == b'1000'
                    assert args[args.index(b'--limit') + 1] == b'10'
                if mode == 'child-failure':
                    os.kill(next(iter(pids.values())), signal.SIGTERM)
                    assert launcher.wait(timeout=20) == 1
                else:
                    launcher.send_signal(mode)
                    assert launcher.wait(timeout=20) == 128 + mode
                assert_reaped(errors.read_text())
                assert 'concluídas com sucesso' not in output.read_text()
            finally:
                if launcher.poll() is None:
                    launcher.terminate()
                launcher.wait(timeout=20)
        print(f'PASS: encerramento e coleta de todos os filhos ({mode})', flush=True)


def wait_for_children(process, errors):
    deadline = time.monotonic() + 10
    while True:
        pids = launched(errors.read_text())
        if set(pids) == IDS:
            return pids
        assert process.poll() is None and time.monotonic() < deadline, errors.read_text()
        time.sleep(.01)


if __name__ == '__main__':
    local_checks()
