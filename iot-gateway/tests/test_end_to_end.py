#!/usr/bin/env python3
"""SIM-04.4: real replayers → temporary Mosquitto → real Gateway."""
import csv
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import tempfile

from test_cli import GATEWAY, PREFIX, READY, publish, temporary_broker, terminate, wait_for

REPO = Path(__file__).resolve().parents[2]
IDS = (1, 2, 3, 6, 7, 8, 9, 11, 12, 13)


def expected_samples():
    expected = {machine: [] for machine in IDS}
    with (REPO / 'data/predictive_maintenance_v3.csv').open(newline='') as source:
        for row in csv.DictReader(source):
            machine = int(row['machine_id'])
            if machine not in expected or len(expected[machine]) == 3:
                continue
            expected[machine].append({
                'machine_id': machine, 'machine_type': row['machine_type'],
                'timestamp': row['timestamp'],
                'temperature': float(row['temperature_motor']) if row['temperature_motor'] else None,
                'current': float(row['current_phase_avg']) if row['current_phase_avg'] else None,
                'power': None,
                'state': {'idle': 'IDLE', 'normal': 'NORMAL', 'peak': 'PEAK'}.get(row['operating_mode'], 'UNKNOWN'),
                'failure_within_24h': int(row['failure_within_24h'])})
    for machine, rows in expected.items():
        kind = 'CNC' if machine <= 3 else 'Pump' if machine <= 9 else 'Compressor'
        assert len(rows) == 3 and all(row['machine_type'] == kind for row in rows)
    return expected


def records(path):
    return [json.loads(line[len(PREFIX):]) for line in path.read_text().splitlines()
            if line.startswith(PREFIX)]


def launched(path):
    return {int(machine): int(pid) for machine, pid in
            re.findall(r'Máquina (\d+) iniciada: PID (\d+)', path.read_text())}


