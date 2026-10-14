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
