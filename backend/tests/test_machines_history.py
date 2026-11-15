#!/usr/bin/env python3
"""Testes de GET /api/machines e /api/machines/{id}/history (SIM-06.4).

Sobe um InterSCity falso em 127.0.0.1 e aponta o backend para ele.
Não acessa a instância do InterSCity da UFMA.
"""
import json
import os
import socket
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HIST_PATH_SUFFIX = "/data"
UUID_A = "18fa502d-2427-4485-a3a6-0b3f64585634"  # com histórico (com duplicatas)
UUID_B = "c29d711f-1f60-44c4-91ea-d72f49343c4e"  # resposta sem capabilities
UUID_C = "6c5496cf-1d67-4476-bcb9-e9243da7feb7"  # 404 "Resource not found"

HIST_A = {
    "resources": [
        {
            "uuid": UUID_A,
            "capabilities": {
                "sim_operating_state": [{"value": "IDLE", "date": "2024-01-01T00:24:00.000Z"}],
                "sim_motor_temperature": [
                    {"value": 42.01, "date": "2024-01-01T00:43:00.000Z"},
                    {"value": 49.08, "date": "2024-01-01T00:24:00.000Z"},
                    {"value": 42.01, "date": "2024-01-01T00:43:00.000Z"},
                    {"value": 49.08, "date": "2024-01-01T00:24:00.000Z"},
                ],
                "sim_failure_within_24h": [{"value": 0, "date": "2024-01-01T00:24:00.000Z"}],
            },
        }
    ]
}

RECEIVED = []  # (caminho, corpo JSON)


class FakeInterscity(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_POST(self):
        length = int(self.headers.get("Content-Length", 0))
        RECEIVED.append((self.path, json.loads(self.rfile.read(length).decode())))
        if self.path == f"/collector/resources/{UUID_A}{HIST_PATH_SUFFIX}":
            self._reply(200, json.dumps(HIST_A))
        elif self.path == f"/collector/resources/{UUID_B}{HIST_PATH_SUFFIX}":
            self._reply(200, json.dumps({"resources": [{"uuid": UUID_B, "capabilities": {}}]}))
        elif self.path == f"/collector/resources/{UUID_C}{HIST_PATH_SUFFIX}":
            self._reply(404, '{"error":"Resource not found"}')
        else:
            self._reply(404, '{"error":"Not found"}')

    def _reply(self, status, text):
        data = text.encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def request(url, method="GET"):
    data = b"" if method == "POST" else None
    req = urllib.request.Request(url, data=data, method=method)
    try:
        with urllib.request.urlopen(req, timeout=5) as r:
            return r.status, r.read().decode()
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode()


def wait_ready(url, proc, timeout=5.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            raise RuntimeError(f"processo terminou cedo com código {proc.returncode}")
        try:
            request(url)
            return
        except OSError:
            time.sleep(0.1)
    raise RuntimeError("servidor não respondeu a tempo")


def main():
    if len(sys.argv) != 2:
        print("uso: test_machines_history.py <binario>", file=sys.stderr)
        return 2
    binary = sys.argv[1]

    fake = ThreadingHTTPServer(("127.0.0.1", 0), FakeInterscity)
    threading.Thread(target=fake.serve_forever, daemon=True).start()
    fake_base = f"http://127.0.0.1:{fake.server_address[1]}"

    fd, machines = tempfile.mkstemp(suffix=".conf")
    with os.fdopen(fd, "w") as f:
        f.write(f"1 {UUID_A}\n2 {UUID_B}\n3 {UUID_C}\n")

    port = free_port()
    base = f"http://127.0.0.1:{port}"
    proc = subprocess.Popen(
        [binary, "--port", str(port), "--machines", machines,
         "--interscity-url", fake_base, "--interscity-timeout-ms", "2000"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        wait_ready(base + "/api/health", proc)

        status, body = request(base + "/api/machines")
        assert status == 200, (status, body)
        assert json.loads(body) == {"machines": [
            {"machine_id": 1, "uuid": UUID_A},
            {"machine_id": 2, "uuid": UUID_B},
            {"machine_id": 3, "uuid": UUID_C}]}, body
        print("PASS: GET /api/machines lista o mapeamento, sem consultar o InterSCity")

        q = "start=2024-01-01T00:00:00&end=2024-01-01T01:00:00"
        status, body = request(f"{base}/api/machines/1/history?{q}")
        assert status == 200, (status, body)
        h = json.loads(body)
        assert h["machine_id"] == 1 and h["uuid"] == UUID_A, h
        assert h["start"] == "2024-01-01T00:00:00" and h["end"] == "2024-01-01T01:00:00", h
        s = h["series"]
        assert s["motor_temperature"] == [
            {"timestamp": "2024-01-01T00:24:00.000Z", "value": 49.08},
            {"timestamp": "2024-01-01T00:43:00.000Z", "value": 42.01}], s["motor_temperature"]
        assert s["operating_state"] == [{"timestamp": "2024-01-01T00:24:00.000Z", "value": "IDLE"}]
        assert s["current"] == []
        assert s["failure_within_24h"] == [{"timestamp": "2024-01-01T00:24:00.000Z", "value": 0}]
        print("PASS: histórico sem duplicatas, em ordem, com texto, número e zero")

        assert RECEIVED[-1] == (f"/collector/resources/{UUID_A}/data",
                                {"start_date": "2024-01-01T00:00:00",
                                 "end_date": "2024-01-01T01:00:00"}), RECEIVED[-1:]
        print("PASS: backend pediu o período em UTC ao Data Collector")

        status, body = request(f"{base}/api/machines/2/history?{q}")
        assert status == 200 and json.loads(body)["series"]["motor_temperature"] == [], body
        print("PASS: máquina sem capabilities devolve séries vazias")

        status, body = request(f"{base}/api/machines/3/history?{q}")
        assert status == 200, (status, body)
        assert all(v == [] for v in json.loads(body)["series"].values()), body
        print("PASS: 404 'Resource not found' do Data Collector vira série vazia")

        status, body = request(f"{base}/api/machines/1/history?start=2024-01-01T00:00:00")
        assert status == 400, (status, body)
        status, body = request(f"{base}/api/machines/1/history?start=2024-1-1T00:00:00&end=2024-01-01T01:00:00")
        assert status == 400, (status, body)
        status, body = request(f"{base}/api/machines/1/history?start=2024-01-01T02:00:00&end=2024-01-01T01:00:00")
        assert status == 400, (status, body)
        print("PASS: parâmetros ausentes, formato errado e início após fim -> 400")

        status, body = request(f"{base}/api/machines/9/history?{q}")
        assert status == 404, (status, body)
        status, body = request(f"{base}/api/machines/1x/history?{q}")
        assert status == 404, (status, body)
        print("PASS: máquina desconhecida e id inválido -> 404")

        status, _ = request(f"{base}/api/machines/1/history?{q}", method="POST")
        assert status == 405, status
        status, _ = request(f"{base}/api/machines", method="POST")
        assert status == 405, status
        print("PASS: POST nas rotas de máquinas -> 405")
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait()
        fake.shutdown()
        fake.server_close()
        os.unlink(machines)

    print("test_machines_history: todos os testes passaram.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
