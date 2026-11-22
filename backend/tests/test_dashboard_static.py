#!/usr/bin/env python3
"""SIM-07: o backend serve o dashboard com --web-root, sem mudar a API.

Sobe o backend real com uma pasta temporária e confere tipos, cabeçalhos,
bloqueio de caminhos para fora da pasta, métodos e o comportamento sem a
opção. Não acessa o InterSCity (nenhuma rota /api/machines é chamada).
"""
import http.client
import json
import os
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time

INDEX = "<!doctype html><title>Smart Industry Monitor</title>"


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def raw(port, path, method="GET"):
    """Envia o caminho exatamente como escrito (sem normalização do cliente)."""
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=3)
    try:
        conn.putrequest(method, path, skip_accept_encoding=True)
        if method == "POST":
            conn.putheader("Content-Length", "0")
        conn.endheaders()
        resp = conn.getresponse()
        return resp.status, {k.lower(): v for k, v in resp.getheaders()}, resp.read()
    finally:
        conn.close()


def start(binary, port, machines, web_root=None):
    args = [binary, "--port", str(port), "--machines", machines,
            "--interscity-url", "http://127.0.0.1:9", "--interscity-timeout-ms", "500"]
    if web_root is not None:
        args += ["--web-root", web_root]
    return subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)


def wait_ready(port, proc, timeout=5.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            raise RuntimeError(f"backend terminou cedo: {proc.stderr.read()}")
        try:
            raw(port, "/api/health")
            return
        except OSError:
            time.sleep(0.1)
    raise RuntimeError("backend não respondeu a tempo")


def stop(proc):
    proc.send_signal(signal.SIGTERM)
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()


def main():
    if len(sys.argv) != 2:
        print("uso: test_dashboard_static.py <binario>", file=sys.stderr)
        return 2
    binary = sys.argv[1]
    base = tempfile.mkdtemp(prefix="sim07-web-")
    web = os.path.join(base, "dashboard")
    os.makedirs(web)
    with open(os.path.join(web, "index.html"), "w") as f:
        f.write(INDEX)
    with open(os.path.join(web, "app.js"), "w") as f:
        f.write("console.log('ok');")
    with open(os.path.join(web, "notas.txt"), "w") as f:
        f.write("texto")
    with open(os.path.join(base, "segredo.js"), "w") as f:
        f.write("secret")
    os.symlink(os.path.join(base, "segredo.js"), os.path.join(web, "link-fora.js"))
    machines = os.path.join(base, "machines.conf")
    with open(machines, "w") as f:
        f.write("1 18fa502d-2427-4485-a3a6-0b3f64585634\n")

    try:
        port = free_port()
        proc = start(binary, port, machines, web)
        try:
            wait_ready(port, proc)

            status, headers, body = raw(port, "/")
            assert status == 200 and body.decode() == INDEX, (status, body)
            assert headers["content-type"] == "text/html; charset=utf-8", headers
            assert headers.get("x-content-type-options") == "nosniff", headers
            assert headers.get("cache-control") == "no-cache", headers
            print("PASS: / entrega o index.html com tipo HTML e cabeçalhos de segurança")

            status, headers, body = raw(port, "/app.js")
            assert status == 200 and body == b"console.log('ok');", (status, body)
            assert headers["content-type"] == "text/javascript; charset=utf-8", headers
            print("PASS: /app.js com tipo JavaScript")

            status, headers, body = raw(port, "/", method="HEAD")
            assert status == 200 and body == b"", (status, body)
            assert headers["content-type"] == "text/html; charset=utf-8", headers
            print("PASS: HEAD / responde 200 sem corpo")

            blocked = ["/../segredo.js", "/%2e%2e/segredo.js", "/%2E%2E%2Fsegredo.js",
                       "/link-fora.js", "/notas.txt", "/nao-existe.js", "/.env"]
            for path in blocked:
                status, headers, _ = raw(port, path)
                assert status == 404, (path, status)
                assert headers["content-type"].startswith("application/json"), (path, headers)
            print(f"PASS: {len(blocked)} caminhos proibidos ou inexistentes -> 404 JSON")

            status, _, _ = raw(port, "/", method="POST")
            assert status == 405, status
            print("PASS: POST / -> 405")

            status, headers, body = raw(port, "/api/health")
            assert status == 200 and json.loads(body) == {"status": "ok"}, (status, body)
            assert headers["content-type"].startswith("application/json"), headers
            status, _, _ = raw(port, "/api/nao-existe")
            assert status == 404, status
            print("PASS: /api/ continua com a API (health 200, rota inexistente 404)")
        finally:
            stop(proc)
        out = proc.stdout.read()
        assert f"dashboard em http://127.0.0.1:{port}/" in out, out
        print("PASS: inicialização informa o endereço do dashboard")

        port = free_port()
        proc = start(binary, port, machines)
        try:
            wait_ready(port, proc)
            status, headers, _ = raw(port, "/")
            assert status == 404 and headers["content-type"].startswith("application/json"), status
            status, _, _ = raw(port, "/app.js")
            assert status == 404, status
            print("PASS: sem --web-root nada fora de /api/ é servido (comportamento anterior)")
        finally:
            stop(proc)

        result = subprocess.run(
            [binary, "--port", str(free_port()), "--machines", machines,
             "--web-root", os.path.join(base, "nao-existe")],
            capture_output=True, text=True, timeout=5)
        assert result.returncode == 1 and "pasta do dashboard inválida" in result.stderr, result
        print("PASS: --web-root inexistente impede a inicialização com mensagem clara")
    finally:
        shutil.rmtree(base, ignore_errors=True)
    print("Todos os testes do dashboard estático passaram.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
