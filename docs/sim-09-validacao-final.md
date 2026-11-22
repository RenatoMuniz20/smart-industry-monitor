# SIM-09.5 — Validação integrada final

## Base e objetivo

Rodada de 22/11/2026, branch `fix/sim-09`, working tree inicialmente limpo.
HEAD analisado: `6e54eaf5c44c213db75d5b65655d5f4941b0f8e0`
(`feat(sim-09): reconcile ambiguous telemetry deliveries`).

Objetivo: validar persistência e retomada após indisponibilidade e reinício,
consolidando o requisito de tolerância a falhas solicitado pelo professor.
Não foram alterados módulos C, esquema SQLite, política HTTP/backoff ou arquitetura.
Foi acrescentado somente `restart_outage_test` à suíte existente
`iot-gateway/tests/test_delivery_worker.py`, já executada por `make test-mqtt`.
Os testes anteriores de recuperação sem reinício e reinício com uma amostra
foram preservados: cobriam partes do fluxo, mas não a sequência completa com
cinco pendências e restauração posterior ao reinício.

## Arquitetura e responsabilidades

`Replayer → MQTT/Mosquitto → Gateway → SQLite → worker → InterSCity`

| Componente | Responsabilidade |
| --- | --- |
| Thread principal do Gateway | Assina, valida tópico/JSON, mapeia UUID, prepara payload, persiste e registra duplicatas |
| SQLite local | Fila durável e cursor de deduplicação; WAL, synchronous=FULL, busy timeout de 1000 ms |
| Único worker HTTP | Conexão SQLite própria e cliente libcurl exclusivo; entrega FIFO, backoff e reconciliação |
| Resource Adaptor / Data Collector | Destino definitivo no middleware; aceitação HTTP e consulta da última amostra são evidências distintas |

Os estados são pending → inflight ao iniciar tentativa, com incremento de
attempt_count. HTTP 201 confirma cursor/remoção atomicamente. HTTP 429/5xx volta
a pending e usa backoff 1/2/4/8/16/30 s. Recusa definitiva torna blocked,
preservando motivo e permitindo processar próximos elegíveis. Timeout/rede
mantém inflight: somente igualdade de timestamp na consulta confirma; ausência
ou timestamp anterior exige duas consultas separadas por 2 s. Timestamp posterior
não confirma e autoriza reenvio com risco de duplicação. Consulta falha/inválida
preserva inflight e usa backoff. Não há transação aberta durante HTTP/esperas.

## Reprodução local

Dependências utilizadas: GCC/C11, pthread, Python padrão, Mosquitto e suas
ferramentas, json-c 0.19, libmosquitto 2.1.2, libcurl 8.23.0 e SQLite 3.54.0.
Os testes criam diretórios/bancos temporários, broker isolado em localhost e HTTP
falso. Aguardam SUBACK/prontidão, têm timeouts e limpeza de processos em finally.
Não exigem broker público, InterSCity real ou alterações manuais de registros.

Na raiz do repositório:

```sh
make -C iot-gateway clean
make -C iot-gateway all
make -C iot-gateway test
make -C iot-gateway test-mqtt
make -C iot-gateway test-e2e
make -C dataset-replayer test
make -C dataset-replayer test-mqtt
make -C backend test
git diff --check
```

Para repetir somente a sequência principal, depois do build (sem nova suíte):

```sh
cd iot-gateway
PYTHONPATH=tests python3 -B - <<'PY'
from pathlib import Path
from tempfile import TemporaryDirectory
from test_delivery_worker import restart_outage_test
from test_cli import temporary_broker
from test_interscity_gateway import UUID_OK
with TemporaryDirectory(prefix='sim09-final-') as directory:
    root = Path(directory)
    mapping = root / 'machines.conf'
    mapping.write_text(f'1 {UUID_OK}\n')
    with temporary_broker(root) as (_, port):
        restart_outage_test(root, mapping, port)
PY
```

## Cenário principal e evidências

