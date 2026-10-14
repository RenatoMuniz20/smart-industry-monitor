# Dataset Replayer

Componente em C11 que lê e valida o dataset industrial linha a linha, sem
carregá-lo inteiro em memória. Na arquitetura do Smart Industry Monitor, é a
entrada dos dados históricos: nesta etapa, seleciona uma máquina e imprime
seus registros. MQTT ainda não faz parte deste componente nesta etapa.

## Requisitos e compilação

Linux, GCC e Make. Execute os comandos abaixo a partir da raiz do repositório:

```sh
make -C dataset-replayer
make -C dataset-replayer test
make -C dataset-replayer clean
```

A compilação usa C11 e os avisos `-Wall -Wextra -Wpedantic`. O executável fica
em `dataset-replayer/build/dataset-replayer`; `clean` remove os artefatos.

## Execução

Após compilar:

```sh
# Primeiros cinco registros da máquina 6
dataset-replayer/build/dataset-replayer data/predictive_maintenance_v3.csv --machine-id 6 --limit 5

# Todos os registros da máquina 6
dataset-replayer/build/dataset-replayer data/predictive_maintenance_v3.csv --machine-id 6

# Execução pelo Make
make -C dataset-replayer run ARGS="../data/predictive_maintenance_v3.csv --machine-id 6 --limit 5"
```

A sintaxe é `dataset-replayer <csv> --machine-id <id> [--limit <quantidade>]`.
`--machine-id` é obrigatório; ambos os valores devem ser inteiros positivos.
Sem `--limit`, todos os registros da máquina são selecionados. Uma máquina
inexistente encerra normalmente, com uma mensagem e zero registros.

O CSV tem 15 colunas, sem coluna `index`, e 24.042 registros no dataset atual.
O leitor valida o cabeçalho e cada registro percorrido; ao atingir o limite,
encerra a leitura sem validar as linhas restantes. A API oferece um callback
opcional para processar cada registro válido.

Campos `double` vazios são representados por `NAN`. Na saída, `N/A` indica um
valor ausente, não zero. Campos inteiros vazios ou inválidos causam erro.

## Normalização de telemetria

O módulo independente `telemetry.h` oferece
`int telemetry_normalize(const DatasetRow *row, Telemetry *out)`, chamado pelo
`main.c` após o filtro por máquina. `Telemetry` contém `machine_id`, `machine_type`,
`timestamp`, `temperature`, `current`, `power`, `state` e `failure_within_24h`.
Os textos são copiados para arrays próprios de 64 bytes; o timestamp é
preservado sem conversão ou adição de fuso horário.

`temperature_motor` vira `temperature` e `current_phase_avg` vira `current`,
preservando valores e `NAN`, sem conversão de unidades. `power` é sempre `NAN`,
pois o dataset não fornece potência diretamente. Os modos exatos `idle`,
`normal` e `peak` viram `TELEMETRY_STATE_IDLE`, `TELEMETRY_STATE_NORMAL` e
`TELEMETRY_STATE_PEAK`; qualquer outro texto, inclusive vazio, vira
`TELEMETRY_STATE_UNKNOWN` (valor zero).

`failure_within_24h` aceita apenas 0 ou 1 e mantém seu significado preditivo,
sem indicar falha operacional atual nem alterar o estado. A função retorna
0 em sucesso ou -1 para argumentos nulos ou indicador inválido, preservando
integralmente a saída em erro. Não realiza alocação dinâmica ou operações de
entrada e saída.

A CLI exibe todos esses campos, com estados `UNKNOWN`, `IDLE`, `NORMAL` ou
`PEAK` e valores ausentes como `N/A`. `--limit` conta apenas registros
selecionados, normalizados e exibidos com sucesso. Um erro de normalização
interrompe a leitura e informa a linha do CSV, sem contar o registro inválido.
O processamento continua em streaming. `make test` executa os testes do leitor,
da normalização e da CLI; estes últimos usam ferramentas padrão do Linux e
arquivos temporários removidos ao final.

