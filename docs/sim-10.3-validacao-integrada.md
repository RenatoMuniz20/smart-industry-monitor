# SIM-10.3 — Validação integrada Compose e recuperação automática

Em 24/11/2026, os cenários A/B/C passaram no projeto Docker Compose exclusivo
`sim103-validation`: dez produtores publicaram 30 telemetrias por cenário,
a fila preservou 30 pendências durante indisponibilidade e recriação do Gateway,
e o worker recuperou todas sem republicação MQTT. Os valores recuperados foram
comparados ao CSV, ao Collector e ao SQLite persistido do falso.

**Parecer atualizado:** pipeline integrado aprovado nos cenários executados.
A corrida foi corrigida exclusivamente na sincronização do teste; a suíte MQTT
do Replayer passou na execução inicial e em três repetições adicionais.
Recomenda-se o encerramento técnico da SIM-10.3, sujeito à revisão da equipe.
O caso de locale continua SKIP e a inspeção visual do dashboard permanece NOT RUN.
Não houve acesso à instância UFMA, alteração do TAP, staging, commit, push ou PR.

## Base e isolamento

- Branch `main`, HEAD `db40b270e402924732e106a2266f90cde1e17bc6`.
- Inicialmente, somente `docs/demonstracao-arch-linux.md` estava não rastreado;
  esse arquivo foi preservado sem edição nesta rodada.
- Docker 29.9.2, Compose 5.6.1. As dependências locais estavam disponíveis:
  libmicrohttpd 1.0.10, libmosquitto 2.1.2, json-c 0.19, libcurl 8.23.0,
  SQLite 3.54.0. Não foi necessário instalar pacotes no host.
- Portas 127.0.0.1:18080 (Backend) e :18000 (falso) estavam livres.
  Mosquitto ficou somente na rede interna, sem publicar 1883 no host.
- `docker-compose.yml` e `docker-compose.fake.yml` foram resolvidos com
  `-p sim103-validation`. Um terceiro arquivo temporário apenas nomeou volumes
  novos para separar os cenários. Nenhum Compose do repositório foi editado.
- Gateway e Backend: `--interscity-url http://interscity-fake:8000`, conferido
  no Compose resolvido e nos argumentos dos contêineres antes de cada replay.
- `scripts/fake-stack.sh` foi inspecionado, mas não executado: fixa o projeto
  `smart-industry-fake`. Não foram usados wipe, reset do falso ou `down -v`.
- Nenhum volume, imagem ou contêiner de outro projeto foi modificado/removido.

Os arquivos lidos incluíram os dois Compose, fake-stack, documentação do falso,
SIM-09 final, dashboard, contrato/serialização SIM-11, fila, worker, rotas REST e
Dockerfiles. O contrato MQTT tem dez campos obrigatórios:
`machine_id`, `machine_type`, `timestamp`, `temperature`, `current`, `power`,
`vibration_rms`, `rpm`, `state`, `failure_within_24h`.

## Resultados dos cenários

| Cenário | Esperado | Observado | Resultado |
| --- | --- | --- | --- |
| Build Compose | Imagens compiladas | Gateway, Backend, Replayer e falso construídos; sem erro ou warning do compilador | PASS |
| Inicialização Compose | Serviços saudáveis | Mosquitto, falso e Backend healthy; Gateway running, SQLite aberto e SUBACK confirmado na execução atual | PASS |
| Replay de dez máquinas A | 30 telemetrias | Dez processos C simultâneos; dez resumos com 3 PUBACKs; 30 JSONs comparados integralmente ao CSV | PASS |
| Entrega ao falso A | Dados armazenados | 30 Adaptor 201; 177 medições no total, correspondência exata com CSV/Collector/SQLite | PASS |
| Fila e cursores A | Fila drenada | 0 pending/inflight/blocked; dez cursores nos últimos timestamps publicados | PASS |
| Indisponibilidade B | Registros persistidos | 30 pending; 0 cursores; 503 no worker; zero medições no falso | PASS |
| Reinício Gateway C | Dados preservados | SIGTERM=0; worker finalizado; outro ID de contêiner, mesmo volume; 30 registros com IDs, UUIDs, datas e payloads preservados | PASS |
| SQLite após reinício | Integridade e WAL | journal_mode=wal; integrity_check=ok | PASS |
| Recuperação HTTP C | Fila drenada | 30 entregas após up; zero novas telemetrias MQTT no Gateway recriado; fila vazia e dez cursores | PASS |
| Dados recuperados C | Mesmos 30 registros publicados | 177 medições exatas no Collector e no SQLite do falso; sem falta, extras ou duplicatas nesta rodada | PASS |
| Backend e dashboard | Consultas corretas | Health/arquivos estáticos 200; latest com dez máquinas; histórico máquina 1 conferido; latest 502 durante down e 200 após up | PASS para HTTP/API |
| Dashboard em navegador | Renderização e atualização visual | Não foi aberto navegador nesta rodada | NOT RUN |

