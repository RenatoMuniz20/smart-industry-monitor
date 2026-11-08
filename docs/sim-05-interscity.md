# SIM-05 — Integração com o InterSCity (SIM-05.1 a SIM-05.4)

## Objetivo e escopo

Preparar e validar manualmente a integração do Smart Industry Monitor com o
middleware InterSCity, antes de qualquer alteração no IoT Gateway em C.

| Subetapa | Conteúdo | Data |
| --- | --- | --- |
| SIM-05.1 | Docker e Docker Compose no ambiente local | 31/10/2026 |
| SIM-05.2 | Acesso à instância InterSCity do LSDi/UFMA | 31/10/2026 |
| SIM-05.3 | Cadastro das capabilities e da máquina 1 | 31/10/2026 |
| SIM-05.4 | Envio manual de uma telemetria e consulta do dado armazenado | 08/11/2026 |

Nenhum módulo C, teste ou script existente foi alterado. Todas as operações
foram executadas manualmente com `curl` e `jq`, a partir do Ubuntu 24.04 (WSL2).

## Ambiente

Ubuntu 24.04 no WSL2 (Windows 11), `curl` e `jq`. Docker 29.9.1 (Docker Desktop
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

## Recurso da máquina 1

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
- `power` não é enviado. A política para medições ausentes (`null`) será
  definida e testada na SIM-05.5.

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

## Comportamentos relevantes para a SIM-05.5

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

## Cuidados com a instância compartilhada

- Nunca usar DELETE em capabilities. Recursos não podem ser removidos; correções
  apenas por PUT ou por `status`.
- Identificar recursos pela descrição iniciada por `Smart Industry Monitor` e
  capabilities pelo prefixo `sim_`.
- Testes de carga ou de escalabilidade contra a instância dependem de
  autorização do professor.

## Reprodução (somente leitura)

```sh
export INTERSCITY_URL="https://cidadesinteligentes.lsdi.ufma.br/interscity_lh"
UUID=18fa502d-2427-4485-a3a6-0b3f64585634

curl -sS "$INTERSCITY_URL/catalog/capabilities" \
  | jq -r '.capabilities[] | select(.name | startswith("sim_")) | "\(.id)\t\(.function)\t\(.name)"'

curl -sS "$INTERSCITY_URL/catalog/resources/$UUID" \
  | jq '.data | if type == "string" then fromjson else . end | {uuid, description, capabilities}'

curl -sS -X POST "$INTERSCITY_URL/collector/resources/$UUID/data/last" | jq .
```

## Pendências

- Cadastrar as máquinas 2, 3, 6, 7, 8, 9, 11, 12 e 13 com um script que só cria
  o que ainda não existe.
- SIM-05.5: envio pelo IoT Gateway em C (`libcurl`), começando pela máquina 1.
- SIM-05.6: validação do fluxo Replayer → Mosquitto → Gateway → InterSCity com
  dez máquinas.
- Reportar ao LSDi a falha na chamada Adaptor → Catalog.
