# SIM-04 — Validação integrada do IoT Gateway

## Objetivo e escopo

Validar o fluxo completo dos dez Dataset Replayers ao IoT Gateway por MQTT,
sem acrescentar componentes ou integração HTTP. Execução em 14/10/2026, sobre
`0a7fbb3` (SIM-04.3). Nesta entrega foram adicionados somente automação de
integração, alvo Make e documentação; nenhum módulo C ou Replayer foi alterado.

## Arquitetura e dependências

```text
CSV → dez processos Dataset Replayer → Mosquitto local temporário
    → mqtt_subscriber → callback do Gateway → gateway_telemetry_parse
    → stdout: válidas / stderr: inválidas e erros
```

O Gateway aguarda CONNACK e SUBACK com QoS 1 antes de anunciar prontidão.
Assina `smart-industry/machines/+/telemetry`; cada produtor publica em
`smart-industry/machines/{machine_id}/telemetry`, com MQTT 3.1.1, QoS 1 e retain
false. A conexão é reutilizada. PUBACK confirma recepção no broker; o teste
verifica separadamente o processamento nos logs do Gateway.

Ambiente: Arch Linux, C11/GCC, Make, pkg-config, libmosquitto 2.1.2, json-c 0.19,
Bash 5.1 ou superior, Python 3 e ferramentas Mosquitto. A nova automação usa
somente a biblioteca padrão Python e auxiliares dos testes anteriores. `/proc`
é utilizado para observar os processos no Linux. Dependências de teste das
suítes anteriores, como `stdbuf`, permanecem necessárias para a regressão.

## Máquinas e contrato preservado

| Tipo original | IDs | Confirmadas por produtor | Recebidas por ID nesta execução |
| --- | --- | --- | --- |
| CNC | 1, 2, 3 | 3, 3, 3 | 3, 3, 3 |
| Pump | 6, 7, 8, 9 | 3, 3, 3, 3 | 3, 3, 3, 3 |
| Compressor | 11, 12, 13 | 3, 3, 3 | 3, 3, 3 |

CNC representa equipamentos motorizados; o dataset não tem a classificação
literal Motor. Os tipos originais são preservados.

As oito propriedades são `machine_id`, `machine_type`, `timestamp`,
`temperature`, `current`, `power`, `state` e `failure_within_24h`. O teste
compara cada mensagem às três primeiras amostras selecionadas de seu ID no CSV,
incluindo valores decimais convertidos a double, sem truncamento para inteiro.
Medições vazias são comparadas como `null`; o cenário de controle também
verifica número zero versus `null`. `power` permanece `null`, pois a fonte não
fornece potência diretamente. O timestamp é o da amostra, sem conversão de
fuso ou substituição pelo instante de recebimento.

Estados permanecem UNKNOWN/IDLE/NORMAL/PEAK. O indicador inteiro 0/1
`failure_within_24h` é preditivo, sem gerar alerta operacional. A correspondência
tópico/ID é exigida pelo validador do Gateway; o launcher foi observado com
os IDs corretos e o módulo publicador usa o tópico contratual. O teste não
cria um segundo assinante para observar tópicos: mensagens aceitas pelo
Gateway passaram por essa validação, já coberta também nos testes locais.

## Reprodução automatizada

Na raiz do repositório:

```sh
make -C iot-gateway test-e2e
```

O alvo compila o Gateway e chama `make -C ../dataset-replayer all` a partir
do diretório do Gateway. Executa `iot-gateway/tests/test_end_to_end.py`, que
reutiliza `temporary_broker`, `wait_for`, `publish` e `terminate` da suíte CLI.
Não altera testes já aprovados. Os demais alvos continuam independentes.

A sequência automatizada é:

1. Iniciar Mosquitto temporário em `127.0.0.1`, porta dinâmica isolada, acesso
   anônimo e persistência desabilitada; aguardar disponibilidade.
2. Iniciar o Gateway na mesma porta e aguardar a linha de prontidão, emitida
   somente após confirmação da assinatura.
3. Executar o launcher existente com `LIMIT=3`, `INTERVAL_MS=1000`,
   `MQTT_HOST=127.0.0.1`, `MQTT_PORT` temporária e `CSV_PATH` do repositório.
4. Observar dez PIDs distintos simultaneamente e seus argumentos em `/proc`.
   Aguardar o launcher concluir e verificar dez resumos de três confirmações.
5. Aguardar os logs do Gateway conterem todas as três amostras de cada ID.
   Aceitar repetições dessas amostras, sem exigir ordem global entre máquinas.
6. Publicar uma amostra válida de controle, aguardar seu processamento,
   publicar `{}`, aguardar rejeição, publicar outra válida e aguardar seu log.
7. Enviar SIGTERM ao Gateway, exigir saída 0 e coletar os processos.

Esperas têm timeouts finitos e diagnósticos incluem os logs dos componentes.
Os filhos são aguardados, o broker é encerrado pelo seu próprio PID e os
arquivos temporários são removidos. Em falha, a limpeza alcança também o grupo
privado do launcher e produtores. Nenhum broker externo é encerrado.

## Procedimento manual

Comandos abaixo na raiz. Primeiro terminal:

```sh
make -C dataset-replayer all
make -C iot-gateway all
mosquitto -c config/mosquitto/mosquitto.conf -v
```

Se já houver broker local escutando em 1883, utilize-o sem iniciar outro.
Segundo terminal:

```sh
iot-gateway/build/iot-gateway --host 127.0.0.1 --port 1883 \
  --client-id smart-industry-gateway --timeout-ms 5000
```

Aguardar `Gateway pronto: smart-industry/machines/+/telemetry (QoS 1).`.
Terceiro terminal:

