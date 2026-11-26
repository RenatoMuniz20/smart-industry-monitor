#!/usr/bin/env python3
"""SIM-12: isolated Compose runs; read-only measurements, no application changes.

One run per scenario, no volume deletion. Use a NEW output directory and project
prefix for a repeat. Only the fake endpoint is accepted. Python standard library.
"""
import argparse
from collections import Counter, defaultdict
import csv
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path
import re
import socket
import statistics
import subprocess
import threading
import time
import urllib.error
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
IDS = (1, 2, 3, 6, 7, 8, 9, 11, 12, 13)
SERVICES = ('mosquitto', 'gateway', 'backend', 'interscity-fake', 'replayer')
TARGET = 'http://interscity-fake:8000'


def command(args, env=None, timeout=240):
    result = subprocess.run(args, cwd=ROOT, env=env, text=True, capture_output=True, timeout=timeout)
    if result.returncode:
        raise RuntimeError(f'{args}: {result.returncode}\n{result.stdout[-3000:]}\n{result.stderr[-3000:]}')
    return result.stdout.strip()


def save(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2, sort_keys=True) + '\n')


def expected_data():
    mapping = {}
    for line in (ROOT / 'config/interscity/machines.conf').read_text().splitlines():
        if line.strip() and not line.lstrip().startswith('#'):
            machine, uuid = line.split()
            mapping[int(machine)] = uuid
    selected = {i: [] for i in IDS}
    with (ROOT / 'data/predictive_maintenance_v3.csv').open() as source:
        for row in csv.DictReader(source):
            machine = int(row['machine_id'])
            if machine in selected and len(selected[machine]) < 60:
                selected[machine].append(row)
    expected, cursors = {}, {}
    for machine, rows in selected.items():
        timestamps = [row['timestamp'].replace(' ', 'T') for row in rows]
        assert len(rows) == 60 and len(set(timestamps)) == 60 and timestamps == sorted(timestamps)
        cursors[str(machine)] = timestamps[-1]
        for row, timestamp in zip(rows, timestamps):
            values = {'sim_operating_state': {'idle':'IDLE', 'normal':'NORMAL', 'peak':'PEAK'}.get(row['operating_mode'], 'UNKNOWN'),
                      'sim_failure_within_24h': int(row['failure_within_24h'])}
            for column, cap in [('temperature_motor','sim_motor_temperature'), ('current_phase_avg','sim_current'),
                                ('vibration_rms','sim_vibration_rms'), ('rpm','sim_rpm')]:
                value = float(row[column]) if row[column] else float('nan')
                if math.isfinite(value):
                    values[cap] = value
            for capability, value in values.items():
                key = (mapping[machine], capability, timestamp + '.000Z')
                assert key not in expected
                expected[key] = value
    return expected, cursors


