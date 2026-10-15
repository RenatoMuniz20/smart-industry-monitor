# SIM-03 — Validação final da publicação MQTT

## Objetivo e ambiente

Validar a entrega de telemetrias históricas do CSV ao Mosquitto, incluindo
confirmação, reprodução temporal e dez publicadores independentes. Validação
realizada em 14/10/2026, sobre o commit `1a6ef2d` (SIM-03.4), sem alterações
funcionais ou novos testes permanentes nesta etapa.

Ambiente observado: Arch Linux, GCC 16.2.1, C11, Bash 5.3.15 e libmosquitto
2.1.2. Build com `-Wall -Wextra -Wpedantic`; testes C com `assert`, integração
Python 3 (biblioteca padrão), Mosquitto, `mosquitto_sub` e GNU `stdbuf`.

## Arquitetura e fluxo

```text
CSV → csv_read_file → filtro machine_id → telemetry_normalize
    → telemetry_to_json → mqtt_publisher_publish → PUBACK → contador
```

A CLI valida os argumentos antes de conectar. Cada processo mantém uma conexão
MQTT 3.1.1, sessão limpa, Client ID `sim-<machine_id>-<pid>`, QoS 1 e retain
false. Aguarda CONNACK aceito e PUBACK do MID correspondente usando
`mosquitto_loop`, sem threads próprias. O retorno inicial de
`mosquitto_publish` não é tratado como confirmação. `--limit` conta somente
confirmações; erros interrompem o callback, fecham o contexto e produzem erro
com contagem parcial, sem resumo de sucesso. O modo tradicional imprime os
registros e permanece sem espera e sem conexão MQTT.

## Seleção das máquinas e tópicos

| Tipo original | IDs | Mensagens observadas por ID na demonstração |
| --- | --- | --- |
| CNC | 1, 2, 3 | 3, 3, 3 |
| Pump | 6, 7, 8, 9 | 3, 3, 3, 3 |
| Compressor | 11, 12, 13 | 3, 3, 3 |

O dataset contém os tipos CNC, Pump, Compressor e Robotic Arm; não contém
máquinas classificadas literalmente como Motor. Os três CNC representam
equipamentos motorizados, preservando `machine_type` na mensagem.

Cada máquina publica exclusivamente em
`smart-industry/machines/{machine_id}/telemetry`. Para observar todas, a
assinatura é `smart-industry/machines/+/telemetry`.

## Contrato JSON

Exatamente oito propriedades, sem campos adicionais:

| Propriedade | Representação |
| --- | --- |
| `machine_id` | Número inteiro |
| `machine_type` | String original do dataset |
| `timestamp` | String original da amostra, sem atribuição de fuso |
| `temperature` | Número de `temperature_motor`, ou `null` |
| `current` | Número de `current_phase_avg`, ou `null` |
| `power` | Número ou `null`; na normalização atual, sempre `null` |
| `state` | String `UNKNOWN`, `IDLE`, `NORMAL` ou `PEAK` |
| `failure_within_24h` | Número inteiro 0 ou 1, indicador preditivo |

`NAN` e ambos os infinitos tornam-se `null`, mantendo as propriedades.
A potência permanece `null` porque a fonte não fornece esse valor diretamente.
O timestamp indica o instante original da amostra, não o horário de recebimento.
Strings são escapadas, UTF-8 inválido é rejeitado e o buffer é preservado em
erro. Doubles usam `DBL_DECIMAL_DIG` e locale numérico C restrito à thread.

Mensagem efetivamente recebida da máquina 1, apresentada após decodificação:

```json
{"machine_id":1,"machine_type":"CNC","timestamp":"2024-01-01 00:00:00","temperature":49.51,"current":5.1,"power":null,"state":"IDLE","failure_within_24h":0}
```

## Reprodução temporal

