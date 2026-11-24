# Demonstração presencial no Arch Linux

Verificação em 10/11/2026. Execute os blocos de shell em Bash e a partir da raiz
do repositório. Este documento prepara a execução; nenhum envio real foi feito.

## Resultado da preparação

**Notebook: PARCIALMENTE PRONTO.** O fluxo MQTT local está pronto e os sete
alvos passaram. O fluxo completo continua pendente: as tentativas TCP 443 ao
InterSCity expiraram antes de TLS. Consultas de dados persistidos também estão
pendentes de conectividade. Nenhuma telemetria foi enviada à instância real.

- Branch `main`, commit `8afab5ddf06299d7eb295430eb0d8d3af1d5b1ec`, merge do PR #5.
- `origin`: https://github.com/RenatoMuniz20/smart-industry-monitor.git.
- Fetch concluído; `HEAD...origin/main`: 0 commits de cada lado. Nenhum pull necessário.
- Na inspeção inicial anterior, árvore limpa. Nesta rodada, somente este roteiro
  já estava não rastreado; nenhuma alteração de código foi feita.
- Única alteração de documentação desta preparação: este arquivo. Sem staging/commit.
- Porta 1883 livre na inspeção; `systemctl is-active mosquitto`: `inactive`.
  Confira novamente no dia. Não foi habilitado serviço nem instalado pacote.

| Dependência | Disponibilidade observada |
| --- | --- |
| GCC | 16.2.1 |
| Make | 4.4.1 |
| pkg-config (pkgconf) | 3.0.10 |
| Bash | 5.3.20 (launcher exige >=5.1) |
| json-c | 0.19, encontrada por pkg-config |
| libcurl / curl | 8.23.0, encontrada por pkg-config; OpenSSL 3.6.5 e HTTPS |
| Mosquitto, pub/sub, libmosquitto | Pacote 2.1.2-2; libmosquitto 2.1.2 via pkg-config; header /usr/include/mosquitto.h presente |
| Python | 3.14.8 |
| jq | 1.8.2 |
| CA | ca-certificates e utils 20240618-1; Mozilla 3.131.1-1 |

Bundle CA disponível em `/etc/ssl/certs/ca-certificates.crt`. Não se verificou o
certificado remoto: a conexão falhou antes do handshake. Não usar `curl -k`.

A instalação do Mosquitto foi confirmada nesta rodada; não é necessário instalar
pacotes adicionais nem usar sudo. Os três executáveis estão em `/usr/bin`.

```bash
pkg-config --modversion libmosquitto json-c libcurl
# 2.1.2 / 0.19 / 8.23.0
```

## Compilação e testes

| Alvo solicitado | Resultado nesta preparação |
| --- | --- |
| dataset-replayer all | PASS, exit 0; binários já atualizados |
| dataset-replayer test | PASS, exit 0; caso de locale alternativo pulado |
| dataset-replayer test-mqtt | PASS, exit 0 |
| iot-gateway all | PASS, exit 0; binários já atualizados |
| iot-gateway test | PASS, exit 0 |
| iot-gateway test-mqtt | PASS, exit 0 |
| iot-gateway test-e2e | PASS, exit 0; dez produtores e 30 telemetrias CSV |

Na primeira tentativa desta rodada, o sandbox impediu a criação de sockets com
`PermissionError: Operation not permitted`. Os quatro alvos que abrem sockets
foram reexecutados fora do sandbox e passaram, sem sudo. Isso foi uma restrição
do ambiente do agente, não falha dos componentes. Nenhum warning apareceu nas
saídas; `all` não recompilou arquivos porque os binários já estavam atualizados.

Evidência E2E observada:

```text
PASS: 10 produtores simultâneos, 30 publicações confirmadas, 30 telemetrias CSV processadas
CONTAGEM POR ID: {"1": 3, "2": 3, "3": 3, "6": 3, "7": 3, "8": 3, "9": 3, "11": 3, "12": 3, "13": 3}
PASS: válida/inválida/válida; 32 válidas no total; SIGTERM=0; filhos coletados
```

