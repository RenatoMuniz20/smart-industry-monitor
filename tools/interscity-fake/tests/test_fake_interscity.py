#!/usr/bin/env python3
"""Testes do InterSCity falso com as mesmas requisições do IoT Gateway e do backend.

Os corpos e caminhos seguem o código em C:
  - gateway: iot-gateway/src/interscity_client.c e interscity_payload.c
    (POST /adaptor/resources/{uuid}/data e consulta {"uuids":[uuid]} em data/last)
  - backend: backend/src/interscity_query.c
    (data/last com vários uuids; /collector/resources/{uuid}/data com start/end)
Não acessa a rede além de 127.0.0.1.
"""
import json
import os
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.request
from pathlib import Path

FAKE = Path(__file__).resolve().parents[1] / "fake_interscity.py"
UUID_A = "18fa502d-2427-4485-a3a6-0b3f64585634"
UUID_B = "c29d711f-1f60-44c4-91ea-d72f49343c4e"
UUID_C = "6c5496cf-1d67-4476-bcb9-e9243da7feb7"
UNKNOWN = "00000000-0000-4000-8000-000000000099"
CAPS = ["sim_motor_temperature", "sim_current", "sim_vibration_rms", "sim_rpm",
        "sim_operating_state", "sim_failure_within_24h"]
passed = 0


def ok(label):
    global passed
    passed += 1
    print(f"PASS: {label}")


def gateway_body(timestamp="2024-01-01T00:00:00", temperature=49.51, state="IDLE", failure=0):
    """Igual a expected_body() de iot-gateway/tests/test_interscity_gateway.py."""
    return {"data": {
        "sim_motor_temperature": [{"value": temperature, "timestamp": timestamp}],
        "sim_current": [{"value": 5.1, "timestamp": timestamp}],
        "sim_vibration_rms": [{"value": 0.81, "timestamp": timestamp}],
        "sim_rpm": [{"value": 860.9, "timestamp": timestamp}],
        "sim_operating_state": [{"value": state, "timestamp": timestamp}],
        "sim_failure_within_24h": [{"value": failure, "timestamp": timestamp}],
    }}


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def post(base, path, body=None, raw=None):
    data = raw if raw is not None else (b"" if body is None else json.dumps(body).encode())
    req = urllib.request.Request(base + path, data=data, method="POST",
                                 headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=5) as r:
            return r.status, json.loads(r.read() or b"null")
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read() or b"null")


def get(base, path):
    with urllib.request.urlopen(base + path, timeout=5) as r:
        return r.status, json.loads(r.read())


