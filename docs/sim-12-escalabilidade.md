# SIM-12 — Carga e capacidade do pipeline

## Base, objetivo e escopo

Testes executados em 26/11/2026, branch `main`, HEAD
`e424b253d8dbe10613b55f648307d89e0b1c07ed` (merge da PR #17), working tree inicialmente limpo.
Objetivo: aumentar a frequência MQTT mantendo dez produtores e a arquitetura
aprovada. Não houve alteração de C, Makefiles, Dockerfiles ou arquivos Compose.
Foi criado apenas o orquestrador de teste `scripts/sim12-load.py`, este relatório
e evidências compactas em `docs/evidencias/sim-12/`.

Uma execução válida por cenário, dez processos C independentes, LIMIT=60 por
máquina. IDs 1,2,3,6,7,8,9,11,12,13; mesmas primeiras 60 linhas de cada máquina
no CSV. O contrato atual inclui vibração e rotação (SIM-11).

FAKE_LATENCY_MS=60, FAKE_JITTER_MS=0, FAKE_STORE_DELAY_MS=0,
FAKE_FAIL_RATE=0 e FAKE_MAX_INFLIGHT=0 (sem limite artificial de concorrência).
O atraso de 60 ms é configurado no falso, não uma latência ponta a ponta medida.

## Isolamento e ambiente

Gateway e Backend foram verificados no Compose renderizado **e nos argumentos
dos contêineres** com `http://interscity-fake:8000`. O script fake-stack.sh não
foi utilizado, pois fixa o projeto smart-industry-fake. Nenhum acesso à UFMA.
Cada cenário criou gateway_data e fake_interscity_data novos e comprovou antes
do replay: fila=0, cursores=0, samples do falso=0, requisições/aceites=0.

| Cenário | Projeto exclusivo | Porta falso (loopback) | Porta Backend (loopback) |
| --- | --- | --- | --- |
| A | `sim12-20261126t205424-a` | 18100 | 18180 |
| B | `sim12-20261126t205424-b` | 18101 | 18181 |
| C | `sim12-20261126t205424-c` | 18102 | 18182 |

Os volumes têm nomes `<projeto>_gateway_data` e
`<projeto>_fake_interscity_data`. Ao terminar, somente os contêineres/redes desses
projetos foram removidos com down **sem -v**; todos os volumes foram preservados.
Não se reutilizou smart-industry-fake, sim101-validation, sim102-validation ou
sim103-validation. O piloto de instrumentação também teve seu próprio projeto.

Docker 29.9.2, Compose 5.6.1, kernel 7.2.11-arch1-1,
12 CPUs lógicas e 15.53 GiB disponíveis ao daemon.
Havia 5 contêineres preexistentes ativos, mantidos sem alteração.
O host não foi dedicado nem teve CPU/memória isoladas para o benchmark.

As quatro imagens de aplicação foram compiladas do HEAD uma única vez. B/C
usaram aliases dos mesmos IDs imutáveis de imagem, conferidos em runtime;
a imagem Mosquitto também foi a mesma. IDs estão nos summaries JSON.

## Método de medição

- Prontidão: broker/falso saudáveis e linha Gateway pronto (CONNACK/SUBACK) antes
  de iniciar o replay. Backend health e HTML do dashboard responderam 200.
- Publicações: dez linhas Publicações confirmadas: 60 do launcher/Replayer,
  que somente contam PUBACKs. Não são confirmação de processamento HTTP.
- Recepção/persistência: contagem de Telemetria válida e de eventos persistida
  do Gateway, separada de suas entregas HTTP 201 com confirmação SQLite.
- HTTP: adaptor_requests e adaptor_created de `/__fake/stats`; apenas POSTs ao
  Resource Adaptor, não soma de healthchecks/consultas do Collector.
- Telemetrias armazenadas: 600 pares distintos (uuid,date), 60 por máquina.
  Medições: linhas da tabela samples do falso por capability, incluindo valores
  categóricos. Chaves e valores foram comparados integralmente com o CSV;
  duplicatas por (uuid,capability,date) foram rejeitadas na validação.
- Fila: SELECT somente leitura periódico com total/pending/inflight/blocked e
  cursores. Alvo de 1 s; mediana observada ~1,063 s com o custo dos probes.
  Cada ponto combina leituras separadas, não snapshot atômico entre serviços.
- Recursos: docker stats em stream, por IDs somente dos contêineres do cenário.
  Cadência observada ~0,5 s; frames do CLI não são amostras estatisticamente
  independentes. CPU% é o valor exibido pelo Docker, sem normalizar pelos 12 CPUs;
  MemUsage é convertido em MiB, não uma medição de RSS realizada pelo roteiro.
- Médias de CPU são médias simples dos frames até drenagem; no Replayer podem
  incluir valores zero após término. Todos os máximos são **máximos observados**,
  não picos absolutos. A coleta de recursos inicia após criar o Replayer e não
  cobre integralmente o arranque.
- Duração do replay: State.FinishedAt menos State.StartedAt do contêiner Docker.
  Inclui o launcher, conexão e leitura; não é intervalo entre primeiro/último
  pacote MQTT. Taxa de publicação = 600 / essa duração.
- Duração total: relógio monotônico, imediatamente antes do comando de subida do
  Replayer até observar replay encerrado + fila vazia + 600 HTTP 201. Exclui build
  e inicialização da infraestrutura; inclui criação do Replayer e atraso de
  observação dos probes. Taxa de entrega = 600 / essa duração observada.
- Backend: consulta `/api/machines/latest` aproximadamente a cada 5 s, além das
  verificações inicial/final, representando polling do painel. Todas responderam
  200. Não houve navegador nem carga de renderização JavaScript.
- Sem latências individuais/P50/P95: o timestamp do dataset é histórico, não
  instante de envio/recepção. Não foi usado para inferir latência de rede.

Timeout de drenagem: 180 s por cenário. Não foi atingido. O roteiro interrompe
a sequência caso uma execução não drene ou falhe em integridade/contagem.

## Resultados comparativos

Tempos em segundos e taxas em telemetrias/s. Cada linha representa 600 entradas.

| Cenário | Intervalo ms | Replay (Docker) | Até drenagem (observado) | Publicação média | Entrega média | Fila total máx. observada | Pending máx. observado |
| --- | --- | --- | --- | --- | --- | --- | --- |
| A | 1000 | 59.118 | 60.593 | 10.149 | 9.902 | 10 | 10 |
| B | 500 | 29.618 | 39.318 | 20.258 | 15.260 | 136 | 135 |
| C | 200 | 11.922 | 39.334 | 50.328 | 15.254 | 410 | 409 |

A fila total inclui pending, inflight e blocked; não confundir com apenas o
estado pending. A cadência nominal agregada é 10/20/50 telemetrias/s; a primeira
amostra de cada processo é imediata, e a execução finita tem 59 intervalos.

Em **cada** cenário: 600 PUBACKs, 600 telemetrias recebidas, 600 persistidas,
600 POSTs/HTTP 201 e 600 confirmações SQLite. Zero retentativas, recuperações por
consulta, blocked, UUIDs desconhecidos, erros de validação, erros SQLite ou
reinícios de contêiner. Fila final com zero registros em todos os estados.
Foram 1.800 telemetrias nos três cenários válidos, não contando o piloto excluído.

### Telemetrias versus medições

Cada execução armazenou **3.522 medições no total, distribuídas entre as seis
capabilities, para 600 telemetrias publicadas em cada cenário**. Não são 3.522 mensagens MQTT. Faltam 78 valores em
relação ao máximo de 600 × 6 porque medições numéricas ausentes são omitidas;
`power=null` não gera capability de potência.

| Capability | Registros em cada cenário |
| --- | --- |
| sim_current | 586 |
| sim_failure_within_24h | 600 |
| sim_motor_temperature | 578 |
| sim_operating_state | 600 |
| sim_rpm | 588 |
| sim_vibration_rms | 570 |

Os três conjuntos de dados armazenados tiveram o mesmo SHA-256 e coincidiram
com o conteúdo esperado. O falso confirmou samples_received=samples_stored=3522,
adaptor_requests=adaptor_created=600, sem descartes por UUID ou duplicações.

### CPU e memória dos contêineres

Valores observados durante a fase medida; não representam reservas/limites.

| Cenário | Serviço | CPU média % | CPU máxima observada % | Memória máxima observada MiB | Frames |
| --- | --- | --- | --- | --- | --- |
| A | mosquitto | 0.439 | 2.19 | 10.910 | 117 |
| A | gateway | 4.757 | 5.42 | 10.920 | 117 |
| A | interscity-fake | 6.076 | 30.22 | 25.800 | 117 |
| A | backend | 0.480 | 2.59 | 4.051 | 117 |
| A | replayer | 0.045 | 0.06 | 11.990 | 117 |
| B | mosquitto | 0.473 | 2.19 | 9.238 | 74 |
| B | gateway | 5.231 | 5.94 | 11.350 | 74 |
| B | interscity-fake | 6.308 | 30.48 | 27.800 | 74 |
| B | backend | 0.457 | 2.42 | 3.766 | 74 |
| B | replayer | 0.068 | 0.10 | 12.990 | 74 |
| C | mosquitto | 0.462 | 2.32 | 11.610 | 74 |
| C | gateway | 5.189 | 6.90 | 12.040 | 74 |
| C | interscity-fake | 6.308 | 30.17 | 26.230 | 74 |
| C | backend | 0.455 | 2.39 | 3.254 | 74 |
| C | replayer | 0.070 | 0.25 | 13.270 | 74 |

### Cursores e integridade

Os dez cursores foram iguais nos três cenários e correspondem ao último timestamp
esperado da seleção. PRAGMA integrity_check retornou ok nos bancos do Gateway e
do falso; Gateway manteve journal_mode=wal. Todos os contêineres terminaram com
ExitCode=0 e RestartCount=0 após a coleta, antes do down.

| Máquina | Cursor final em A, B e C |
| --- | --- |
| 1 | 2024-01-01T16:00:00 |
| 2 | 2024-01-01T18:04:00 |
| 3 | 2024-01-01T16:06:00 |
| 6 | 2024-01-01T15:58:00 |
| 7 | 2024-01-01T15:44:00 |
| 8 | 2024-01-01T13:43:00 |
| 9 | 2024-01-01T16:04:00 |
| 11 | 2024-01-01T16:47:00 |
| 12 | 2024-01-01T15:53:00 |
| 13 | 2024-01-01T15:20:00 |

## Interpretação

A entrada de A (~10/s) ficou abaixo da capacidade de saída neste perfil, e a
fila voltou a zero durante o replay, absorvendo os lotes simultâneos das dez
máquinas. B (~20/s) e C (~50/s) excederam temporariamente a saída: o backlog
cresceu para máximos observados de 136 e 410 registros e drenou após o replay.

B/C terminaram a entrega em ~39,3 s, apesar de C terminar de publicar muito antes.
Isso é compatível com o worker HTTP único, que espera cada resposta antes da
próxima entrega. Só o atraso artificial de 60 ms já implica um teto teórico
ideal de 16,67 POSTs/s para esse fluxo serial. A taxa medida de ~15,3/s é coerente
com esse limite mais rede, serialização e transações SQLite; seus custos não
foram isolados. Não há evidência de saturação de CPU nos valores observados.
Não é uma medição da capacidade máxima do InterSCity real.

A concorrência dos dez produtores e o desacoplamento MQTT/SQLite/worker permitem
continuar recebendo enquanto a saída é mais lenta. Não houve evidência de perda:
as contagens e o conteúdo esperado no middleware coincidiram, todos os registros
persistidos foram confirmados e a fila esvaziou. Isso vale para estes ensaios sem
falhas injetadas, não é garantia exactly-once ou de carga sustentada ilimitada.

O teste avalia capacidade com aumento de frequência em arquitetura fixa; não
implementa nem comprova escalabilidade horizontal. Uma entrada sustentada acima
da saída tende a crescer a fila até algum limite de armazenamento, que não foi
medido neste teste finito de 600 mensagens.

## Reprodução e evidências

Da raiz do repositório, com Docker disponível e portas 18100..18102 e
18180..18182 livres:

```sh
python3 -B scripts/sim12-load.py \
  --project-prefix sim12-repeticao-01 \
  --output /tmp/sim12-repeticao-01
```

Escolher prefixo e diretório novos a cada rodada. O script não apaga volumes e
recusa recursos preexistentes. Requer Python 3.11+ (somente biblioteca padrão) e Docker
Compose com suporte aos overrides !reset/!override já existentes no projeto.
`--resume` retoma só cenários ainda não executados e exige o mesmo HEAD/prefixo,
sem repetir nem reutilizar os volumes dos cenários já concluídos.

Evidências em [evidencias/sim-12](evidencias/sim-12/):

- manifest.json: HEAD, ambiente, configuração de coleta e timestamps da rodada.
  O arquivo preservado inclui também `dataset_sha256` e `runner_sha256`; o script
  atual não calcula nem acrescenta esses campos automaticamente. Os IDs das
  imagens estão nos summaries JSON.
- a/b/c-initial.json: estados vazios comprovados antes de publicar.
- a/b/c-summary.json: resultados, recursos, contadores, cursores, checksums e
  eventos de erro; todos com success=true.
- a/b/c-queue.csv e a/b/c-resources.csv: observações temporais compactas.
- a/b/c-excerpts.log: dez confirmações por processo e resumos de encerramento,
  sem armazenar centenas de payloads.
- instrumentation-note.json: registro do piloto excluído e da retomada.

O primeiro piloto A (projeto sim12-20261126t205124-a) foi excluído porque a saída
do Docker stats continha controles ANSI que o leitor JSON inicial não tratava.
O coletor foi corrigido e A repetido em volumes novos. A rodada válida de A foi
preservada quando uma pré-verificação de porta interrompeu a entrada de B, antes
de criar carga/volumes B; portas distintas resolveram a condição e B/C foram
retomados. Foram problemas do roteiro, não do Gateway; não se modificou a aplicação.
Os volumes do piloto também permanecem. Nenhum down -v, wipe ou reset foi usado.

## Limitações e conclusão

Uma execução válida por cenário, sem repetição estatística/intervalos de
confiança e sem ordem randomizada. Host compartilhado com outros contêineres,
cache de imagens/arquivos não controlado e overhead de observação não isolado.
Os tempos de drenagem têm resolução de observação (~1,06 s), por isso diferenças
pequenas entre B e C não são significativas. Recursos são frames do CLI, não
perfis de CPU nem provas de pico absoluto.

HTTP local com atraso artificial não reproduz DNS/TLS, RabbitMQ, armazenamento,
carga ou rede da UFMA. Não foram injetadas falhas de rede, 5xx, reinícios ou falta
de disco nestes cenários; recuperação já possui evidências na SIM-10.3/SIM-09.
Sessão MQTT limpa, PUBACK não atômico com COMMIT, deduplicação por cursor e
possibilidade de duplicação após ambiguidade continuam válidos. Não há limite
configurável para a fila nem garantia exactly-once.

Os critérios da SIM-12 foram atendidos nesta validação: 600 telemetrias em cada
cenário, nenhuma perda observada, middleware com valores esperados e drenagem
mesmo após entrada temporariamente acima da saída. Encerramento sujeito à revisão
da equipe, sem Kubernetes, mudança de arquitetura, commit, push, merge ou PR.
