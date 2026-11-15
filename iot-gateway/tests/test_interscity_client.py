#!/usr/bin/env python3
"""Testa o cliente HTTP do Gateway contra um InterSCity falso local.

Sobe um servidor HTTP/1.1 em 127.0.0.1 (somente biblioteca padrão), executa
build/test_interscity_client e confere cada requisição recebida: método,
caminho, cabeçalhos e corpo. Não acessa a rede externa nem a instância real.

SIM-08: o falso Data Collector responde em /collector/resources/data/last.
Essas consultas ficam em COLLECTOR_QUERIES, separadas das publicações (RECEIVED).
Sufixos lentos (999 e 997) respondem 201 depois de SLOW_DELAY_S segundos, para
forçar timeout no cliente (o limite usado nos testes de integração é 2 s).
"""
import json
import os
import socket
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

CLIENT_TEST = os.environ.get('SIM_INTERSCITY_CLIENT_TEST', './build/test_interscity_client')
PREFIX = '/interscity_lh'  # simula o prefixo da instância do LSDi
COLLECTOR_PATH = PREFIX + '/collector/resources/data/last'
SLOW_SUFFIXES = ('999', '997')  # respondem depois do timeout do cliente
SLOW_DELAY_S = 4                # folga: timeout de 2 s nos testes de integração (2 s não era confiável)
RECEIVED = []           # publicações no Adaptor
COLLECTOR_QUERIES = []  # consultas ao Data Collector (uuid de cada uma)
LOCK = threading.Lock()


class MockAdaptor(BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'  # permite reaproveitar a conexão

    def log_message(self, *args):  # silencia o log padrão
        pass

    def respond(self, status, body=b'{}'):
        self.send_response(status)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        length = int(self.headers.get('Content-Length', '0'))
        body = self.rfile.read(length)
        if self.path == COLLECTOR_PATH:
            self.collector(body)
            return
        with LOCK:
            RECEIVED.append({
                'path': self.path,
                'content_type': self.headers.get('Content-Type'),
                'expect': self.headers.get('Expect'),
                'user_agent': self.headers.get('User-Agent'),
                'body': body.decode('utf-8'),
            })
        suffix = self.path.rstrip('/').split('/')[-2][-3:] if self.path.count('/') >= 4 else ''
        try:
            if suffix in SLOW_SUFFIXES:
                time.sleep(SLOW_DELAY_S)
                self.respond(201)
            elif suffix in ('400', '500'):
                self.respond(int(suffix), b'{"error":"simulado"}')
            else:
                self.respond(201)
        except (BrokenPipeError, ConnectionResetError):
            pass  # o cliente desistiu (teste de timeout)

    def collector(self, body):
        uuid = json.loads(body.decode('utf-8'))['uuids'][0]
        with LOCK:
            COLLECTOR_QUERIES.append(uuid)
        try:
            if uuid[-3:] == '999':  # armazenada apesar do timeout do Adaptor
                payload = {'resources': [{'id': uuid, 'capabilities': {'sim_operating_state': [
                    {'value': 'IDLE', 'date': '2024-01-01T00:02:00.000Z'}]}}]}
                self.respond(200, json.dumps(payload).encode('utf-8'))
            else:
                self.respond(404, b'{"error":"Resource not found"}')
        except (BrokenPipeError, ConnectionResetError):
            pass


def closed_port():
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]


def main():
    server = ThreadingHTTPServer(('127.0.0.1', 0), MockAdaptor)
    server.daemon_threads = True
    threading.Thread(target=server.serve_forever, daemon=True).start()
    port = server.server_address[1]
    env = dict(os.environ,
               SIM_INTERSCITY_MOCK_URL=f'http://127.0.0.1:{port}{PREFIX}',
               SIM_INTERSCITY_CLOSED_URL=f'http://127.0.0.1:{closed_port()}')
    try:
        result = subprocess.run([CLIENT_TEST], env=env, timeout=30)
    finally:
        server.shutdown()
        server.server_close()
    if result.returncode != 0:
        print(f'FAIL: {CLIENT_TEST} terminou com código {result.returncode}', file=sys.stderr)
        return 1

    def path(uuid_suffix):
        return f'{PREFIX}/adaptor/resources/00000000-0000-4000-8000-000000000{uuid_suffix}/data'

    expected = [
        (path('201'), '{"data":{"n":[{"value":1}]}}'),
        (path('201'), '{"data":{"n":[{"value":2}]}}'),
        (path('400'), '{"bad":true}'),
        (path('500'), '{"data":{}}'),
        (path('201'), '{"data":{"n":[{"value":3}]}}'),
        (path('999'), '{"data":{"slow":[{"value":1}]}}'),
        (path('201'), '{"data":{"n":[{"value":4}]}}'),
    ]
    with LOCK:
        received = list(RECEIVED)
    errors = []
    if [(r['path'], r['body']) for r in received] != expected:
        errors.append('requisições recebidas diferentes das esperadas:\n'
                      + json.dumps(received, indent=2, ensure_ascii=False))
    for r in received:
        if r['content_type'] != 'application/json':
            errors.append(f"Content-Type inesperado: {r['content_type']!r}")
        if r['expect'] is not None:
            errors.append(f"cabeçalho Expect enviado: {r['expect']!r}")
        if r['user_agent'] != 'smart-industry-gateway':
            errors.append(f"User-Agent inesperado: {r['user_agent']!r}")
        json.loads(r['body'])  # todo corpo enviado é JSON válido
    if errors:
        print('FAIL: ' + '\n'.join(errors), file=sys.stderr)
        return 1
    print(f'PASS: cliente HTTP com {len(received)} requisições conferidas '
          '(201, 400, 500, timeout, porta fechada e reuso de conexão)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