class Resources:
    def __init__(self, ids, path, start):
        self.path, self.start, self.rows, self.errors = path, start, [], []
        self.roles = {cid: role for role, cid in ids.items()}
        self.proc = subprocess.Popen(['docker', 'stats', '--no-trunc', '--format', '{{json .}}', *ids.values()],
                                     stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        self.thread = threading.Thread(target=self.read)
        self.thread.start()

    def read(self):
        for line in self.proc.stdout:
            # Docker 29 emits terminal erase/cursor escapes even through a pipe.
            line = re.sub(r'\x1b\[[0-?]*[ -/]*[@-~]', '', line).strip()
            if not line:
                continue
            try:
                data = json.loads(line)
                role = self.roles[data['ID']]
                usage = data['MemUsage'].split('/')[0].strip()
                match = re.fullmatch(r'([0-9.]+)([A-Za-z]+)', usage)
                units = {'B':1, 'KiB':1024, 'MiB':1024**2, 'GiB':1024**3, 'kB':1000, 'MB':1000**2, 'GB':1000**3}
                mib = float(match[1]) * units[match[2]] / 1024**2
                self.rows.append((round(time.monotonic() - self.start, 3), role,
                                  float(data['CPUPerc'].rstrip('%')), round(mib, 4)))
            except (KeyError, ValueError, TypeError) as error:
                if len(self.errors) < 10:
                    self.errors.append(str(error))

    def stop(self):
        self.proc.terminate()
        self.proc.wait(timeout=10)
        self.thread.join(timeout=10)
        assert not self.thread.is_alive(), 'docker stats reader did not stop'
        with self.path.open('w') as file:
            writer = csv.writer(file)
            writer.writerow(['elapsed_s','service','cpu_percent','memory_mib'])
            writer.writerows(self.rows)
        summary = {}
        for role in self.roles.values():
            rows = [r for r in self.rows if r[1] == role]
            assert rows, f'No Docker stats samples for {role}'
            summary[role] = {'samples':len(rows), 'cpu_mean_percent':round(statistics.mean(r[2] for r in rows), 3),
                             'cpu_max_percent':max(r[2] for r in rows), 'memory_max_mib':max(r[3] for r in rows)}
        return summary


def scenario(label, interval, prefix, output, image_ids, expected, expected_cursors):
    project = f'{prefix}-{label.lower()}'
    offset = ord(label) - ord('A')
    backend_port, fake_port = 18180 + offset, 18100 + offset  # Distinct ports also avoid TCP TIME_WAIT on reruns.
    env = dict(os.environ, COMPOSE_PROJECT_NAME=project, INTERSCITY_URL=TARGET, LIMIT='60', INTERVAL_MS=str(interval),
               FAKE_LATENCY_MS='60', FAKE_JITTER_MS='0', FAKE_STORE_DELAY_MS='0', FAKE_FAIL_RATE='0',
               FAKE_MAX_INFLIGHT='0', FAKE_BACKEND_PORT=str(backend_port), FAKE_INTERSCITY_PORT=str(fake_port))
    base = ['docker','compose','-p',project,'-f','docker-compose.yml','-f','docker-compose.fake.yml']
    def compose(*args):
        return command([*base, *args], env)
    def info(service):
        return json.loads(command(['docker','inspect',compose('ps','-aq',service)]))[0]
    def query(sql):
        return json.loads(compose('exec','-T','gateway','sqlite3','-readonly','/data/telemetry.db',sql))
    def get(port, path):
        with urllib.request.urlopen(f'http://127.0.0.1:{port}{path}', timeout=5) as response:
            return json.load(response)
    def fake_sql(sql):
        code = 'import sqlite3,json; c=sqlite3.connect("file:/data/interscity-fake.db?mode=ro",uri=True); print(json.dumps(c.execute(' + repr(sql) + ').fetchall())); c.close()'
        return json.loads(compose('exec','-T','interscity-fake','python3','-c',code))
    qsql = "SELECT json_object('total',count(*),'pending',coalesce(sum(status='pending'),0),'inflight',coalesce(sum(status='inflight'),0),'blocked',coalesce(sum(status='blocked'),0)) FROM telemetry_queue;"
    assert not command(['docker','ps','-aq','--filter',f'label=com.docker.compose.project={project}'])
    for suffix in ('gateway_data','fake_interscity_data'):
        assert not command(['docker','volume','ls','-q','--filter',f'name=^{project}_{suffix}$']), 'Volume must be NEW'
    for port in (backend_port, fake_port):
        with socket.socket() as sock:
            sock.bind(('127.0.0.1',port))
    config = json.loads(compose('config','--format','json'))
    for role in ('gateway','backend'):
        argv = config['services'][role]['command']
        assert argv[argv.index('--interscity-url') + 1] == TARGET
    assert not config['services']['mosquitto'].get('ports')
    measurements, monitor, resource_summary = [], None, None
    summary = {'scenario':label, 'interval_ms':interval, 'project':project, 'success':False}
    replay_done_at = None
    try:
        if not image_ids:
            print('Building current HEAD once (shared image IDs for all scenarios).', flush=True)
            compose('build','gateway','backend','replayer','interscity-fake')
            for role in SERVICES:
                tag = 'eclipse-mosquitto:2' if role == 'mosquitto' else f'{project}-{role}'
                image_ids[role] = json.loads(command(['docker','image','inspect',tag]))[0]['Id']
        else:
            for role in SERVICES:
                if role != 'mosquitto':
                    command(['docker','image','tag',image_ids[role],f'{project}-{role}'])
        compose('up','-d','--no-build','--wait','--wait-timeout','60','mosquitto','interscity-fake','gateway','backend')
        deadline = time.monotonic() + 30
        while 'Gateway pronto:' not in compose('logs','gateway'):
            assert time.monotonic() < deadline, 'Gateway SUBACK timeout'
            time.sleep(.2)
        for role in ('gateway','backend'):
            cfg = info(role)['Config']['Cmd']
            assert cfg[cfg.index('--interscity-url') + 1] == TARGET
        assert get(backend_port,'/api/health') == {'status':'ok'}
        with urllib.request.urlopen(f'http://127.0.0.1:{backend_port}/', timeout=5) as page:
            assert page.status == 200 and b'<html' in page.read().lower()
        baseline = get(fake_port,'/__fake/stats')
        assert baseline['samples_stored'] == baseline['adaptor_requests'] == baseline['adaptor_created'] == 0
        assert baseline['config'] == {'latency_ms':60.0,'jitter_ms':0.0,'store_delay_ms':0.0,'fail_rate':0.0,'max_inflight':0}
        assert not baseline['down']
        assert query(qsql)['total'] == 0 and query('SELECT count(*) FROM delivered_cursor;') == 0
        assert fake_sql('SELECT count(*) FROM samples') == [[0]]
        summary['initial'] = {'queue':query(qsql), 'cursors':0, 'fake':baseline}
        (output/f'{label.lower()}-initial.json').write_text(json.dumps(summary['initial'], indent=2) + '\n')
        started = time.monotonic()
        compose('--profile','replay','up','-d','--no-build','--no-deps','replayer')
        ids = {role:info(role)['Id'] for role in SERVICES}
        assert all(info(role)['Image'] == image_ids[role] for role in SERVICES)
        monitor = Resources(ids, output/f'{label.lower()}-resources.csv', started)
        next_probe, last_status, backend_codes = 0, -10, []
        while True:
            q = query(qsql)
            stats = get(fake_port,'/__fake/stats')
            elapsed = time.monotonic() - started
            if elapsed >= next_probe:
                try:
                    get(backend_port,'/api/machines/latest')
                    backend_codes.append(200)
                except urllib.error.HTTPError as error:
                    backend_codes.append(error.code)
                next_probe = elapsed + 5
            state = info('replayer')['State']
            if not state['Running'] and replay_done_at is None:
                assert state['ExitCode'] == 0, compose('logs','replayer')
                replay_done_at = time.monotonic() - started
            sample = {'elapsed_s':round(elapsed,3), **q, 'http_created':stats['adaptor_created'],
                      'measurements_stored':stats['samples_stored'],
                      'cursors':query('SELECT count(*) FROM delivered_cursor;')}
            measurements.append(sample)
            if elapsed - last_status >= 10:
                print(f'{label} {elapsed:.1f}s queue={q["total"]} HTTP201={stats["adaptor_created"]} replay_done={replay_done_at is not None}', flush=True)
                last_status = elapsed
            if replay_done_at is not None and q['total'] == 0 and stats['adaptor_created'] == 600:
                drain_time = time.monotonic() - started
                break
            assert elapsed < 180, f'Queue did not drain in 180s: {q}, {stats}'
            time.sleep(max(0, 1 - (time.monotonic() - started - elapsed)))
        resource_summary = monitor.stop()
        assert not monitor.errors, monitor.errors
        monitor = None
        final_stats = get(fake_port,'/__fake/stats')
        cursor_rows = query("SELECT json_group_array(json_object('machine_id',machine_id,'last_timestamp',last_timestamp)) FROM delivered_cursor;")
        cursors = {str(r['machine_id']):r['last_timestamp'] for r in cursor_rows}
        assert cursors == expected_cursors, cursors
        assert compose('exec','-T','gateway','sqlite3','-readonly','/data/telemetry.db','PRAGMA integrity_check; PRAGMA journal_mode;') == 'ok\nwal'
        actual_rows = fake_sql('SELECT uuid,capability,date,value FROM samples ORDER BY uuid,capability,date,id')
        actual = {}
        duplicates = 0
        for uuid, cap, date, raw in actual_rows:
            key = (uuid, cap, date)
            duplicates += key in actual
            actual[key] = json.loads(raw)
        assert actual == expected and duplicates == 0, 'Stored values/capabilities differ from CSV or duplicated'
        assert len({(r[0],r[2]) for r in actual_rows}) == 600
        assert fake_sql('PRAGMA integrity_check') == [['ok']]
        for key in ('adaptor_bad_request','adaptor_unknown_uuid','adaptor_injected_503','adaptor_overload_503','down_503'):
            assert final_stats[key] == 0, (key, final_stats)
        assert final_stats['adaptor_requests'] == final_stats['adaptor_created'] == 600
        assert final_stats['samples_stored'] == final_stats['samples_received'] == len(expected)
        assert set(backend_codes) == {200}, backend_codes
        assert len(get(backend_port,'/api/machines/latest')['machines']) == 10
        docker_replay_state = info('replayer')['State']
        replay_runtime = (datetime.fromisoformat(docker_replay_state['FinishedAt']) -
                          datetime.fromisoformat(docker_replay_state['StartedAt'])).total_seconds()
        compose('stop')  # Only this scenario; collect final summaries before down.
        logs = {role:compose('logs','--no-color',role) for role in ('gateway','replayer','backend')}
        pub_counts = [int(n) for n in re.findall(r'Publicações confirmadas: (\d+)', logs['replayer'])]
        assert pub_counts == [60]*10 and 'Dez máquinas concluídas com sucesso.' in logs['replayer']
        persisted = len(re.findall(r'Fila SQLite: machine_id=\d+ persistida ',logs['gateway']))
        delivered = len(re.findall(r'Worker HTTP: id=\d+ machine_id=\d+ entregue ',logs['gateway']))
        valid = len(re.findall(r'Telemetria válida:',logs['gateway']))
        assert persisted == delivered == valid == 600, (persisted, delivered, valid)
        tokens = ['Erro SQLite','erro fatal','Telemetria inválida','não persistida','sem UUID','Erro MQTT',
                  'resultado ambíguo','blocked (','nova tentativa','reconciliação indisponível','duplicata ignorada']
        events = {token:logs['gateway'].count(token) for token in tokens}
        assert not any(events.values()), events
        exits = {role:{'exit_code':info(role)['State']['ExitCode'], 'restarts':info(role)['RestartCount']} for role in SERVICES}
        assert all(v == {'exit_code':0,'restarts':0} for v in exits.values()), exits
        with (output/f'{label.lower()}-queue.csv').open('w') as file:
            writer = csv.DictWriter(file, fieldnames=measurements[0].keys())
            writer.writeheader()
            writer.writerows(measurements)
        summary.update(success=True, mqtt_confirmed=sum(pub_counts), gateway_received=valid,
                       gateway_persisted=persisted, gateway_http_delivered=delivered,
                       http_accepted=final_stats['adaptor_created'], telemetry_samples_stored=600,
                       measurements_stored=len(actual_rows), measurements_by_capability=dict(Counter(r[1] for r in actual_rows)),
                       duplicate_measurements=duplicates, replay_duration_observed_s=round(replay_done_at,3),
                       total_drain_observed_s=round(drain_time,3),
                       replay_runtime_docker_s=round(replay_runtime,3),
                       publish_mean_per_s=round(600/replay_runtime,3), deliver_mean_per_s=round(600/drain_time,3),
                       queue_max_observed=max(r['total'] for r in measurements),
                       pending_max_observed=max(r['pending'] for r in measurements),
                       blocked_max_observed=max(r['blocked'] for r in measurements),
                       queue_samples=len(measurements), final_queue=q, final_cursors=cursors,
                       fake_final_stats=final_stats, resources=resource_summary, gateway_events=events,
                       backend_poll_http_codes=dict(Counter(backend_codes)), exits=exits,
                       docker_replayer_started_at=docker_replay_state['StartedAt'],
                       docker_replayer_finished_at=docker_replay_state['FinishedAt'],
                       image_ids=image_ids.copy(),
                       stored_content_sha256=hashlib.sha256(json.dumps(actual_rows,separators=(',',':')).encode()).hexdigest())
        essential = []
        for role, text in logs.items():
            essential.extend(line for line in text.splitlines() if any(k in line for k in
                ['Publicações confirmadas:', 'Dez máquinas concluídas', 'Gateway pronto',
                 'persistidas,', 'entregues,', 'encerrado por solicitação', 'encerrando backend']))
        (output/f'{label.lower()}-excerpts.log').write_text('\n'.join(essential) + '\n')
        print(f'PASS {label}: 600 telemetries, {len(actual_rows)} capability values; replay={replay_done_at:.2f}s drain={drain_time:.2f}s peak_observed={summary["queue_max_observed"]}', flush=True)
    except BaseException as error:
        summary['error'] = str(error)
        (output/f'{label.lower()}-diagnostic.log').write_text(compose('logs','--tail','30'))
        raise
    finally:
        # Measurement errors must NEVER skip cleanup of our own Compose project.
        try:
            if monitor:
                try:
                    resource_summary = monitor.stop()
                except Exception as error:
                    summary['resource_cleanup_error'] = str(error)[:1000]
            if resource_summary:
                summary['resources'] = resource_summary
            save(output/f'{label.lower()}-summary.json',summary)
        finally:
            compose('--profile','replay','down')  # NEVER -v: preserve volumes/evidence.
            assert not command(['docker','ps','-aq','--filter',f'label=com.docker.compose.project={project}'])



def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--resume', action='store_true', help='Skip only successful recorded scenarios; keep original evidence')
    parser.add_argument('--output', default='docs/evidencias/sim-12')
    parser.add_argument('--project-prefix', default='sim12-' + datetime.now(timezone.utc).strftime('%Y%m%dt%H%M%S'))
    args = parser.parse_args()
    assert re.fullmatch(r'sim12-[a-z0-9-]+',args.project_prefix), 'Use a new sim12-* prefix'
    output = (ROOT / args.output).resolve()
    assert args.resume or not output.exists(), 'Preserve prior evidence; choose a new output directory'
    expected, cursors = expected_data()
    before = command(['docker','ps','-q']).splitlines()
    host = json.loads(command(['docker','info','--format','{{json .}}']))
    output.mkdir(parents=True, exist_ok=args.resume)
    manifest = {'head':command(['git','rev-parse','HEAD']), 'branch':command(['git','branch','--show-current']),
                'started_utc':datetime.now(timezone.utc).isoformat(), 'project_prefix':args.project_prefix,
                'docker_version':host['ServerVersion'],'compose_version':command(['docker','compose','version','--short']),
                'cpus':host['NCPU'],'memory_bytes':host['MemTotal'],'kernel':host['KernelVersion'],
                'preexisting_running_containers':len(before),'expected_telemetries':600,
                'expected_measurements':len(expected),'queue_interval_target_s':1,'backend_poll_target_s':5}
    if args.resume:
        original = json.loads((output/'manifest.json').read_text())
        assert original['head'] == manifest['head'] and original['project_prefix'] == args.project_prefix
        original['resumed_utc'] = datetime.now(timezone.utc).isoformat()
        manifest = original
    save(output/'manifest.json',manifest)
    images = {}
    for label, interval in [('A',1000),('B',500),('C',200)]:
        prior = output/f'{label.lower()}-summary.json'
        if args.resume and prior.exists():
            recorded = json.loads(prior.read_text())
            assert recorded['success'] and recorded['interval_ms'] == interval
            if images:
                assert images == recorded['image_ids']
            images = recorded['image_ids']
            print(f'{label}: keeping successful prior results (no replay, no volume reuse).', flush=True)
            continue
        scenario(label,interval,args.project_prefix,output,images,expected,cursors)
    after = command(['docker','ps','-q']).splitlines()
    manifest.update(finished_utc=datetime.now(timezone.utc).isoformat(),
                    preexisting_running_container_ids_preserved=set(before).issubset(after), success=True)
    assert manifest['preexisting_running_container_ids_preserved']
    save(output/'manifest.json',manifest)
    print('All scenarios passed. Scenario volumes retained; other environments untouched.',flush=True)


if __name__ == '__main__':
    main()
