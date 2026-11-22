# Backend (SIM-06)

Servidor REST em C (libmicrohttpd) que consulta o InterSCity com libcurl e json-c.

## Estado
- **SIM-06.1:** `GET /api/health`; 404 para rotas desconhecidas; 405 para método errado; encerramento limpo por SIGINT/SIGTERM.
- **SIM-06.2:** `interscity_query_last()`: POST em `/collector/resources/data/last`.
- **SIM-06.3:** `GET /api/machines/latest`: último valor de cada máquina.
- **SIM-06.4:** `GET /api/machines` e `GET /api/machines/{id}/history?start=&end=` (UTC, sem repetidos, em ordem).
- **SIM-06.5:** `make smoke-real`: teste somente leitura contra a instância da UFMA.
- **SIM-06.6:** o servidor escuta só em `127.0.0.1` por padrão. Para aceitar conexões da rede, use `--bind 0.0.0.0` conscientemente.
- **SIM-07:** com `--web-root PASTA`, GET/HEAD fora de `/api/` entregam o dashboard (`static_files.c`: só html, css, js, svg, png e ico; bloqueia `..`, arquivos ocultos e links para fora da pasta). Sem a opção, nada muda.

Detalhes e resultados: [docs/sim-06-backend.md](../docs/sim-06-backend.md).

## Dependências
    sudo apt install build-essential libmicrohttpd-dev libcurl4-openssl-dev libjson-c-dev python3

## Compilar e testar
    make -C backend test          # 52 da API + 30 do módulo estático + 9 do dashboard servido; sem acessar a UFMA
    make -C backend smoke-real    # manual: consulta a instância real (somente leitura)

## Executar
Rodar a partir da raiz do repositório:

    backend/build/backend --port 8080 --machines config/interscity/machines.conf
    curl -s http://127.0.0.1:8080/api/machines
    curl -s http://127.0.0.1:8080/api/machines/latest
    curl -s "http://127.0.0.1:8080/api/machines/1/history?start=2024-01-01T00:00:00&end=2024-01-01T01:00:00"

Opções: `--port N`, `--bind IP` (padrão `127.0.0.1`), `--machines ARQUIVO` (padrão `../config/interscity/machines.conf`, relativo à pasta `backend/`), `--interscity-url URL`, `--interscity-timeout-ms N`, `--web-root PASTA` (dashboard; ex.: `dashboard`).

Com o dashboard:

    backend/build/backend --port 8080 --machines config/interscity/machines.conf --web-root dashboard
    # abra http://127.0.0.1:8080/
