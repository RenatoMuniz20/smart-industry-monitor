#!/usr/bin/env python3
"""Testa o cliente HTTP do Gateway contra um InterSCity falso local.

Sobe um servidor HTTP/1.1 em 127.0.0.1 (somente biblioteca padrão), executa
build/test_interscity_client e confere cada requisição recebida: método,
caminho, cabeçalhos e corpo. Não acessa a rede externa nem a instância real.
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
RECEIVED = []
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
            if suffix == '999':
                time.sleep(2)
                self.respond(201)
            elif suffix in ('400', '500'):
                self.respond(int(suffix), b'{"error":"simulado"}')
            else:
                self.respond(201)
        except (BrokenPipeError, ConnectionResetError):
            pass  # o cliente desistiu (teste de timeout)


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