class Fake:
    def __init__(self, resources, *extra):
        self.port = free_port()
        self.base = f"http://127.0.0.1:{self.port}"
        self.proc = subprocess.Popen(
            [sys.executable, "-I", str(FAKE), "--port", str(self.port), "--resources", resources, *extra],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            if self.proc.poll() is not None:
                raise RuntimeError(f"fake terminou: {self.proc.stderr.read()}")
            try:
                get(self.base, "/__fake/health")
                return
            except OSError:
                time.sleep(0.05)
        raise RuntimeError("fake não respondeu")

    def stop(self):
        if self.proc.poll() is None:
            self.proc.send_signal(signal.SIGTERM)
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
        return self.proc.returncode


def main():
    tmp = Path(tempfile.mkdtemp(prefix="interscity-fake-"))
    resources = tmp / "machines.conf"
    resources.write_text(f"# comentário\n1 {UUID_A}\n2 {UUID_B}\n\n3 {UUID_C}\n")
    db = tmp / "fake.db"

    fake = Fake(str(resources), "--db", str(db))
    try:
        b = fake.base
        # Gateway: consulta antes do primeiro envio.
        status, body = post(b, "/collector/resources/data/last", {"uuids": [UUID_A]})
        assert (status, body) == (404, {"error": "Resource not found"}), (status, body)
        ok("Collector sem dados -> 404 Resource not found (como a instância real)")

        status, body = post(b, f"/adaptor/resources/{UUID_A}/data", gateway_body())
        assert (status, body) == (201, {}), (status, body)
        ok("envio do gateway (6 capabilities) -> 201 {}")

        status, body = post(b, "/collector/resources/data/last", {"uuids": [UUID_A]})
        assert status == 200, (status, body)
        res = body["resources"]
        assert len(res) == 1 and res[0]["uuid"] == UUID_A, res
        caps = res[0]["capabilities"]
        assert sorted(caps) == sorted(CAPS), caps
        assert caps["sim_motor_temperature"] == [{"value": 49.51, "date": "2024-01-01T00:00:00.000Z"}], caps
        assert caps["sim_operating_state"][0]["value"] == "IDLE"
        assert caps["sim_failure_within_24h"][0]["value"] == 0
        ok("data/last no formato que o gateway lê (uuid, capabilities, date ...000Z)")

        post(b, f"/adaptor/resources/{UUID_A}/data", gateway_body("2024-01-01 00:03:00", 40.58, "NORMAL", 1))
        post(b, f"/adaptor/resources/{UUID_A}/data", gateway_body("2024-01-01T00:03:00", 40.58, "NORMAL", 1))
        status, body = post(b, "/collector/resources/data/last", {"uuids": [UUID_A]})
        caps = body["resources"][0]["capabilities"]
        assert caps["sim_motor_temperature"] == [{"value": 40.58, "date": "2024-01-01T00:03:00.000Z"}], caps
        assert caps["sim_operating_state"][0]["value"] == "NORMAL"
        ok("data/last traz a amostra mais recente; aceita data com espaço ou T")

        # Backend: /api/machines/latest pede todos os uuids de uma vez.
        status, body = post(b, "/collector/resources/data/last", {"uuids": [UUID_A, UUID_B, UUID_C]})
        assert status == 200 and [r["uuid"] for r in body["resources"]] == [UUID_A], body
        ok("vários uuids: só os que têm dados aparecem (o backend completa com null)")

        # Backend: histórico.
        status, body = post(b, f"/collector/resources/{UUID_A}/data",
                            {"start_date": "2024-01-01T00:00:00", "end_date": "2024-01-01T00:03:00"})
        assert status == 200, (status, body)
        temps = body["resources"][0]["capabilities"]["sim_motor_temperature"]
        assert [s["date"] for s in temps] == ["2024-01-01T00:00:00.000Z"] + ["2024-01-01T00:03:00.000Z"] * 2, temps
        ok("histórico com início e fim inclusivos, em ordem; envio repetido grava cópia")

        status, body = post(b, f"/collector/resources/{UUID_A}/data",
                            {"start_date": "2024-01-01T00:01:00", "end_date": "2024-01-01T00:02:00"})
        assert status == 200 and body["resources"][0]["capabilities"] == {}, body
        status, body = post(b, f"/collector/resources/{UUID_B}/data",
                            {"start_date": "2024-01-01T00:00:00", "end_date": "2024-01-01T01:00:00"})
        assert (status, body) == (404, {"error": "Resource not found"}), (status, body)
        ok("janela sem amostras -> capabilities vazio; recurso sem dados -> 404")

        status, body = post(b, f"/collector/resources/{UUID_A}/data/last", {"capabilities": ["sim_rpm"]})
        assert status == 200 and list(body["resources"][0]["capabilities"]) == ["sim_rpm"], body
        ok("filtro de capabilities e rota /{uuid}/data/last")

        status, body = post(b, f"/adaptor/resources/{UNKNOWN}/data", gateway_body())
        assert (status, body) == (201, {}), (status, body)
        status, _ = post(b, "/collector/resources/data/last", {"uuids": [UNKNOWN]})
        assert status == 404, status
        ok("uuid fora do machines.conf: 201 e descarte silencioso (como a instância real)")

        bad = [
            (None, b"{nao-json"),
            ({"dados": {}}, None),
            ({"data": {}}, None),
            ({"data": {"sim-x": [{"value": 1, "timestamp": "2024-01-01T00:00:00"}]}}, None),
            ({"data": {"sim_rpm": [{"timestamp": "2024-01-01T00:00:00"}]}}, None),
            ({"data": {"sim_rpm": [{"value": 1, "timestamp": "2024-02-30T00:00:00"}]}}, None),
            ({"data": {"sim_rpm": [{"value": [1], "timestamp": "2024-01-01T00:00:00"}]}}, None),
        ]
        for body, raw in bad:
            status, _ = post(b, f"/adaptor/resources/{UUID_A}/data", body, raw)
            assert status == 400, (body, raw, status)
        ok(f"{len(bad)} corpos inválidos -> 400 (o gateway bloqueia, não repete)")

        status, body = post(b, f"/adaptor/resources/{UUID_B}/data",
                            {"data": json.dumps({"sim_rpm": [{"value": 1.0, "timestamp": "2024-01-01T00:00:00"}]})})
        assert status == 201, (status, body)
        ok("data como texto JSON também é aceito (tolerância da instância real)")

        status, body = post(b, "/collector/resources/data/last", {"uuids": "x"})
        assert status == 400, status
        status, _ = post(b, "/nao-existe", {})
        assert status == 404, status
        ok("consulta sem lista de uuids -> 400; rota desconhecida -> 404")

        status, stats = get(b, "/__fake/stats")
        assert stats["adaptor_created"] == 5 and stats["adaptor_unknown_uuid"] == 1, stats
        assert stats["adaptor_bad_request"] == len(bad) and stats["samples_stored"] == 19, stats
        ok("estatísticas contam envios, descartes, erros e amostras")

        status, _ = post(b, "/__fake/down")
        status, body = post(b, f"/adaptor/resources/{UUID_A}/data", gateway_body())
        assert status == 503, status
        status, _ = post(b, "/collector/resources/data/last", {"uuids": [UUID_A]})
        assert status == 503, status
        post(b, "/__fake/up")
        status, _ = post(b, f"/adaptor/resources/{UUID_A}/data", gateway_body("2024-01-01T00:06:00"))
        assert status == 201, status
        ok("/__fake/down -> 503 (o gateway reagenda); /__fake/up volta ao normal")

        assert fake.stop() == 0
        ok("SIGTERM encerra com código 0")

        fake = Fake(str(resources), "--db", str(db))
        status, body = post(fake.base, "/collector/resources/data/last", {"uuids": [UUID_A]})
        assert body["resources"][0]["capabilities"]["sim_rpm"][0]["date"] == "2024-01-01T00:06:00.000Z", body
        ok("dados persistem no arquivo SQLite após reiniciar")

        post(fake.base, "/__fake/reset")
        status, _ = post(fake.base, "/collector/resources/data/last", {"uuids": [UUID_A]})
        assert status == 404, status
        ok("/__fake/reset apaga amostras e zera estatísticas")
        fake.stop()

        # Falhas e carga simuladas.
        fake = Fake(str(resources), "--fail-rate", "1")
        status, _ = post(fake.base, f"/adaptor/resources/{UUID_A}/data", gateway_body())
        assert status == 503, status
        fake.stop()
        ok("--fail-rate 1 -> todo envio recebe 503")

        fake = Fake(str(resources), "--store-delay-ms", "600")
        post(fake.base, f"/adaptor/resources/{UUID_A}/data", gateway_body())
        status, _ = post(fake.base, "/collector/resources/data/last", {"uuids": [UUID_A]})
        assert status == 404, status
        time.sleep(0.8)
        status, _ = post(fake.base, "/collector/resources/data/last", {"uuids": [UUID_A]})
        assert status == 200, status
        fake.stop()
        ok("--store-delay-ms: 201 antes de a amostra aparecer no Collector")

        fake = Fake(str(resources), "--latency-ms", "300", "--max-inflight", "1")
        started = time.monotonic()
        results = []
        threads = [threading.Thread(target=lambda: results.append(
            post(fake.base, f"/adaptor/resources/{UUID_A}/data", gateway_body())[0])) for _ in range(3)]
        for t in threads:
            t.start()
        for t in threads:
            t.join()
        elapsed = time.monotonic() - started
        assert sorted(results) == [201, 503, 503], results
        assert elapsed >= 0.3, elapsed
        _, stats = get(fake.base, "/__fake/stats")
        assert stats["adaptor_overload_503"] == 2 and stats["inflight_peak"] == 1, stats
        fake.stop()
        ok("--latency-ms atrasa; --max-inflight 1 recusa as simultâneas com 503")

        bad_conf = tmp / "ruim.conf"
        bad_conf.write_text("1 uuid-invalido\n")
        r = subprocess.run([sys.executable, "-I", str(FAKE), "--resources", str(bad_conf)],
                           capture_output=True, text=True, timeout=10)
        assert r.returncode != 0 and "esperado" in r.stderr, r
        r = subprocess.run([sys.executable, "-I", str(FAKE), "--resources", str(resources), "--fail-rate", "2"],
                           capture_output=True, text=True, timeout=10)
        assert r.returncode == 2, r
        ok("machines.conf inválido ou opção fora da faixa impedem a inicialização")
    finally:
        fake.stop()
        for f in tmp.iterdir():
            f.unlink()
        tmp.rmdir()

    print(f"Todos os {passed} testes do InterSCity falso passaram.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
