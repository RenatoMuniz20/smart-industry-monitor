#!/usr/bin/env python3
"""Testes de GET /api/machines/latest (SIM-06.3).

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

LAST_PATH = "/collector/resources/data/last"
UUID_A = "18fa502d-2427-4485-a3a6-0b3f64585634"
UUID_B = "c29d711f-1f60-44c4-91ea-d72f49343c4e"
UUID_C = "6c5496cf-1d67-4476-bcb9-e9243da7feb7"  # sem dados no InterSCity falso

FIXTURE = {
    "resources": [
        {
            "uuid": UUID_A,
            "capabilities": {
                "sim_operating_state": [{"value": "IDLE", "date": "2024-01-01T00:21:00.000Z"}],
                "sim_motor_temperature": [{"value": 49.51, "date": "2024-01-01T00:21:00.000Z"}],
                "sim_current": [{"value": 3.2, "date": "2024-01-01T00:21:00.000Z"}],
                "sim_vibration_rms": [{"value": 0.81, "date": "2024-01-01T00:21:00.000Z"}],
                "sim_rpm": [{"value": 860.9, "date": "2024-01-01T00:21:00.000Z"}],
                "sim_failure_within_24h": [{"value": 0, "date": "2024-01-01T00:21:00.000Z"}],
            },
        },
        {
            "uuid": UUID_B,
            "capabilities": {
                "sim_operating_state": [{"value": "RUNNING", "date": "2024-01-01T00:43:00.000Z"}],
                "sim_motor_temperature": [{"value": 42.01, "date": "2024-01-01T00:43:00.000Z"}],
                "sim_failure_within_24h": [{"value": 0, "date": "2024-01-01T00:43:00.000Z"}],
            },
        },
    ]
}

RECEIVED = []  # (caminho, corpo JSON) de cada POST recebido


class FakeInterscity(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_POST(self):
        length = int(self.headers.get("Content-Length", 0))
        RECEIVED.append((self.path, json.loads(self.rfile.read(length).decode())))
        if self.path == LAST_PATH:
            self._reply(200, json.dumps(FIXTURE))
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


def start_backend(binary, port, machines_path, interscity_url):
    return subprocess.Popen(
        [binary, "--port", str(port), "--machines", machines_path,
         "--interscity-url", interscity_url, "--interscity-timeout-ms", "2000"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)


def write_machines(lines):
    fd, path = tempfile.mkstemp(suffix=".conf")
    with os.fdopen(fd, "w") as f:
        f.write("\n".join(lines) + "\n")
    return path


def stop(proc):
    if proc.poll() is None:
        proc.kill()
        proc.wait()


def main():
    if len(sys.argv) != 2:
        print("uso: test_machines_latest.py <binario>", file=sys.stderr)
        return 2
    binary = sys.argv[1]

    fake = ThreadingHTTPServer(("127.0.0.1", 0), FakeInterscity)
    threading.Thread(target=fake.serve_forever, daemon=True).start()
    fake_base = f"http://127.0.0.1:{fake.server_address[1]}"

    machines = write_machines([f"1 {UUID_A}", "# comentário ignorado",
                               f"2 {UUID_B}", f"3 {UUID_C}"])
    try:
        # Cenário 1: InterSCity responde.
        port = free_port()
        base = f"http://127.0.0.1:{port}"
        proc = start_backend(binary, port, machines, fake_base)
        try:
            wait_ready(base + "/api/health", proc)

            status, body = request(base + "/api/machines/latest")
            assert status == 200, (status, body)
            data = json.loads(body)["machines"]
            assert [m["machine_id"] for m in data] == [1, 2, 3], data
            print("PASS: 3 máquinas na ordem do arquivo (comentário ignorado)")

            m1 = data[0]
            assert m1["uuid"] == UUID_A
            assert m1["operating_state"] == {"value": "IDLE", "timestamp": "2024-01-01T00:21:00.000Z"}
            assert m1["motor_temperature"]["value"] == 49.51
            assert m1["current"]["value"] == 3.2
            assert m1["failure_within_24h"]["value"] == 0
            assert m1["vibration_rms"] == {"value": 0.81, "timestamp": "2024-01-01T00:21:00.000Z"}
            assert m1["rpm"]["value"] == 860.9
            print("PASS: máquina 1 com texto, número, zero e timestamp corretos")

            m2 = data[1]
            assert m2["operating_state"]["value"] == "RUNNING"
            assert m2["current"] is None
            assert m2["vibration_rms"] is None and m2["rpm"] is None
            print("PASS: capability ausente na resposta vira null")

            m3 = data[2]
            assert m3["uuid"] == UUID_C
            assert m3["operating_state"] is None and m3["motor_temperature"] is None
            assert m3["vibration_rms"] is None and m3["rpm"] is None
            print("PASS: máquina sem dados no InterSCity vira nulls")

            assert RECEIVED and RECEIVED[-1] == (LAST_PATH, {"uuids": [UUID_A, UUID_B, UUID_C]}), RECEIVED[-1:]
            print("PASS: backend enviou os 3 uuids em um único POST")

            status, _ = request(base + "/api/machines/latest", method="POST")
            assert status == 405, status
            status, _ = request(base + "/nao-existe")
            assert status == 404, status
            print("PASS: POST em /latest -> 405; rota inexistente -> 404")
        finally:
            stop(proc)

        # Cenário 2: InterSCity fora do ar.
        port2 = free_port()
        dead_base = f"http://127.0.0.1:{free_port()}"
        proc2 = start_backend(binary, port2, machines, dead_base)
        try:
            base2 = f"http://127.0.0.1:{port2}"
            wait_ready(base2 + "/api/health", proc2)
            status, body = request(base2 + "/api/machines/latest")
            assert status == 502, (status, body)
            assert json.loads(body) == {"error": "falha ao consultar o InterSCity"}, body
            print("PASS: InterSCity fora do ar -> 502 com mensagem genérica")
        finally:
            stop(proc2)
    finally:
        fake.shutdown()
        fake.server_close()
        os.unlink(machines)

    # Cenário 3: arquivo de máquinas inválido -> o backend não sobe.
    bad = write_machines(["1 uuid-invalido"])
    try:
        r = subprocess.run([binary, "--port", str(free_port()), "--machines", bad],
                           capture_output=True, text=True, timeout=10)
        assert r.returncode == 1 and "uuid inválido" in r.stderr, (r.returncode, r.stderr)
        print("PASS: uuid inválido no arquivo -> backend recusa iniciar")
    finally:
        os.unlink(bad)

    print("test_machines_latest: todos os testes passaram.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