def main():
    expected = expected_samples()
    with tempfile.TemporaryDirectory(prefix='sim04-e2e-') as directory:
        root = Path(directory)
        with temporary_broker(root) as (broker, port):
            gateway_out, gateway_err = root / 'gateway.out', root / 'gateway.err'
            producer_out, producer_err = root / 'producers.out', root / 'producers.err'
            launcher = gateway = None
            try:
                with gateway_out.open('w') as gout, gateway_err.open('w') as gerr, \
                        producer_out.open('w') as pout, producer_err.open('w') as perr:
                    gateway = subprocess.Popen([GATEWAY, '--host', '127.0.0.1', '--port', str(port),
                        '--client-id', f'sim04-integrated-{os.getpid()}'], stdout=gout, stderr=gerr)
                    diagnostic = lambda: '\n'.join(f'{p.name}:\n{p.read_text()}' for p in
                        (gateway_out, gateway_err, producer_out, producer_err))
                    wait_for(gateway, lambda: READY in gateway_out.read_text().splitlines(), diagnostic)
                    env = dict(os.environ, LIMIT='3', INTERVAL_MS='1000', MQTT_HOST='127.0.0.1',
                               MQTT_PORT=str(port), CSV_PATH=str(REPO / 'data/predictive_maintenance_v3.csv'))
                    launcher = subprocess.Popen([str(REPO / 'dataset-replayer/scripts/run-ten-machines.sh')],
                        env=env, stdout=pout, stderr=perr, start_new_session=True)
                    wait_for(launcher, lambda: set(launched(producer_err)) == set(IDS), diagnostic)
                    pids = launched(producer_err)
                    assert len(set(pids.values())) == 10, diagnostic()
                    for machine, pid in pids.items():
                        args = Path(f'/proc/{pid}/cmdline').read_bytes().split(b'\0')
                        assert b'--publish' in args, diagnostic()
                        for option, value in ((b'--machine-id', str(machine)), (b'--limit', '3'),
                                              (b'--interval-ms', '1000'), (b'--mqtt-port', str(port))):
                            assert args[args.index(option) + 1] == value.encode(), diagnostic()
                    assert launcher.wait(timeout=20) == 0, diagnostic()
                    assert producer_out.read_text().count('Publicações confirmadas: 3\n') == 10, diagnostic()
                    assert 'Dez máquinas concluídas com sucesso.' in producer_out.read_text(), diagnostic()
                    assert len(producer_err.read_text().splitlines()) == 10, diagnostic()
                    assert all(not Path(f'/proc/{pid}').exists() for pid in pids.values()), diagnostic()

                    def complete():
                        received = records(gateway_out)
                        return all(all(sample in received for sample in samples) for samples in expected.values())
                    # PUBACK does not prove Gateway processing: wait for the actual logs.
                    wait_for(gateway, complete, diagnostic)
                    assert gateway_err.read_text() == '', diagnostic()
                    baseline = records(gateway_out)
                    counts = {machine: sum(row['machine_id'] == machine for row in baseline) for machine in IDS}
                    assert all(count >= 3 for count in counts.values()), diagnostic()
                    for row in baseline:
                        assert row['machine_id'] in IDS and type(row['machine_id']) is int, diagnostic()
                        assert set(row) == set(expected[row['machine_id']][0]), diagnostic()
                        assert row in expected[row['machine_id']], diagnostic()
                        assert type(row['failure_within_24h']) is int, diagnostic()
                        assert row['power'] is None, diagnostic()
                        for key in ('temperature', 'current'):
                            assert row[key] is None or type(row[key]) in (int, float), diagnostic()

                    # Isolated control samples distinguish probes from the 30 CSV samples.
                    before = dict(expected[1][0], timestamp='sim04-probe-before', temperature=0, current=None)
                    after = dict(before, timestamp='sim04-probe-after')
                    publish(port, json.dumps(before).encode())
                    wait_for(gateway, lambda: before in records(gateway_out), diagnostic)
                    publish(port, b'{}')
                    wait_for(gateway, lambda: 'Telemetria inválida:' in gateway_err.read_text(), diagnostic)
                    assert gateway.poll() is None, diagnostic()
                    publish(port, json.dumps(after).encode())
                    wait_for(gateway, lambda: after in records(gateway_out), diagnostic)
                    errors = gateway_err.read_text().splitlines()
                    assert errors and all('oito propriedades obrigatórias' in line for line in errors), diagnostic()
                    gateway.send_signal(signal.SIGTERM)
                    assert gateway.wait(timeout=10) == 0, diagnostic()
                    assert gateway_out.read_text().splitlines()[-1] == 'Gateway encerrado por solicitação.', diagnostic()
                    assert not Path(f'/proc/{gateway.pid}').exists(), diagnostic()
                    final_records = records(gateway_out)
                    normal = [row for row in final_records if row['timestamp'] not in (before['timestamp'], after['timestamp'])]
                    assert all(row in expected.get(row['machine_id'], []) for row in normal), diagnostic()
                    counts = {machine: sum(row['machine_id'] == machine for row in normal) for machine in IDS}
                    assert before in final_records and after in final_records, diagnostic()
                    print(f'PASS: 10 produtores simultâneos, 30 publicações confirmadas, {len(normal)} telemetrias CSV processadas', flush=True)
                    print('CONTAGEM POR ID:', json.dumps(counts, sort_keys=True), flush=True)
                    print('EXEMPLO RECEBIDO:', json.dumps(normal[0], ensure_ascii=False), flush=True)
                    print('CONTINUIDADE:', errors[0], flush=True)
                    print(f'PASS: válida/inválida/válida; {len(final_records)} válidas no total; SIGTERM=0; filhos coletados', flush=True)
            finally:
                # A private session contains only this test's launcher and children.
                if launcher is not None:
                    try:
                        os.killpg(launcher.pid, signal.SIGTERM)
                    except ProcessLookupError:
                        pass
                    terminate(launcher)
                    try:
                        os.killpg(launcher.pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass
                terminate(gateway)
        assert broker.poll() is not None
    print('Validação integrada SIM-04.4 concluída; broker e arquivos temporários removidos.')


if __name__ == '__main__':
    main()
