# smart-industry-monitor

Projeto final da disciplina de Sistemas Distribuídos (UFMA): protótipo
distribuído de monitoramento industrial. Telemetrias históricas de um dataset
público são reproduzidas por processos independentes, transportadas por MQTT,
validadas por um IoT Gateway, enviadas ao middleware InterSCity e expostas por
um backend REST em C.

## Arquitetura

```text
Dataset CSV (Industrial Machine Predictive Maintenance v3)
  → Dataset Replayers em C (um processo por máquina)
  → MQTT 3.1.1, QoS 1 (Eclipse Mosquitto)
  → IoT Gateway em C (validação do contrato JSON e reconexão MQTT)
  → Entrega direta SIM-08 ou fila SQLite + worker HTTP independente SIM-09
  → HTTPS (libcurl)
  → InterSCity: Kong → Resource Adaptor → RabbitMQ → Data Collector
  → Backend REST em C (libmicrohttpd; consulta ao InterSCity por libcurl e json-c)
  → Dashboard web (pendente)
```

Conceitos de Sistemas Distribuídos demonstrados até aqui: processos
concorrentes independentes, comunicação assíncrona por publish/subscribe,
broker como intermediário desacoplador, gateway de protocolos (MQTT → HTTP),
middleware baseado em microsserviços, REST/JSON sobre dados consultados,
confirmações em camadas (PUBACK do broker, HTTP 201 do Adaptor e consulta ao
Data Collector) e consultas idempotentes separadas de escritas.

## Situação dos componentes

| Etapa | Componente | Situação | Documentação |
| --- | --- | --- | --- |
| SIM-01 | Leitura e validação do CSV em C | Concluída | [Dataset Replayer](dataset-replayer/README.md) |
| SIM-02 | Normalização para o contrato de telemetria | Concluída | [Dataset Replayer](dataset-replayer/README.md) |
| SIM-03 | Publicação MQTT com QoS 1 | Concluída | [SIM-03](docs/sim-03-validacao.md) |
| SIM-04 | IoT Gateway: assinatura MQTT e validação | Concluída | [IoT Gateway](iot-gateway/README.md), [SIM-04](docs/sim-04-validacao.md) |
| SIM-05 | Integração com o InterSCity (instância do LSDi/UFMA) | Concluída | [SIM-05](docs/sim-05-interscity.md) |
| SIM-06 | Backend REST em C: consulta ao InterSCity e API | Concluída | [SIM-06](docs/sim-06-backend.md), [Backend](backend/README.md) |
| SIM-08 | Tolerância a falhas no Gateway: reconexão MQTT, deduplicação e consulta antes do reenvio | Concluída | [SIM-08](docs/sim-08-tolerancia-falhas.md) |
| SIM-09 | Fila durável SQLite e worker HTTP com reconciliação | Concluída | [Fila durável](docs/sim-09-fila-duravel.md), [Validação final](docs/sim-09-validacao-final.md) |
| — | Dashboard web | Pendente | — |
| — | Escalabilidade | Pendente | — |
| SIM-10.1 | Mosquitto e IoT Gateway em Compose com SQLite persistente | Implementação inicial | [Containerização](docs/sim-10-containerizacao.md) |
| SIM-10.2 | Dataset Replayer e Backend REST no Compose | Implementação inicial | [Containerização](docs/sim-10-containerizacao.md) |
| SIM-11 | Vibração (`vibration_rms`) e rotação (`rpm`) em todo o pipeline | Concluída; capabilities 50 e 51 criadas na UFMA | [SIM-11](docs/sim-11-vibracao-rpm.md) |
| — | Containerização do Dashboard e implantação em nuvem | Pendente | — |

## Estrutura do repositório

| Caminho | Conteúdo |
| --- | --- |
| `data/predictive_maintenance_v3.csv` | Dataset (24.042 registros, 15 colunas) |
| `dataset-replayer/` | Replayer em C: leitura do CSV, normalização, JSON e publicação MQTT |
| `iot-gateway/` | Gateway em C: assinatura MQTT, validação e envio ao InterSCity |
| `iot-gateway/scripts/register-interscity-machines.sh` | Cadastro idempotente das máquinas no InterSCity |
| `backend/` | Backend REST em C: `/api/health`, `/api/machines`, `/api/machines/latest` e histórico por máquina; testes e `smoke-real` |
| `config/mosquitto/mosquitto.conf` | Broker de desenvolvimento (somente `127.0.0.1`) |
| `config/interscity/machines.conf` | Mapeamento `machine_id` → UUID dos recursos no InterSCity |
| `docs/` | Validações e decisões de cada etapa |

As dez máquinas usadas são CNC 1, 2 e 3; Pump 6, 7, 8 e 9; e Compressor 11, 12 e 13.

## Ambiente

