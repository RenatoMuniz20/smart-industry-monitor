# Backend (SIM-06)

Servidor REST em C (libmicrohttpd) que consulta o InterSCity com libcurl e json-c.

Estado atual:
- **SIM-06.1 (feito):** `GET /api/health`; 404 para rotas desconhecidas; 405 para método errado; encerramento limpo por SIGINT/SIGTERM.
- **SIM-06.2 (feito):** `interscity_query_last()` faz POST em `/collector/resources/data/last`.
- **SIM-06.3 (feito):** `GET /api/machines/latest` devolve o último valor de cada máquina.
- **SIM-06.4 (em teste):**
  - `GET /api/machines`: lista do mapeamento.
  - `GET /api/machines/{id}/history?start=AAAA-MM-DDTHH:MM:SS&end=AAAA-MM-DDTHH:MM:SS`: histórico em UTC, sem timestamps repetidos e em ordem cronológica.
- Pendente: teste com a instância real da UFMA (SIM-06.5).

## Dependências
    sudo apt install build-essential libmicrohttpd-dev libcurl4-openssl-dev libjson-c-dev python3

## Compilar e testar
    make -C backend test

## Executar
Rodar a partir da pasta `backend/` (o caminho padrão do mapeamento é `../config/interscity/machines.conf`):

    ./build/backend --port 8080
    curl -s http://127.0.0.1:8080/api/machines
    curl -s "http://127.0.0.1:8080/api/machines/1/history?start=2024-01-01T00:00:00&end=2024-01-01T01:00:00"
    # Ctrl+C para encerrar

Opções: `--machines ARQUIVO`, `--interscity-url URL`, `--interscity-timeout-ms N`.
