# SIM-05 — Integração com o InterSCity (SIM-05.1 a SIM-05.6)

## Objetivo e escopo

Integrar o Smart Industry Monitor ao middleware InterSCity: preparar e validar
a API manualmente e, em seguida, fazer o IoT Gateway em C encaminhar cada
telemetria válida ao InterSCity, validando o fluxo com as dez máquinas.

| Subetapa | Conteúdo | Data |
| --- | --- | --- |
| SIM-05.1 | Docker e Docker Compose no ambiente local | 31/10/2026 |
| SIM-05.2 | Acesso à instância InterSCity do LSDi/UFMA | 31/10/2026 |
| SIM-05.3 | Cadastro das capabilities e da máquina 1 | 31/10/2026 |
| SIM-05.4 | Envio manual de uma telemetria e consulta do dado armazenado | 08/11/2026 |
| SIM-05.5 | Envio pelo IoT Gateway em C (`libcurl`), primeira máquina real | 08/11/2026 |
| SIM-05.6 | Validação do fluxo completo com as dez máquinas | 08/11/2026 |

Nas subetapas 05.1 a 05.4, nenhum código foi alterado: as operações foram
executadas manualmente com `curl` e `jq`, a partir do Ubuntu 24.04 (WSL2). As
subetapas 05.5 e 05.6 acrescentam módulos ao IoT Gateway e alteram seu `main.c`,
sem modificar o validador, o assinante MQTT nem o Dataset Replayer.

## Ambiente

Ubuntu 24.04 no WSL2 (Windows 11), `curl` e `jq`; para o Gateway, `libcurl`
8.5.0 com OpenSSL (pacote `libcurl4-openssl-dev`). Docker 29.9.1 (Docker Desktop
4.96.0) e Docker Compose v5.6.0, validados com `hello-world`, acesso à internet
a partir de um container e um serviço Compose com porta publicada em
`127.0.0.1`. O Docker não é necessário para usar a instância do LSDi; fica
disponível para a futura containerização dos componentes do projeto.

## Instância utilizada

URL base: `https://cidadesinteligentes.lsdi.ufma.br/interscity_lh`

O material da disciplina (2018) ensina a instalação local pelo `dev-env`
(`./project setup` e `./project start`). A inspeção do repositório consolidado
`interscity-platform` (tag `v0.2.1`, commit `3162c3c`, outubro de 2023) mostrou
dependências antigas (Kong 0.11.2, PostgreSQL 9.4/9.5, RabbitMQ 3.6.5, base de
build `ruby:2.7-buster`) e um procedimento de desenvolvimento que exige Ruby no
hospedeiro. Optou-se pela instância mantida pelo LSDi. O clone local é usado
somente como referência do código da API (`config/routes.rb`, controllers e
`requests.md`) e como plano B.

A instância executa uma versão próxima, mas não idêntica, à do clone: por
exemplo, o cadastro de recurso devolve `data` como objeto, e não como texto,
e o Data Collector responde 404 para um recurso ainda sem dados.

## Arquitetura observada

```text
cliente (curl; futuramente o IoT Gateway)
  → Apache 2.4.41 (cidadesinteligentes.lsdi.ufma.br, prefixo /interscity_lh)
  → Kong 0.11.0 (API Gateway: /catalog, /adaptor, /collector)
  → Resource Catalog | Resource Adaptor | Data Collector

Envio de dados:
  Resource Adaptor → RabbitMQ (exchange topic "data_stream", chave "<uuid>.<capability>")
  → Data Collector (worker DataReceiver) → MongoDB
```

Evidências: cabeçalhos `Server: Apache/2.4.41`, `Via: kong/0.11.0` e
`X-Kong-Upstream-Latency` nas respostas; código de
`resource-adaptor/app/models/data_manager.rb` e
`data-collector/app/workers/data_receiver.rb` no clone.

O Kong é um API Gateway HTTP, a porta de entrada do middleware. O IoT Gateway
do projeto é um gateway de protocolos (MQTT → HTTP). São papéis diferentes.

