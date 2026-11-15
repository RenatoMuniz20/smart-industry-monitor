#!/usr/bin/env python3
"""Testes do esqueleto do backend (SIM-06.1).

Sobe o binário em uma porta local livre e verifica:
  - GET /api/health -> 200 {"status": "ok"}
  - rota desconhecida -> 404
  - POST /api/health -> 405
  - SIGTERM -> encerramento limpo (código 0)

Não acessa a instância do InterSCity da UFMA.
"""
import json
import signal
import socket
import subprocess
import sys
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
    port = free_port()
    base = f"http://127.0.0.1:{port}"
    proc = subprocess.Popen([binary, "--port", str(port)],
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

    print("test_backend: todos os testes passaram.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
