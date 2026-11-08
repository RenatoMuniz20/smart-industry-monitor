# smart-industry-monitor

Projeto final da disciplina de Sistemas Distribuídos (UFMA): protótipo
distribuído de monitoramento industrial. Telemetrias históricas de um dataset
público são reproduzidas por processos independentes, transportadas por MQTT,
validadas por um IoT Gateway e enviadas ao middleware InterSCity.

## Arquitetura

```text
Dataset CSV (Industrial Machine Predictive Maintenance v3)
  → Dataset Replayers em C (um processo por máquina)
  → MQTT 3.1.1, QoS 1 (Eclipse Mosquitto)
  → IoT Gateway em C (validação do contrato JSON)
  → HTTPS (libcurl)
  → InterSCity: Kong → Resource Adaptor → RabbitMQ → Data Collector
```

Conceitos de Sistemas Distribuídos demonstrados até aqui: processos
concorrentes independentes, comunicação assíncrona por publish/subscribe,
broker como intermediário desacoplador, gateway de protocolos (MQTT → HTTP),
middleware baseado em microsserviços e REST/JSON, e confirmações em camadas
(PUBACK do broker, HTTP 201 do Adaptor e consulta ao Data Collector).

## Situação dos componentes

| Etapa | Componente | Situação | Documentação |
| --- | --- | --- | --- |
| SIM-01 | Leitura e validação do CSV em C | Concluída | [Dataset Replayer](dataset-replayer/README.md) |
| SIM-02 | Normalização para o contrato de telemetria | Concluída | [Dataset Replayer](dataset-replayer/README.md) |
| SIM-03 | Publicação MQTT com QoS 1 | Concluída | [SIM-03](docs/sim-03-validacao.md) |
| SIM-04 | IoT Gateway: assinatura MQTT e validação | Concluída | [IoT Gateway](iot-gateway/README.md), [SIM-04](docs/sim-04-validacao.md) |
| SIM-05 | Integração com o InterSCity (instância do LSDi/UFMA) | Concluída | [SIM-05](docs/sim-05-interscity.md) |
| — | Backend/API REST em C | Pendente | — |
| — | Dashboard web | Pendente | — |
| — | Tolerância a falhas e escalabilidade | Pendente | — |
| — | Docker Compose e implantação em nuvem | Pendente | — |

## Estrutura do repositório

| Caminho | Conteúdo |
| --- | --- |
| `data/predictive_maintenance_v3.csv` | Dataset (24.042 registros, 15 colunas) |
| `dataset-replayer/` | Replayer em C: leitura do CSV, normalização, JSON e publicação MQTT |
| `iot-gateway/` | Gateway em C: assinatura MQTT, validação e envio ao InterSCity |
| `iot-gateway/scripts/register-interscity-machines.sh` | Cadastro idempotente das máquinas no InterSCity |
| `config/mosquitto/mosquitto.conf` | Broker de desenvolvimento (somente `127.0.0.1`) |
| `config/interscity/machines.conf` | Mapeamento `machine_id` → UUID dos recursos no InterSCity |
| `docs/` | Validações e decisões de cada etapa |

As dez máquinas usadas são CNC 1, 2 e 3; Pump 6, 7, 8 e 9; e Compressor 11, 12 e 13.

## Ambiente

Desenvolvido e validado no Ubuntu 24.04 (WSL2). Dependências:

```sh
sudo apt install build-essential pkg-config libjson-c-dev libmosquitto-dev \
  libcurl4-openssl-dev mosquitto mosquitto-clients python3 curl jq
```

Os documentos da SIM-03 e da SIM-04 registram também uma validação em Arch Linux.

## Compilação e testes

Na raiz do repositório:

```sh
make -C dataset-replayer all
make -C dataset-replayer test
make -C dataset-replayer test-mqtt

make -C iot-gateway all
make -C iot-gateway test       # sem broker e sem rede externa
make -C iot-gateway test-mqtt  # Mosquitto temporário em porta dinâmica
make -C iot-gateway test-e2e   # dez produtores reais → Gateway
```

Nenhum teste automatizado acessa a instância real do InterSCity: o cliente HTTP
é testado contra um servidor falso em `127.0.0.1`.

## Execução do fluxo completo

Em terminais separados, na raiz do repositório:

```sh
# 1. Broker (verifique antes se a porta está livre: sudo ss -ltnp | grep ':1883')
mosquitto -c config/mosquitto/mosquitto.conf

# 2. Gateway com envio ao InterSCity (sem as opções --interscity-*, apenas valida e registra)
iot-gateway/build/iot-gateway --host 127.0.0.1 --port 1883 \
  --interscity-url https://cidadesinteligentes.lsdi.ufma.br/interscity_lh \
  --interscity-map config/interscity/machines.conf

# 3. Dez produtores, após a linha "Gateway pronto"
LIMIT=3 INTERVAL_MS=1000 MQTT_HOST=127.0.0.1 MQTT_PORT=1883 \
  bash ./dataset-replayer/scripts/run-ten-machines.sh
```

A instância do InterSCity é compartilhada pelas equipes da disciplina: cada
execução do passo 3 grava novas amostras, inclusive cópias de amostras já
enviadas. Consultas e cuidados estão em [SIM-05](docs/sim-05-interscity.md).

## Limitações atuais

- `power` não existe no dataset e é publicado como `null`;
  `failure_within_24h` é uma predição do dataset, não uma falha medida.
- Não há reconexão MQTT automática, reenvio HTTP nem deduplicação.
- O Gateway envia ao InterSCity de forma síncrona numa única thread: adequado
  para as dez máquinas (média de 69 ms por envio), não para centenas.
- O Mosquitto de desenvolvimento aceita conexões anônimas somente em localhost;
  essa configuração não é adequada para implantação exposta.