## Contrato da API utilizado

| Operação | Requisição | Resposta observada |
| --- | --- | --- |
| Consultar capability | `GET /catalog/capabilities/{name}` | 200, ou 404 se não existir |
| Criar capability | `POST /catalog/capabilities` com `{"name", "description", "capability_type": "sensor"}` | 201 |
| Listar recursos | `GET /catalog/resources?page=N&per_page=M` | 200; 40 por página por padrão, do mais recente ao mais antigo |
| Buscar recurso | `GET /catalog/resources/search?description=...` | 200 |
| Consultar recurso | `GET /catalog/resources/{uuid}` | 200 |
| Cadastrar recurso | `POST /catalog/resources` com `{"data": {...}}` | 201 |
| Enviar telemetria | `POST /adaptor/resources/{uuid}/data` com `{"data": {"<capability>": [{...}]}}` | 201 com `{}` |
| Consultar dados | `POST /collector/resources/{uuid}/data` e `.../data/last` | 200; 404 antes de haver dados |

Fontes: `config/routes.rb`, controllers e `requests.md` do Catalog, do Adaptor
e do Data Collector no clone, confirmados na instância.

`DELETE /catalog/capabilities/{name}` existe, mas nunca deve ser utilizado:
a instância é compartilhada. Recursos não têm rota de remoção.

## Capabilities do projeto

| id | Nome | Origem no contrato | Observação |
| --- | --- | --- | --- |
| 46 | `sim_motor_temperature` | `temperature` (`temperature_motor`) | |
| 47 | `sim_current` | `current` (`current_phase_avg`) | |
| 48 | `sim_operating_state` | `state` | `IDLE`, `NORMAL`, `PEAK` ou `UNKNOWN` |
| 49 | `sim_failure_within_24h` | `failure_within_24h` | Predição do dataset, não falha ocorrida |

Todas foram criadas com `capability_type: sensor` (`function: 0`). O prefixo
`sim_` evita colisão com capabilities de outras equipes. A capability
`temperatura` já existente descreve temperatura de sala e não foi reutilizada.
`power` não tem capability, pois é sempre `null`. As descrições não citam
unidades, ainda não confirmadas no dataset.

Cada criação foi protegida por um GET prévio: o POST só era enviado quando a
consulta retornava 404.

## Recursos das máquinas

### Máquina 1 (cadastro manual)

| Campo | Valor |
| --- | --- |
| `id` | 2825 |
| `uuid` | `18fa502d-2427-4485-a3a6-0b3f64585634` |
| `description` | `Smart Industry Monitor - CNC - machine_id 1` |
| `uri` | `smart-industry-monitor/machines/1` (espelha o tópico MQTT) |
| `lat` / `lon` | -2.558 / -44.309 (posição simbólica na região da UFMA; o dataset não tem localização) |
| `status` | `active` |
| `collect_interval` | 1 (intervalo do replay nos testes, não o período original do dataset) |
| `capabilities` | As quatro `sim_*` |

O mapeamento `machine_id` → UUID fica em `config/interscity/machines.conf`.

### Demais máquinas (script idempotente)

As outras nove máquinas foram cadastradas por
`iot-gateway/scripts/register-interscity-machines.sh`, que automatiza o
procedimento manual com as mesmas proteções:

- confere as quatro capabilities e recusa um mapeamento com CRLF;
- lê o tipo de cada máquina no próprio CSV;
- para cada máquina: se já está no mapeamento, apenas confere a descrição na
  instância; se existe na instância mas não no mapeamento, recupera o UUID; só
  cadastra (pelo Catalog) o que não existe em lugar nenhum;
- qualquer resposta diferente de 201, ou timeout, interrompe sem repetir;
- `--dry-run` mostra o plano sem alterar a instância nem o arquivo.

Execução real: `1 conferidas, 0 recuperadas, 9 criadas`. Em seguida, o
`--dry-run` retornou `10 conferidas, 0 ações planejadas`, comprovando a
idempotência. O script foi versionado antes de ser executado.

