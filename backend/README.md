# Backend (SIM-06)

Servidor REST em C (libmicrohttpd). Estado atual: **SIM-06.1 — esqueleto**.

Implementado: `GET /api/health` (200), rotas desconhecidas (404), método errado (405), encerramento limpo por SIGINT/SIGTERM.
Ainda não implementado: consulta ao InterSCity (SIM-06.2 a 06.5).

## Dependências
    sudo apt install build-essential libmicrohttpd-dev python3

## Compilar e testar
    make -C backend test

## Executar manualmente
    ./backend/build/backend --port 8080
    curl -i http://127.0.0.1:8080/api/health
    # Ctrl+C para encerrar
