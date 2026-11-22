#!/usr/bin/env bash
# Pipeline completo com o InterSCity falso, sem tocar na instância da UFMA.
#
#   scripts/fake-stack.sh up        sobe mosquitto, InterSCity falso, gateway e backend
#   scripts/fake-stack.sh replay    reproduz o dataset (LIMIT linhas por máquina, INTERVAL_MS entre envios)
#   scripts/fake-stack.sh stats     estatísticas do InterSCity falso e fila do gateway
#   scripts/fake-stack.sh logs      últimos logs
#   scripts/fake-stack.sh down      para tudo (mantém os dados)
#   scripts/fake-stack.sh wipe      para tudo e apaga os volumes DESTE projeto (fila e dados falsos)
#
# Variáveis opcionais do InterSCity falso: FAKE_LATENCY_MS, FAKE_JITTER_MS,
# FAKE_STORE_DELAY_MS, FAKE_FAIL_RATE, FAKE_MAX_INFLIGHT, FAKE_INTERSCITY_PORT.
# Porta do dashboard/backend no host: FAKE_BACKEND_PORT (padrão 8080).
# O mosquitto deste projeto não publica porta no host.
set -euo pipefail
cd "$(dirname "$0")/.."

# Projeto separado: volumes próprios (fila do gateway inclusive).
export COMPOSE_PROJECT_NAME=smart-industry-fake
# Sempre o falso, mesmo que a sessão tenha outro INTERSCITY_URL exportado.
export INTERSCITY_URL=http://interscity-fake:8000

compose() { docker compose -f docker-compose.yml -f docker-compose.fake.yml "$@"; }
fake_port="${FAKE_INTERSCITY_PORT:-8000}"
backend_port="${FAKE_BACKEND_PORT:-8080}"

gateway_running() {
    [ -n "$(compose ps -q --status running gateway 2>/dev/null)" ]
}

wait_gateway() {
    if ! gateway_running; then
        echo "O gateway não está rodando. Suba antes com: $0 up" >&2
        return 1
    fi
    for _ in $(seq 1 30); do
        if compose logs gateway 2>/dev/null | grep -q "Gateway pronto"; then
            echo "Gateway pronto (assinatura MQTT confirmada)."
            return 0
        fi
        sleep 1
    done
    echo "Gateway não ficou pronto em 30 s. Veja: $0 logs" >&2
    return 1
}

case "${1:-}" in
    up)
        if ! compose up -d --build mosquitto interscity-fake gateway backend || ! gateway_running; then
            echo >&2
            echo "Falha ao subir. Se o erro for 'ports are not available', a porta já está em uso:" >&2
            echo "  - ${backend_port} (backend): pare o Compose normal (docker compose down) ou use FAKE_BACKEND_PORT=8081" >&2
            echo "  - ${fake_port} (InterSCity falso): use FAKE_INTERSCITY_PORT=8001" >&2
            echo "Contêineres com portas publicadas: docker ps --format '{{.Names}} {{.Ports}}'" >&2
            exit 1
        fi
        wait_gateway
        echo "Dashboard: http://127.0.0.1:${backend_port}/   InterSCity falso: http://127.0.0.1:${fake_port}/__fake/stats"
        ;;
    replay)
        wait_gateway
        LIMIT="${LIMIT:-60}" INTERVAL_MS="${INTERVAL_MS:-1000}" compose --profile replay up -d --build replayer
        echo "Replayer iniciado (LIMIT=${LIMIT:-60} por máquina, INTERVAL_MS=${INTERVAL_MS:-1000}). Acompanhe com: $0 stats"
        ;;
    stats)
        echo "InterSCity falso:"
        curl -fsS "http://127.0.0.1:${fake_port}/__fake/stats"; echo
        # Itens confirmados (HTTP 201) saem da fila; fila vazia = tudo entregue.
        queue=$(compose exec -T gateway sqlite3 -readonly /data/telemetry.db \
            "SELECT status || ': ' || COUNT(*) FROM telemetry_queue GROUP BY status;" 2>&1) || true
        echo "Fila do gateway: ${queue:-vazia (tudo entregue)}"
        echo "Último timestamp entregue por máquina:"
        compose exec -T gateway sqlite3 -readonly /data/telemetry.db \
            "SELECT '  máquina ' || machine_id || ': ' || last_timestamp FROM delivered_cursor ORDER BY machine_id;" || true
        ;;
    logs)
        compose logs --tail="${TAIL:-30}" interscity-fake gateway backend
        ;;
    down)
        compose --profile replay down
        ;;
    wipe)
        compose --profile replay down -v
        ;;
    *)
        sed -n '2,13p' "$0" | sed 's/^# \{0,1\}//'
        exit 2
        ;;
esac