| machine_id | Tipo | id | UUID |
| --- | --- | --- | --- |
| 1 | CNC | 2825 | `18fa502d-2427-4485-a3a6-0b3f64585634` |
| 2 | CNC | 2826 | `c29d711f-1f60-44c4-91ea-d72f49343c4e` |
| 3 | CNC | 2827 | `fd4a18b8-14ba-49af-a9a0-d96399a39702` |
| 6 | Pump | 2828 | `ff56eafb-6949-4007-b253-c93fe624b643` |
| 7 | Pump | 2829 | `d9e4a7e3-5323-4dfc-af77-eb00610c67c9` |
| 8 | Pump | 2830 | `0698bbb2-f1ed-4626-bca8-a8af2f182ea0` |
| 9 | Pump | 2831 | `f49dbde7-3f4f-46ba-8344-fa23bb94829b` |
| 11 | Compressor | 2832 | `a1ef2f0a-648f-4e30-9de3-cf39bfb6128c` |
| 12 | Compressor | 2833 | `d31669ef-7b92-47e2-8aa1-a9332a6d8841` |
| 13 | Compressor | 2834 | `6c5496cf-1d67-4476-bcb9-e9243da7feb7` |

Todas usam descrição `Smart Industry Monitor - <tipo> - machine_id <id>`, `uri`
`smart-industry-monitor/machines/<id>` e os demais campos da máquina 1.

### Decisão: cadastro pelo Catalog

O cadastro foi tentado primeiro pelo Adaptor (`POST /adaptor/resources`), ponto
de entrada previsto para os IoT Gateways. Em 31/10/2026:

1. A requisição não recebeu nenhum byte em 30 s (`curl: (28)`).
2. A listagem dos recursos mais recentes e a busca por descrição (validada com
   um recurso conhecido) mostraram que nada foi criado.
3. Um POST com corpo `{}` respondeu 400 em 0,49 s: o Adaptor recebe requisições.
4. O código (`resource-adaptor/lib/platform.rb`) repassa o cadastro ao Catalog
   pelo endereço configurado em `CATALOGUER_HOST`.

Conclusão: falha na chamada interna Adaptor → Catalog da instância. O recurso
foi cadastrado diretamente em `POST /catalog/resources`, com o mesmo corpo.
As telemetrias continuam entrando pelo Adaptor, único serviço com rota de dados.
O problema deve ser reportado ao LSDi.

## Formato das telemetrias

```json
{
  "data": {
    "sim_motor_temperature":  [{"value": 49.51,  "timestamp": "2024-01-01T00:00:00"}],
    "sim_current":            [{"value": 5.1,    "timestamp": "2024-01-01T00:00:00"}],
    "sim_operating_state":    [{"value": "IDLE", "timestamp": "2024-01-01T00:00:00"}],
    "sim_failure_within_24h": [{"value": 0,      "timestamp": "2024-01-01T00:00:00"}]
  }
}
```

- Cada capability recebe uma lista, exigência do Adaptor.
- Atributo único `value`; os tipos do contrato JSON são preservados.
- O timestamp histórico é preservado; muda apenas a notação (espaço → `T`,
  ISO 8601). Sem fuso informado, o Collector o interpreta como UTC.
- O Collector armazena `timestamp` como `date`.
- `power` não é enviado.
- Medição ausente (`null`) omite sua capability; zero é enviado (decisão da
  SIM-05.5.2).

## Validação SIM-05.4 (08/11/2026)

Amostra: primeiro registro da máquina 1 (`2024-01-01 00:00:00`, temperatura
49.51, corrente 5.1, estado `IDLE`, `failure_within_24h` 0).

| Passo | Resultado |
| --- | --- |
| Consulta ao Collector antes do envio | 404 `{"error":"Resource not found"}` |
| `POST /adaptor/resources/{uuid}/data` | 201 `{}` |
| Collector após 3 s (`/data` e `/data/last`) | 4 capabilities com 49.51, 5.1, `IDLE` e 0; `date` `2024-01-01T00:00:00.000Z` |