As 32 válidas incluem duas sondas adicionais do teste de continuidade; o replay
CSV produziu exatamente 30 nesta execução. O JSON inválido foi rejeitado.
Broker temporário encerrado, filhos coletados. Logs individuais em
`/tmp/sim-<módulo>-<alvo>.log`; códigos dos quatro alvos reexecutados em
`/tmp/sim-tests-unrestricted.json`. Esses arquivos são temporários.

Testes isolados compilados e aprovados: csv_reader, telemetry, telemetry_json,
gateway_telemetry, interscity_mapping, interscity_payload e cliente HTTP
(7 requisições locais: 201, 400, 500, timeout, porta fechada e reuso).
Sem warnings nos módulos compilados. telemetry_json pulou o caso de locale
numérico alternativo por indisponibilidade do locale.

Na primeira invocação manual dos testes CSV e mapping, o diretório de trabalho
era a raiz e as fixtures relativas não foram encontradas. Reexecutados no
diretório de cada módulo, ambos passaram. Não foi defeito do código ou do Arch.
Os comandos `make -C` abaixo já escolhem o diretório correto.

Para repetir a verificação antes da apresentação, executar:

```bash
make -C dataset-replayer all
make -C dataset-replayer test
make -C dataset-replayer test-mqtt
make -C iot-gateway all
make -C iot-gateway test
make -C iot-gateway test-mqtt
make -C iot-gateway test-e2e
```

Testes HTTP usam somente loopback. Testes MQTT usam brokers temporários em portas
dinâmicas, não a instância compartilhada. Nenhum código foi modificado.

## Inspeção SIM-05 e conectividade

Foram lidos README raiz, README do Gateway, SIM-05, main.c, os três módulos
InterSCity, machines.conf e o script de cadastro. O script não foi executado.
O mapping passou no teste que confere os dez IDs do arquivo do repositório:
1, 2, 3, 6, 7, 8, 9, 11, 12, 13. UUIDs são canônicos, únicos e sem CRLF.
Os valores completos estão em `config/interscity/machines.conf` e na tabela da SIM-05.
Sua existência remota continua pendente, não foi inferida a partir do arquivo.

O Gateway valida antes de encaminhar, procura o UUID e monta capabilities fixas.
Estado e predição são sempre enviados; medições null são omitidas; power não é
enviado. Timestamp histórico é preservado, com separador T. libcurl faz POST JSON
em `/adaptor/resources/{uuid}/data`, reutiliza conexão, mantém verificação TLS
padrão e aceita apenas HTTP 201 como sucesso. Não segue redirecionamentos nem
repete POST. Timeout HTTP padrão 5000 ms, configurável de 1 a 60000; conexão
também recebe esse limite. MQTT tem timeout separado, 1 a 5000 ms.

Logs distinguem telemetria inválida, UUID ausente, payload inválido, falha de
rede/timeout e status HTTP. O resumo conta enviadas/falhas/sem mapeamento ao
encerrar. Falha HTTP não encerra o processo; exit 0 não prova sucesso dos envios.

Na rodada anterior, as quatro capabilities, dez recursos e dez Collectors
expiraram antes de TLS. Nesta rodada, um GET da capability sim_operating_state
voltou a expirar na conexão TCP, com timeout de conexão 5 s/total 15 s:

- DNS: resolve para `200.137.134.102` (alias www.lsdi.ufma.br).
- Rota: via `192.168.1.1`, interface `wlan0`; a existência da rota não prova alcance.
- TCP 443: curl 28 após aproximadamente 5 s; socket Python independente também
  expirou em 5 s. Nenhum TCP conectado, handshake TLS ou resposta HTTP observado.
- Certificado: não avaliado; não há evidência de erro de certificado.
- API: não avaliada; não confundir ausência de resposta com HTTP 4xx/5xx.
- Controle HTTPS `https://archlinux.org/`: HTTP 200, curl exit 0, verificação TLS 0.
  Há acesso HTTPS externo funcional, mas isso não prova acesso ao destino UFMA.

