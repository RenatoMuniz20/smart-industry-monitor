# Backend (SIM-06)

Servidor REST em C (libmicrohttpd) que consulta o InterSCity com libcurl e json-c.

Estado atual:
- **SIM-06.1 (feito):** `GET /api/health`; 404 para rotas desconhecidas; 405 para método errado; encerramento limpo por SIGINT/SIGTERM.
- **SIM-06.2 (feito):** `interscity_query_last()` faz POST em `/collector/resources/data/last` e lê o último valor de cada capability.
- **SIM-06.3 (em teste):** `GET /api/machines/latest` lê `config/interscity/machines.conf`, consulta o InterSCity e devolve o último valor de cada máquina.
- Pendente: `/api/machines`, `/api/machines/{id}/history` (SIM-06.4) e teste com a instância real da UFMA (SIM-06.5).

## Dependências
    sudo apt install build-essential libmicrohttpd-dev libcurl4-openssl-dev libjson-c-dev python3

## Compilar e testar
    make -C backend test

## Executar
Rodar a partir da pasta `backend/` (o caminho padrão do mapeamento é `../config/interscity/machines.conf`):

    ./build/backend --port 8080
    curl -s http://127.0.0.1:8080/api/machines/latest
    # Ctrl+C para encerrar

Opções: `--machines ARQUIVO`, `--interscity-url URL`, `--interscity-timeout-ms N`.