O teste inicia o Gateway real, desabilita o HTTP falso por evento e publica cinco
telemetrias da máquina 1 com timestamps distintos. A perda real da conexão HTTP
deixa a primeira inflight; o Collector retorna 503. Registra o snapshot e
attempt_count, encerra com SIGTERM e reinicia com o mesmo caminho e inode do banco.
Antes de restaurar o HTTP, compara todos os registros com o snapshot e verifica
nova consulta malsucedida, sem POST de reenvio. Após restauração, duas consultas
respondem ausência e autorizam nova tentativa. Verifica corpos, UUID, ordem,
contagens, cursor e fila vazia. Nenhum INSERT/UPDATE/DELETE de fixture é usado
nesse cenário; todas as transições vêm do Gateway.

Resultados observados, também repetidos sob ASan/UBSan:

| Momento | Registros | Estados / tentativas | Cursor |
| --- | --- | --- | --- |
| Antes de SIGTERM | 5 | inflight + 4 pending; attempt_count=[1,0,0,0,0] | Vazio |
| Depois de reiniciar, ainda indisponível | 5 | Snapshot integral preservado | Vazio |
| Após restaurar e concluir | 0 | 0 pending, 0 inflight, 0 blocked | 2024-01-01T00:04:00 |

Foram seis POSTs ao Adaptor: um inicial ambíguo e cinco com HTTP 201. Foram
quatro consultas ao Collector: duas com 503 e duas indicando ausência. A primeira
amostra foi reenviada somente depois da reconciliação. As cinco entregas da
segunda execução vieram do banco anterior (zero novas persistências nessa
execução), com corpos e ordem conferidos. Ambos os SIGTERMs retornaram 0.

Trecho essencial produzido pelo teste:

```text
EVIDÊNCIA antes do reinício: 5 persistidas; estados=[inflight,pending,pending,pending,pending]; attempt_count=[1,0,0,0,0]; cursor vazio; SIGTERM=0.
EVIDÊNCIA após reinício: 5 entregues HTTP 201; 0 pendentes/inflight/blocked; POSTs=6 (1 ambíguo + 5 confirmados); consultas=4 (2 HTTP 503 + 2 ausências); cursor=2024-01-01T00:04:00; SIGTERM=0.
```

## Resultados da rodada

| Comando / verificação | Resultado efetivo |
| --- | --- |
| Gateway clean / all | Sucesso, sem warnings GCC |
| Gateway test | Passou: módulos locais, SQLite, 24 consultas do cliente HTTP e CLI |
| Gateway test-mqtt | Passou: SIM-08, persistência, reconciliação e cenário principal SIM-09.5 |
| Gateway test-e2e | Passou: dez produtores, 30 telemetrias CSV, 32 válidas incluindo continuidade |
| Replayer test | Concluiu com código 0; **SKIP parcial** do locale alternativo |
| Replayer test-mqtt | Passou: QoS/PUBACK, falhas, temporização e dez processos |
| Backend test | **Não executou os testes**: build falhou com código 2 por ausência de microhttpd.h |
| ASan + UBSan, detect_leaks=1, halt_on_error=1 | Fila C, worker completo incluindo reinício/restauração, persistência MQTT e cliente HTTP passaram sem diagnósticos |
| git diff --check | Sem problemas de whitespace |

O SKIP do Replayer foi `locale numérico alternativo indisponível`; não conta
como validação aprovada desse caso. O backend apresentou
`fatal error: microhttpd.h: No such file or directory`; libmicrohttpd não está
disponível neste ambiente. Nenhuma dependência foi instalada e nenhum teste
anterior foi enfraquecido. Para repetir o backend em Arch Linux, instalar a
dependência do projeto (`sudo pacman -S libmicrohttpd`) e executar novamente
`make -C backend test`. A execução da SIM-08 confirmou sete publicações HTTP,
deduplicação, recuperação por consulta, reenvio único e SIGTERM=0.

Não foi encontrado defeito funcional bloqueante no Gateway durante esta rodada.
A falta da dependência do backend e do locale alternativo são limitações do
ambiente, não resultados positivos presumidos.

Para reproduzir os sanitizers a partir da raiz (binários somente em /tmp):

