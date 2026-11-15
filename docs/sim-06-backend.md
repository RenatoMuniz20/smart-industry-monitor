# SIM-06 — Backend REST em C

## Objetivo
Expor os dados das dez máquinas, já gravados no InterSCity, por uma API REST que o dashboard possa consumir. O backend consulta o InterSCity; não recebe telemetria diretamente.

## Arquitetura
```text
InterSCity (Data Collector)
  ← POST /collector/resources/data/last        (último valor, uma chamada para as 10 máquinas)
  ← POST /collector/resources/{uuid}/data      (histórico do período, uma chamada por máquina)
        ↓
Backend em C (libmicrohttpd + libcurl + json-c)
  → GET /api/health
  → GET /api/machines
  → GET /api/machines/latest
  → GET /api/machines/{id}/history?start=&end=
        ↓
Dashboard (SIM-07, pendente)
```

## Endpoints

| Método | Rota | Resposta |
|---|---|---|
| GET | `/api/health` | `{"status":"ok"}` |
| GET | `/api/machines` | Mapeamento `machine_id` ↔ `uuid`, sem consultar o InterSCity |
| GET | `/api/machines/latest` | Último valor de cada máquina, com timestamp |
| GET | `/api/machines/{id}/history?start=AAAA-MM-DDTHH:MM:SS&end=AAAA-MM-DDTHH:MM:SS` | Séries do período, em UTC |

Exemplo de `/api/machines/latest` (valores reais de 15/11/2026):
```json
{"machines":[{"machine_id":1,"uuid":"18fa502d-2427-4485-a3a6-0b3f64585634",
  "operating_state":{"value":"IDLE","timestamp":"2024-01-01T00:21:00.000Z"},
  "motor_temperature":{"value":49.7,"timestamp":"2024-01-01T00:21:00.000Z"},
  "current":{"value":5.3,"timestamp":"2024-01-01T00:21:00.000Z"},
  "failure_within_24h":{"value":0,"timestamp":"2024-01-01T00:21:00.000Z"}}, ...]}
```

Exemplo de histórico: `series` tem as chaves `operating_state`, `motor_temperature`, `current` e `failure_within_24h`. Cada item é `{"timestamp": ..., "value": ...}`.

## Regras de dados
- **Último valor:** cada máquina tem seu próprio timestamp. Capability ausente ou máquina sem dados vira `null`.
- **Histórico:** o InterSCity devolve registros repetidos (reenvios já gravados). O backend ordena por data e remove timestamps repetidos antes de responder. A remoção não altera o InterSCity.
- **Sem dados:** o Data Collector responde 404 `Resource not found` para máquina sem registros; o backend devolve séries vazias, não erro.
- **Números:** saem no menor formato que preserva o valor (`49.51`, não `49.509999999999998`).
- **Datas:** sem fuso no pedido; tratadas como UTC, que é o que o InterSCity usa.

## Códigos de resposta
| Código | Quando |
|---|---|
| 200 | Resposta normal (inclusive séries vazias) |
| 400 | `start`/`end` ausentes, fora do formato `AAAA-MM-DDTHH:MM:SS` ou com início depois do fim |
| 404 | Rota inexistente, máquina desconhecida ou `id` que não é número |
| 405 | Método diferente de GET |
| 413 | Histórico maior que 1 MiB; reduza o período |
| 500 | Erro interno (por exemplo, memória) |
| 502 | InterSCity fora do ar, timeout ou resposta inválida. Detalhe só no log do servidor |

## Configuração
- `--port N` (padrão 8080)
- `--bind IP` (padrão `127.0.0.1`). `0.0.0.0` aceita conexões da rede; use só se for necessário.
- `--machines ARQUIVO` (padrão `../config/interscity/machines.conf`, relativo à pasta `backend/`)
- `--interscity-url URL` (padrão a instância da UFMA)
- `--interscity-timeout-ms N` (padrão 5000)

O arquivo de máquinas é validado na inicialização: UUID inválido, id ou UUID repetido, linha muito longa ou arquivo vazio fazem o backend recusar iniciar.

## Verificação

### Testes locais (`make -C backend test`)
Todos usam um InterSCity falso em `127.0.0.1`. Nenhum acessa a UFMA.

| Suíte | Verificações | O que cobre |
|---|---|---|
| `test_backend.py` | 6 | `/api/health`, 404, 405, bind padrão em 127.0.0.1, bind inválido, SIGTERM |
| `test_interscity_query.py` | 25 | Consulta `/data/last` e histórico: valores, zeros, capabilities ausentes, HTTP 500, JSON inválido, lista vazia, uuid inválido, conexão recusada, duplicatas, ordem, 404 sem dados, datas inválidas |
| `test_machines_latest.py` | 8 | `/latest`: ordem do arquivo, valores, nulls, POST único com os 3 uuids, 405/404, InterSCity fora do ar (502), arquivo inválido |
| `test_machines_history.py` | 8 | `/api/machines`, histórico com duplicatas, UTC enviado ao Data Collector, séries vazias, 400, 404, 405 |
| **Total** | **47** | |

Nota: a PR #6 e a mensagem do commit da SIM-06.4/06.5 dizem "62 verificações". Foi um erro de contagem: a soma correta era 45 na época, e agora são 47 com os dois testes de bind.

### Teste contra a instância real (`make -C backend smoke-real`)
Somente leitura, fora do `make test`. Rodado em 15/11/2026: **17 de 17** verificações passaram.
- `/api/machines/latest`: 200 em 0,90 s; 10 de 10 máquinas com último valor.
- Histórico das 10 máquinas na janela 2024-01-01T00:00 a 01:00: 0,18 a 0,21 s cada, sem repetidos e em ordem.
- Janela de 2030: séries vazias.
- Data fora do formato: 400.

Cada série do histórico tem 2 ou 3 registros nessa janela, coerente com os últimos valores (entre 00:08 e 00:43). Máquina 1 tem 2 registros de `current`; medição ausente é omitida pelo desenho do projeto.

## Limitações conhecidas
- **Sem autenticação.** O servidor não tem login. Por padrão só aceita conexões locais; com `--bind 0.0.0.0` ele fica exposto na rede.
- **Uma consulta por vez por requisição.** Cada chamada ao InterSCity bloqueia a requisição até o timeout (5 s por padrão). Adequado para o volume atual, não para muitos clientes simultâneos.
- **Sem cache.** Cada `/latest` consulta o InterSCity de novo.
- **Histórico limitado a 1 MiB.** Períodos muito longos devolvem 413.
- **Duplicatas no InterSCity não são corrigidas.** O backend só as oculta na resposta. A correção na origem (gateway e reenvios) fica para a etapa de tolerância a falhas.
- **Dados de teste.** Os valores da instância são de 2024 (dataset histórico replayado), não de tempo real.