O Gateway não possui healthcheck no Compose atual. Seu estado running não foi
tratado como prontidão: o teste esperou `Gateway pronto` dos logs do contêiner
atual, emitido após CONNACK/SUBACK QoS 1. O health do falso continua 200 durante
`down`; esse controle simula falha dos endpoints InterSCity, não morte do processo.
O health do Backend é local; a indisponibilidade aparece em `/api/machines/latest`
como HTTP 502, não necessariamente em `/api/health`.

## A — Dados e contrato SIM-11

Cada um dos IDs 1, 2, 3, 6, 7, 8, 9, 11, 12 e 13 teve três mensagens MQTT
confirmadas e recebidas. Os dez campos, tipos, nulls, valores e timestamps foram
comparados às três primeiras linhas de cada máquina no CSV. A captura de
`docker top` comprova os dez processos C concorrentes com LIMIT=3/INTERVAL_MS=1000.

As capabilities são `sim_motor_temperature`, `sim_current`, `sim_vibration_rms`,
`sim_rpm`, `sim_operating_state` e `sim_failure_within_24h`. power é null e não
possui capability. Medição null omite sua capability, mantendo zero numérico.

| Capability | Medições armazenadas por cenário A/C |
| --- | --- |
| sim_motor_temperature | 30 |
| sim_current | 28 |
| sim_vibration_rms | 29 |
| sim_rpm | 30 |
| sim_operating_state | 30 |
| sim_failure_within_24h | 30 |
| Total | 177 |

As três omissões vêm do CSV: current da máquina 1 às 00:21, current da máquina 3
às 00:00 e vibration_rms da máquina 8 às 00:00, todos em 01/01/2024. Portanto,
30 telemetrias × seis capabilities possíveis correspondem a 180 valores
potenciais; com três valores ausentes, são 177 medições no total, não por capability. `/latest` retorna a última medição
**disponível por capability**: a corrente da máquina 1 fica em 5.3 às 00:03,
enquanto as demais leituras avançam para 00:21. Isso corresponde à omissão de
null, não a perda de entrega. O histórico da máquina 1 tem duas correntes e três
amostras em cada uma das outras cinco séries.

O Collector foi consultado em `/collector/resources/data`, com os dez UUIDs do
mapa. Foi comparado um multiconjunto de `(uuid, capability, date, value)` contra
o CSV, incluindo multiplicidades. Depois de encerrar o falso, o arquivo SQLite
persistido também foi lido e comparado ao Collector: 177 entradas, integrity=ok.
A leitura usou mount readonly, contêiner sem rede e SQLite immutable somente após
confirmar ausência de WAL pendente. Em execução, as consultas usaram readonly
normal. Não foram escritas fixtures em banco.

PUBACK confirma recepção MQTT pelo broker; HTTP 201 confirma aceitação no Adaptor;
a consulta do Collector e a leitura do banco do falso comprovam armazenamento
neste ambiente de teste. Não constituem homologação de persistência na UFMA.

## B/C — Persistência, encerramento e retomada

B começou com novos volumes, fila vazia, cursor vazio e falso sem amostras.
`POST /__fake/down` retornou `down:true`; stats confirmou o estado e uma consulta
Collector retornou 503 antes do replay. O Gateway continuou validando/persistindo
as 30 mensagens enquanto o worker recebeu 503 e reagendou em pending.

