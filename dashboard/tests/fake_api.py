#!/usr/bin/env python3
"""API falsa do backend para desenvolver e testar o dashboard (SIM-07).

Responde /api/health, /api/machines/latest e /api/machines/{id}/history no
mesmo formato do backend em C e serve os arquivos de dashboard/. Os dados vêm
do CSV do dataset: o "relógio" do dataset anda 3 minutos a cada 5 s reais,
como se o replayer estivesse enviando. Não acessa o InterSCity.

Uso:
  python3 dashboard/tests/fake_api.py [--port 8090] [--start 2024-01-02T16:00:00]
                                      [--no-data 13] [--step-seconds 5]
Controles (para testes): GET /__fake/down faz a API responder 502;
GET /__fake/up volta ao normal.
"""
import argparse
import csv
import json
import mimetypes
import re
import time
from datetime import datetime, timedelta, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse

ROOT = Path(__file__).resolve().parents[2]
WEB = ROOT / "dashboard"
CSV = ROOT / "data" / "predictive_maintenance_v3.csv"
MACHINES = [1, 2, 3, 6, 7, 8, 9, 11, 12, 13]
STATES = {"idle": "IDLE", "normal": "NORMAL", "peak": "PEAK"}
FIELDS = {
    "operating_state": "state",
    "motor_temperature": "temperature",
    "current": "current",
    "vibration_rms": "vibration_rms",
    "rpm": "rpm",
    "failure_within_24h": "failure",
}
TYPES = {".html": "text/html; charset=utf-8", ".css": "text/css; charset=utf-8",
         ".js": "text/javascript; charset=utf-8"}


def iso(dt):
    return dt.strftime("%Y-%m-%dT%H:%M:%S.000Z")


def number(text):
    """Campos vazios do dataset viram None, como no replayer (o gateway não os envia)."""
    text = (text or "").strip()
    return float(text) if text else None


def load():
    data = {m: [] for m in MACHINES}
    with open(CSV, newline="") as f:
        for row in csv.DictReader(f):
            m = int(row["machine_id"])
            if m not in data:
                continue
            data[m].append({
                "t": datetime.strptime(row["timestamp"], "%Y-%m-%d %H:%M:%S").replace(tzinfo=timezone.utc),
                "state": STATES.get(row["operating_mode"].strip(), "UNKNOWN"),
                "temperature": number(row["temperature_motor"]),
                "current": number(row["current_phase_avg"]),
                "vibration_rms": number(row["vibration_rms"]),
                "rpm": number(row["rpm"]),
                "failure": None if number(row["failure_within_24h"]) is None else int(number(row["failure_within_24h"])),
            })
    for rows in data.values():
        rows.sort(key=lambda r: r["t"])
    return data


class Fake:
    def __init__(self, args):
        self.data = load()
        self.no_data = set(args.no_data)
        self.base = datetime.fromisoformat(args.start).replace(tzinfo=timezone.utc)
        self.step = args.step_seconds
        self.started = time.monotonic()
        self.down = False

    def now(self):
        ticks = int((time.monotonic() - self.started) / self.step)
        return self.base + timedelta(minutes=3 * ticks)

    def rows(self, machine, start=None, end=None):
        if machine in self.no_data:
            return []
        limit = self.now()
        return [r for r in self.data[machine]
                if r["t"] <= limit and (start is None or r["t"] >= start) and (end is None or r["t"] <= end)]

    def latest(self):
        out = []
        for m in MACHINES:
            rows = self.rows(m)
            item = {"machine_id": m, "uuid": f"00000000-0000-4000-8000-{m:012d}"}
            for api_key, key in FIELDS.items():
                # Como no InterSCity: a última leitura que tem esse campo.
                last = next((r for r in reversed(rows) if r[key] is not None), None)
                item[api_key] = None if last is None else {"value": last[key], "timestamp": iso(last["t"])}
            out.append(item)
        return {"machines": out}

    def history(self, machine, start, end):
        rows = self.rows(machine, start, end)
        series = {api_key: [{"timestamp": iso(r["t"]), "value": r[key]} for r in rows if r[key] is not None]
                  for api_key, key in FIELDS.items()}
        return {"machine_id": machine, "uuid": f"00000000-0000-4000-8000-{machine:012d}",
                "start": iso(start), "end": iso(end), "series": series}


def parse_time(text):
    if not text or not re.fullmatch(r"\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}", text):
        return None
    try:
        return datetime.fromisoformat(text).replace(tzinfo=timezone.utc)
    except ValueError:
        return None


def make_handler(fake):
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def send(self, status, body, ctype="application/json"):
            raw = body if isinstance(body, bytes) else json.dumps(body).encode()
            self.send_response(status)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(raw)))
            self.send_header("Cache-Control", "no-cache")
            self.end_headers()
            self.wfile.write(raw)

        def do_GET(self):
            url = urlparse(self.path)
            path = url.path
            if path == "/__fake/down":
                fake.down = True
                return self.send(200, {"down": True})
            if path == "/__fake/up":
                fake.down = False
                return self.send(200, {"down": False})
            if path.startswith("/api/"):
                return self.api(path, parse_qs(url.query))
            name = "index.html" if path == "/" else path.lstrip("/")
            file = (WEB / name).resolve()
            if WEB not in file.parents or not file.is_file() or file.suffix not in TYPES:
                return self.send(404, {"error": "não encontrado"})
            return self.send(200, file.read_bytes(), TYPES[file.suffix])

        def api(self, path, query):
            if path == "/api/health":
                return self.send(200, {"status": "ok"})
            if fake.down:
                return self.send(502, {"error": "InterSCity indisponível"})
            if path == "/api/machines/latest":
                return self.send(200, fake.latest())
            m = re.fullmatch(r"/api/machines/(\d+)/history", path)
            if m:
                machine = int(m.group(1))
                if machine not in MACHINES:
                    return self.send(404, {"error": "máquina desconhecida"})
                start = parse_time(query.get("start", [None])[0])
                end = parse_time(query.get("end", [None])[0])
                if start is None or end is None or start > end:
                    return self.send(400, {"error": "datas inválidas"})
                body = fake.history(machine, start, end)
                if len(json.dumps(body)) > 1024 * 1024:
                    return self.send(413, {"error": "resposta grande demais"})
                return self.send(200, body)
            return self.send(404, {"error": "não encontrado"})

    return Handler


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", type=int, default=8090)
    parser.add_argument("--start", default="2024-01-02T16:00:00", help="horário do dataset (UTC) no início")
    parser.add_argument("--no-data", type=int, nargs="*", default=[], help="máquinas sem leituras")
    parser.add_argument("--step-seconds", type=float, default=5.0, help="segundos reais por amostra de 3 min")
    args = parser.parse_args()
    fake = Fake(args)
    server = ThreadingHTTPServer(("127.0.0.1", args.port), make_handler(fake))
    print(f"API falsa + dashboard em http://127.0.0.1:{args.port}/ (dataset a partir de {args.start} UTC)", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