## Serialização JSON de telemetria (SIM-03.1)

O módulo independente `telemetry_json.h` converte uma `Telemetry` para JSON,
sem alterar a estrutura, a leitura CSV, a normalização ou a CLI atual. Não
publica mensagens MQTT e não realiza operações de rede ou entrada e saída.

```c
int telemetry_to_json(const Telemetry *telemetry, char *buffer, size_t capacity);
```

O chamador fornece o buffer e sua capacidade, incluindo o terminador `\0`.
O retorno é 0 em sucesso ou -1 para argumentos inválidos, buffer insuficiente,
textos inválidos, indicador de falha diferente de 0/1 ou erro de formatação/locale.
O buffer inteiro permanece inalterado em erro. Os arrays textuais devem conter
terminador dentro de seus 64 bytes e UTF-8 válido. Aspas, barras invertidas e
caracteres de controle são escapados; UTF-8 válido é preservado.

Exemplo de saída (exatamente oito propriedades):

```json
{"machine_id":6,"machine_type":"Robotic Arm","timestamp":"2024-01-01 00:21:00","temperature":49.5,"current":3.5,"power":null,"state":"NORMAL","failure_within_24h":1}
```

Os três campos `double` são números ou `null`: `NAN` e ambos os infinitos viram
`null`, sem omitir propriedades. `power` normalmente é `null`, conforme a
normalização. O timestamp permanece intacto, sem presumir fuso horário.
Estados são `UNKNOWN`, `IDLE`, `NORMAL` ou `PEAK`; enumerações não reconhecidas
seguem a convenção atual da CLI e viram `UNKNOWN`. O indicador preditivo
`failure_within_24h` permanece um número inteiro 0 ou 1.

A formatação usa `DBL_DECIMAL_DIG`, podendo exibir mais casas que a CLI para
preservar o valor do `double`. Um locale numérico POSIX `C` é aplicado apenas
à thread durante a formatação e o anterior é restaurado; o locale global não
é alterado. Não há dependência externa de JSON. O alvo continua sendo Linux,
com biblioteca C que ofereça `newlocale`, `uselocale` e `freelocale`.

O JSON é montado em buffer temporário de 2.048 bytes antes de copiar para o
destino. Cada texto de até 63 bytes expande para no máximo 378 bytes mais duas
aspas; os dois textos, as chaves, pontuação, estados, inteiros e três números
em notação científica cabem nesse limite no alvo Linux. Os limites e retornos
de formatação também são verificados. Os buffers auxiliares de texto são
dimensionados para essa expansão; não há alocação dinâmica explícita no módulo,
embora a biblioteca C possa alocar internamente ao criar o locale.

`make -C dataset-replayer all` também compila o objeto do serializador.
Execute `make -C dataset-replayer test` a partir da raiz para rodar os testes
existentes e `test_telemetry_json`, incluindo a integração da normalização com
o JSON. O teste de locale alternativo informa `SKIP` se nenhum dos locales
candidatos estiver instalado; os demais testes continuam normalmente.

## Cliente publicador MQTT (SIM-03.2)

O módulo `mqtt_publisher.h` usa libmosquitto, MQTT 3.1.1 e uma conexão
reutilizável. Ainda não está integrado ao CSV nem à CLI. No Arch Linux:

```sh
sudo pacman -S mosquitto pkgconf
pkg-config --modversion libmosquitto
make -C dataset-replayer all
make -C dataset-replayer test
make -C dataset-replayer test-mqtt
```