| Snapshot | Registros | Estados | attempt_count | Cursores |
| --- | --- | --- | --- | --- |
| B inicial | 0 | — | — | 0 |
| B após replay | 30 | 30 pending | id=1: 2; demais: 0 | 0 |
| C após recriação, ainda down | 30 | 30 pending | id=1: 4; demais: 0 | 0 |
| C após up e recuperação | 0 | nenhum blocked/pending/inflight | registros confirmados removidos | 10 |

Os snapshots completos têm ID, máquina, timestamp, UUID, payload_json, status,
attempt_count e last_error. Entre snapshots, o worker fez tentativas legítimas:
o contador pode aumentar; payload, identidade e quantidade permaneceram iguais.
Não houve alteração manual de qualquer registro SQLite.

O Gateway anterior era `4c7e2e3e7040`; saiu normalmente em SIGTERM, exit=0,
OOMKilled=false. O novo foi `a75cd0d55dd1`, com o mesmo volume
`sim103-validation_b_gateway_data`. Log de encerramento:

```text
Worker HTTP: 0 entregues, 3 retentativas, 0 bloqueadas, 0 recuperadas pelo Data Collector.
Fila SQLite: 30 persistidas, 0 duplicatas ignoradas, 0 sem mapeamento, 0 não preparadas.
Gateway encerrado por solicitação.
```

O resumo do worker é emitido após pthread_join. O contêiner recriado voltou a
confirmar SUBACK. Seu log não possui `Telemetria válida` ou novas persistências:
todas as entregas vieram da fila anterior. Após `/__fake/up`, o worker entregou
os IDs 1 a 30, atualizou os dez cursores e esvaziou a fila. Nenhum blocked.

Stats observado: A teve adaptor_created=30 e samples_stored=177. B, durante down,
registrou samples_stored=0, adaptor_created=0 e down_503=5. Após C, adaptor_created=30,
samples_stored=177 e down_503=6. down_503 inclui consultas do diagnóstico e do
Backend; não é contador exclusivo de POSTs do worker. O falso retorna down antes
de incrementar adaptor_requests. Não se infere quantidade total de tentativas
HTTP desses contadores. A rodada não produziu confirmações ambíguas ou duplicatas;
isso não demonstra exactly-once. As limitações SIM-09 permanecem.

## Regressões anteriores à correção da sincronização

Todas as suítes usaram os testes existentes, no host com sockets locais liberados.
Nenhuma dependência foi acrescentada às imagens finais para executar testes.

| Comando | Resultado efetivo |
| --- | --- |
| make -C tools/interscity-fake test | PASS, exit 0 |
| make -C dataset-replayer test | PASS dos casos executados, exit 0; SKIP parcial de locale |
| make -C dataset-replayer test-mqtt | FAIL, exit 2; repetição isolada também FAIL |
| make -C iot-gateway test | PASS, exit 0 |
| make -C iot-gateway test-mqtt | PASS, exit 0; inclui cenários de falha/reinício SIM-09 |
| make -C iot-gateway test-e2e | PASS, exit 0; 30 CSV, 32 válidas incluindo duas sondas de continuidade |
| make -C backend test | PASS, exit 0; inclui API e dashboard estático |
| Caso de locale numérico alternativo | SKIP, locale indisponível; não aprovado por inferência |

### Defeito identificado no teste de processos

A primeira execução falhou em `test_ten_machines.py:131`, ao buscar
`--interval-ms` no cmdline de um filho anunciado. A repetição isolada falhou em
`:90`, por ausência de `--publish`. Uma execução diagnóstica observou os bytes
lidos de `/proc` sem alterar seu retorno, os arquivos do teste ou as asserções:

```text
PROC_OBSERVED /proc/9292/cmdline b'bash\x00.../dataset-replayer/scripts/run-ten-machines.sh\x00'
```

Causa: o launcher inicia em background e anuncia `$!`; esse anúncio pode ocorrer
antes do exec do filho. `wait_for_children` espera somente os dez anúncios e lê
imediatamente os cmdlines. Nessa janela, um filho ainda apresenta os argumentos
herdados do Bash. Os cenários funcionais anteriores e ambos os replays Compose
com dez processos C passaram; não foi demonstrada falha de publicação C.

