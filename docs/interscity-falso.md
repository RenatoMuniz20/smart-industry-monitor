# InterSCity falso

## Objetivo

Rodar o pipeline inteiro (replayers → MQTT → IoT Gateway → InterSCity → backend
→ dashboard) sem escrever na instância compartilhada do LSDi/UFMA. Ele serve
para a demonstração sem depender da rede e para o teste de escalabilidade.

Decisão da equipe (22/11): por enquanto, os testes de carga usam o InterSCity
falso. Uma instância própria do InterSCity está fora de cogitação. O teste com a
instância real fica para depois, por decisão da equipe.

## O que ele implementa

Só as rotas que o gateway e o backend usam, no formato observado na instância
real (docs/sim-05-interscity.md):

| Rota | Quem usa | Resposta |
| --- | --- | --- |
| `POST /adaptor/resources/{uuid}/data` | Gateway (envio) | 201 `{}` |
| `POST /collector/resources/data/last` com `{"uuids":[...]}` | Gateway (consulta antes de reenviar) e backend (`/api/machines/latest`) | 200 `{"resources":[{"uuid","capabilities":{...}}]}` |
| `POST /collector/resources/{uuid}/data` com `start_date`/`end_date` | Backend (histórico) | 200, amostras em ordem |
| `POST /collector/resources/data` e `/{uuid}/data/last` | — (completam a API) | 200 |

Comportamentos copiados da instância real:

- **UUID desconhecido:** o Adaptor responde 201, mas descarta a amostra. Os UUIDs
  conhecidos vêm do mesmo `config/interscity/machines.conf`.
- **Envios repetidos:** gravam cópias, porque POST não é idempotente. É isso que
  a SIM-08 trata consultando antes de reenviar.
- **Recurso sem dados:** o Collector responde 404 `{"error":"Resource not found"}`
  quando nenhum recurso pedido tem dados.
- **Datas:** `date` volta como `AAAA-MM-DDTHH:MM:SS.000Z`.
- **Campo `data` como texto JSON:** também é aceito, como na instância real.

As amostras ficam em SQLite, num volume próprio no Compose, e sobrevivem a
reinícios.

### Simulação de problemas (para os testes de carga)

Tudo vem desligado por padrão. No Compose, use variáveis de ambiente. Fora do
Compose, use as opções de mesmo nome.

| Variável / opção | Efeito | Reação esperada do gateway |
| --- | --- | --- |
| `FAKE_LATENCY_MS` / `--latency-ms` | Atraso em cada resposta da API. Com 60 ms, fica perto da UFMA (cerca de 69 ms por envio medido). | Vazão do worker cai |
| `FAKE_JITTER_MS` / `--jitter-ms` | Atraso extra aleatório, entre 0 e o valor | — |
| `FAKE_STORE_DELAY_MS` / `--store-delay-ms` | A amostra só aparece no Collector depois desse tempo. Na instância real, o 201 só confirma a entrada na fila do RabbitMQ. | Reconciliação da SIM-08/09 |
| `FAKE_FAIL_RATE` / `--fail-rate` | Fração dos envios respondida com 503 | Reagenda com espera crescente (`pending`) |
| `FAKE_MAX_INFLIGHT` / `--max-inflight` | Acima desse número de requisições simultâneas, responde 503 na hora (simula sobrecarga) | Reagenda |

Controle e medição (não fazem parte da API do InterSCity):

- `GET /__fake/stats`: contadores de envios aceitos, 400, 503 simulados,
  descartes, amostras gravadas, envios por segundo nos últimos 10 s e pico de
  requisições simultâneas.
- `POST /__fake/down` e `POST /__fake/up`: simulam o InterSCity fora do ar
  (503 em tudo) e a volta.
- `POST /__fake/reset`: apaga as amostras e zera os contadores.

### Linguagem

Ele é uma ferramenta de teste, não um componente do sistema, e por isso foi
escrito em Python, só com a biblioteca padrão. Os falsos que já existem nos
testes do gateway e do backend também são em Python.

## Como usar

Use sempre o script. Ele aponta gateway e backend para o falso e usa um projeto
Compose separado (`smart-industry-fake`), com volumes próprios. Assim, a fila do
gateway destes testes nunca é enviada à UFMA, mesmo que depois alguém suba o
Compose normal apontando para a instância real.

    scripts/fake-stack.sh up        # mosquitto, InterSCity falso, gateway e backend
    scripts/fake-stack.sh replay    # 10 máquinas, LIMIT=60 linhas cada, 1 envio/s por máquina
    scripts/fake-stack.sh stats     # contadores do falso + fila do gateway por status
    # dashboard: http://127.0.0.1:8080/
    scripts/fake-stack.sh down      # para (mantém os dados)
    scripts/fake-stack.sh wipe      # para e apaga fila e dados DESTE projeto

O mosquitto deste projeto não publica porta no host: gateway e replayers falam
com ele pela rede interna do Compose. Assim, ele não disputa a 1883 com um
mosquitto instalado no WSL ou com o Compose normal. A porta do dashboard pode
ser trocada com `FAKE_BACKEND_PORT=8081`, e a do falso com
`FAKE_INTERSCITY_PORT=8001`.

Com latência parecida com a da UFMA:

    FAKE_LATENCY_MS=60 FAKE_JITTER_MS=20 scripts/fake-stack.sh up

Sem Docker:

    python3 tools/interscity-fake/fake_interscity.py --port 8000 \
        --resources config/interscity/machines.conf --db /tmp/interscity-fake.db
    iot-gateway/build/iot-gateway ... --interscity-url http://127.0.0.1:8000
    backend/build/backend ... --interscity-url http://127.0.0.1:8000 --web-root dashboard

## Ajuste no backend

O backend devolvia 502 em `/api/machines/latest` quando o Collector respondia
404 `Resource not found`, o que acontece quando nenhuma máquina tem dados ainda.
Agora esse caso é tratado como "sem dados", igual ao histórico já fazia, e o
dashboard mostra "Sem dados" em vez de "InterSCity indisponível". O teste novo
está em `backend/tests/test_machines_latest.py`.

## Testes

| Teste | Resultado | Onde rodou |
| --- | --- | --- |
| `make -C tools/interscity-fake test` (21 verificações): formato que o gateway e o backend leem, 404 sem dados, histórico inclusivo e em ordem, cópias de envios repetidos, UUID desconhecido descartado, 7 corpos inválidos → 400, persistência após reinício, SIGTERM, down/up/reset, falhas, atraso de armazenamento, latência e limite de simultâneas | Passou | Ambiente de desenvolvimento do Claude |
| Vazão do próprio falso (envio com 6 capabilities, conexão mantida) | Cerca de 2.900 envios/s com 1 conexão e 2.500 com 8. `data/last` em 0,4 ms. É mais de 150 vezes a vazão atual do gateway (cerca de 14 envios/s), então o falso não é o gargalo no teste de carga. | Ambiente de desenvolvimento do Claude |
| `make -C backend test` com o cenário novo do 404 | **Pendente na máquina da equipe** (libmicrohttpd indisponível no ambiente do Claude) | — |
| Pipeline completo no Compose (`scripts/fake-stack.sh`) | **Pendente na máquina da equipe** (sem Docker no ambiente do Claude) | — |

Durante a medição de vazão apareceu um atraso de cerca de 40 ms por resposta: o
servidor enviava cabeçalho e corpo em dois pacotes, e o cliente segurava o ACK.
A correção foi enviar a resposta num único pacote (`wbufsize`).
