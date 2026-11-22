# SIM-11 — Vibração e rotação no pipeline

## Objetivo

Levar duas colunas do dataset, `vibration_rms` e `rpm`, do CSV até a API REST,
para que o dashboard (SIM-07) mostre seis medições por máquina em vez de quatro.
As demais colunas não transportadas continuam fora do escopo.

## Contrato de telemetria

O contrato passa de oito para dez propriedades. As duas novas ficam entre
`power` e `state`:

```json
{"machine_id":1,"machine_type":"CNC","timestamp":"2024-01-01 00:00:00",
 "temperature":49.51,"current":5.1,"power":null,"vibration_rms":0.81,"rpm":860.9,
 "state":"IDLE","failure_within_24h":0}
```

| Campo | Origem no CSV | Regra |
| --- | --- | --- |
| `vibration_rms` | `vibration_rms` | Número finito ou `null` quando a célula está vazia (cerca de 4% das linhas) |
| `rpm` | `rpm` | Número finito ou `null` quando a célula está vazia (cerca de 2% das linhas) |

O dataset não informa a unidade da vibração; nenhuma unidade é atribuída.
`rpm` é rotação em rotações por minuto, como o nome da coluna indica.

## InterSCity

| Capability | Origem | Tipo |
| --- | --- | --- |
| `sim_vibration_rms` | `vibration_rms` | `sensor` |
| `sim_rpm` | `rpm` | `sensor` |

Medição ausente omite a capability; zero é enviado, como nas outras medições.
As dez máquinas não precisam ser recadastradas: o Data Collector associa uma
capability ao recurso no primeiro envio (registrado na SIM-05).

As capabilities são criadas por `iot-gateway/scripts/create-interscity-capabilities.sh`:
consulta antes, cria só o que responde 404, interrompe em qualquer resposta
inesperada e nunca apaga nada. O script de cadastro de máquinas passou a conferir
as seis capabilities.

**A criação na instância compartilhada não tem volta** (capabilities não devem ser
apagadas). Ela deve ser feita uma vez, por quem a equipe autorizar, antes do
primeiro envio real com o contrato novo:

```sh
bash iot-gateway/scripts/create-interscity-capabilities.sh --dry-run
bash iot-gateway/scripts/create-interscity-capabilities.sh
```

## Componentes alterados

| Componente | Mudança |
| --- | --- |
| Dataset Replayer | `Telemetry` ganha `vibration_rms` e `rpm`; JSON com dez campos; saída de texto mostra os dois |
| IoT Gateway | Validador exige as dez propriedades; envio inclui `sim_vibration_rms` e `sim_rpm`; log `Telemetria válida` com os dez campos |
| Backend REST | `/api/machines/latest` e `/api/machines/{id}/history` devolvem `vibration_rms` e `rpm`, com as mesmas regras das outras medições (ordem, sem repetição, `null` ou série vazia quando não há dado) |
| Scripts | Novo `create-interscity-capabilities.sh`; `register-interscity-machines.sh` confere seis capabilities |

## Compatibilidade

- Replayer e Gateway precisam ser atualizados juntos. Um Gateway desta versão
  rejeita mensagens de oito campos, e um Gateway anterior rejeita as de dez.
- Pendências antigas na fila SQLite continuam válidas: o corpo já está pronto,
  com as quatro capabilities da época.
- Amostras gravadas no InterSCity antes da SIM-11 não têm vibração nem rotação.
  Para elas, o backend devolve `null` no último valor e série vazia no histórico.

## Verificação

Feita durante a implementação, antes de rodar no ambiente da equipe:

| Verificação | Resultado |
| --- | --- |
| Replayer: `test_csv_reader`, `test_telemetry`, `test_telemetry_json` (compilados com `-Werror`) | Passaram |
| Replayer: `test_cli.sh` | Passou |
| Replayer com o CSV real, máquina 1 | `vibration_rms=0.81 rpm=860.9` na primeira linha |
| Script de capabilities contra um catálogo falso local | Simulação, criação e segunda execução sem duplicar |
| Gateway e Backend | Somente checagem de sintaxe C; dependem de json-c, libmosquitto, libcurl e libmicrohttpd |

A validação completa fica no ambiente da equipe:

```sh
make -C dataset-replayer clean test test-mqtt
make -C iot-gateway clean all test test-mqtt test-e2e
make -C backend clean test
```

O backend passa de 47 para 51 verificações locais (quatro novas no teste da
consulta ao InterSCity).

## Limitações

- Unidade da vibração desconhecida no dataset.
- Seis colunas do CSV continuam fora do pipeline: `pressure_level`,
  `hours_since_maintenance`, `ambient_temp`, `rul_hours`, `failure_type` e
  `estimated_repair_cost`.
- O TAP prevê `vibration_rms`, agora transportado; `operating_mode` aparece como
  `state` e não há campo `status`. A diferença restante precisa ser registrada no TAP.