Impacto na rodada original: a regressão não era determinística e aquela rodada
não pôde ser declarada toda verde. Os FAILs e seus logs acima permanecem como
registro histórico. A correção autorizada posteriormente foi aplicada somente
em `dataset-replayer/tests/test_ten_machines.py`, conforme a seção seguinte.

### Correção da sincronização e nova validação

Em 24/11/2026, `wait_for_replayers` passou a esperar uma condição observável:
`/proc/<pid>/exe` e argv[0] devem apontar para o binário C do Replayer. As duas
leituras precisam concordar, pois o exec pode ocorrer entre elas. Os argumentos
capturados são então usados nas asserções originais de --publish, --machine-id,
--interval-ms e --limit, sem enfraquecimento. A mesma espera é usada na execução
normal e nos três cenários de sinais/falha de filho.

O prazo usa `time.monotonic()` e mantém 10 s, com polling curto de 10 ms: não há
pausa fixa para presumir prontidão. Em cada varredura são conferidos o estado do
launcher, a existência dos PIDs e os estados de processo, rejeitando zombie/morto.
Bash antes do exec e a breve indisponibilidade do link exe de um PID vivo são
transitórios; PID desaparecido, launcher encerrado ou executável inesperado
produzem falha. Exceções inesperadas não são suprimidas. Timeout informa PID,
argumentos/executável/estado observados, estado do launcher e log. A descoberta
dos PIDs também verifica se o launcher já terminou antes de retornar.

Não foram alterados launcher, código C, aplicação, Makefile ou harness MQTT.
Todas as rodadas abaixo usaram o broker temporário da própria suíte, em porta
local dinâmica; nenhum Compose ou middleware real foi acessado nesta correção.

| Execução após correção | Resultado | Código | Duração |
| --- | --- | --- | --- |
| test local | PASS dos casos executados; SKIP de locale | 0 | 0.632 s |
| test-mqtt inicial | PASS, sem SKIP | 0 | 18.623 s |
| test-mqtt repetição 1 | PASS, sem SKIP | 0 | 18.646 s |
| test-mqtt repetição 2 | PASS, sem SKIP | 0 | 18.633 s |
| test-mqtt repetição 3 | PASS, sem SKIP | 0 | 18.660 s |

Resultado: zero FAIL nas cinco execuções registradas, quatro suítes MQTT completas
aprovadas. O SKIP do locale não representa aprovação desse caso. As demais
regressões do relatório mantêm os resultados anteriores e não foram repetidas
nesta correção. A inspeção visual do dashboard em navegador continua **NOT RUN**.

Os novos logs e results.json estão em
[regressoes-pos-correcao](evidencias/sim-10.3/regressoes-pos-correcao/).
O [diff completo](evidencias/sim-10.3/regressoes-pos-correcao/correcao.diff)
inclui o teste e a atualização deste relatório. As evidências e o SHA256SUMS
anteriores não foram sobrescritos; a nova rodada possui seu próprio manifesto.

## Comandos reproduzíveis

Na raiz do repositório, em Bash. Escolher volumes novos para cada rodada: repetir
as mesmas linhas históricas num volume com cursores pode deduplicá-las e não
constitui replay limpo. Não executar scripts/fake-stack.sh nesta validação.

```bash
export INTERSCITY_URL=http://interscity-fake:8000
export FAKE_BACKEND_PORT=18080 FAKE_INTERSCITY_PORT=18000
export FAKE_LATENCY_MS=0 FAKE_JITTER_MS=0 FAKE_STORE_DELAY_MS=0
export FAKE_FAIL_RATE=0 FAKE_MAX_INFLIGHT=0
export LIMIT=3 INTERVAL_MS=1000
ss -ltn 'sport = :18080 or sport = :18000'
# Se ocupadas, escolher portas livres e ajustar os dois exports.
VALIDATION_RUN=$(date +%Y%m%dT%H%M%S)
VALIDATION_OVERRIDE=/tmp/sim103-${VALIDATION_RUN}-a.yml
cat > "$VALIDATION_OVERRIDE" <<VOLUMES
volumes:
  gateway_data:
    name: sim103-validation_${VALIDATION_RUN}_a_gateway_data
  fake_interscity_data:
    name: sim103-validation_${VALIDATION_RUN}_a_fake_interscity_data
VOLUMES
compose() {
  docker compose -p sim103-validation -f docker-compose.yml \
    -f docker-compose.fake.yml -f "$VALIDATION_OVERRIDE" "$@"
}
compose config
compose --profile replay build
compose up -d --wait --wait-timeout 90 mosquitto interscity-fake gateway backend
compose ps
for service in gateway backend; do
  docker inspect "$(compose ps -q "$service")" \
    --format '{{json .Config.Cmd}} {{json .Mounts}}'
done
compose logs gateway
# Exigir destino http://interscity-fake:8000 nos dois argumentos e
# Gateway pronto no contêiner atual antes de qualquer produtor.
```

