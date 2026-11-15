#!/usr/bin/env python3
"""Teste de fumaça contra a instância REAL do InterSCity da UFMA (SIM-06.5).

SOMENTE LEITURA. O backend só faz consultas (POST /data/last e POST /data do
Data Collector). Não cadastra recursos e não envia dados.

Não roda em `make test`. Execução manual:

    make -C backend smoke-real

Argumentos opcionais: <machines.conf> <url_do_interscity>
"""
import json
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.request

DEFAULT_MACHINES = "../config/interscity/machines.conf"
DEFAULT_URL = "https://cidadesinteligentes.lsdi.ufma.br/interscity_lh"
HIST_START = "2024-01-01T00:00:00"
HIST_END = "2024-01-01T01:00:00"
EMPTY_START = "2030-01-01T00:00:00"
EMPTY_END = "2030-01-01T01:00:00"
SERIES = ["operating_state", "motor_temperature", "current", "failure_within_24h"]

results = []


def check(cond, label):
    results.append(bool(cond))
    print(("PASS: " if cond else "FAIL: ") + label)


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def get(url):
    start = time.monotonic()
    try:
        with urllib.request.urlopen(url, timeout=60) as r:
            status, text = r.status, r.read().decode()
    except urllib.error.HTTPError as e:
        status, text = e.code, e.read().decode()
    elapsed = time.monotonic() - start
    try:
        data = json.loads(text)
    except json.JSONDecodeError:
        data = None
    return status, data, elapsed


def read_expected(path):
    expected = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            machine_id, uuid = line.split()
            expected.append((int(machine_id), uuid))
    return expected


def wait_ready(base, proc, timeout=10.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            raise RuntimeError(f"backend terminou com código {proc.returncode}")
        try:
            get(base + "/api/health")
            return
        except OSError:
            time.sleep(0.1)
    raise RuntimeError("backend não respondeu a tempo")


def strictly_increasing(points):
    ts = [p["timestamp"] for p in points]
    return all(a < b for a, b in zip(ts, ts[1:]))


def main():
    if len(sys.argv) < 2:
        print("uso: smoke_real_interscity.py <binario> [machines.conf] [url]", file=sys.stderr)
        return 2
    binary = sys.argv[1]
    machines = sys.argv[2] if len(sys.argv) > 2 else DEFAULT_MACHINES
    url = sys.argv[3] if len(sys.argv) > 3 else DEFAULT_URL

    expected = read_expected(machines)
    print(f"InterSCity: {url}")
    print(f"Mapeamento: {len(expected)} máquinas em {machines}\n")

    port = free_port()
    base = f"http://127.0.0.1:{port}"
    proc = subprocess.Popen(
        [binary, "--port", str(port), "--machines", machines,
         "--interscity-url", url, "--interscity-timeout-ms", "15000"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        wait_ready(base, proc)
        check(True, "backend subiu com o mapeamento real")

        status, data, dt = get(base + "/api/machines")
        check(status == 200 and [(m["machine_id"], m["uuid"]) for m in data["machines"]] == expected,
              "GET /api/machines devolve os mesmos IDs e UUIDs do arquivo")

        status, data, dt = get(base + "/api/machines/latest")
        check(status == 200, f"GET /api/machines/latest -> 200 ({dt:.2f} s)")
        latest = data["machines"] if data else []
        check([m["machine_id"] for m in latest] == [i for i, _ in expected],
              "latest traz as mesmas máquinas, na mesma ordem")

        print("\nÚltimo valor por máquina:")
        print(f"{'id':>3}  {'estado':<12} {'temp':>8}  {'corrente':>9}  timestamp")
        for m in latest:
            def val(s):
                return "-" if m[s] is None else m[s]["value"]
            ts = m["operating_state"]["timestamp"] if m["operating_state"] else "-"
            print(f"{m['machine_id']:>3}  {str(val('operating_state')):<12} "
                  f"{str(val('motor_temperature')):>8}  {str(val('current')):>9}  {ts}")
        print()
        with_data = sum(1 for m in latest if m["operating_state"] is not None)
        check(with_data == len(expected), f"{with_data}/{len(expected)} máquinas com último valor")

        print()
        for machine_id, uuid in expected:
            q = f"start={HIST_START}&end={HIST_END}"
            status, data, dt = get(f"{base}/api/machines/{machine_id}/history?{q}")
            if status != 200 or data is None:
                check(False, f"máquina {machine_id}: histórico -> {status}")
                continue
            series = data["series"]
            counts = {s: len(series[s]) for s in SERIES}
            ordered = all(strictly_increasing(series[s]) for s in SERIES)
            check(ordered, f"máquina {machine_id}: histórico sem repetidos e em ordem "
                           f"({dt:.2f} s, {counts})")

        status, data, dt = get(f"{base}/api/machines/1/history?start={EMPTY_START}&end={EMPTY_END}")
        empty = status == 200 and data is not None and all(len(data["series"][s]) == 0 for s in SERIES)
        check(empty, "janela de 2030 sem dados devolve séries vazias")

        status, _, _ = get(f"{base}/api/machines/1/history?start=2024-1-1T00:00:00&end={HIST_END}")
        check(status == 400, "data fora do formato -> 400")
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait()

    ok = sum(results)
    print(f"\n{ok} de {len(results)} verificações passaram.")
    return 0 if ok == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