```sh
gcc -std=c11 -Wall -Wextra -Wpedantic -pthread -g \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -Iiot-gateway/include iot-gateway/src/*.c \
  $(pkg-config --cflags --libs json-c libmosquitto libcurl sqlite3) \
  -o /tmp/sim09-final-asan
gcc -std=c11 -Wall -Wextra -Wpedantic -g \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -Iiot-gateway/include iot-gateway/tests/test_telemetry_queue.c \
  iot-gateway/src/telemetry_queue.c $(pkg-config --cflags --libs sqlite3) \
  -o /tmp/sim09-queue-final-asan
gcc -std=c11 -Wall -Wextra -Wpedantic -g \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -Iiot-gateway/include iot-gateway/tests/test_interscity_client.c \
  iot-gateway/src/interscity_client.c $(pkg-config --cflags --libs json-c libcurl) \
  -o /tmp/sim09-client-final-asan
export ASAN_OPTIONS=detect_leaks=1:halt_on_error=1
export UBSAN_OPTIONS=halt_on_error=1
/tmp/sim09-queue-final-asan
(cd iot-gateway && SIM_GATEWAY_TEST=/tmp/sim09-final-asan python3 -B tests/test_delivery_worker.py)
(cd iot-gateway && SIM_GATEWAY_TEST=/tmp/sim09-final-asan python3 -B tests/test_durable_queue.py --mqtt)
(cd iot-gateway && SIM_INTERSCITY_CLIENT_TEST=/tmp/sim09-client-final-asan python3 -B tests/test_interscity_client.py)
```

## Cobertura complementar preservada

| Critério | Teste existente |
| --- | --- |
| 429/5xx, backoff e reset | test_delivery_worker, cenário B |
| Timeout já armazenado, sem segundo POST | recovery_tests, A |
| Collector indisponível e retomada sem reinício | recovery_tests, C/F |
| Reinicialização inflight antes de reenvio | recovery_tests, D, e restart_outage_test |
| UUID/data incompatível, sem confirmação/reenvio | recovery_tests, G (10 respostas), test_interscity_client (24 consultas) |
| Rollback de cursor e remoção | Cenários de confirmação após HTTP 201 e após consulta; test_telemetry_queue |
| blocked preservado, próximo elegível entregue | test_delivery_worker, C |
| Modo direto SIM-08 | test_interscity_gateway --mqtt: deduplicação, consulta e reenvio único; test_cli --mqtt: reconexão |
| Persistência e recepção durante HTTP lento | test_durable_queue e test_delivery_worker, D/F |

## Contrato `/last` e pendência real

O cliente envia `POST /collector/resources/data/last` com `{"uuids":["<UUID>"]}`.
Espera HTTP 200 com `resources[0].id` textual igual ao UUID solicitado e séries
em `capabilities`, cujos elementos possuem `date`. As datas devem ser UTC
completas, `YYYY-MM-DDTHH:MM:SSZ` ou `YYYY-MM-DDTHH:MM:SS.dígitosZ`. Só depois
se reduzem aos 19 caracteres comparados. Offsets e ausência de Z são rejeitados.
Recursos vazios/séries vazias podem indicar ausência; HTTP 404 só indica ausência
com objeto JSON cujo error é exatamente `Resource not found`. Outros formatos
são erros de consulta, não autorização de reenvio.

**Pendente:** compatibilidade com respostas reais da instância UFMA. Nenhuma
requisição real foi executada nesta rodada. Os mocks comprovam o contrato do
cliente, não o formato atual do serviço remoto.

Procedimento manual proposto, a executar pelo integrante autorizado somente
para consulta, escolhendo UUID real existente no mapa e com amostras conhecidas:

```sh
INTERSCITY_URL='https://cidadesinteligentes.lsdi.ufma.br/interscity_lh'
UUID='<substituir por UUID canônico do mapa>'
curl --silent --show-error --connect-timeout 3 --max-time 10 \
  --request POST "$INTERSCITY_URL/collector/resources/data/last" \
  --header 'Content-Type: application/json' \
  --data "{\"uuids\":[\"$UUID\"]}" \
  --write-out '\nHTTP %{http_code}\n'
```