Caminho comprovado: `curl` → Apache → Kong → Adaptor → RabbitMQ → Data
Collector, e consulta de volta pelo mesmo caminho.

## Comportamentos do InterSCity que orientaram o Gateway

1. O 201 do Adaptor indica publicação no RabbitMQ, não armazenamento. O
   armazenamento só é comprovado consultando o Collector, de forma análoga ao
   PUBACK do MQTT, que não comprova o processamento pelo Gateway.
2. UUID desconhecido: o Adaptor responde 201, e o DataReceiver descarta a
   mensagem, registrando apenas no log interno do servidor. O mapeamento precisa
   estar correto.
3. Uma capability não associada ao recurso é associada automaticamente pelo
   DataReceiver. Os nomes devem ser constantes no código, nunca montados
   livremente.
4. Os POSTs não são idempotentes: repetir cria recurso ou amostra duplicada.
   Após timeout, consultar antes de reenviar.
5. As respostas podem diferir do código de referência (por exemplo, `data` como
   objeto ou texto). Devem ser interpretadas de forma tolerante.
6. A instância usa HTTPS: a `libcurl` precisa de suporte a TLS.
7. O Collector responde 404 na consulta de um recurso ainda sem dados.

## SIM-05.5 — Envio pelo IoT Gateway em C (08/11/2026)

O Gateway ganhou três módulos independentes, no estilo dos existentes (retorno
0/-1, `enum` de erros, saída intacta em erro, sem impressão), e a integração no
`main.c`:

| Parte | Arquivos | Responsabilidade | Commit |
| --- | --- | --- | --- |
| 05.5.1 | `interscity_mapping.{h,c}` | Ler `machines.conf`: rejeita CRLF, UUID fora do formato canônico, IDs/UUIDs repetidos, campos extras e arquivo vazio, informando a linha | `200803c` |
| 05.5.2 | `interscity_payload.{h,c}` | Converter `GatewayTelemetry` no corpo JSON do Adaptor | `0f4d299` |
| 05.5.3 | `interscity_client.{h,c}` | `POST .../adaptor/resources/{uuid}/data` com `libcurl` | `852b922` |
| 05.5.4 | `main.c`, `Makefile`, `README.md` do Gateway | Opções de linha de comando e envio após a validação | `80b6786` |

### Decisões

- **Medição ausente** omite a capability; **zero** é enviado. Estado e predição
  são sempre enviados; `power` não é enviado.
- **Timestamp** aceito somente como `AAAA-MM-DD HH:MM:SS` (faixas de mês, dia,
  hora, minuto e segundo verificadas) e enviado em ISO 8601 sem fuso. Um valor
  que o Collector não entendesse seria descartado em silêncio pelo worker.
- **Números** são escritos com a menor representação decimal que reconstrói
  exatamente o mesmo `double` (`49.51`, `50.0`). A formatação padrão da `json-c`
  escreveria `49.509999999999998`. O log `Telemetria válida` da SIM-04 continua
  com a formatação padrão; a diferença é apenas de apresentação.
- **Nomes das capabilities** são constantes fixas (`INTERSCITY_CAP_*`).
- **Uma conexão HTTP/TLS reutilizada** entre envios; certificado sempre
  verificado; somente HTTP e HTTPS permitidos.
- **Somente HTTP 201 é sucesso.** Outras respostas, timeout e falhas de rede são
  classificados separadamente, registrados em stderr e **não encerram o Gateway**.
- **Sem reenvio automático**, pois o POST não é idempotente.
- **Compatibilidade:** sem `--interscity-url` e `--interscity-map`, o Gateway se
  comporta exatamente como na SIM-04.

