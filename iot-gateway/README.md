# IoT Gateway — SIM-04.1

Módulo C11 independente para interpretar e validar tópico e telemetria JSON.
Nesta etapa, `all` gera somente um objeto compilado: não existe executável do
Gateway, assinatura MQTT, CLI, HTTP ou integração com InterSCity. O Dataset
Replayer não é modificado nem utilizado como dependência de compilação.

## Build e testes locais

Requer GCC, Make, pkg-config e json-c 0.19. Na raiz do repositório:

```sh
pkg-config --modversion json-c
make -C iot-gateway all
make -C iot-gateway test
make -C iot-gateway clean
```

O Makefile usa `-std=c11 -Wall -Wextra -Wpedantic`, obtém flags com pkg-config
e vincula apenas json-c no teste. Não requer libmosquitto nem broker.

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
nenhuma comunicação ou confirmação MQTT: isso pertence às próximas subetapas.