Checagens SQLite e endpoints:

```bash
compose exec -T gateway sqlite3 -readonly /data/telemetry.db \
  'PRAGMA journal_mode; PRAGMA integrity_check;'
compose exec -T gateway sqlite3 -header -column -readonly /data/telemetry.db \
  'SELECT id,machine_id,timestamp,status,attempt_count,last_error FROM telemetry_queue ORDER BY id;
   SELECT COUNT(*) AS total FROM telemetry_queue;
   SELECT * FROM delivered_cursor ORDER BY machine_id;'
curl -fsS --max-time 10 http://127.0.0.1:$FAKE_BACKEND_PORT/api/health
for path in / /app.js /style.css; do
  curl -sS --max-time 10 -o /dev/null -w '%{http_code}\n' \
    "http://127.0.0.1:$FAKE_BACKEND_PORT$path"
done
curl -fsS --max-time 10 http://127.0.0.1:$FAKE_INTERSCITY_PORT/__fake/stats | jq .
```

Replay A (um nome novo por rodada), observação e consultas:

```bash
REPLAY_A=sim103-validation-replay-a-$VALIDATION_RUN
compose run -d --no-deps --name "$REPLAY_A" replayer
# Executar imediatamente enquanto os produtores estão ativos:
docker top "$REPLAY_A" -eo pid,args
docker wait "$REPLAY_A"    # esperado 0
docker logs "$REPLAY_A"
compose logs gateway
curl -fsS --max-time 10 http://127.0.0.1:$FAKE_BACKEND_PORT/api/machines/latest | jq .
curl -fsS --max-time 10 \
  "http://127.0.0.1:$FAKE_BACKEND_PORT/api/machines/1/history?start=2024-01-01T00:00:00&end=2024-01-02T00:00:00" | jq .
UUIDS=$(awk '!/^[[:space:]]*(#|$)/ {print $2}' config/interscity/machines.conf | jq -R . | jq -s '{uuids:.}')
curl -fsS --max-time 10 -H 'Content-Type: application/json' --data "$UUIDS" \
  http://127.0.0.1:$FAKE_INTERSCITY_PORT/collector/resources/data | jq .
# Esperar fila vazia e conferir os dez cursores antes de encerrar.
compose --profile replay down   # preserva todos os volumes
```

B/C: criar outro override novo, mantendo os volumes de A:

```bash
VALIDATION_OVERRIDE=/tmp/sim103-${VALIDATION_RUN}-b.yml
cat > "$VALIDATION_OVERRIDE" <<VOLUMES
volumes:
  gateway_data:
    name: sim103-validation_${VALIDATION_RUN}_b_gateway_data
  fake_interscity_data:
    name: sim103-validation_${VALIDATION_RUN}_b_fake_interscity_data
VOLUMES
compose config
compose up -d --wait --wait-timeout 90 mosquitto interscity-fake gateway backend
# Repetir inspeção de destinos, prontidão, SQLite e estado inicialmente vazio.
curl -fsS --max-time 10 -X POST http://127.0.0.1:$FAKE_INTERSCITY_PORT/__fake/down
curl -fsS --max-time 10 http://127.0.0.1:$FAKE_INTERSCITY_PORT/__fake/stats | jq .
# Exigir down:true e 503 na consulta Collector antes do replay B.
REPLAY_B=sim103-validation-replay-b-$VALIDATION_RUN
compose run -d --no-deps --name "$REPLAY_B" replayer
docker top "$REPLAY_B" -eo pid,args
docker wait "$REPLAY_B"
# Repetir snapshot SQLite: 30 registros, cursores vazios, sem blocked.
curl -sS --max-time 10 -w '\nHTTP %{http_code}\n' \
  http://127.0.0.1:$FAKE_BACKEND_PORT/api/machines/latest
OLD_GATEWAY=$(compose ps -q gateway)
compose stop -t 30 gateway
docker inspect "$OLD_GATEWAY" --format '{{json .State}}'
compose logs gateway
compose up -d --no-deps --force-recreate gateway
# Conferir novo ID, mesmo volume, novo Gateway pronto e snapshot preservado.
# Não executar nenhum Replayer nesta fase C.
curl -fsS --max-time 10 -X POST http://127.0.0.1:$FAKE_INTERSCITY_PORT/__fake/up
# Acompanhar fila até drenar, respeitando backoff; repetir consultas acima.
compose logs gateway
compose --profile replay down   # sem -v; nunca wipe/reset
```