Desenvolvido e validado no Ubuntu 24.04 (WSL2). Dependências:

```sh
sudo apt install build-essential pkg-config libjson-c-dev libmosquitto-dev \
  libmicrohttpd-dev libcurl4-openssl-dev libsqlite3-dev mosquitto mosquitto-clients python3 curl jq
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

make -C backend test           # 51 verificações; InterSCity falso em 127.0.0.1
make -C backend smoke-real     # manual: consulta a instância da UFMA (somente leitura)
```

Nenhum teste de `make test` acessa a instância real do InterSCity: os clientes
HTTP são testados contra servidores falsos em `127.0.0.1`. O alvo
`smoke-real` consulta a instância real manualmente e só faz leituras.

## Execução do fluxo completo

O exemplo abaixo usa o modo legado direto, sem fila. Execute contra a instância
compartilhada somente quando autorizado. Para Compose com fila, use a seção ao final.
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

# 4. Backend REST (escuta só em 127.0.0.1 por padrão)
backend/build/backend --port 8080 --machines config/interscity/machines.conf
curl -s http://127.0.0.1:8080/api/machines/latest
```

A instância do InterSCity é compartilhada pelas equipes da disciplina. No modo
legado SIM-08 (sem `--queue-db`), dentro de uma mesma execução do Gateway, uma
amostra já enviada (mesma máquina e mesmo timestamp, ou anterior) é ignorada. Reiniciar o Gateway zera essa memória, e
amostras antigas podem ser reenviadas. Cuidados em
[SIM-05](docs/sim-05-interscity.md) e [SIM-08](docs/sim-08-tolerancia-falhas.md).

## Limitações atuais

- O contrato de telemetria tem dez campos e usa 9 das 15 colunas do dataset
  (timestamp, máquina, tipo, temperatura, corrente, vibração, rotação, modo de operação
  e predição de falha); as outras seis colunas ficam fora do pipeline. Ver [SIM-11](docs/sim-11-vibracao-rpm.md).
- `power` não existe no dataset e é publicado como `null`;
  `failure_within_24h` é uma predição do dataset, não uma falha medida.
- No modo legado SIM-08 (sem `--queue-db`), a deduplicação é só em memória. A
  consulta antes do reenvio usa janela fixa de 2 s, e mensagens publicadas durante a queda do
  broker se perdem (sessão MQTT limpa). Detalhes em
  [SIM-08](docs/sim-08-tolerancia-falhas.md).
- No modo legado SIM-08, o envio HTTP é síncrono na thread principal. No modo
  SIM-09/Compose, MQTT persiste no SQLite e um worker HTTP independente entrega
  a fila. Pendências e cursor sobrevivem ao reinício; não há garantia exactly-once,
  e PUBACK MQTT não é atômico com COMMIT SQLite. Ver [SIM-09](docs/sim-09-fila-duravel.md).
- O Mosquitto de desenvolvimento aceita conexões anônimas somente em localhost;
  essa configuração não é adequada para implantação exposta.
- O backend não tem autenticação. Ele escuta só em `127.0.0.1` por padrão;
  `--bind 0.0.0.0` o expõe na rede. Detalhes em [SIM-06](docs/sim-06-backend.md).


## Compose local — SIM-10.1 e SIM-10.2

A subida padrão `docker compose up -d --build` inicia Mosquitto, Gateway e
Backend REST (`http://127.0.0.1:8080/api/health`). O profile `replay` fica desligado.
O Gateway usa obrigatoriamente
`--queue-db /data/telemetry.db`, volume nomeado e mapa somente leitura. MQTT é
publicado apenas em `127.0.0.1:1883` (`MQTT_PORT` permite outra porta local).

O destino padrão `INTERSCITY_URL=http://127.0.0.1:9` é deliberadamente indisponível
**dentro do contêiner**, para validar a fila sem enviar à UFMA. Configurar outro
destino somente quando autorizado. `docker compose down` preserva o volume;
não usar `-v` para conservar pendências. Procedimento, permissões, resultados e
limitações: [SIM-10](docs/sim-10-containerizacao.md).

Somente após verificar `Gateway pronto` nos logs da inicialização atual
(`docker compose logs gateway`), iniciar a demonstração finita das dez máquinas:

```sh
LIMIT=3 INTERVAL_MS=1000 docker compose --profile replay up -d replayer
docker compose wait replayer
docker compose logs replayer
```

O serviço reutiliza o launcher Bash existente, com dez processos C, CSV somente
leitura e `MQTT_HOST=mosquitto`, porta interna 1883. Padrões: dez registros por
máquina e intervalo de 1000 ms. Não há reinício automático do Replayer.
`depends_on: service_started` não comprova SUBACK: a conferência da prontidão
antes da publicação é necessária, pois a sessão MQTT é limpa.
