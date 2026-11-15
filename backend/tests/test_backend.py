#!/usr/bin/env python3
"""Testes do servidor: /api/health, 404, 405, SIGTERM, bind e opção inválida.

Usa um arquivo de máquinas temporário e um InterSCity inexistente (não é consultado aqui).
Não acessa a instância do InterSCity da UFMA.
"""
import json
import os
import shutil
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


def listening_addresses(port):
    out = subprocess.run(["ss", "-ltnH"], capture_output=True, text=True).stdout
    addrs = []
    for line in out.splitlines():
        cols = line.split()
        if len(cols) >= 4 and cols[3].endswith(f":{port}"):
            addrs.append(cols[3])
    return addrs


def main():
    if len(sys.argv) != 2:
        print("uso: test_backend.py <binario>", file=sys.stderr)
        return 2
    binary = sys.argv[1]

    fd, machines = tempfile.mkstemp(suffix=".conf")
    with os.fdopen(fd, "w") as f:
        f.write("1 18fa502d-2427-4485-a3a6-0b3f64585634\n")

    try:
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

            if shutil.which("ss"):
                addrs = listening_addresses(port)
                assert addrs and all(a == f"127.0.0.1:{port}" for a in addrs), addrs
                print("PASS: por padrão o servidor escuta só em 127.0.0.1")
            else:
                print("SKIP: comando 'ss' indisponível; bind padrão não verificado")

            proc.send_signal(signal.SIGTERM)
            code = proc.wait(timeout=5)
            assert code == 0, code
            print("PASS: SIGTERM -> encerramento limpo (código 0)")
        finally:
            if proc.poll() is None:
                proc.kill()
                proc.wait()

        r = subprocess.run([binary, "--port", str(free_port()), "--machines", machines,
                            "--bind", "999.1.1.1"],
                           capture_output=True, text=True, timeout=10)
        assert r.returncode == 1 and "endereço inválido" in r.stderr, (r.returncode, r.stderr)
        print("PASS: --bind inválido -> backend recusa iniciar")
    finally:
        os.unlink(machines)

    print("test_backend: todos os testes passaram.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
