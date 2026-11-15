# Backend (SIM-06)

Servidor REST em C (libmicrohttpd) e cliente de consulta ao InterSCity (libcurl + json-c).

Estado atual:
- **SIM-06.1 (feito):** `GET /api/health` (200), rotas desconhecidas (404), método errado (405), encerramento limpo por SIGINT/SIGTERM.
- **SIM-06.2 (em teste):** `interscity_query_last()` faz POST em `/collector/resources/data/last` e lê o último valor de cada capability. Testado contra um InterSCity falso em 127.0.0.1.
- Pendente: conversão para o formato da API e endpoints de máquinas (SIM-06.3 a 06.5).

## Dependências
    sudo apt install build-essential libmicrohttpd-dev libcurl4-openssl-dev libjson-c-dev python3

## Compilar e testar
    make -C backend test

## Executar o servidor
    ./backend/build/backend --port 8080
    curl -i http://127.0.0.1:8080/api/health
    # Ctrl+C para encerrar