O sandbox inicialmente causou curl 6 (DNS bloqueado); fora dele o DNS funcionou.
O timeout ao destino UFMA persistiu fora do sandbox. Possíveis causas incluem
filtro na rede de origem/destino, rota intermediária ou serviço de destino sem
resposta. Não é possível concluir que o servidor esteja fora do ar. Próximo
passo: repetir o GET em outra rede, como hotspot, e comparar; solicitar diagnóstico
ao LSDi com horário, IP de destino e erro, se necessário. Não alterar TLS.
Não foram repetidas dezenas de consultas pois a falha ocorre antes de HTTP.

## Consultas antes e depois (somente leitura)

Abra Bash na raiz:

```bash
cd /home/lgfas/projects/ufma/sistemas-distribuidos/smart-industry-monitor
export INTERSCITY_URL=https://cidadesinteligentes.lsdi.ufma.br/interscity_lh
getent ahosts cidadesinteligentes.lsdi.ufma.br
curl -v --connect-timeout 5 --max-time 20 \
  "$INTERSCITY_URL/catalog/capabilities/sim_operating_state"
```

Verifique todas as capabilities e os UUIDs, sem recadastro:

```bash
for cap in sim_motor_temperature sim_current sim_operating_state sim_failure_within_24h; do
  curl --fail-with-body -sS --connect-timeout 5 --max-time 20 \
    "$INTERSCITY_URL/catalog/capabilities/$cap" || break
  printf '\n'
done
while read -r id uuid; do
  [[ -z "$id" || "$id" == \#* ]] && continue
  printf 'machine_id=%s UUID=%s\n' "$id" "$uuid"
  curl --fail-with-body -sS --connect-timeout 5 --max-time 20 \
    "$INTERSCITY_URL/catalog/resources/$uuid" |
    jq -e '.data | if type == "string" then fromjson else . end |
      {uuid, description, capabilities}'
done < config/interscity/machines.conf
```

Conferir descrição `Smart Industry Monitor - <tipo> - machine_id <id>`, UUID e
as quatro capabilities de cada recurso. Interromper a preparação se divergirem.

O Collector usa **POST para consulta**, sem criar amostras. Não confundir com
POST no Adaptor. Antes da demonstração, salvar respostas completas em /tmp:

```bash
snapshot_collector() {
  local destino="$1" id uuid code
  mkdir -p "$destino"
  while read -r id uuid; do
    [[ -z "$id" || "$id" == \#* ]] && continue
    code=$(curl -sS --connect-timeout 5 --max-time 20 -X POST \
      -o "$destino/$id.json" -w '%{http_code}' \
      "$INTERSCITY_URL/collector/resources/$uuid/data") || return 1
    printf '%s %s\n' "$id" "$code" >> "$destino/status.txt"
    [[ "$code" == 200 ]] || {
      cat "$destino/$id.json"
      echo "Collector retornou $code para $id; investigar antes de contar."
      return 1
    }
    jq . "$destino/$id.json" || return 1
  done < config/interscity/machines.conf
}
ANTES=$(mktemp -d /tmp/sim-antes-XXXXXX)
snapshot_collector "$ANTES"
```

Um 404 pode significar recurso ainda sem dados conforme SIM-05; conferir Catalog
e corpo antes de assumir zero. Nunca transformar timeout/erro em contagem zero.
Como a instância não respondeu, conferir o esquema retornado antes de usar o
filtro abaixo. Ele exige explicitamente `resources[].capabilities` e array da
capability; uma estrutura diferente resulta em erro, não em zero silencioso.

```bash
contagem_estado() {
  jq -e '[.resources[] | .capabilities.sim_operating_state |
    if type == "array" then length else error("capability ausente ou esquema diferente") end] |
    if length > 0 then add else error("nenhum recurso retornado") end' "$1"
}
for id in 1 2 3 6 7 8 9 11 12 13; do
  printf '%s ' "$id"
  contagem_estado "$ANTES/$id.json"
done
```

## Três terminais — execução real somente após autorização

Cada replay acrescenta amostras, inclusive duplicatas históricas. O bloco dos
produtores com envio ao InterSCity NÃO foi executado nesta preparação. Usar logs novos por sessão.
Em cada terminal, entrar na raiz com o `cd` acima e iniciar Bash.