`--interval-ms` aceita 1 a 60000, com padrão 1000 ms. A primeira publicação
é imediata após conexão e processamento; antes de cada próxima publicação
selecionada, aguarda-se o intervalo após a confirmação anterior. `nanosleep`
retoma o tempo restante em `EINTR`; outro erro aborta a execução. Não há pausa
após o último registro, inclusive por limite ou fim de arquivo. A ordem é
preservada. Trata-se de cadência fixa sequencial, sem reproduzir os intervalos
históricos do CSV e sem garantia de precisão de tempo real.

## Procedimento local

Na raiz do repositório (Arch Linux; dependências já instaladas nesta validação):

```sh
sudo pacman -S mosquitto pkgconf
make -C dataset-replayer all
mosquitto -c config/mosquitto/mosquitto.conf -v
```

Em outro terminal, iniciar o assinante antes dos publicadores:

```sh
mosquitto_sub -h 127.0.0.1 -p 1883 -V mqttv311 -q 1 \
  -t 'smart-industry/machines/+/telemetry' -v
```

Publicar três amostras por equipamento:

```sh
LIMIT=3 INTERVAL_MS=1000 MQTT_HOST=127.0.0.1 MQTT_PORT=1883 \
  ./dataset-replayer/scripts/run-ten-machines.sh
```

O script usa dez processos independentes e aguarda todos. SIGINT/SIGTERM ou
falha de um filho provocam SIGTERM aos restantes e coleta com `wait`.
Retornos: 0 em sucesso, 1 em falha, 130/143 por SIGINT/SIGTERM. O padrão de
`LIMIT` é 10; `CSV_PATH` permite trocar o arquivo. Não há sincronização exata
entre equipamentos.

## Testes executados e cobertura existente

| Comando ou evidência | Resultado efetivo |
| --- | --- |
| `make -C dataset-replayer clean` | Sucesso |
| `make -C dataset-replayer all` | Sucesso, nenhum warning GCC |
| `make -C dataset-replayer test` | Sucesso nos casos executados; um SKIP de locale |
| `make -C dataset-replayer test-mqtt` | Sucesso, incluindo dez máquinas |
| `git diff --check` | Sem erros |

Não foram duplicados testes. A cobertura foi reutilizada dos arquivos em
[`dataset-replayer/tests`](../dataset-replayer/tests/):

- `test_csv_reader.c`, `test_telemetry.c` e `test_cli.sh`: leitura, campos
  ausentes, normalização, estados, argumentos e compatibilidade da CLI.
- `test_telemetry_json.c`: oito propriedades e tipos, não finitos, strings,
  UTF-8, limites de buffer, precisão e integração da normalização.
- `test_mqtt_publisher.c` e `test_mqtt_integration.py`: tópicos, MQTT 3.1.1,
  flags QoS 1/retain false, CONNACK/PUBACK, MID incorreto, conexão reutilizada,
  mensagens recebidas, limites, falhas e contagem parcial sem reconexão.
- `replayer_timing()` no teste de integração e `wrap_nanosleep.c`: duas
  esperas para três publicações, 100 ms e padrão 1000 ms, primeira/última sem
  pausa, ordem, timestamps, EINTR, falha da espera e modo tradicional sem espera.
- `test_ten_machines.py`: dez processos simultâneos, 30 mensagens nos dez
  tópicos, tipos e timestamps, intervalo configurado de 500 ms no cenário
  automatizado, SIGINT/SIGTERM, falha de filho e broker indisponível, com
  verificação de ausência dos PIDs após a coleta.

**SKIP observado:** `SKIP: locale numérico alternativo indisponível.` O caso
com locale alternativo não foi aprovado nem executado. Os locales observados
foram C, C.utf8, POSIX e en_US.utf8; os candidatos do teste não estavam
instalados. Os demais casos de serialização passaram.

Os testes MQTT foram executados fora do sandbox com autorização para sockets
locais, usando broker temporário em loopback, sem serviço público.

## Evidências da demonstração complementar

