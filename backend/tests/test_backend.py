#!/usr/bin/env python3
"""Testes do servidor (SIM-06.1): /api/health, 404, 405 e SIGTERM.

Usa um arquivo de máquinas temporário e um InterSCity inexistente (não é consultado aqui).
Não acessa a instância do InterSCity da UFMA.
"""
import json
import os
import signal
import socket
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def request(url, method="GET"):
    data = b"" if method == "POST" else None
    req = urllib.request.Request(url, data=data, method=method)
    try:
        with urllib.request.urlopen(req, timeout=3) as r:
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
        print("uso: test_backend.py <binario>", file=sys.stderr)
        return 2
    binary = sys.argv[1]

    fd, machines = tempfile.mkstemp(suffix=".conf")
    with os.fdopen(fd, "w") as f:
        f.write("1 18fa502d-2427-4485-a3a6-0b3f64585634\n")

    port = free_port()
    base = f"http://127.0.0.1:{port}"
    proc = subprocess.Popen(
        [binary, "--port", str(port), "--machines", machines,
         "--interscity-url", "http://127.0.0.1:9", "--interscity-timeout-ms", "1000"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        wait_ready(base + "/api/health", proc)

        status, body = request(base + "/api/health")
        assert status == 200, status
        assert json.loads(body) == {"status": "ok"}, body
        print("PASS: GET /api/health -> 200 {status: ok}")

        status, _ = request(base + "/rota-inexistente")
        assert status == 404, status
        print("PASS: rota desconhecida -> 404")

        status, _ = request(base + "/api/health", method="POST")
        assert status == 405, status
        print("PASS: POST /api/health -> 405")

        proc.send_signal(signal.SIGTERM)
        code = proc.wait(timeout=5)
        assert code == 0, code
        print("PASS: SIGTERM -> encerramento limpo (código 0)")
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait()
        os.unlink(machines)

    print("test_backend: todos os testes passaram.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