Terminal 1 — conferir porta e iniciar um único broker:

```bash
ss -ltnp 'sport = :1883'
systemctl is-active mosquitto
if [[ -z "$(ss -H -ltn 'sport = :1883')" ]]; then
  mosquitto -c config/mosquitto/mosquitto.conf -v 2>&1 | tee /tmp/sim-broker.log
else
  echo 'Porta 1883 ocupada: identifique o processo antes de continuar.'
fi
```

Se houver broker de serviço, escolher conscientemente usá-lo após conferir a
configuração ou pará-lo (`sudo systemctl stop mosquitto`) antes do broker do
projeto. Não iniciar outro na mesma porta. O projeto escuta só 127.0.0.1, aceita
anônimo e não persiste mensagens. Não alterar o serviço automaticamente.

Terminal 2 — Gateway com HTTPS, apenas quando a sessão real estiver autorizada:

```bash
set -o pipefail
iot-gateway/build/iot-gateway --host 127.0.0.1 --port 1883 \
  --interscity-url https://cidadesinteligentes.lsdi.ufma.br/interscity_lh \
  --interscity-map config/interscity/machines.conf \
  --interscity-timeout-ms 5000 2>&1 | tee /tmp/sim-gateway.log
```

Aguardar `Gateway pronto: smart-industry/machines/+/telemetry (QoS 1).`
Isso confirma CONNACK e SUBACK QoS 1. A linha de InterSCity habilitado deve
informar dez máquinas. Não iniciar produtores se houver erro.

Terminal 3 — após prontidão e autorização de envio:

```bash
set -o pipefail
LIMIT=3 INTERVAL_MS=1000 MQTT_HOST=127.0.0.1 MQTT_PORT=1883 \
  bash ./dataset-replayer/scripts/run-ten-machines.sh 2>&1 | tee /tmp/sim-replayers.log
```

O launcher exibe dez IDs/PIDs, inicia processos C concorrentes e espera todos.
Eles duram poucos segundos; mostre os PIDs no log ou, durante o replay, use no
terminal 1 uma segunda aba `pgrep -af '/build/dataset-replayer'`.
Esperado: dez linhas `Publicações confirmadas: 3` e conclusão das dez máquinas.
Broker com `-v` mostra PUBLISH/PUBACK; Gateway deve registrar 30 válidas e 30 HTTP 201.

Ainda no terminal 3, depois de aguardar todos os envios HTTP:

```bash
rg -c 'Publicações confirmadas: 3' /tmp/sim-replayers.log
rg -c '^Telemetria válida:' /tmp/sim-gateway.log
rg -c 'enviada \(HTTP 201,' /tmp/sim-gateway.log
sed -n 's/^Telemetria válida: //p' /tmp/sim-gateway.log |
  jq -s 'group_by(.machine_id) | map({machine_id: .[0].machine_id, quantidade: length})'
```

Esperado 10, 30, 30 e três mensagens para cada ID. QoS 1 pode duplicar mensagens;
contagens maiores devem ser explicadas, não escondidas. Não repetir envio para
resolver timeout HTTP: ele pode ter criado a amostra mesmo sem resposta.

Depois, no mesmo Bash das consultas, sem perder ANTES e a função:

```bash
DEPOIS=$(mktemp -d /tmp/sim-depois-XXXXXX)
snapshot_collector "$DEPOIS"
for id in 1 2 3 6 7 8 9 11 12 13; do
  a=$(contagem_estado "$ANTES/$id.json") || break
  d=$(contagem_estado "$DEPOIS/$id.json") || break
  printf 'machine_id=%s antes=%s depois=%s delta=%s\n' "$id" "$a" "$d" "$((d-a))"
done
```

Esperado +3 por máquina e +30 no total em sim_operating_state, sempre presente.
O Collector é assíncrono: se necessário aguarde alguns segundos e repita apenas
a consulta, usando outro diretório DEPOIS. /data/last não comprova incremento de
contagem e os timestamps são históricos. Se houver paginação/limite na resposta,
obter todas as páginas antes de comparar. Compare também valores/datas com o CSV.