Uso e mensagens: [README do IoT Gateway](../iot-gateway/README.md#envio-ao-interscity-sim-055).

### Testes automatizados

Nenhum teste existente foi alterado. Nenhum teste automatizado acessa a
instância real.

| Alvo | Testes novos | Resultado em 08/11/2026 |
| --- | --- | --- |
| `make -C iot-gateway test` | Mapeamento e payload (C, `assert`); cliente HTTP contra um InterSCity falso em 127.0.0.1 (201, 400, 500, timeout, porta fechada, reuso de conexão); opções e mapeamentos inválidos do Gateway | Aprovado, sem avisos |
| `make -C iot-gateway test-mqtt` | Mosquitto temporário + InterSCity falso + Gateway real: corpo de cada envio, medição omitida, reuso da conexão, máquina sem mapeamento, resposta 500 e continuidade | Aprovado |
| `make -C iot-gateway test-e2e` | Nenhum (regressão) | Aprovado, com as mesmas saídas da SIM-04 |

O resultado idêntico do `test-e2e` comprova que o modo padrão foi preservado.

### Primeiro envio real (SIM-05.5.5)

Máquina 2 (ainda sem dados), 3 amostras, Gateway com
`--interscity-url https://cidadesinteligentes.lsdi.ufma.br/interscity_lh` e
`--interscity-map config/interscity/machines.conf`.

| Timestamp | CSV (temperatura / corrente / modo / predição) | Data Collector |
| --- | --- | --- |
| 2024-01-01 00:00:00 | 49.47 / 4.61 / idle / 0 | 49.47 / 4.61 / IDLE / 0 |
| 2024-01-01 00:24:00 | 49.08 / 4.13 / idle / 0 | 49.08 / 4.13 / IDLE / 0 |
| 2024-01-01 00:43:00 | 42.01 / 5.21 / idle / 0 | 42.01 / 5.21 / IDLE / 0 |

Antes do envio, o Collector respondia 404 para a máquina 2. As 12 medições
(3 amostras × 4 capabilities) coincidiram com o CSV, com as datas históricas
preservadas. Resumo do Gateway: `3 enviadas, 0 falhas, 0 sem mapeamento`.

| Envio | Tempo | Conexão |
| --- | --- | --- |
| 1º | 306 ms | Nova (inclui a negociação TLS) |
| 2º | 64 ms | Reutilizada |
| 3º | 63 ms | Reutilizada |

## SIM-05.6 — Validação com dez máquinas (08/11/2026)

```text
10 Dataset Replayers (C) → Mosquitto → IoT Gateway (C) → HTTPS → InterSCity (LSDi) → Data Collector
```

Execução: broker com `config/mosquitto/mosquitto.conf`, Gateway com as opções
InterSCity e `LIMIT=3 INTERVAL_MS=1000` em `run-ten-machines.sh`. As amostras
de cada máquina foram contadas no Collector antes e depois, pela capability
`sim_operating_state`, que é sempre enviada.

| machine_id | Antes | Depois | Diferença |
| --- | --- | --- | --- |
| 1 | 1 | 4 | +3 |
| 2 | 3 | 6 | +3 |
| 3, 6, 7, 8, 9, 11, 12, 13 | 0 | 3 | +3 |

- Replayers: 10 processos, `Publicações confirmadas: 3` em cada um.
- Gateway: `InterSCity: 30 enviadas, 0 falhas, 0 sem mapeamento.`
- Tempo dos 30 envios: mínimo 58 ms, média 69 ms, máximo 263 ms. O máximo é
  compatível com o custo de abrir a conexão TLS observado na SIM-05.5.5; a
  contagem de conexões novas desta execução não foi registrada.

**Duplicatas observadas.** As máquinas 1 e 2 já tinham amostras com os mesmos
timestamps (SIM-05.4 e SIM-05.5.5), e o InterSCity gravou as novas cópias. Isso
comprova, por medição, que o envio não é idempotente e que o middleware não
deduplica amostras.

**Carga da thread única.** Com dez mensagens por segundo e média de 69 ms por
envio, o Gateway passa cerca de 69% do tempo aguardando o InterSCity. O fluxo
acompanhou a carga, com folga de cerca de 30% para variações de rede.

### Critérios da SIM-05.6

| Critério | Evidência |
| --- | --- |
| Máquinas cadastradas | 10 recursos; `--dry-run` com 10 conferidas |
| Publicação MQTT confirmada | 30 PUBACKs (10 × 3) |
| Gateway recebeu e validou | 30 telemetrias válidas encaminhadas (resumo: 30 enviadas, 0 falhas, 0 sem mapeamento) |
| Requisição HTTP executada e confirmada | 30 respostas HTTP 201 |
| Dado recuperável por consulta | +3 amostras por máquina no Data Collector |
| Erros diagnosticados por camada | MQTT, validação, mapeamento, montagem do JSON e HTTP têm mensagens distintas |

## Limitações conhecidas

- **Envio síncrono numa única thread.** Enquanto um POST aguarda resposta, novas
  mensagens MQTT esperam. Adequado para as dez máquinas (cerca de 69% de
  ocupação); não para centenas, que exigirão agrupar amostras ou paralelizar.
- **Perda em caso de falha.** O QoS 1 já foi confirmado ao broker quando o envio
  acontece; uma amostra cujo envio falha não é reenviada.
- **201 não é armazenamento.** O Adaptor confirma a publicação no RabbitMQ; a
  persistência só é comprovada consultando o Data Collector.
- **Sem deduplicação**, no Gateway e no InterSCity.
- **Instância compartilhada**, sem garantia de disponibilidade no dia da
  demonstração, e com a falha Adaptor → Catalog descrita acima.
- **Sinal durante um envio** só é observado após o envio terminar (até o timeout).

## Cuidados com a instância compartilhada

- Nunca usar DELETE em capabilities. Recursos não podem ser removidos; correções
  apenas por PUT ou por `status`.
- Identificar recursos pela descrição iniciada por `Smart Industry Monitor` e
  capabilities pelo prefixo `sim_`.
- Testes de carga ou de escalabilidade contra a instância dependem de
  autorização do professor.

## Reprodução

### Consultas (somente leitura)

```sh
export INTERSCITY_URL="https://cidadesinteligentes.lsdi.ufma.br/interscity_lh"
UUID=18fa502d-2427-4485-a3a6-0b3f64585634

curl -sS "$INTERSCITY_URL/catalog/capabilities" \
  | jq -r '.capabilities[] | select(.name | startswith("sim_")) | "\(.id)\t\(.function)\t\(.name)"'

curl -sS "$INTERSCITY_URL/catalog/resources/$UUID" \
  | jq '.data | if type == "string" then fromjson else . end | {uuid, description, capabilities}'

curl -sS -X POST "$INTERSCITY_URL/collector/resources/$UUID/data/last" | jq .
```

### Fluxo completo (grava dados na instância compartilhada)

Cada execução grava novas amostras, inclusive cópias de amostras já enviadas.
Na raiz do repositório, com o broker em execução (`mosquitto -c config/mosquitto/mosquitto.conf`):

```sh
make -C iot-gateway all
make -C dataset-replayer all

# Terminal do Gateway
iot-gateway/build/iot-gateway --host 127.0.0.1 --port 1883 \
  --interscity-url "$INTERSCITY_URL" --interscity-map config/interscity/machines.conf

# Outro terminal, após "Gateway pronto"
LIMIT=3 INTERVAL_MS=1000 MQTT_HOST=127.0.0.1 MQTT_PORT=1883 \
  bash ./dataset-replayer/scripts/run-ten-machines.sh
```

## Pendências

- Reportar ao LSDi a falha na chamada Adaptor → Catalog.
- Revisar e integrar a branch `feature/sim-05-interscity-integration` à `main`.
- Etapas seguintes que dependem desta integração: backend em C consultando o
  Catalog e o Data Collector; tolerância a falhas (reenvio com deduplicação);
  escalabilidade do envio (agrupamento ou paralelismo).