## Evidências e estado final

Arquivos em [evidencias/sim-10.3](evidencias/sim-10.3/), com hashes em
[SHA256SUMS](evidencias/sim-10.3/SHA256SUMS). Os snapshots JSON preservam corpos
HTTP sem edição, logs e resultados. Principais entradas:

- `A/B-config.json` e `runtime.json`: destino efetivo, health, mounts, IDs e SIGTERM.
- `A/B-processes.txt`, `A/B-replayer.log` e `A/B-telemetry.json`: processos,
  confirmações MQTT e os 30 JSONs efetivamente recebidos por cenário.
  `expected-30.json` preserva as 30 entradas esperadas.
- `B-outage.json` e `C-restart-outage.json`: todos os 30 registros, tentativas e cursores.
- `A/C-delivered.json`: fila vazia, dez cursores, WAL e integridade.
- `A/C-collector.json`, `A/C-latest.json`, `A/C-history-1.json`, `A/B-fake-sqlite.json`:
  respostas consultadas e banco definitivo do falso.
- `A-stats.json`, `B-outage-stats.json`, `C-stats.json`, logs do Gateway e regressões.

Os serviços Compose foram encerrados normalmente ao final. Permaneceram somente
os três contêineres de replay encerrados com exit 0 e seis volumes deste projeto:

```text
sim103-validation_gateway_data                 # rodada preliminar
sim103-validation_fake_interscity_data         # rodada preliminar
sim103-validation_a_gateway_data               # cenário A conferido
sim103-validation_a_fake_interscity_data        # cenário A conferido
sim103-validation_b_gateway_data               # cenários B/C
sim103-validation_b_fake_interscity_data        # cenários B/C
```

Uma rodada preliminar enviou 30 registros apenas ao falso isolado, mas o coletor
ad hoc procurava um caminho de processo sem `scripts/../` e não capturou a
simultaneidade. Seus dados/logs foram preservados; A foi repetido em volumes
novos. Houve também um conflito de nome com o contêiner preliminar encerrado;
usaram-se nomes novos, sem removê-lo. A comparação inicial de latest foi ajustada
no coletor temporário para respeitar nulls omitidos. Nenhum desses ajustes mudou
código, testes ou dados do produto. As ocorrências estão em `preliminar/`.

## Problemas restantes e revisão

Não foi encontrado bloqueio operacional nos cenários integrados de persistência
ou recuperação. A corrida da regressão MQTT foi corrigida no teste e não reapareceu
em quatro execuções consecutivas da suíte. Não há bloqueio funcional conhecido
nos cenários validados. Locale alternativo continua SKIP; inspeção visual em
navegador continua NOT RUN, sem extrapolar os resultados de HTTP/API.

Como melhoria operacional não bloqueante, a prontidão do Gateway deve continuar
sendo conferida nos logs da execução atual: service_started não substitui SUBACK.
O roteiro do dashboard deve explicar timestamps distintos por capability.
Não foi necessário adicionar funcionalidades, dependências ou mudar a arquitetura.
Recomenda-se o encerramento técnico da SIM-10.3 no escopo do falso, sujeito à
revisão da equipe, com as limitações SKIP/NOT RUN explícitas. Nenhum staging,
commit, push, merge ou PR foi realizado. Aguardamos revisão antes de alterações
adicionais.
