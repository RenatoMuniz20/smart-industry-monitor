# IoT Gateway — SIM-04.1 a SIM-04.3

Módulo C11 independente para interpretar e validar tópico e telemetria JSON.
O validador JSON (SIM-04.1) e o assinante MQTT (SIM-04.2) são módulos separados.
O executável `build/iot-gateway` (SIM-04.3) integra assinatura, callback e
validação, com CLI e encerramento por sinais. Não há HTTP nem InterSCity.
O Dataset Replayer não é modificado nem utilizado como dependência de compilação.

## Build e testes locais

Requer GCC, Make, pkg-config, json-c 0.19 e libmosquitto 2.1.2. Na raiz do repositório:

```sh
pkg-config --modversion json-c
make -C iot-gateway all
make -C iot-gateway test
make -C iot-gateway clean
```

O Makefile usa `-std=c11 -Wall -Wextra -Wpedantic`, obtém flags com pkg-config
e vincula cada teste apenas à biblioteca necessária (json-c ou libmosquitto).
`make test` não abre conexões nem exige broker. O módulo de validação permanece
independente de MQTT.

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

O objeto JSON deve conter exatamente as oito propriedades distintas abaixo,
sem adicionais, todas obrigatórias e com tipos verificados antes da conversão:

| Propriedade | Regra |
| --- | --- |
| `machine_id` | Inteiro positivo até `INT_MAX`, igual ao ID do tópico |
| `machine_type` | String não vazia, até 63 bytes |
| `timestamp` | String não vazia, até 63 bytes, preservada sem conversão |
| `temperature`, `current`, `power` | Número inteiro/fracionário finito ou `null` |
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
explícita: oito propriedades distintas não garante oito ocorrências no texto.
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
seguida de um objeto JSON com os oito campos, mantendo `null` diferente de
zero. Textos são representados com escaping da json-c; não são interpolados
como texto bruto no terminal. Exemplo observado no teste de integração:

```text
Telemetria válida: {"machine_id":1,"machine_type":"CNC","timestamp":"original sample","temperature":0.0,"current":null,"power":null,"state":"PEAK","failure_within_24h":1}
```

Para mensagem inválida, stderr recebe o diagnóstico do validador e o tamanho,
sem imprimir o payload ou tópico não confiáveis. Exemplo observado:

```text
Telemetria inválida: JSON inválido, incompleto ou UTF-8 inválido (payload=18 bytes).
```

A mensagem inválida é descartada e o Gateway continua recebendo. Não há
armazenamento, agregação, alertas ou transmissão HTTP. Logs válidos reformatam
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
comparando os oito campos às amostras do CSV e permitindo duplicações QoS 1.
Também injeta válida/`{}`/válida e encerra por SIGTERM após observar os logs.
Requer Linux (`/proc`), Bash 5.1+, Python 3, Mosquitto e `mosquitto_pub`, além
das dependências já descritas. Não altera nem encerra brokers externos.

Procedimento manual, resultados efetivamente observados, regressões e
limitações: [SIM-04 — Validação integrada](../docs/sim-04-validacao.md).