```sh
LIMIT=3 INTERVAL_MS=1000 MQTT_HOST=127.0.0.1 MQTT_PORT=1883 \
  ./dataset-replayer/scripts/run-ten-machines.sh
```

Após os produtores terminarem, aguardar as mensagens no Gateway antes de
usar Ctrl+C. O intervalo é uma cadência fixa após PUBACK, não a reprodução dos
intervalos históricos do CSV. Esta seção é um procedimento de reprodução;
as evidências abaixo vieram da automação isolada, não de uma sessão manual.

## Resultados efetivamente observados

- Dez processos produtores independentes e dez IDs distintos, com limite 3
  e intervalo 1000 ms nos argumentos de cada processo.
- Trinta publicações confirmadas por PUBACK, conforme os dez resumos do launcher.
- Trinta telemetrias do CSV processadas pelo Gateway: três por ID, conforme
  tabela. Não foram observadas duplicações nessa execução.
- Nenhum diagnóstico de validação durante o cenário normal dos produtores.
- Todos os oito campos coincidiram com as amostras esperadas do CSV; potência
  nula, tipos originais e timestamps preservados.
- Duas válidas adicionais de controle processadas, separadas por uma rejeição:
  32 válidas no total e um diagnóstico de mensagem inválida observado.
- Launcher e Gateway retornaram 0; Gateway encerrado por SIGTERM. PIDs dos
  produtores e Gateway ausentes após coleta; broker temporário encerrado.

Trechos efetivamente produzidos pelo teste:

```text
PASS: 10 produtores simultâneos, 30 publicações confirmadas, 30 telemetrias CSV processadas
CONTAGEM POR ID: {"1": 3, "2": 3, "3": 3, "6": 3, "7": 3, "8": 3, "9": 3, "11": 3, "12": 3, "13": 3}
CONTINUIDADE: Telemetria inválida: Esperadas exatamente as oito propriedades obrigatórias (payload=2 bytes).
PASS: válida/inválida/válida; 32 válidas no total; SIGTERM=0; filhos coletados
```

Exemplo recebido e decodificado do log do Gateway:

```json
{"machine_id":1,"machine_type":"CNC","timestamp":"2024-01-01 00:00:00","temperature":49.51,"current":5.1,"power":null,"state":"IDLE","failure_within_24h":0}
```

As amostras de controle usam ID 1 e timestamps `sim04-probe-before` e
`sim04-probe-after` para separá-las da contagem do CSV. Elas são injetadas por
`mosquitto_pub`, com temperatura zero e corrente/potência nulas. Após `{}`,
o processo permaneceu ativo e processou a segunda amostra. Os logs temporários
não são versionados; o teste imprime os resultados e inclui os logs em falhas.
Não foram produzidas screenshots nem adicionados artefatos gerados.

Dataset: `data/predictive_maintenance_v3.csv`, SHA-256
`8c9277131a596133d78a3945a57b52038b3582dfa516e0ec20b70a0a8165aade`.

## Regressões executadas

| Comando | Resultado |
| --- | --- |
| `make -C dataset-replayer all` | Sucesso, nenhum warning emitido |
| `make -C dataset-replayer test` | Sucesso nos casos executados; SKIP de locale |
| `make -C dataset-replayer test-mqtt` | Sucesso, incluindo dez processos e sinais |
| `make -C iot-gateway all` | Sucesso, nenhum warning GCC |
| `make -C iot-gateway test` | Sucesso, parser/assinante/CLI locais |
| `make -C iot-gateway test-mqtt` | Sucesso, assinante e CLI com broker temporário |
| `make -C iot-gateway test-e2e` | Sucesso, cenário completo descrito acima |
| `git diff --check` | Sem erros |

O teste anterior de locale alternativo informou
`SKIP: locale numérico alternativo indisponível.` Esse caso não foi considerado
aprovado. Nenhuma suíte solicitada ficou sem execução. A regressão da CLI
confirmou SIGINT e SIGTERM com saída 0 e falha de comunicação com saída não zero.
Os testes MQTT exigiram autorização para sockets locais fora do sandbox.
Sanitizers não foram reexecutados nesta subetapa, pois nenhum código C mudou.

## Limitações e avaliação de aceite

- QoS 1 admite duplicações. A automação aceita pelo menos três mensagens por ID
  e exige representação das três amostras esperadas; não implementa deduplicação.
- PUBACK do produtor não é confirmação de processamento pelo Gateway; os logs
  são aguardados separadamente. Isso não constitui protocolo de confirmação
  de aplicação nem persistência das telemetrias.
- Mensagens anteriores à assinatura podem ser perdidas. Sessões são limpas;
  não existe reconexão automática.
- Preparação DNS/TCP pode ser síncrona e exceder o prazo de confirmação.
  Callbacks/logs são síncronos e podem prolongar polling ou resposta a sinais.
- Chaves JSON repetidas mantêm o último valor, conforme a política documentada
  da json-c. Permanecem os limites de texto e demais restrições da SIM-04.1.
- Sem TLS/autenticação, armazenamento local, HTTP ou InterSCity nesta etapa.
  Pausas longas dos produtores mantêm a limitação de keepalive já documentada.
- SIGKILL/falha do sistema impedem garantir encerramento gracioso. A escolha
  de porta temporária tem a pequena janela habitual entre reserva e abertura.

Os cenários executados atenderam aos critérios da SIM-04.4: dez produtores,
integridade, contagens, continuidade após erro e encerramento controlado.
Não foi necessário modificar módulos anteriores. A subetapa está tecnicamente
concluída para revisão; o encerramento formal da SIM-04 depende da aprovação
do projeto. Integração com InterSCity permanece reservada à etapa posterior.
