# SIM-08: tolerância a falhas no IoT Gateway

Escopo: o IoT Gateway (`iot-gateway/`) passa a tratar três falhas do caminho
MQTT → InterSCity: queda do broker, duplicatas e respostas ambíguas do envio HTTP.
Os demais componentes (replayers, backend, InterSCity) não foram alterados.

## Comportamento

| Situação | Antes (SIM-05) | Agora (SIM-08) |
|---|---|---|
| Broker cai depois de conectar | Gateway encerra com código 1 | Gateway continua vivo; reconecta com pausa de 1 s, 2 s, 4 s… (teto de 30 s) e reassina `smart-industry/machines/+/telemetry` |
| Falha ao conectar na inicialização | Encerra com código 1 | Igual (sem mudança) |
| Mesma amostra chega de novo (mesma máquina, timestamp igual ou anterior ao último enviado) | Enviada de novo | Ignorada, com linha `ignorada` no stdout |
| HTTP 201 do Adaptor | Enviada | Enviada (sem mudança) |
| Outro status HTTP (ex.: 500) | Falha, sem reenvio | Igual: falha definitiva, sem reenvio |
| Timeout ou erro de rede no POST (resposta ambígua) | Falha, sem reenvio | Espera 2 s, consulta o Data Collector. Se a amostra estiver lá, conta como enviada e não reenvia. Se não estiver, reenvia **uma vez**. Se a consulta falhar, não reenvia |

A deduplicação é feita por máquina, em memória, e vale só para a execução atual.
Ao reiniciar o Gateway, a memória zera.

## Componentes alterados

- `src/main.c`: reconexão com pausa crescente, deduplicação, regra de consulta
  antes do reenvio, novos contadores no resumo.
- `src/interscity_client.c` e `include/interscity_client.h`: consulta
  `POST /collector/resources/data/last` (`interscity_client_last_timestamp`) e
  classificação de respostas ambíguas (`interscity_client_is_ambiguous`).
  O POST de publicação continua sem nenhuma repetição interna.
- `Makefile`: `json-c` também no cliente HTTP.
- `src/mqtt_subscriber.c` (biblioteca): **sem alteração**. Ela continua sem
  reconexão automática; a reconexão fica no `main.c`. Por isso os testes
  `test_mqtt_subscriber.c` e o caso `disconnect` do `test_mqtt_integration.py`
  continuam valendo sem mudança.

## Constantes

| Nome | Valor | Onde |
|---|---|---|
| `RECONNECT_FIRST_MS` | 1000 ms | `main.c` |
| `RECONNECT_MAX_MS` | 30000 ms | `main.c` |
| `CONSULT_WAIT_MS` | 2000 ms | `main.c` |
| `RESPONSE_LIMIT` (resposta de consulta) | 65536 bytes | `interscity_client.c` |

## Limitações conhecidas

1. **Deduplicação só em memória.** Reiniciar o Gateway pode reenviar amostras
   já entregues.
2. **Janela de 2 s.** Se o Data Collector armazenar a amostra depois desses 2 s,
   o Gateway reenvia e pode gerar uma duplicata. A janela é fixa no código.
3. **Consulta por "último valor".** A verificação é `última data armazenada >=
   timestamp da amostra`. Se a máquina já tiver uma amostra mais nova (de uma
   execução anterior), a amostra perdida pode parecer armazenada. Não há como
   verificar a presença exata com o endpoint `/last`.
4. **Amostras fora de ordem são ignoradas.** Uma amostra com timestamp menor ou
   igual ao último enviado para a mesma máquina é tratada como duplicata, mesmo
   que seja válida.
5. **Perda durante a queda do broker.** A sessão MQTT continua com
   `clean session = true`: mensagens publicadas enquanto o Gateway estava
   desconectado não são entregues. Sessão persistente é uma etapa separada.
6. **Sem autenticação** e sem TLS no MQTT (igual às etapas anteriores).
7. **Testes contra InterSCity falso.** Nenhum teste deste pacote acessa a
   instância real do LSDi.

## Verificação

Executada pelo autor em ambiente WSL, com o Gateway compilado pelo `make`:

- `make test`: passou (inclui `test_interscity_client`, `test_cli` e
  `test_interscity_gateway` sem broker).
- `make test-mqtt`: passou (inclui `test_cli --mqtt`, com o modo
  `network-error` verificando reconexão, e `test_interscity_gateway --mqtt`
  com 7 publicações conferidas: duplicata ignorada, timeout confirmado pelo
  coletor, timeout com reenvio único, 500 sem reenvio e SIGTERM com código 0).

O mock do InterSCity responde lento em 4 s nos sufixos de teste `999` e `997`,
com timeout do cliente em 2 s. A folga de 4 s foi escolhida porque, com 2 s, o
teste viu uma resposta em 1957 ms.

## Como rodar

```bash
cd iot-gateway
make clean && make
make test
make test-mqtt
```

`make test-mqtt` exige o Mosquitto instalado (`mosquitto` e `mosquitto_pub`).