`all` compila o objeto MQTT. Somente o teste MQTT é vinculado à libmosquitto;
o executável principal mantém suas dependências anteriores. `make test` inclui
validações locais do módulo e não abre conexões nem exige broker ativo.
`test-mqtt` requer Python 3 (biblioteca padrão), Mosquitto, `mosquitto_sub` e
`stdbuf` (GNU coreutils). Inicia e encerra um broker temporário em loopback
com porta dinâmica, sem interferir no broker de desenvolvimento em 1883.
Também usa pares TCP controlados para verificar recusa de CONNACK, ausência
de CONNACK/PUBACK, PUBACK de outro identificador e desconexão. As threads
utilizadas pelo harness Python são exclusivas dos testes; o módulo C não cria
threads. Os processos e arquivos temporários são removidos ao final.

Interface pública (retorno 0 em sucesso e -1 em erro):

```c
int mqtt_publisher_connect(MqttPublisher **out, const char *host, int port,
                           const char *client_id, int timeout_ms);
int mqtt_publisher_publish(MqttPublisher *publisher, int machine_id,
                           const char *json);
int mqtt_publisher_close(MqttPublisher **publisher);
int mqtt_publisher_topic(int machine_id, char *buffer, size_t capacity);
```

Inicialize `MqttPublisher *publisher = NULL` e passe seu endereço a `connect`.
Use `127.0.0.1`, porta 1883 e timeout de 5000 ms para desenvolvimento local.
Host, porta e client ID são fornecidos pelo chamador. O client ID deve ser
não vazio e único entre os clientes do broker; recomenda-se combinar máquina
e PID. O chamador é responsável por essa unicidade. O timeout aceita 1 a
5000 ms. Somente um contexto pode estar ativo por processo, com chamadas
sequenciais e sem uso concorrente. O módulo controla `mosquitto_lib_init` e
`mosquitto_lib_cleanup`; não deve compartilhar o processo com outros usuários
independentes da libmosquitto.

`connect` aguarda CONNACK aceito via callback antes de fornecer o contexto.
`publish` constrói `smart-industry/machines/{machine_id}/telemetry`, exigindo
ID positivo, e envia uma string JSON não vazia com QoS 1 e retain desabilitado.
O JSON deve vir do serializador; o módulo MQTT não implementa outro parser.
A função de tópico inclui o terminador na capacidade e preserva o destino
em erro. O envio retorna sucesso somente após callback de publicação com o
mesmo MID retornado por `mosquitto_publish`, que em QoS 1 indica recebimento
do PUBACK. O sucesso inicial de `mosquitto_publish` sozinho não basta.

QoS 1 confirma a recepção pelo broker e permite duplicação; não confirma o
processamento pelos assinantes nem persistência em disco. As chamadas usam
`mosquitto_loop` com esperas bloqueantes curtas e prazo monotônico. Não há
threads no módulo, reconexão automática, repetição de publicação, TLS,
autenticação ou persistência. O client usa sessão limpa. Chamadas de publicação
sucessivas reutilizam a mesma conexão.

**Limite de timeout:** a API documentada para uso com `mosquitto_loop` exige
`mosquitto_connect`, cuja preparação DNS/TCP é síncrona. O prazo começa antes
dessa chamada, mas não consegue interrompê-la: a espera por CONNACK/PUBACK é
limitada, enquanto DNS/TCP podem exceder 5 segundos conforme o sistema
operacional. Portanto não há garantia de timeout total de conexão para hosts
remotos indisponíveis. O módulo evita sinais globais e threads para contornar
essa limitação. Referência: https://mosquitto.org/api/files/mosquitto-h.html.

Erros de rede e timeout invalidam o contexto: encerre-o e informe a falha ao
chamador. Um timeout de PUBACK não prova que o broker deixou de receber a
mensagem. Argumentos inválidos não invalidam uma conexão saudável. `close`
aceita ponteiros nulos, tenta DISCONNECT quando a conexão está saudável,
destrói o cliente, finaliza a biblioteca e limpa o ponteiro mesmo em erro.
Após encerrar, um novo contexto pode ser aberto. Não use aliases do ponteiro
já liberado. Como não há loop em segundo plano, longos períodos sem chamadas
podem exceder o keepalive de 60 segundos e causar desconexão.