Foi necessária apenas uma execução complementar para o intervalo solicitado
de 1000 ms, sem alterar os testes existentes. Usou o script real com `LIMIT=3`
e `INTERVAL_MS=1000`, assinante pronto antes do lançamento e broker temporário
em `127.0.0.1:35115`. A verificação observou `/proc/<pid>/cmdline`, decodificou
as mensagens e comparou tipos e os três timestamps de cada máquina com o CSV.

- Dez PIDs simultâneos: 95807, 95808, 95809, 95810, 95811, 95812, 95813,
  95814, 95815 e 95816, respectivamente para IDs 1, 2, 3, 6, 7, 8, 9, 11, 12 e 13.
- Dez tópicos distintos e 30 mensagens: três por ID, conforme tabela acima.
- Log do broker: dez conexões `sim-*`, 30 PUBLISH recebidos desses clientes,
  30 PUBACK enviados a eles e dez DISCONNECT recebidos, confirmando também
  a reutilização de cada conexão para as três mensagens.
- Oito propriedades em cada mensagem, tipos originais, timestamps na ordem
  do CSV e `power: null` em todas.
- Dez resumos `Publicações confirmadas: 3`, seguidos de
  `Dez máquinas concluídas com sucesso.`; script e assinante retornaram 0.
- Duração monotônica do script: 2,027 s. É uma medição observada, não promessa
  de desempenho. A precisão por intervalo é coberta pelos testes temporais.
- Após a conclusão, nenhum dos dez PIDs existia; os processos auxiliares do
  assinante e broker também foram aguardados e encerrados.

Trecho efetivamente produzido pelo broker:

```text
1792022258: New client connected from 127.0.0.1:53734 as sim-1-95807 (p4, c1, k60).
1792022258: Received PUBLISH from sim-1-95807 (d0, q1, r0, m1, 'smart-industry/machines/1/telemetry', ... (184 bytes))
1792022258: Sending PUBACK to sim-1-95807 (m1, rc0)
```

Logs brutos desta sessão ficaram em `/tmp/sim03-evidence-p7velhec/`
(`broker.log`, `messages.log`, `subscriber.log`, `launcher.out`, `launcher.err`).
São temporários, não versionados; o trecho e os resultados acima preservam as
evidências no repositório. Nenhuma screenshot foi produzida.

Dataset validado: `data/predictive_maintenance_v3.csv`, SHA-256
`8c9277131a596133d78a3945a57b52038b3582dfa516e0ec20b70a0a8165aade`.

## Limitações e critérios de aceite

- DNS/TCP em `mosquitto_connect` são síncronos: o limite de 5 s não garante
  timeout total dessa preparação; CONNACK/PUBACK têm espera limitada.
- Pausas longas não processam eventos MQTT e podem ultrapassar o keepalive de
  60 s com atrasos adicionais. Não há reconexão automática.
- QoS 1 admite duplicações e confirma recepção no broker; não garante
  persistência nem processamento pelo Gateway ou InterSCity. Ausência de
  PUBACK não prova ausência de recebimento pelo broker.
- Sem TLS/autenticação nesta etapa; configuração local permite acesso anônimo
  e desabilita persistência. Um contexto MQTT por processo, uso sequencial.
- SIGKILL ou falha do sistema impedem limpeza pelo script. Limites são máximos;
  um CSV com menos amostras encerra normalmente antes de atingi-los.
- O cenário alternativo de locale permanece não verificado neste ambiente.

Os critérios funcionais de leitura até publicação, JSON, confirmação, cadência,
limites, simultaneidade e encerramento foram demonstrados pelos casos
executados. Não foi identificado defeito funcional bloqueante nessa validação.
A SIM-03 está tecnicamente apta à revisão de encerramento, com as limitações
e o SKIP explícitos. O encerramento formal depende da revisão do projeto;
Gateway e InterSCity permanecem fora desta entrega.
