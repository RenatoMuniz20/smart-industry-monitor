#!/usr/bin/env python3
"""InterSCity falso: Resource Adaptor + Data Collector no formato da instância real.

Substitui a instância compartilhada do LSDi/UFMA em demonstrações e testes de
carga. Implementa só o que o IoT Gateway e o backend usam:

  POST /adaptor/resources/{uuid}/data           201 {}  (grava amostras)
  POST /collector/resources/data/last           {"uuids": [...]}
  POST /collector/resources/data                {"uuids": [...], "start_date", "end_date"}
  POST /collector/resources/{uuid}/data/last
  POST /collector/resources/{uuid}/data         {"start_date", "end_date"}

Comportamentos copiados da instância real (docs/sim-05-interscity.md):
  - O Adaptor responde 201 {} também para UUID desconhecido, mas descarta.
  - Envios repetidos gravam cópias (POST não é idempotente).
  - O Collector responde 404 {"error":"Resource not found"} quando nenhum
    recurso pedido tem dados.
  - "date" volta como AAAA-MM-DDTHH:MM:SS.000Z (UTC).

Para testes de carga e de falha (todos desligados por padrão):
  --latency-ms / --jitter-ms   atraso de cada resposta da API
  --store-delay-ms             amostra só aparece no Collector depois desse tempo
                               (o 201 real só confirma a fila do RabbitMQ)
  --fail-rate                  fração de envios ao Adaptor respondidos com 503
  --max-inflight               acima disso, 503 imediato (simula sobrecarga)

Controle e medição (fora da API do InterSCity):
  GET  /__fake/health   GET /__fake/stats
  POST /__fake/down     POST /__fake/up     POST /__fake/reset

Somente biblioteca padrão do Python. Não acessa a rede além de responder.
"""
import argparse
import json
import os
import random
import re
import signal
import sqlite3
import sys
import threading
import time
from collections import deque
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

MAX_BODY = 1024 * 1024
UUID_RE = re.compile(r"[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}")
CAPABILITY_RE = re.compile(r"[A-Za-z0-9_]{1,64}")
TIME_RE = re.compile(r"(\d{4})-(\d{2})-(\d{2})[T ](\d{2}):(\d{2}):(\d{2})(?:\.\d+)?Z?")
ADAPTOR_RE = re.compile(r"/adaptor/resources/([^/]+)/data")
COLLECTOR_ONE_RE = re.compile(r"/collector/resources/([^/]+)/data(/last)?")
NOT_FOUND = {"error": "Resource not found"}


class BadRequest(Exception):
    pass


def normalize_date(text):
    """'2024-01-01 00:21:00' ou '2024-01-01T00:21:00[.fff][Z]' -> '2024-01-01T00:21:00.000Z'."""
    if not isinstance(text, str):
        raise BadRequest("timestamp deve ser texto")
    m = TIME_RE.fullmatch(text.strip())
    if not m:
        raise BadRequest(f"timestamp inválido: {text[:40]}")
    year, month, day, hour, minute, second = (int(g) for g in m.groups())
    try:
        datetime(year, month, day, hour, minute, second)
    except ValueError:
        raise BadRequest(f"timestamp inválido: {text[:40]}")
    return f"{year:04d}-{month:02d}-{day:02d}T{hour:02d}:{minute:02d}:{second:02d}.000Z"


def now_date():
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%S.000Z")


def load_resources(path):
    """Mesmo formato de config/interscity/machines.conf: '<machine_id> <uuid>' por linha."""
    uuids = set()
    with open(path) as f:
        for number, line in enumerate(f, 1):
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split()
            if len(parts) != 2 or not UUID_RE.fullmatch(parts[1]):
                raise SystemExit(f"{path}:{number}: esperado '<machine_id> <uuid>'")
            uuids.add(parts[1])
    if not uuids:
        raise SystemExit(f"{path}: nenhum recurso")
    return uuids