Encerrar: produtores concluem sozinhos; Ctrl+C no Gateway depois de esvaziar os
envios, conferir `30 enviadas, 0 falhas, 0 sem mapeamento` e encerramento normal;
por último Ctrl+C no broker. Sinal durante HTTP pode levar até o timeout.
Conferir novamente `ss -ltnp 'sport = :1883'`. Não usar kill -9.

## Plano alternativo local, sem internet

Validado nesta rodada, sem dependência de internet:

```bash
make -C iot-gateway test-e2e
```

Esse alvo usa dez produtores reais, broker temporário em porta dinâmica, espera
prontidão, verifica dez processos simultâneos, 30 confirmações MQTT, JSON contra
CSV e descarte de JSON inválido; encerra seus próprios processos. Não exige rede
externa e não comprova persistência InterSCity.

Alternativa manual: mesmos terminais 1 e 3, mas terminal 2 com:

```bash
set -o pipefail
iot-gateway/build/iot-gateway --host 127.0.0.1 --port 1883 \
  2>&1 | tee /tmp/sim-gateway-local.log
```

Aguardar Gateway pronto antes do terminal 3. Conferir 30 válidas e três por ID no
log local, com os mesmos filtros acima. Sem opções InterSCity não há HTTP.
Não injetar mensagens de teste com Gateway remoto habilitado.

## Sequência da apresentação e diagnóstico

1. Explicar CSV → dez processos C → MQTT QoS 1 → Gateway → HTTPS → InterSCity → Collector.
2. Mostrar dez PIDs e IDs; processos independentes, publicação concorrente.
3. Mostrar PUBLISH/PUBACK no broker e confirmações dos produtores.
4. Mostrar JSON validado pelo Gateway e três mensagens por máquina.
5. Mostrar HTTP 201 e tempo de cada envio, quando o fluxo remoto estiver autorizado.
6. Consultar Collector, comparar estado antes/depois, valores e datas históricas.
7. Explicar confirmações: PUBACK confirma broker; 201 confirma aceitação no
   Adaptor, não persistência; consulta ao Collector observa persistência.

| Problema | Diagnóstico e ação |
| --- | --- |
| Não compila | pkg-config das três libs; mosquitto.h requer pacote mosquitto |
| Address already in use | ss na 1883; identificar serviço/processo; manter apenas um broker |
| Gateway não pronto | conferir broker, porta, client-id único, CONNACK/SUBACK; reiniciar explicitamente após corrigir |
| Nenhuma mensagem | iniciar Gateway antes dos produtores; conferir host/porta/CSV e logs dos dez PIDs |
| Erro de mapping | conferir arquivo LF, dez pares, UUIDs e descrição remota; não recadastrar |
| curl 6 | falha DNS: getent, conexão da rede |
| curl 28 / HTTP 000 | timeout de rota/TCP/servidor; testar outra rede e avisar LSDi; usar plano local |
| curl 60 | data/hora e CA/certificado; corrigir causa sem desabilitar TLS |
| HTTP diferente de 201 | guardar diagnóstico; não repetir POST cegamente; consultar Collector |
| 201 sem incremento | aguardar processamento; conferir UUID/capability, resposta completa e erros do serviço com LSDi |
| Contagens diferentes | QoS 1, sessões anteriores, instância compartilhada, limite da consulta ou falha HTTP |

Sem reconexão automática, reenvio, deduplicação ou fila durável no Gateway.
HTTP é síncrono numa thread; rede lenta pode acumular mensagens MQTT.
power é null no dataset; failure_within_24h é predição, não falha medida.

Pendências para a demonstração completa: resolver conectividade, verificar
TLS/APIs/UUIDs, validar esquema/contagem do Collector e obter autorização para
ensaio remoto com 30 POSTs. O fluxo MQTT local e o plano alternativo E2E estão
validados. Antes da apresentação, conferir novamente a porta 1883 e repetir E2E.
A pendência de merge no final de SIM-05 é histórica: o PR #5 já está na main.
