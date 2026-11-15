#!/usr/bin/env python3
"""Testes do cliente de consulta ao InterSCity (SIM-06.2).

Sobe um InterSCity falso em 127.0.0.1 e roda o driver C contra ele.
Não acessa a instância da UFMA.
"""
import json
import subprocess
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

LAST_PATH = "/collector/resources/data/last"
UUID_A = "18fa502d-2427-4485-a3a6-0b3f64585634"
UUID_B = "c29d711f-1f60-44c4-91ea-d72f49343c4e"

FIXTURE_OK = {
    "resources": [
        {
            "uuid": UUID_A,
            "capabilities": {
                "sim_operating_state": [{"value": "IDLE", "date": "2024-01-01T00:21:00.000Z"}],
                "sim_motor_temperature": [{"value": 49.51, "date": "2024-01-01T00:21:00.000Z"}],
                "sim_current": [{"value": 3.2, "date": "2024-01-01T00:21:00.000Z"}],
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

RECEIVED = []  # (caminho, corpo) de cada POST recebido


class FakeInterscity(BaseHTTPRequestHandler):
    def log_message(self, *args):  # silencia o log padrão
        pass

    def do_POST(self):
        length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(length).decode()
        RECEIVED.append((self.path, body))

        prefix, sep, rest = self.path.partition(LAST_PATH)
        if sep == "" or rest != "":
            self._reply(404, '{"error":"Not found"}')
            return
        scenario = prefix.lstrip("/")
        if scenario == "ok":
            self._reply(200, json.dumps(FIXTURE_OK))
        elif scenario == "http500":
            self._reply(500, '{"error":"boom"}')
        elif scenario == "badjson":
            self._reply(200, "isto não é json")
        elif scenario == "empty":
            self._reply(200, '{"resources":[]}')
        else:
            self._reply(404, '{"error":"Unknown scenario"}')

    def _reply(self, status, text):
        data = text.encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)


def main():
    if len(sys.argv) != 2:
        print("uso: test_interscity_query.py <driver_c>", file=sys.stderr)
        return 2
    driver = sys.argv[1]

    server = ThreadingHTTPServer(("127.0.0.1", 0), FakeInterscity)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    base = f"http://127.0.0.1:{server.server_address[1]}"

    try:
        result = subprocess.run([driver, base], capture_output=True, text=True, timeout=60)
    finally:
        server.shutdown()
        server.server_close()

    print(result.stdout, end="")
    if result.stderr:
        print(result.stderr, end="", file=sys.stderr)
    assert result.returncode == 0, f"driver terminou com código {result.returncode}"

    expected_body = json.dumps({"uuids": [UUID_A, UUID_B]}, separators=(",", ":"))
    paths_bodies = [(p, json.loads(b)) for p, b in RECEIVED if p.endswith(LAST_PATH)]
    assert ("/ok" + LAST_PATH, {"uuids": [UUID_A, UUID_B]}) in paths_bodies, paths_bodies
    print("PASS: corpo enviado = " + expected_body)
    print("test_interscity_query.py: todos os testes passaram.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