class Store:
    """Amostras em SQLite (arquivo ou memória). Um lock serializa o acesso."""

    def __init__(self, path):
        self.lock = threading.Lock()
        self.db = sqlite3.connect(path, check_same_thread=False, isolation_level=None)
        if path != ":memory:":
            self.db.execute("PRAGMA journal_mode=WAL")
            self.db.execute("PRAGMA synchronous=NORMAL")
        self.db.execute(
            "CREATE TABLE IF NOT EXISTS samples ("
            " id INTEGER PRIMARY KEY, uuid TEXT NOT NULL, capability TEXT NOT NULL,"
            " date TEXT NOT NULL, value TEXT NOT NULL, visible_at REAL NOT NULL)")
        self.db.execute("CREATE INDEX IF NOT EXISTS samples_lookup ON samples(uuid, capability, date)")
        self.capabilities = {row[0] for row in self.db.execute("SELECT DISTINCT capability FROM samples")}

    def add(self, uuid, rows, visible_at):
        with self.lock:
            self.db.execute("BEGIN")
            self.db.executemany(
                "INSERT INTO samples(uuid, capability, date, value, visible_at) VALUES (?,?,?,?,?)",
                [(uuid, cap, date, value, visible_at) for cap, date, value in rows])
            self.db.execute("COMMIT")
            self.capabilities.update(cap for cap, _, _ in rows)

    def query(self, uuids, capabilities, start, end, last):
        """{uuid: {capability: [{"value", "date"}]}} só com séries que têm amostras visíveis.

        Uma consulta por (uuid, capability), sempre pelo índice (uuid, capability, date),
        para continuar rápido com centenas de milhares de amostras."""
        now = time.time()
        out = {}
        with self.lock:
            caps = list(capabilities) if capabilities else sorted(self.capabilities)
            for uuid in uuids:
                for cap in caps:
                    if last:
                        rows = self.db.execute(
                            "SELECT date, value FROM samples WHERE uuid=? AND capability=? AND visible_at<=?"
                            " AND date>=? AND date<=? ORDER BY date DESC, id DESC LIMIT 1",
                            (uuid, cap, now, start or "", end or "~")).fetchall()
                    else:
                        rows = self.db.execute(
                            "SELECT date, value FROM samples WHERE uuid=? AND capability=? AND visible_at<=?"
                            " AND date>=? AND date<=? ORDER BY date, id",
                            (uuid, cap, now, start or "", end or "~")).fetchall()
                    if rows:
                        out.setdefault(uuid, {})[cap] = [{"value": json.loads(v), "date": d} for d, v in rows]
        return out

    def with_data(self, uuids):
        """Recursos (entre os pedidos) que têm alguma amostra visível."""
        now = time.time()
        with self.lock:
            return {u for u in uuids if self.db.execute(
                "SELECT 1 FROM samples WHERE uuid=? AND visible_at<=? LIMIT 1", (u, now)).fetchone()}

    def count(self):
        with self.lock:
            return self.db.execute("SELECT COUNT(*) FROM samples").fetchone()[0]

    def reset(self):
        with self.lock:
            self.db.execute("DELETE FROM samples")


class Stats:
    COUNTERS = ("adaptor_requests", "adaptor_created", "adaptor_bad_request", "adaptor_unknown_uuid",
                "adaptor_injected_503", "adaptor_overload_503", "down_503", "samples_received",
                "collector_requests", "collector_not_found")

    def __init__(self):
        self.lock = threading.Lock()
        self.started = time.monotonic()
        self.values = dict.fromkeys(self.COUNTERS, 0)
        self.recent = deque()  # instantes dos 201 do Adaptor, para a taxa
        self.inflight = 0
        self.inflight_peak = 0

    def add(self, name, amount=1):
        with self.lock:
            self.values[name] += amount
            if name == "adaptor_created":
                self.recent.append(time.monotonic())

    def enter(self, limit):
        with self.lock:
            if limit and self.inflight >= limit:
                return False
            self.inflight += 1
            self.inflight_peak = max(self.inflight_peak, self.inflight)
            return True

    def leave(self):
        with self.lock:
            self.inflight -= 1

    def snapshot(self):
        with self.lock:
            now = time.monotonic()
            while self.recent and now - self.recent[0] > 10:
                self.recent.popleft()
            data = dict(self.values)
            data["created_per_second_10s"] = round(len(self.recent) / 10, 2)
            data["inflight"] = self.inflight
            data["inflight_peak"] = self.inflight_peak
            data["uptime_s"] = round(now - self.started, 1)
            return data

    def reset(self):
        with self.lock:
            self.values = dict.fromkeys(self.COUNTERS, 0)
            self.recent.clear()
            self.inflight_peak = self.inflight


