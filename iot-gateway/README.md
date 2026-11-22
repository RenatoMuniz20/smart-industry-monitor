# IoT Gateway — SIM-04.1 a SIM-05.5

Módulo C11 independente para interpretar e validar tópico e telemetria JSON.
O validador JSON (SIM-04.1) e o assinante MQTT (SIM-04.2) são módulos separados.
O executável `build/iot-gateway` (SIM-04.3) integra assinatura, callback e
validação, com CLI e encerramento por sinais. Desde a SIM-05.5, também pode
encaminhar cada telemetria válida ao InterSCity por HTTP, somente quando
habilitado por opções (ver [Envio ao InterSCity](#envio-ao-interscity-sim-055)).
O Dataset Replayer não é modificado nem utilizado como dependência de compilação.

## Build e testes locais

Requer GCC, Make, pkg-config, json-c 0.19 e libmosquitto 2.1.2. Desde a SIM-05.5,
também libcurl com suporte a TLS (no Ubuntu 24.04: `libcurl4-openssl-dev`).
Na raiz do repositório:

```sh
pkg-config --modversion json-c
make -C iot-gateway all
make -C iot-gateway test
make -C iot-gateway clean
```

O Makefile usa `-std=c11 -Wall -Wextra -Wpedantic`, obtém flags com pkg-config
e vincula cada teste apenas à biblioteca necessária (json-c ou libmosquitto).
`make test` não exige broker nem rede externa; o teste do cliente HTTP usa apenas
um servidor falso em 127.0.0.1. O módulo de validação permanece independente de MQTT.

## Interface

```c
int gateway_telemetry_parse(const char *topic, const void *payload,
                            size_t payload_length, GatewayTelemetry *out,
                            GatewayTelemetryError *error);
const char *gateway_telemetry_error_string(GatewayTelemetryError error);
```

Retorna 0 em sucesso ou -1 em falha. `out` permanece integralmente inalterado
em erro. `error` é opcional, recebe o diagnóstico ou `GATEWAY_TELEMETRY_OK`.
O módulo não imprime nem acessa arquivos. O tópico deve ser terminado em NUL;
o payload não precisa de terminador: seu comprimento exclui qualquer NUL.
Comprimentos zero ou superiores a `INT_MAX` são rejeitados antes do parsing.
O chamador deve fornecer regiões de memória válidas para os tamanhos informados.

`GatewayTelemetry` contém o ID e indicador preditivo como `int`; tipo, timestamp
e estado são arrays próprios de 64 bytes, com até 63 bytes decodificados mais
terminador. Cada medição tem um `double` e um booleano `*_present`: JSON `null`
produz presença falsa e valor zero; número zero produz presença verdadeira.
A estrutura não contém ponteiros nem campos alocados dinamicamente. json-c
aloca seus objetos temporários, liberados antes do retorno, inclusive em erro.

## Contrato validado

Tópico: `smart-industry/machines/{machine_id}/telemetry`, com dígitos decimais,
ID positivo representável em `int` e sufixo exato, sem segmentos extras. Zeros
à esquerda são aceitos numericamente; não há whitelist dos dez IDs.

O objeto JSON deve conter exatamente as dez propriedades distintas abaixo (oito até a
SIM-10; `vibration_rms` e `rpm` entraram na SIM-11),
sem adicionais, todas obrigatórias e com tipos verificados antes da conversão:

| Propriedade | Regra |
| --- | --- |
| `machine_id` | Inteiro positivo até `INT_MAX`, igual ao ID do tópico |
| `machine_type` | String não vazia, até 63 bytes |
| `timestamp` | String não vazia, até 63 bytes, preservada sem conversão |
| `temperature`, `current`, `power`, `vibration_rms`, `rpm` | Número inteiro/fracionário finito ou `null` |
| `state` | String `UNKNOWN`, `IDLE`, `NORMAL` ou `PEAK` |
| `failure_within_24h` | Inteiro 0 ou 1, sem aceitar booleanos |

Não há coerção de strings/booleanos para números, faixas industriais, validação
de calendário ou whitelist de tipos de máquina. `failure_within_24h` mantém
significado preditivo, sem indicar falha operacional atual. `power` pode ser
numérico, embora o produtor atual o publique como `null`.

Antes do parsing, uma validação UTF-8 por comprimento explícito rejeita
codificações excessivamente longas, surrogates, valores acima de U+10FFFF,
sequências incompletas e bytes de continuação incorretos. Isso complementa a
checagem parcial oferecida pela json-c 0.19, preservando UTF-8 válido.

O parsing usa `json_tokener_parse_ex` com comprimento explícito, flags
`JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8` e verificação do conteúdo
restante, permitindo somente espaço, tabulação, CR e LF após o objeto.
Após sucesso da json-c, uma checagem lexical complementar rejeita aspas simples
fora de strings e números fora da gramática JSON: sinal negativo opcional,
parte inteira sem zeros extras, fração e expoente com dígitos obrigatórios.
Ela respeita strings e escapes, permitindo apóstrofos e textos como `1.` dentro
das strings. Não constrói objetos, não interpreta propriedades e não substitui
o parser principal da json-c. Frações e expoentes JSON válidos são aceitos.
Rejeita JSON incompleto, NUL bruto, UTF-8 inválido, conteúdo adicional, textos
decodificados com NUL e números não finitos. Uma checagem curta de escapes NUL,
após o parsing da biblioteca, também evita nomes de chave truncados pela
representação interna da json-c; uma sequência literal `\\u0000` é preservada.

## Testes e limitações

`test_gateway_telemetry.c` inclui uma mensagem real registrada em
[SIM-03 — Validação MQTT](../docs/sim-03-validacao.md), como fixture embutida.
Cobre os estados, flags, medições presentes/ausentes, zero, negativos, decimais,
campos obrigatórios/adicionais, tipos, tópicos/IDs, textos e buffers sem NUL,
UTF-8, JSON inválido, conteúdo adicional e saída preservada. Uma chamada válida
após falhas comprova a reutilização do módulo. Casos específicos cobrem os
defeitos reproduzidos da json-c 0.19, limites Unicode válidos, frações/expoentes,
escapes em strings, zero com presença verdadeira nas três medições e ID INT_MAX
com tópico correspondente.

**Chaves repetidas:** json-c 0.19 não expõe opção pública para rejeitá-las no
tokener. A biblioteca mantém o último valor, e a validação se aplica ao objeto
resultante. Esse comportamento foi verificado por teste e é uma limitação
explícita: dez propriedades distintas não garante dez ocorrências no texto.
Não foi implementado parser manual nem scanner de chaves duplicadas.

O limite de profundidade do tokener é o padrão da biblioteca (32). Não há limite
menor de payload além de `INT_MAX`; objetos grandes podem consumir memória antes
de serem rejeitados pelo contrato. Alocação insuficiente é tratada como erro,
mas não há teste de injeção de falha de alocação nesta etapa. O módulo não faz
nenhuma comunicação ou confirmação MQTT: o assinante abaixo é independente.


## Assinante MQTT (SIM-04.2)

```c
typedef struct MqttSubscriber MqttSubscriber;
typedef void (*MqttMessageHandler)(const char *topic, const void *payload,
                                   size_t payload_length, void *context);
int mqtt_subscriber_connect(MqttSubscriber **out, const char *host, int port,
                            const char *client_id, int timeout_ms,
                            MqttMessageHandler handler, void *context);
int mqtt_subscriber_poll(MqttSubscriber *subscriber, int timeout_ms);
int mqtt_subscriber_close(MqttSubscriber **subscriber);
MqttSubscriberError mqtt_subscriber_last_error(void);
const char *mqtt_subscriber_error_string(MqttSubscriberError error);
```

As três operações retornam 0 em sucesso e -1 em erro, sem imprimir logs.
O enum e seu texto permitem consultar o diagnóstico da última operação,
inclusive quando uma tentativa de conexão já liberou o contexto. Consulte-o
antes de chamar outra operação. O diagnóstico é local ao processo e não é
thread-safe; não armazena uma lista de falhas anteriores.

Inicialize `MqttSubscriber *subscriber = NULL` e passe seu endereço a `connect`.
`host`, porta (1..65535), client ID não vazio e callback são obrigatórios;
`context` pode ser NULL. O chamador deve fornecer um client ID único no broker.
O módulo admite um contexto ativo por processo e controla a inicialização e
finalização da libmosquitto; não deve compartilhar o processo com outros
usuários independentes da biblioteca. Uso sequencial, sem reentrância: não
chame `connect`, `poll` ou `close` de dentro do callback.

A conexão usa MQTT 3.1.1 explicitamente, sessão limpa e filtro fixo
`smart-industry/machines/+/telemetry`, com QoS solicitado 1. O retorno de
sucesso exige CONNACK aceito, envio de SUBSCRIBE, SUBACK com MID correspondente,
uma única concessão e QoS exatamente 1. Recusa, QoS 0/2/128, MID divergente,
erro de rede ou timeout retornam erro e liberam o contexto parcial, mantendo
`*out == NULL`. Uma nova tentativa explícita é permitida após a falha.

O timeout de conexão aceita 1..5000 ms, compartilhado pelas esperas CONNACK e
SUBACK e medido com relógio monotônico. O loop síncrono usa esperas de até
100 ms. **Limitação:** `mosquitto_connect` faz preparação DNS/TCP síncrona;
o prazo começa antes dela, mas não pode interrompê-la, podendo ultrapassar
cinco segundos. O módulo assinante não usa threads, sinais globais ou protocolo
manual; os sinais da aplicação são tratados pelo `main.c`.

Após conectar, o chamador executa `poll` regularmente. Cada chamada faz uma
iteração de `mosquitto_loop`, com timeout 0..5000 ms (0 é não bloqueante).
Ausência de mensagem retorna 0. Erro de rede/desconexão invalida o contexto,
e chamadas posteriores continuam retornando erro até `close`; não há
reconexão automática. O timeout limita a espera da biblioteca, não a duração
de um callback do usuário. A aplicação deve manter o processamento frequente
para atender ao keepalive de 60 s.

O callback recebe bytes sem presumir NUL, inclusive payload vazio ou binário,
com tópico e comprimento preservados. Esses ponteiros são emprestados e
expiram ao retornar do callback: copie os dados se precisar retê-los. O
callback também pode ocorrer durante `connect`, após SUBSCRIBE e antes de
seu retorno, se o broker enviar dados antes do SUBACK. O assinante não
interpreta JSON, não chama o validador e não cria logs finais de aplicação.
QoS 1 admite duplicações e não significa validação/processamento da mensagem.

`close` aceita ponteiro nulo, tenta enviar DISCONNECT quando saudável, destrói
o cliente, libera recursos da biblioteca e zera o ponteiro do chamador,
inclusive em erro. Não aguarda resposta ao DISCONNECT. Não use aliases do
ponteiro após liberar. Não há TLS, autenticação, persistência ou deduplicação.

### Testes de comunicação

```sh
make -C iot-gateway clean
make -C iot-gateway all
make -C iot-gateway test
make -C iot-gateway test-mqtt
```

O último alvo requer Python 3, `mosquitto` e `mosquitto_pub`; usa broker
temporário em loopback com porta dinâmica, acesso anônimo e sem persistência.
Não exige broker previamente ativo nem utiliza os dez produtores. O auxiliar
C informa READY somente após CONNACK e SUBACK, antes de o harness publicar.
Verifica três mensagens, tópicos e bytes/comprimentos exatos, payload vazio,
NUL/byte não UTF-8, polling sem mensagens, desconexão, fechamento e reabertura.
Payload não JSON é intencional no teste isolado do assinante. O teste da CLI
verifica separadamente a integração com o validador.

Pares TCP mínimos de teste verificam CONNACK recusado, ausência de CONNACK ou
SUBACK, MID incorreto, concessões QoS 0/2/128 e quantidade incorreta de concessões.
Não são um broker completo; apenas injetam respostas de handshake. As threads
Python são restritas ao harness: o módulo C usa somente loop síncrono. Processos,
sockets e arquivos temporários são encerrados mesmo em falha. Testes locais
continuam separados e não exigem rede. Falhas de alocação/relógio e perda de
rede durante o próprio envio de SUBSCRIBE não têm injeção específica nesta
etapa; os caminhos correspondentes possuem tratamento de erro no módulo.


## Aplicação e CLI (SIM-04.3)

Na raiz, com o broker de desenvolvimento em outro terminal:

```sh
make -C iot-gateway all
mosquitto -c config/mosquitto/mosquitto.conf -v
```

Inicie o Gateway em um segundo terminal:

```sh
iot-gateway/build/iot-gateway
iot-gateway/build/iot-gateway --help
# Exemplo de configuração explícita:
iot-gateway/build/iot-gateway --host 127.0.0.1 --port 1883   --client-id smart-industry-gateway --timeout-ms 5000
```

| Opção | Padrão | Validação |
| --- | --- | --- |
| `--host` | `127.0.0.1` | Texto não vazio, sem começar com `--` |
| `--port` | `1883` | Inteiro de 1 a 65535 |
| `--client-id` | `smart-industry-gateway` | Texto não vazio, sem começar com `--` |
| `--timeout-ms` | `5000` | Inteiro de 1 a 5000 |
| `--help` | — | Usado sozinho; imprime ajuda e retorna 0 sem broker |

Opções podem aparecer em qualquer ordem; duplicadas, desconhecidas, sem valor
ou fora dos limites retornam 1 com diagnóstico em stderr. Números aceitam
somente dígitos, sem sinais ou espaços. O client ID deve ser único no broker;
o padrão é adequado para uma instância do Gateway.

O contexto do callback é inicializado antes de `connect`, pois mensagens podem
chegar antes de seu retorno. Após CONNACK e SUBACK confirmados, stdout recebe:

```text
Gateway pronto: smart-industry/machines/+/telemetry (QoS 1).
```

Somente então os produtores devem ser iniciados. O loop usa polling síncrono
de 250 ms. Cada callback passa tópico, bytes e comprimento ao validador, sem
presumir NUL. Uma telemetria válida produz uma linha `Telemetria válida: `
seguida de um objeto JSON com os dez campos, mantendo `null` diferente de
zero. Textos são representados com escaping da json-c; não são interpolados
como texto bruto no terminal. Exemplo observado no teste de integração:

```text
Telemetria válida: {"machine_id":1,"machine_type":"CNC","timestamp":"original sample","temperature":0.0,"current":null,"power":null,"vibration_rms":null,"rpm":0.0,"state":"PEAK","failure_within_24h":1}
```

Para mensagem inválida, stderr recebe o diagnóstico do validador e o tamanho,
sem imprimir o payload ou tópico não confiáveis. Exemplo observado:

```text
Telemetria inválida: JSON inválido, incompleto ou UTF-8 inválido (payload=18 bytes).
```

A mensagem inválida é descartada e o Gateway continua recebendo. Sem as opções
InterSCity, não há armazenamento, agregação, alertas ou transmissão HTTP. Logs válidos reformatam
números como `double`; timestamp e textos decodificados permanecem preservados.

Ctrl+C/SIGINT ou SIGTERM solicitam encerramento normal (código 0). Os handlers
apenas atualizam uma flag `volatile sig_atomic_t`. O loop observa a flag antes
de classificar erro de polling e fecha o Subscriber fora do handler. Uma
solicitação normal produz `Gateway encerrado por solicitação.` em stdout.
Erros de argumentos, conexão, comunicação, logging ou fechamento retornam 1.
O diagnóstico MQTT é obtido antes de `close`, que sobrescreve o último erro.

Permanecem as limitações de DNS/TCP síncrono: sinal durante conexão pode levar
até a conclusão dessa operação ou do prazo de confirmações para ser observado.
Durante a recepção, a resposta ao sinal depende do polling e da duração do
callback/saída de logs. Não há reconexão automática. SIGKILL não executa limpeza.
As limitações de chaves repetidas, tamanho e demais módulos continuam válidas.

`make test` inclui `tests/test_cli.py` sem broker, para ajuda e argumentos.
`make test-mqtt` executa a comunicação isolada do assinante e
`tests/test_cli.py --mqtt`, com broker temporário reutilizando auxiliares da
SIM-04.2. Cobre conexão, sequência válida/inválida/válida, null/zero, escaping,
broker indisponível, falha de comunicação, SIGINT/SIGTERM e coleta do processo.
Esse alvo não exige os dez produtores; a validação completa está no alvo separado abaixo.


## Validação com dez produtores (SIM-04.4)

```sh
make -C iot-gateway test-e2e
```

O alvo compila os executáveis necessários e usa o launcher existente com
`LIMIT=3 INTERVAL_MS=1000`, host loopback e porta dinâmica de um Mosquitto
temporário. Aguarda a prontidão do Gateway, verifica dez processos simultâneos,
30 publicações confirmadas e pelo menos três mensagens válidas por ID,
comparando os dez campos às amostras do CSV e permitindo duplicações QoS 1.
Também injeta válida/`{}`/válida e encerra por SIGTERM após observar os logs.
Requer Linux (`/proc`), Bash 5.1+, Python 3, Mosquitto e `mosquitto_pub`, além
das dependências já descritas. Não altera nem encerra brokers externos.

Procedimento manual, resultados efetivamente observados, regressões e
limitações: [SIM-04 — Validação integrada](../docs/sim-04-validacao.md).


## Envio ao InterSCity (SIM-05.5)

Contrato da API, decisões e evidências da integração:
[SIM-05 — Integração com o InterSCity](../docs/sim-05-interscity.md).

```sh
iot-gateway/build/iot-gateway \
  --interscity-url https://cidadesinteligentes.lsdi.ufma.br/interscity_lh \
  --interscity-map config/interscity/machines.conf
```

| Opção | Padrão | Validação |
| --- | --- | --- |
| `--interscity-url` | — | `http://` ou `https://`, sem espaços nem caracteres de controle; `/` finais ignoradas |
| `--interscity-map` | — | Arquivo `<machine_id> <uuid>` (ver `interscity_mapping.h`) |
| `--interscity-timeout-ms` | `5000` | Inteiro de 1 a 60000, por envio, conexão incluída |

URL e mapeamento são usados juntos; informar apenas um deles, ou só o timeout,
é erro de argumento. **Sem essas opções, o Gateway se comporta exatamente como
na SIM-04**, com as mesmas mensagens. O cliente HTTP é criado e o mapeamento é
carregado antes da conexão MQTT; mapeamento inválido encerra com código 1 e
informa a linha, por exemplo:

```text
Erro no mapeamento InterSCity: Quebra de linha CRLF (caractere CR) no mapeamento (linha 2).
```

Com o envio habilitado, stdout recebe antes da prontidão:

```text
InterSCity habilitado: <url> (10 máquinas mapeadas, timeout 5000 ms).
```

Cada telemetria válida continua gerando `Telemetria válida: ...` e, em seguida:

1. O UUID é obtido do mapeamento. Máquina sem UUID não é enviada, pois o Data
   Collector descartaria em silêncio um UUID desconhecido.
2. `interscity_payload` monta o corpo (medição ausente omite a capability;
   `power` não é enviado; `vibration_rms` e `rpm` vão para `sim_vibration_rms` e
   `sim_rpm`; timestamp `AAAA-MM-DD HH:MM:SS` vira ISO 8601 sem fuso).
3. `interscity_client` faz `POST {url}/adaptor/resources/{uuid}/data`, reutilizando
   a mesma conexão HTTP/TLS entre envios. Somente HTTP 201 é sucesso.

```text
InterSCity: machine_id=1 enviada (HTTP 201, 87 ms, conexão reutilizada).
InterSCity: machine_id=2 sem UUID no mapeamento; não enviada.
InterSCity: machine_id=5 falhou: Resposta HTTP diferente de 201 (HTTP 500, 12 ms).
```

Sucessos vão para stdout e falhas para stderr. Nenhuma falha HTTP, de rede ou de
timeout encerra o Gateway. No encerramento, antes de `Gateway encerrado por
solicitação.`, stdout recebe o resumo `InterSCity: N enviadas, F falhas, U sem mapeamento.`

**Limitações desta etapa:**

- Envio síncrono no callback, numa única thread: enquanto um POST aguarda
  resposta, novas mensagens MQTT esperam. Cada envio deve ser mais rápido que o
  intervalo agregado das máquinas; medido na validação real.
- Sem reenvio automático: o POST não é idempotente, e um timeout não informa se
  o dado foi aceito. O QoS 1 já foi confirmado ao broker quando o callback roda,
  então uma amostra cujo envio falha é perdida.
- HTTP 201 indica publicação pelo Adaptor no RabbitMQ, não armazenamento: a
  persistência só é comprovada consultando o Data Collector.
- Um sinal durante um envio é observado após o envio terminar (até o timeout).

`make test` inclui `tests/test_interscity_client.py` (cliente contra um
InterSCity falso: 201, 400, 500, timeout, porta fechada e reuso de conexão) e
`tests/test_interscity_gateway.py` sem argumentos (opções, mapeamentos inválidos
e o mapeamento real do repositório, sem broker). `make test-mqtt` executa também
`tests/test_interscity_gateway.py --mqtt`: Mosquitto temporário, InterSCity falso
e Gateway real, conferindo o corpo de cada envio, a omissão de medição ausente,
o reuso da conexão, máquina sem mapeamento, resposta 500 e continuidade após
falhas. Nenhum teste automatizado acessa a instância real do LSDi.

## Fila durável SQLite — SIM-09.1 (módulo isolado)

`telemetry_queue.h`/`telemetry_queue.c` implementam somente persistência local,
sem chamadas ou dependências de MQTT, HTTP, libmosquitto ou libcurl no módulo.
A integração opcional do `main.c` com a fila está descrita na SIM-09.2 abaixo.
`make all` compila e vincula o objeto SQLite ao Gateway; sem `--queue-db`, o
comportamento da SIM-08 permanece preservado. Não existe thread HTTP.

A dependência é SQLite, com funções JSON habilitadas (builds padrão 3.38+),
obtida via `pkg-config sqlite3`. A implementação foi validada com SQLite 3.54.0.
No Arch Linux, o pacote é `sqlite`. Para compilar e executar os testes:

```sh
pkg-config --modversion sqlite3
make -C iot-gateway all
make -C iot-gateway test
```

O teste C usa diretórios temporários em `/tmp`, remove bancos e verifica que
arquivos WAL/SHM não permaneceram após fechar as conexões. Não usa dados reais
nem requer broker para os casos SQLite. Os demais testes do alvo são mantidos.

### Esquema e API

`telemetry_queue` contém `id INTEGER PRIMARY KEY AUTOINCREMENT`, `machine_id`,
`timestamp`, `uuid`, `payload_json`, `status`, `attempt_count`, `last_error` e
`created_at`, com unicidade `(machine_id, timestamp)`. O timestamp da amostra
é normalizado, exatamente `YYYY-MM-DDTHH:MM:SS`, sem atribuição de fuso. Os
componentes têm os mesmos intervalos básicos do payload da SIM-08; calendário
completo não é validado. UUID é canônico minúsculo e o payload é um objeto JSON,
armazenado uma só vez. `created_at` é o instante UTC de inserção gerado pelo
SQLite, distinto do instante histórico da amostra.

`delivered_cursor` armazena somente `machine_id` (chave primária) e
`last_timestamp`. Não guarda outra cópia do payload. A deduplicação persistente
ignora amostras com timestamp menor ou igual ao último confirmado por máquina,
como a semântica atual da SIM-08. **Não atende à entrega de amostras fora de
ordem.** Avançar o cursor sobre amostras mais novas não remove automaticamente
itens antigos já enfileirados; o worker respeita essa política.

As operações públicas são:

- `telemetry_queue_open`/`telemetry_queue_close`: abertura/inicialização e fechamento.
- `telemetry_queue_enqueue`/`telemetry_queue_check_duplicate`: inserção pendente
  atômica e consulta consultiva de duplicatas.
- `telemetry_queue_peek`: primeiro item não bloqueado por ID de inserção, incluindo
  `pending` e `inflight`. Não reserva o item nem mantém transação aberta.
- `telemetry_queue_start_attempt`: apenas `pending` → `inflight`, incrementando
  uma vez o número de tentativas iniciadas.
- `telemetry_queue_retry`: apenas `inflight` → `pending`, com erro registrado,
  para falha transitória cujo resultado já é definitivamente conhecido.
- `telemetry_queue_block`: `pending` ou `inflight` → `blocked`, com motivo.
- `telemetry_queue_confirm`: apenas item `inflight`; avança o cursor sem retroceder
  e remove o registro na mesma transação.
- `telemetry_queue_cursor`/`telemetry_queue_count`: leitura do cursor e quantidade
  total ainda não entregue, incluindo pendentes, inflight e bloqueados.
- `telemetry_queue_item_clear`: libera os textos dinâmicos de um item consultado.

Retornos distintos: `OK`, `DUPLICATE_QUEUED` (qualquer estado na fila),
`DUPLICATE_DELIVERED` (cursor tem precedência), `EMPTY` e `ERROR`. Cada erro
pode fornecer um `TelemetryQueueError` com código estendido SQLite e descrição;
argumentos/estados inválidos também retornam erro. Ausência de item ou transição
incompatível não é considerada sucesso. Consulte os contratos e assinaturas no
header. Inicialize o item de saída com `{0}` e limpe-o antes de reutilizar;
`peek` preserva a saída em erro ou fila vazia. Payload e último erro são cópias
próprias do item, liberadas com `item_clear`. Os campos de entrada são strings
terminadas em NUL e são copiados para o banco.

### Durabilidade, concorrência e limites

Cada conexão verifica `journal_mode=WAL`, configura `synchronous=FULL` e usa
`sqlite3_busy_timeout` de 1000 ms. Bancos em memória são rejeitados, pois não
ativam WAL. Use banco em armazenamento local compatível com WAL; não se promete
suporte a sistemas de arquivos de rede. O SQLite deve ser compilado com suporte
a threads. Um contexto não pode ser compartilhado entre threads: a
thread MQTT e a thread HTTP abrem conexões separadas para o mesmo
arquivo. O módulo usa `SQLITE_OPEN_NOMUTEX` e não cria threads.

Inserção faz `BEGIN IMMEDIATE`, consulta cursor/duplicata e insere ou retorna
duplicata, com COMMIT verificado. Confirmação faz `BEGIN IMMEDIATE`, upsert
monotônico do cursor, DELETE e COMMIT. Qualquer falha tenta ROLLBACK; falha de
rollback é reportada e invalida a conexão para operações até seu fechamento.
Os estados usam UPDATEs condicionais atômicos. A aquisição da tentativa é uma
operação separada da consulta; um segundo chamador que perdeu a disputa recebe
erro. Nenhuma chamada de rede pode ocorrer dentro dessas transações curtas.

Registros `inflight` sobrevivem ao fechamento/reabertura **sem retornar a
pending**. Eles representam envio potencialmente ambíguo e são reconciliados
pelo worker na SIM-09.4, sem reset do contador de tentativas. Bloqueados continuam persistidos e contados, mas não são retornados
por `peek`; esta etapa não fornece desbloqueio automático. Não há retries HTTP,
expiração, limpeza automática, migração de esquema existente incompatível ou
limite de tamanho da fila além dos limites normais do SQLite/disco.

Os testes cobrem FIFO, duplicatas, estados e tentativas, cursor monotônico,
reabertura, bloqueio, dois contextos independentes, contenção de escrita,
argumentos inválidos e falhas SQLite. Triggers de teste interrompem remoção ou
atualização do cursor e verificam rollback sem confirmação parcial. Fechar e
reabrir demonstra persistência; não equivale a um teste de perda física de
energia. Durabilidade depende também das garantias de sincronização do sistema
operacional e do armazenamento. Falhas de COMMIT/ROLLBACK por I/O real não são
injetadas nesta etapa; seus retornos são verificados no módulo.


## Recepção MQTT para SQLite — SIM-09.2

Para habilitar persistência e entrega pelo worker HTTP (SIM-09.3), use `--queue-db`:

```sh
iot-gateway/build/iot-gateway \
  --interscity-url http://127.0.0.1:8000 \
  --interscity-map config/interscity/machines.conf \
  --queue-db ./data/telemetry.db
```

O diretório do banco deve existir; o programa não cria diretórios. Caminhos
relativos são resolvidos a partir do diretório atual. Arquivos de banco são
dados locais e não devem ser versionados. Caminho vazio, opção repetida ou fila
sem URL e mapa são argumentos inválidos. A URL recebe validação básica local,
compatível com o contrato atual do cliente direto, sem resolver host ou abrir
conexões. Ela configura o destino do worker e não é armazenada em cada registro.
`--interscity-timeout-ms` limita cada requisição do worker (padrão 5000 ms).
InterSCity indisponível não impede persistência MQTT; erro de rede suspende
as entregas por ser ambíguo, conforme a seção SIM-09.3.

Inicialização: argumentos → mapa validado → banco/esquema aberto → cliente
HTTP criado → worker/SQLite próprio inicializado → MQTT.
Falha no mapa ou banco encerra com código 1 antes de consumir mensagens.
O contexto está completo antes de `mqtt_subscriber_connect`, inclusive para
callbacks entregues durante a conexão. Logs de habilitação e prontidão:

```text
Fila SQLite habilitada (10 máquinas mapeadas; worker HTTP independente).
Gateway pronto: smart-industry/machines/+/telemetry (QoS 1).
```

Cada mensagem mantém a validação e o log `Telemetria válida`. Com a fila ativa,
o callback consulta o UUID no mapa, constrói o corpo com
`interscity_payload_build`, normaliza o timestamp pela lógica da SIM-08 e chama
`telemetry_queue_enqueue` diretamente. Não existe pré-consulta de duplicata:
a operação SQLite faz a verificação atomicamente. Exemplo de log:

```text
Fila SQLite: machine_id=1 persistida (ts 2024-01-01T00:00:00, pending).
Fila SQLite: machine_id=1 duplicata ignorada (ts 2024-01-01T00:00:00, já enfileirada).
```

Duplicatas podem ser `já enfileirada` (qualquer estado na fila) ou `já confirmada`
(conforme cursor persistente). Não geram outro registro. Mensagens sem UUID,
JSON inválido ou falha na construção do payload são diagnosticadas e descartadas
individualmente, sem armazenar dados não enviáveis. A recepção continua.

Erro de persistência é fatal: stderr recebe código estendido SQLite e mensagem
escapada; a flag de erro impede callbacks seguintes e interrompe o loop, inclusive
durante conexão/reconexão. Não registra persistência para a operação que falhou.
MQTT e SQLite são fechados pelo fluxo principal e o processo retorna 1. SIGINT
ou SIGTERM normais retornam 0 após o fechamento. A reconexão MQTT da SIM-08
continua disponível, mas não é usada para tentar contornar uma falha SQLite.

O resumo `Fila SQLite` informa persistências desta execução: persistidas,
duplicatas ignoradas, sem mapeamento e não preparadas. Um resumo separado
`Worker HTTP` informa entregas HTTP 201, retentativas, bloqueios e recuperações
pelo Data Collector separadamente. Nenhum
deles representa a quantidade total já existente no banco. Novos registros
entram como `pending`, com `attempt_count=0`, antes de serem reclamados pelo worker.

**Limite de garantia:** o PUBACK MQTT é gerenciado pela libmosquitto e não é
atômico com o COMMIT SQLite. QoS 1 não garante redelivery após falha de banco;
uma amostra pode ser perdida antes de persistir mesmo com erro corretamente
reportado. Não foi alterado o protocolo MQTT. A gravação e o busy timeout
SQLite também ocupam o callback síncrono. Permanecem as limitações de fila,
ordenação e `inflight` descritas acima. HTTP nunca é executado no callback MQTT.

Sem `--queue-db`, permanecem o envio direto, deduplicação em memória, tratamento
de timeout com consulta ao Data Collector/reenvio único, reconexão e resumos
legados; sem URL/mapa, permanece o modo de validação e logging.

`tests/test_durable_queue.py` integra `make test` para CLI/inicialização e
`make test-mqtt` para recepção real, SQLite temporário e InterSCity falso que
registra requisições. Uma fixture anterior `inflight` com Collector indisponível mantém a fila
aguardando reconciliação para isolar os testes de recepção: três registros pending com campos/corpo conferidos,
duplicatas, reinicialização, HTTP indisponível e ausência de POST ao Adaptor durante falha de consulta, JSON
inválido, ID sem mapa, erro de payload, cursor confirmado e erro SQLite fatal
injetado por trigger. Verifica fechamento por sinais e ausência de WAL/SHM
residuais. Os testes existentes da SIM-08 não foram modificados.


## Entrega HTTP independente — SIM-09.3

`delivery_worker.h/.c` mantém um único pthread no modo `--queue-db`. A thread
principal continua validando MQTT e persistindo com sua conexão SQLite; o worker
abre, utiliza e fecha outra conexão para o mesmo arquivo. Não se compartilham
handles SQLite nem o cliente HTTP. `delivery_worker_start` cria o cliente
libcurl na thread principal **antes** de `pthread_create`, aguarda a abertura do
SQLite do worker e retorna 0/-1. `delivery_worker_failed` consulta uma flag
atômica. `delivery_worker_stop` solicita parada, executa `pthread_join`, imprime
os contadores HTTP e somente então destrói o cliente e seus recursos.

O worker utiliza UUID e JSON já persistidos. `peek` retorna o registro elegível
mais antigo por ID (bloqueados são ignorados); `start_attempt` faz
`pending → inflight` e incrementa `attempt_count`. Nenhuma transação permanece
aberta durante HTTP. O resultado determina a transição:

| Resultado | Ação |
| --- | --- |
| HTTP 201 | `confirm`: cursor monotônico e remoção em transação única; só depois registra entrega |
| HTTP 429 ou 5xx | `retry`: volta a pending com last_error; espera 1, 2, 4, 8, 16, 30 segundos (teto) |
| Outro status HTTP | `block`: preserva registro e motivo; continua os próximos elegíveis |
| Timeout/erro de rede | Preserva inflight e inicia reconciliação automática (SIM-09.4) |
| Inflight existente | Reconcilia antes de enviar registros posteriores; MQTT continua gravando |
| Erro local/SQLite | Falha fatal comunicada por flag atômica; processo termina com código 1 |

O contador de retentativas conta retornos a `pending` para nova tentativa;
pode incluir uma espera interrompida antes do próximo POST.
Após entrega confirmada o backoff reinicia em 1 segundo. Fila vazia é consultada
a cada 250 ms; esperas observam parada em fatias de até 50 ms, sem busy waiting.
A SIM-09.4 substitui a suspensão permanente da SIM-09.3 pela reconciliação
automática descrita abaixo. O estado inflight permanece no banco até uma decisão
confirmada; falhas temporárias não encerram a rotina. Diagnósticos de consultas
ficam no log; a API existente não altera last_error enquanto mantém inflight.
Apenas um Gateway/worker deve consumir o arquivo: não há protocolo para múltiplos
consumidores. A URL é global da execução; ao reiniciar, deve apontar ao mesmo
InterSCity dos UUIDs persistidos.

SIGINT/SIGTERM continuam apenas atualizando a flag do handler. O fluxo principal
fecha MQTT, solicita parada e aguarda o join antes de fechar o banco principal.
Uma requisição ativa pode terminar ou alcançar seu timeout: se retornar 201, a
confirmação SQLite é concluída antes de sair; se ambígua, mantém inflight. O
limite prático depende do timeout HTTP configurado, acrescido das operações
SQLite (busy timeout de 1000 ms por operação) e das limitações DNS/TCP síncronas
já documentadas. Falha na confirmação após HTTP 201 é fatal: não anuncia entrega,
a transação é revertida e o registro fica recuperável. Não há cancelamento
abrupto de pthread nem operações complexas nos handlers.

Exemplos de logs (formatos; resultados de testes são emitidos pela suíte):

```text
Worker HTTP: id=1 machine_id=1 entregue (HTTP 201; SQLite confirmado).
Worker HTTP: id=2 HTTP 503; pending; nova tentativa em 1000 ms.
Worker HTTP: id=3 blocked (HTTP 400).
Worker HTTP: id=4 inflight; início da reconciliação.
```

`make -C iot-gateway test-mqtt` inclui `tests/test_delivery_worker.py`, com
Mosquitto, SQLite e HTTP temporários locais. Verifica três entregas FIFO e seus
payloads/cursor, 503 e 429 com backoff/reset, 400 seguido de entrega, timeout sem
reenvio, reinicialização inflight, MQTT durante HTTP bloqueado por evento,
SIGTERM durante sucesso/timeout/backoff e trigger que impede confirmação após
201 (rollback do cursor e da remoção, sem falso sucesso). Respostas lentas são
controladas por eventos; todos os processos têm timeout e limpeza em finally.
Não utiliza o InterSCity real. Os testes de recepção da SIM-09.2 mantêm cobertura
de validação, payload, persistência, duplicatas e erro SQLite fatal; ausência de
POST ao Adaptor é exigida enquanto a consulta da pendência inflight falha.

Sem `--queue-db`, não se cria worker ou conexão SQLite; o fluxo direto da SIM-08,
seus logs e a consulta ao Data Collector permanecem inalterados. QoS 1 ainda
admite duplicações e seu ACK não é atômico com persistência. Não há garantia exactly-once, múltiplos workers ou garantia de persistência
no Data Collector decorrente apenas do HTTP 201. A reconciliação atual utiliza
a identidade aproximada do timestamp e pode produzir duplicatas.


## Reconciliação e recuperação automática — SIM-09.4

Procedimento e evidências: [SIM-09 — Fila durável](../docs/sim-09-fila-duravel.md).

Fluxo: **MQTT → validação/payload → SQLite → worker HTTP → InterSCity**. A thread
MQTT persiste usando sua conexão; o único worker usa outra conexão SQLite e seu
cliente libcurl exclusivo tanto para POST ao Adaptor quanto para
`POST /collector/resources/data/last`. Não há transações abertas nas consultas,
requisições ou esperas. Nenhuma opção nova de CLI ou alteração de esquema.

O worker não encerra por indisponibilidade HTTP: a cabeça FIFO permanece inflight
e impede envios posteriores até resolver a ambiguidade. MQTT continua gravando.
Quando a consulta voltar a funcionar, a recuperação prossegue automaticamente,
sem reiniciar o Gateway. Registros inflight encontrados ao reiniciar seguem a
mesma política, mantendo attempt_count; blocked não são apagados nem reenviados.

As constantes `RECONCILE_INITIAL_MS=2000` e `CONSISTENCY_WINDOW_MS=2000` estão no
worker: espera inicial de 2 s antes da primeira consulta de cada reconciliação,
inclusive após reinício, e mais 2 s entre evidências de ausência. As decisões são:

| Resultado da consulta | Decisão |
| --- | --- |
| Timestamp exatamente igual | `confirm` atômico; conta recuperação pelo Data Collector, sem novo POST |
| `found=0` | Espera 2 s e consulta novamente; duas ausências consecutivas autorizam `retry` |
| Timestamp anterior | Mesma política de duas consultas da ausência |
| Timestamp posterior | Não confirma nem remove; `retry` autoriza reenvio com aviso de risco de duplicação |
| Timeout/rede/HTTP inesperado/resposta inválida | Mantém inflight, não reenvia; consulta novamente com backoff |

Falha entre duas respostas de ausência reinicia a evidência: não se presume
que uma consulta malsucedida confirma ausência. O backoff é compartilhado com
as recusas transitórias de POST: 1, 2, 4, 8, 16, 30 s, com teto de 30 s; uma
entrega HTTP 201 ou recuperação confirmada o reinicia em 1 s. Um reenvio sempre
passa por `inflight → pending → start_attempt → inflight`, incrementando
attempt_count. Falhas SQLite, inclusive confirmar após consulta bem-sucedida,
continuam fatais e não contam entrega/recuperação.

O cliente existente foi ajustado pontualmente para não converter respostas
incompatíveis em ausência: verifica objeto completo com comprimento explícito,
NUL bruto/conteúdo adicional, tipos das séries e datas conhecidas. HTTP 404 só
significa ausência com objeto JSON cujo `error` é `Resource not found`. Respostas
válidas da SIM-08 permanecem aceitas. O recurso retornado deve ter `uuid` textual
igual ao UUID solicitado, como na resposta real da instância UFMA. Datas aceitas são exclusivamente UTC completas:
`YYYY-MM-DDTHH:MM:SSZ` ou `YYYY-MM-DDTHH:MM:SS.dígitosZ`, com ao menos um dígito
na fração. Sufixos arbitrários, ausência de `Z` e offsets são rejeitados; não há
comparação por prefixo entre fusos diferentes. Somente depois da validação a
data é reduzida aos 19 caracteres usados na comparação. UUID incompatível ou
data inválida resulta em `INTERSCITY_CLIENT_RESPONSE`, sem confirmação, cursor
ou reenvio autorizado; uma consulta válida posterior permite recuperação.
O fluxo legado, sem `--queue-db`, permanece
com sua política anterior (inclusive comparação `>=` na deduplicação em memória).

`/last` informa apenas a amostra mais recente de capabilities conhecidas; não é
uma consulta de presença exata. Timestamp igual é **identidade aproximada** e não
prova todo o conteúdo das capabilities. Timestamp posterior não prova a presença
da amostra atual. Uma amostra pode já ter sido aceita e ainda não aparecer por
consistência eventual; mesmo duas consultas não eliminam essa possibilidade.
O reenvio prioriza entrega e **pode duplicar dados**. Não há exactly-once nem
idempotency keys. O cursor continua ignorando novas entradas com timestamp menor
ou igual ao último confirmado; não resolve amostras fora de ordem. PUBACK MQTT
e COMMIT SQLite continuam não atômicos: dados não persistidos podem ser perdidos.

SIGTERM interrompe esperas; após observar parada não inicia novo POST. Uma
consulta/POST ativa termina ou atinge o timeout configurado; confirmação válida
pode ser concluída antes de sair, mas resultados não resolvidos ficam inflight.
O join continua anterior à destruição do cliente. Os limites práticos de timeout,
DNS/TCP síncrono, busy timeout SQLite e logs síncronos permanecem.

Para executar os testes de recuperação e regressão com serviços falsos locais:

```sh
make -C iot-gateway clean
make -C iot-gateway all
make -C iot-gateway test
make -C iot-gateway test-mqtt
make -C iot-gateway test-e2e
# Apenas worker/reconciliação (incluído em test-mqtt):
(cd iot-gateway && python3 -B tests/test_delivery_worker.py)
```

A suíte separa contagens de POSTs de telemetria e consultas. Cobre timeout com
amostra armazenada e sem armazenamento; respostas anteriores/posteriores;
indisponibilidade com MQTT ativo e retomada sem reinício; reabertura inflight;
Collector 503; respostas malformadas/incompatíveis; SIGTERM durante consulta;
consistência eventual com falha intermediária; rollback após confirmação por
consulta. Preserva testes de 201, 429/5xx, blocked, deduplicação, HTTP lento,
SIGTERM e rollback da SIM-09.3, além das suítes legadas da SIM-08. O teste imprime
as contagens observadas e o PID mantido no cenário de indisponibilidade; somente
servidores temporários locais são utilizados.