Embora use POST, esse endpoint é uma consulta. Não chamar `/adaptor/.../data`.
Conferir status, id, tipos das séries e datas Z/com fração. Resposta vazia não
comprova compatibilidade de uma resposta positiva. Registrar somente trecho
mínimo necessário e o resultado observado. Não aceitar offsets pelo prefixo.

## Garantias e limitações

- A proteção começa no COMMIT SQLite, sujeita à durabilidade do armazenamento.
- PUBACK MQTT não é atômico com COMMIT. Sessão MQTT limpa pode perder publicações
  durante indisponibilidade/desconexão do Gateway ou broker.
- Não há exactly-once: respostas ambíguas e consistência eventual podem duplicar.
- `/last` não prova o conteúdo integral de cada amostra; igualdade é identidade
  aproximada. HTTP 201 não garante persistência definitiva no Collector.
- Cursor ignora novas entradas com timestamp menor/igual ao confirmado; não
  atende mensagens fora de ordem.
- blocked exige diagnóstico; não há desbloqueio automático ou descarte silencioso.
- Não há limite de capacidade configurável. Falta de espaço/erro SQLite é fatal;
  crescimento da fila precisa de acompanhamento operacional.
- Há um único consumidor por arquivo. Callbacks/logs e operações DNS/TCP podem
  ser síncronos. Encerramento depende do timeout HTTP e operações SQLite.

## Preparação para Docker Compose (sem implementação)

1. Volume persistente montando o **diretório**, não somente o arquivo SQLite;
   banco e arquivos WAL/SHM precisam de armazenamento local adequado ao WAL.
2. Diretório previamente criado e com leitura/escrita/criação para o UID/GID
   do Gateway, inclusive após recriação do contêiner.
3. Caminho configurado explicitamente, por exemplo `--queue-db /data/telemetry.db`.
   Não usar camada efêmera para pendências. Não apagar volume ao recriar serviços.
4. Broker e endpoint InterSCity acessíveis da rede do contêiner; localhost
   representa o próprio contêiner. Definir endereços conforme o Compose futuro.
5. Tempo de parada compatível com timeout HTTP e confirmação SQLite, permitindo
   SIGTERM/join antes de encerramento forçado. Manter o destino dos UUIDs ao reiniciar.

Não foram criados Dockerfiles nem alterado Compose.

## Proposta de ajuste do TAP (pendente de aprovação)

Proposta baseada na restrição informada pela equipe; o PDF original não foi
localizado no repositório nesta inspeção e não foi editado:

> É vedado manter banco próprio como repositório definitivo ou histórico de
> telemetrias. Admite-se SQLite local exclusivamente como fila temporária durável
> no IoT Gateway, para suportar indisponibilidade, retentativas e reinicialização.
> Registros confirmados são removidos; cursores de deduplicação e registros
> bloqueados para diagnóstico podem permanecer. O SQLite não substitui o
> InterSCity, que continua sendo o destino definitivo e middleware consultado
> pelo backend.

A SIM-09 foi incluída em resposta ao requisito de tolerância a falhas solicitado
pelo professor. A exceção proposta formaliza a função de buffer de entrega,
sem introduzir banco histórico, dashboard ou novos serviços. Aprovação dessa
atualização do TAP e demonstração em ambiente real permanecem com a equipe.


## Parecer e pendências para encerramento

A validação local sustenta o encerramento técnico da SIM-09, sujeito à revisão
da equipe: persistência, reinício e recuperação automática foram demonstrados
no mesmo cenário, sem perda das cinco mensagens já persistidas e sem alterações
arquiteturais. Não equivale a homologação da instância real ou do futuro deploy.

Pendências explícitas: conferir formato real do Collector em consulta autorizada;
repetir backend com libmicrohttpd; repetir o caso de locale quando disponível;
aprovar a exceção proposta no TAP; configurar volume/permissões/rede/parada no
Compose futuro e demonstrar reinício com esse volume. Nenhum staging, commit,
push, merge, Docker ou dashboard foi realizado nesta etapa.
