# SIM-09 — Fila durável e recuperação automática

Validação integrada e proposta de encerramento: [SIM-09.5](sim-09-validacao-final.md).

## Escopo e execução

O modo opcional `--queue-db` desacopla recepção e HTTP:

`MQTT → validação/mapeamento/payload → SQLite → worker HTTP → InterSCity`

A thread principal e o único worker usam conexões SQLite separadas, WAL,
synchronous=FULL e busy timeout de 1000 ms. O cliente libcurl é criado antes
da thread, utilizado exclusivamente pelo worker e destruído após join. Nenhuma
transação permanece aberta durante HTTP ou esperas. Sem `--queue-db`, continua
o fluxo direto da SIM-08.

```sh
# O diretório data deve existir previamente.
iot-gateway/build/iot-gateway \
  --interscity-url http://127.0.0.1:8000 \
  --interscity-map config/interscity/machines.conf \
  --queue-db ./data/telemetry.db
```

## Estados e decisões

- `pending`: elegível por ordem de inserção; start_attempt torna inflight e
  incrementa attempt_count antes do POST do UUID/JSON persistidos.
- `inflight`: tentativa iniciada, inclusive resultado ambíguo. Sobrevive ao
  reinício, sem reset do contador. Bloqueia o envio de registros posteriores
  enquanto é reconciliada; MQTT continua persistindo.
- `blocked`: recusa HTTP definitiva; registro e last_error permanecem no banco.
  O worker pode processar os próximos elegíveis.

HTTP 201 confirma cursor e remoção em uma transação. HTTP 429/5xx retorna a
pending com backoff de 1, 2, 4, 8, 16 e 30 s (teto). Timeout/rede mantém inflight:
aguarda 2 s e consulta `/collector/resources/data/last`, com o mesmo cliente.

| Consulta | Ação |
| --- | --- |
| Timestamp igual | Confirma atomicamente; conta recuperação, sem outro POST |
| Ausente ou anterior | Segunda consulta após 2 s; duas evidências consecutivas autorizam retry |
| Posterior | Não confirma; autoriza retry e avisa risco de duplicação |
| Consulta malsucedida/inválida | Preserva inflight e consulta novamente com backoff |

Falha de consulta interrompe a sequência de evidências de ausência. Entrega ou
recuperação confirmada reinicia o backoff em 1 s. Falha SQLite continua fatal,
inclusive depois de HTTP 201 ou consulta que identifica timestamp igual; não
há confirmação parcial nem contador de entrega incrementado nesse caso.

SIGTERM interrompe esperas. Uma requisição ativa pode terminar/atingir timeout;
confirmações válidas são finalizadas e pendências não resolvidas preservadas.
O fluxo principal aguarda pthread_join antes de liberar o cliente HTTP.

## Evidências reproduzíveis

`tests/test_delivery_worker.py` usa Mosquitto temporário, SQLite temporário e
HTTP falso controlado por eventos, com contagens distintas de publicações e
consultas. Não utiliza a instância real da UFMA.

Na execução da SIM-09.4 foi observado, sem reiniciar o Gateway:

1. Perda real da conexão do POST ao Adaptor; cabeça mantida inflight.
2. Cinco registros persistidos, sendo quatro posteriores pending.
3. Três consultas ao Collector recebendo HTTP 503, com backoff observado.
4. Serviço falso restaurado mantendo o mesmo PID do Gateway.
5. Duas consultas de ausência após restauração; reenvio autorizado.
6. Cinco entregas HTTP 201 e fila vazia; cursor em `2024-01-01T00:04:00`.
   Foram seis POSTs de telemetria: o inicial ambíguo e cinco confirmados.

Também foram verificados: timeout com amostra armazenada (uma consulta e nenhum
POST repetido da amostra), ausência e timestamp anterior (duas consultas,
attempt_count=2 antes do sucesso), reinício com inflight, timestamp posterior
sem exclusão antecipada, consultas 503, dez respostas malformadas/incompatíveis,
consistência eventual com falha intermediária, SIGTERM durante consulta lenta
e rollback da confirmação por consulta. Os casos HTTP 201, 429/5xx, blocked,
deduplicação, recepção durante HTTP lento e rollback após 201 permanecem cobertos.

Comandos para reproduzir:

