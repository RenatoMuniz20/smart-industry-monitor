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