class Fake:
    def __init__(self, args):
        self.args = args
        self.resources = load_resources(args.resources)
        self.store = Store(args.db)
        self.stats = Stats()
        self.down = False
        self.random = random.Random(args.seed)
        self.random_lock = threading.Lock()

    def chance(self):
        with self.random_lock:
            return self.random.random()

    def delay(self):
        total = self.args.latency_ms
        if self.args.jitter_ms:
            with self.random_lock:
                total += self.random.uniform(0, self.args.jitter_ms)
        if total > 0:
            time.sleep(total / 1000)

    # ---- Resource Adaptor ----
    def adaptor(self, uuid, body):
        self.stats.add("adaptor_requests")
        if self.args.fail_rate and self.chance() < self.args.fail_rate:
            self.stats.add("adaptor_injected_503")
            return 503, {"error": "falha simulada"}
        if not isinstance(body, dict) or "data" not in body:
            self.stats.add("adaptor_bad_request")
            return 400, {"error": "corpo deve ser {\"data\": {...}}"}
        data = body["data"]
        if isinstance(data, str):  # a instância real aceita data como texto JSON
            try:
                data = json.loads(data)
            except ValueError:
                data = None
        if not isinstance(data, dict) or not data:
            self.stats.add("adaptor_bad_request")
            return 400, {"error": "data deve ser um objeto com capabilities"}
        rows = []
        try:
            for cap, samples in data.items():
                if not CAPABILITY_RE.fullmatch(cap) or not isinstance(samples, list) or not samples:
                    raise BadRequest(f"capability inválida: {str(cap)[:40]}")
                for sample in samples:
                    if not isinstance(sample, dict) or "value" not in sample:
                        raise BadRequest(f"amostra sem value em {cap}")
                    value = sample["value"]
                    if isinstance(value, (dict, list)):
                        raise BadRequest(f"value deve ser número, texto ou booleano em {cap}")
                    date = normalize_date(sample["timestamp"]) if "timestamp" in sample else now_date()
                    rows.append((cap, date, json.dumps(value)))
        except BadRequest as err:
            self.stats.add("adaptor_bad_request")
            return 400, {"error": str(err)}
        self.stats.add("samples_received", len(rows))
        if uuid not in self.resources:
            # Igual à instância real: aceita e descarta em silêncio.
            self.stats.add("adaptor_unknown_uuid")
        else:
            self.store.add(uuid, rows, time.time() + self.args.store_delay_ms / 1000)
        self.stats.add("adaptor_created")
        return 201, {}

    # ---- Data Collector ----
    def collector(self, uuids, body, last):
        self.stats.add("collector_requests")
        body = body if isinstance(body, dict) else {}
        caps = body.get("capabilities") or []
        if not isinstance(caps, list) or not all(isinstance(c, str) for c in caps):
            return 400, {"error": "capabilities deve ser lista de nomes"}
        try:
            start = normalize_date(body["start_date"]) if body.get("start_date") else None
            end = normalize_date(body["end_date"]) if body.get("end_date") else None
        except BadRequest as err:
            return 400, {"error": str(err)}
        uuids = [u for u in dict.fromkeys(uuids) if isinstance(u, str) and UUID_RE.fullmatch(u)]
        present = self.store.with_data(uuids) if uuids else set()
        if not present:
            self.stats.add("collector_not_found")
            return 404, NOT_FOUND
        found = self.store.query(uuids, caps, start, end, last)
        resources = [{"uuid": u, "capabilities": found.get(u, {})} for u in uuids if u in present]
        return 200, {"resources": resources}


