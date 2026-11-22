# SIM-10.1 — Mosquitto e Gateway com SQLite persistente

## Base e escopo

Branch `feature/docker-compose`, working tree inicialmente limpo. HEAD e main
local: `13050d8e6b4c8c7c86776c0514395e4851722603`, merge da PR #10 (`fix/sim-09`).
A ancestralidade do encerramento SIM-09 (`a65d5e1`) em main foi conferida antes
da implementação. Não houve merge, alteração da main ou mudança em C/Makefiles.

Dois serviços, na rede padrão do Compose: Mosquitto e IoT Gateway. InterSCity
continua externo; replayers, backend e dashboard não foram containerizados.

## Decisões

- `iot-gateway/Dockerfile`: build em Arch Linux com `make all` existente;
  estágio final sem compiladores. Bibliotecas json-c, libmosquitto, libcurl e
  SQLite, além de certificados CA para HTTPS. O CLI sqlite3 permite diagnóstico.
- A tentativa inicial com Bookworm falhou: json-c 0.16 não fornece
  `json_tokener_error_memory`. Trixie com json-c 0.18 compilou, mas falhou no caso
  JSON válido `test_valid_lexemes` da suíte existente. A presença da API no
  [header 0.18](https://github.com/json-c/json-c/blob/json-c-0.18/json_tokener.h)
  não bastou para garantir compatibilidade de comportamento. A imagem final
  Arch usa json-c 0.19, como a SIM-09; os testes C passaram. Não foi alterado C
  ou enfraquecido teste para acomodar bibliotecas antigas. O estágio comum de
  dependências mantém as mesmas bibliotecas no build e na execução.
- Gateway executa diretamente como UID/GID `10001:10001`. A imagem prepara
  `/data` com esse proprietário e modo 0750. Um volume novo herda essa preparação;
  volumes existentes precisam ter permissões compatíveis. Não há chown de
  volumes arbitrários na inicialização nem execução do Gateway como root.
- `gateway_data` monta o **diretório** `/data`. Banco, WAL e SHM ficam juntos em
  armazenamento local adequado ao WAL. Nome físico padrão:
  `smart-industry-monitor_gateway_data`, prefixado pelo projeto Compose.
- Mapa do repositório montado somente leitura em
  `/etc/smart-industry/machines.conf`; CLI aponta explicitamente esse caminho.
- `--host mosquitto --port 1883` usa DNS da rede Compose. A configuração Docker
  escuta em `0.0.0.0` **dentro** do contêiner. A publicação no host é somente
  `127.0.0.1:${MQTT_PORT:-1883}`. O arquivo de desenvolvimento local não mudou.
- Mosquitto roda como UID/GID 1883, sem persistência MQTT, logs no stdout.
  Healthcheck publica mensagem vazia em `smart-industry/health` com QoS 1,
  fora do filtro de telemetrias; Gateway inicia após broker saudável.
- Ambos usam `restart: unless-stopped`, sem réplicas adicionais. ENTRYPOINT
  direto preserva SIGTERM; Gateway tem timeout HTTP de 5000 ms e tolerância de
  parada de 30 s para concluir requisição/transação e join.

## Execução e destino HTTP

```sh
docker compose config
docker compose build gateway
docker compose up -d mosquitto gateway
docker compose logs --tail=30 mosquitto gateway
```

`INTERSCITY_URL` é interpolada pelo Compose no argumento `--interscity-url`;
não exige suporte novo a variáveis no código C. O valor padrão é
**`http://127.0.0.1:9`**, endereço deliberadamente indisponível dentro do Gateway.
Isso permite testar persistência sem publicar na instância compartilhada. Nesse
modo, mensagens ficam inflight aguardando reconciliação; não há entrega HTTP.

Para outro destino autorizado, definir antes do up:

```sh
export INTERSCITY_URL='http://endereco-autorizado:8000'
```

O endereço deve ser acessível do contêiner: localhost é o próprio contêiner.
Para HTTPS, certificados CA já estão instalados e a validação TLS existente
permanece ativa. O bundle foi conferido, mas não houve teste HTTPS real na UFMA.
O destino precisa corresponder aos UUIDs do mapa e das pendências existentes.
Não apontar esta demonstração para a UFMA sem autorização específica.

Se a porta local 1883 estiver ocupada, definir `export MQTT_PORT=18883` antes de
executar os comandos. Não encerre brokers externos para liberar a porta.

O modo fila é obrigatório no Compose: `--queue-db /data/telemetry.db`.
A persistência SQLite não implica persistência MQTT ou garantia exactly-once;
valem as limitações da [SIM-09](sim-09-fila-duravel.md).

## Reprodução da verificação de persistência

Usar projeto isolado e o mesmo nome de projeto em todos os comandos:

```sh
export COMPOSE_PROJECT_NAME=sim101-validation
export MQTT_PORT=18883
export INTERSCITY_URL=http://127.0.0.1:9
docker compose config
docker compose up -d --build mosquitto gateway
docker compose logs gateway
# Aguardar a linha Gateway pronto antes de publicar.
docker compose exec -T gateway id
docker compose exec -T gateway sqlite3 -readonly /data/telemetry.db \
  'PRAGMA journal_mode; PRAGMA integrity_check; SELECT count(*) FROM telemetry_queue;'
docker compose exec -T gateway ls -ln /data
```

O banco novo deve estar vazio. Para a amostra **sintética**, somente com o destino
local indisponível acima, publicar pelo cliente da própria imagem Mosquitto:

```sh
docker compose exec -T mosquitto mosquitto_pub -h mosquitto -q 1 \
  -t smart-industry/machines/1/telemetry \
  -m '{"machine_id":1,"machine_type":"CNC","timestamp":"2024-01-01 00:00:00","temperature":49.51,"current":5.1,"power":null,"state":"IDLE","failure_within_24h":0}'
docker compose exec -T gateway sqlite3 -readonly /data/telemetry.db \
  'SELECT machine_id,timestamp,status,attempt_count FROM telemetry_queue;'
```

Aguardar o estado inflight e guardar a saída. Conferir mounts pelo ID retornado
por `docker compose ps -q gateway`, usando `docker inspect <id>`: `/data` deve
ser volume nomeado RW e o mapa bind RO.

```sh
docker compose stop gateway
docker compose logs --tail=15 gateway
# Conferir ExitCode=0 no docker inspect do contêiner parado.
docker compose start gateway
# Aguardar prontidão e conferir a mesma linha SQL.
docker compose up -d --force-recreate --no-deps gateway
# Aguardar prontidão e conferir novamente linha SQL e mounts.
docker compose exec -T gateway sqlite3 -readonly /data/telemetry.db \
  'PRAGMA journal_mode; PRAGMA integrity_check; SELECT machine_id,timestamp,status,attempt_count FROM telemetry_queue;'
docker compose down
docker volume inspect sim101-validation_gateway_data
```

Não usar `down -v`. Recriar com outro nome de projeto cria outro volume, não
migra os dados. Preservar diretório e WAL/SHM; não copiar apenas o arquivo do
banco enquanto estiver ativo. Volume antigo com proprietário incompatível exige
ajuste administrativo dirigido ao volume correto para UID/GID 10001.

## Resultados observados nesta rodada

Docker Engine 29.9.2 e Compose 5.6.1 disponíveis; acesso ao daemon exigiu execução
fora do sandbox. Validação no projeto isolado, sem interferir em contêineres
preexistentes:

| Verificação | Resultado |
| --- | --- |
| compose config | Configuração válida, dois serviços e volume gateway_data |
| compose build gateway | Passou em Arch Linux, make all sem warnings GCC |
| Testes C na imagem final, sem rede | Parser JSON, fila SQLite, mapeamento, payload e Subscriber passaram |
| up mosquitto gateway | Broker saudável, CONNACK/SUBACK e Gateway pronto |
| Inicialização SQLite | journal_mode=wal e integrity_check=ok; banco anterior preservado na troca da imagem |
| Usuário/permissões | UID/GID 10001; /data 0750; banco/WAL/SHM pertencem a 10001:10001 |
| Mapa e certificados | Mapa RO por inspect; CA bundle presente e legível |
| Persistência real | Duas amostras sintéticas: cabeça inflight:1 e outra pending:0, sem HTTP externo |
| stop/start e force-recreate | SIGTERM com saída 0; mesmo registro, payload e attempt_count no mesmo volume |
| down sem -v | Contêineres/rede do teste removidos; volume nomeado permanece |

Trechos observados:

```text
Fila SQLite habilitada (10 máquinas mapeadas; worker HTTP independente).
Gateway pronto: smart-industry/machines/+/telemetry (QoS 1).
uid=10001(gateway) gid=10001(gateway)
10001:10001 750 /data
10001:10001 644 /data/telemetry.db
10001:10001 644 /data/telemetry.db-wal
10001:10001 644 /data/telemetry.db-shm
```

O volume `sim101-validation_gateway_data` ficou preservado para revisão e contém
**duas amostras sintéticas**. Não reutilizá-lo com destino real. As imagens de build
ficaram em cache; nenhum contêiner de teste ficou executando. Nenhuma telemetria
foi publicada na UFMA.

O primeiro ensaio dos testes C não montou a fixture do mapa e foi repetido com
`config/` somente leitura. Uma consulta auxiliar também foi corrigida para usar
aspas simples em literais SQL (SQLite atual não aceita aquela forma com aspas
duplas). Essas correções ficaram no roteiro temporário de verificação; não
alteraram a aplicação nem as suítes existentes.

Bibliotecas da imagem final observadas: json-c 0.19, libmosquitto 2.1.2,
libcurl 8.23.0 e SQLite 3.54.1. A instalação de pacotes informou alguns itens já
atualizados; não foram warnings do compilador. O CLI sqlite3 e as ferramentas
Mosquitto vêm dos pacotes Arch usados pelas bibliotecas; somente o Gateway é
iniciado no seu serviço.

Para repetir os cinco testes C na imagem de build, sem instalar ferramentas
no host (executar da raiz do repositório):

```sh
docker build --target build -t sim101-gateway-build iot-gateway
docker run --rm --network none \
  --mount "type=bind,source=$(pwd)/iot-gateway/tests,target=/src/tests,readonly" \
  --mount "type=bind,source=$(pwd)/config,target=/config,readonly" \
  sim101-gateway-build sh -ec '
make build/test_gateway_telemetry build/test_telemetry_queue build/test_interscity_mapping build/test_interscity_payload build/test_mqtt_subscriber
./build/test_gateway_telemetry
./build/test_telemetry_queue
./build/test_interscity_mapping
./build/test_interscity_payload
./build/test_mqtt_subscriber'
```

Não foram repetidas as suítes completas MQTT/HTTP/E2E da SIM-09 nesta rodada;
foram executados esses cinco testes C e as verificações Docker acima. Não houve
teste de entrega em serviço HTTP externo. O volume de validação continua com
pendências porque o endpoint local de teste foi mantido indisponível.

## Limitações e revisão

- Mosquitto anônimo, sem TLS, adequado à demonstração local. Não expor publicamente.
- A porta publicada é loopback; outros serviços da rede Compose alcançam o broker.
- Porta ocupada e volume antigo sem permissão causam falha; verificar logs.
- Não escalar o Gateway: o arquivo SQLite deve ter um único worker consumidor.
- Tags de imagens e pacotes recebem atualizações; esta etapa não fixa digests.
- Timeout HTTP é 5 s; revisar grace period ao alterá-lo. DNS/TCP síncrono pode
  exceder a expectativa prática de tempo; manter as limitações conhecidas.
- SQLite não tem limite de capacidade configurável; acompanhar espaço em disco.
- HTTPS real, entrega ao InterSCity externo e demais componentes ficam para
  validações posteriores autorizadas. O teste desta etapa verifica fila durável,
  não entrega externa nem exactly-once.