```sh
make -C iot-gateway clean
make -C iot-gateway all
make -C iot-gateway test
make -C iot-gateway test-mqtt
make -C iot-gateway test-e2e
git diff --check
```

Resultados da rodada SIM-09.4 (histórico; a rodada final está no documento SIM-09.5):

| Validação | Resultado |
| --- | --- |
| clean / all | Sucesso, sem warnings GCC |
| test | Passou: módulos locais, SQLite, cliente HTTP e CLI |
| test-mqtt | Passou: comunicação, SIM-08, persistência e recuperação |
| test-e2e | Passou: dez produtores, 30 telemetrias CSV e 32 válidas incluindo continuidade |
| ASan + UBSan, detect_leaks=1 | Worker/reconciliação e persistência MQTT passaram sem diagnósticos |
| git diff --check | Sem problemas de whitespace |

Na rodada SIM-09.4, nenhuma suíte então solicitada ficou em SKIP.
A SIM-09.5 registra separadamente os testes adicionais e suas limitações de ambiente. Os testes HTTP utilizaram somente
servidores falsos locais. O README detalha os módulos, interfaces e a execução
isolada dos testes.

## Validação da resposta do Collector

O recurso deve trazer `id` textual exatamente igual ao UUID solicitado. A data
precisa ser completa, em UTC: `YYYY-MM-DDTHH:MM:SSZ` ou
`YYYY-MM-DDTHH:MM:SS.dígitosZ`. Fração vazia, sufixo inválido e offsets são
rejeitados, sem comparação de horários de fusos diferentes por prefixo.
Somente uma resposta válida é reduzida aos segundos para comparação.

As 24 consultas dos testes do cliente verificam explicitamente
`INTERSCITY_CLIENT_RESPONSE`,
found=0 e saída preservada para respostas incompatíveis. No worker, UUID
incorreto, ID ausente/não textual e data inválida mantêm inflight/cursor intactos,
sem POST de reenvio, até uma consulta válida posterior. Não foi alterada a
arquitetura nem a política de backoff.

## Limites

`/last` informa apenas o último timestamp de capabilities conhecidas, não a
presença exata de todas as amostras/capabilities. Igualdade é identidade
aproximada e não comprova integralmente seu conteúdo. Uma data posterior não
comprova uma anterior. Consistência eventual além da janela pode causar reenvios
e duplicatas: **não há exactly-once**. A política do cursor ignora novas entradas
com timestamp menor ou igual ao último confirmado e não atende dados fora de
ordem.

PUBACK MQTT não é atômico com COMMIT SQLite: mensagens não persistidas podem ser
perdidas. HTTP 201 não comprova armazenamento pelo Data Collector. Permanecem
limites de DNS/TCP síncrono, timeout HTTP, busy timeout SQLite e logs síncronos.
A URL de reinício deve corresponder ao InterSCity dos UUIDs persistidos. Apenas
um Gateway/worker deve consumir o arquivo. Não há limpeza automática de blocked,
idempotency keys, múltiplos workers ou alterações no protocolo MQTT.


## Garantias operacionais e implantação futura

A fila protege registros cujo COMMIT SQLite foi concluído, sujeito à durabilidade
do armazenamento. Não garante retenção de publicações feitas com o Gateway
fora do ar: a sessão MQTT é limpa e mensagens durante a desconexão podem ser
perdidas. O ACK MQTT não integra uma transação com o SQLite.

Não existe limite de capacidade configurável nem política automática de descarte.
O crescimento da fila depende de espaço em disco; erros SQLite são fatais e
precisam de diagnóstico. Registros blocked ficam preservados para intervenção,
sem mecanismo automático de desbloqueio. SQLite tem função de fila temporária,
não de repositório histórico substituto do InterSCity.

Na futura containerização, `--queue-db` deverá apontar a um diretório em volume
persistente local adequado ao WAL, com permissões para banco, `-wal` e `-shm`.
O diretório precisa existir e sobreviver a reinícios/recriações do contêiner.
O endpoint InterSCity e o broker devem ser acessíveis pela rede do contêiner.
Procedimento, ressalvas do TAP e pendências reais estão no documento SIM-09.5;
nenhum Dockerfile ou Compose foi implementado nesta etapa.