def make_handler(fake):
    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"  # mantém a conexão, como o libcurl espera
        # Cabeçalho e corpo saem num único envio TCP; separados, o ACK atrasado
        # do cliente custava cerca de 40 ms por resposta.
        wbufsize = 64 * 1024
        server_version = "interscity-fake"

        def log_message(self, fmt, *args):
            if fake.args.verbose:
                sys.stderr.write("%s %s\n" % (self.address_string(), fmt % args))

        def reply(self, status, payload):
            raw = json.dumps(payload, ensure_ascii=False).encode()
            self.send_response(status)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Content-Length", str(len(raw)))
            self.end_headers()
            self.wfile.write(raw)

        def read_body(self):
            length = self.headers.get("Content-Length")
            if length is None:
                return None, None
            try:
                length = int(length)
            except ValueError:
                return 400, {"error": "Content-Length inválido"}
            if length < 0 or length > MAX_BODY:
                self.close_connection = True
                return 413, {"error": "corpo grande demais"}
            raw = self.rfile.read(length)
            if not raw.strip():
                return None, None
            try:
                return None, json.loads(raw)
            except ValueError:
                return 400, {"error": "JSON inválido"}

        def do_GET(self):
            path = self.path.split("?", 1)[0]
            if path == "/__fake/health":
                return self.reply(200, {"status": "ok"})
            if path == "/__fake/stats":
                data = fake.stats.snapshot()
                data.update(samples_stored=fake.store.count(), down=fake.down, resources=len(fake.resources),
                            config={k: getattr(fake.args, k) for k in
                                    ("latency_ms", "jitter_ms", "store_delay_ms", "fail_rate", "max_inflight")})
                return self.reply(200, data)
            return self.reply(404, {"error": "Not found"})

        def do_POST(self):
            path = self.path.split("?", 1)[0]
            status, body = self.read_body()
            if status is not None:
                if path.startswith("/adaptor/"):
                    fake.stats.add("adaptor_requests")
                    fake.stats.add("adaptor_bad_request")
                return self.reply(status, body)
            if path.startswith("/__fake/"):
                return self.control(path)
            if not fake.stats.enter(fake.args.max_inflight):
                if path.startswith("/adaptor/"):
                    fake.stats.add("adaptor_requests")
                    fake.stats.add("adaptor_overload_503")
                return self.reply(503, {"error": "sobrecarga simulada"})
            try:
                fake.delay()
                if fake.down:
                    fake.stats.add("down_503")
                    return self.reply(503, {"error": "InterSCity falso fora do ar (simulado)"})
                status, payload = self.route(path, body)
                return self.reply(status, payload)
            finally:
                fake.stats.leave()

        def route(self, path, body):
            m = ADAPTOR_RE.fullmatch(path)
            if m:
                if not UUID_RE.fullmatch(m.group(1)):
                    return 404, {"error": "Not found"}
                return fake.adaptor(m.group(1), body)
            if path in ("/collector/resources/data/last", "/collector/resources/data"):
                uuids = body.get("uuids") if isinstance(body, dict) else None
                if not isinstance(uuids, list):
                    return 400, {"error": "informe uuids"}
                return fake.collector(uuids, body, path.endswith("/last"))
            m = COLLECTOR_ONE_RE.fullmatch(path)
            if m:
                return fake.collector([m.group(1)], body, m.group(2) is not None)
            return 404, {"error": "Not found"}

        def control(self, path):
            if path == "/__fake/down":
                fake.down = True
            elif path == "/__fake/up":
                fake.down = False
            elif path == "/__fake/reset":
                fake.store.reset()
                fake.stats.reset()
            else:
                return self.reply(404, {"error": "Not found"})
            return self.reply(200, {"down": fake.down, "samples_stored": fake.store.count()})

    return Handler


def env_default(name, fallback):
    return os.environ.get(name, fallback)


def parse_args(argv=None):
    p = argparse.ArgumentParser(description="InterSCity falso (Adaptor + Data Collector).")
    p.add_argument("--host", default=env_default("FAKE_HOST", "127.0.0.1"))
    p.add_argument("--port", type=int, default=int(env_default("FAKE_PORT", "8000")))
    p.add_argument("--resources", default=env_default("FAKE_RESOURCES", "config/interscity/machines.conf"),
                   help="arquivo <machine_id> <uuid>; UUIDs fora dele são descartados")
    p.add_argument("--db", default=env_default("FAKE_DB", ":memory:"), help="arquivo SQLite (padrão: memória)")
    p.add_argument("--latency-ms", type=float, default=float(env_default("FAKE_LATENCY_MS", "0")))
    p.add_argument("--jitter-ms", type=float, default=float(env_default("FAKE_JITTER_MS", "0")))
    p.add_argument("--store-delay-ms", type=float, default=float(env_default("FAKE_STORE_DELAY_MS", "0")))
    p.add_argument("--fail-rate", type=float, default=float(env_default("FAKE_FAIL_RATE", "0")))
    p.add_argument("--max-inflight", type=int, default=int(env_default("FAKE_MAX_INFLIGHT", "0")))
    p.add_argument("--seed", type=int, default=None)
    p.add_argument("--verbose", action="store_true")
    args = p.parse_args(argv)
    if not 0 <= args.fail_rate <= 1:
        p.error("--fail-rate deve estar entre 0 e 1")
    for name in ("latency_ms", "jitter_ms", "store_delay_ms", "max_inflight"):
        if getattr(args, name) < 0:
            p.error(f"--{name.replace('_', '-')} não pode ser negativo")
    return args


def main(argv=None):
    args = parse_args(argv)
    fake = Fake(args)
    ThreadingHTTPServer.daemon_threads = True
    ThreadingHTTPServer.request_queue_size = 256
    server = ThreadingHTTPServer((args.host, args.port), make_handler(fake))
    stop = threading.Event()

    def on_signal(signum, frame):
        if not stop.is_set():
            stop.set()
            threading.Thread(target=server.shutdown, daemon=True).start()

    signal.signal(signal.SIGTERM, on_signal)
    signal.signal(signal.SIGINT, on_signal)
    print(f"InterSCity falso em http://{args.host}:{server.server_address[1]} "
          f"({len(fake.resources)} recursos; db={args.db}; latência={args.latency_ms} ms; "
          f"falhas={args.fail_rate:.0%}; máx. simultâneas={args.max_inflight or 'sem limite'})", flush=True)
    try:
        server.serve_forever()
    finally:
        server.server_close()
        print("InterSCity falso encerrado.", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
