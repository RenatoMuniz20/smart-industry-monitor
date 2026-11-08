#!/usr/bin/env bash
# Cadastra as máquinas do Smart Industry Monitor no InterSCity (instância LSDi/UFMA)
# e mantém o mapeamento machine_id -> UUID em config/interscity/machines.conf.
#
# Uso (a partir da raiz do repositório):
#   bash iot-gateway/scripts/register-interscity-machines.sh --dry-run   # só mostra o plano
#   bash iot-gateway/scripts/register-interscity-machines.sh             # executa
#   bash iot-gateway/scripts/register-interscity-machines.sh 2 3         # apenas IDs específicos
#
# Variáveis de ambiente opcionais:
#   INTERSCITY_URL  (padrão: https://cidadesinteligentes.lsdi.ufma.br/interscity_lh)
#   MAPPING_FILE    (padrão: config/interscity/machines.conf)
#   DATASET_CSV     (padrão: data/predictive_maintenance_v3.csv)
#
# Regras de segurança (instância compartilhada; POST não idempotente):
#   - Nunca cadastra uma máquina que já esteja no mapeamento ou na instância.
#   - Qualquer resposta diferente de 201, ou timeout, interrompe o script sem repetir.
#   - Nunca remove nada da instância.
# Detalhes do contrato e das decisões: docs/sim-05-interscity.md

set -euo pipefail

INTERSCITY_URL="${INTERSCITY_URL:-https://cidadesinteligentes.lsdi.ufma.br/interscity_lh}"
MAPPING_FILE="${MAPPING_FILE:-config/interscity/machines.conf}"
DATASET_CSV="${DATASET_CSV:-data/predictive_maintenance_v3.csv}"

CAPABILITIES=(sim_motor_temperature sim_current sim_operating_state sim_failure_within_24h)
DEFAULT_IDS=(1 2 3 6 7 8 9 11 12 13)
LAT=-2.5580
LON=-44.3090
COLLECT_INTERVAL=1

log()  { printf '%s\n' "$*"; }
fail() { printf 'ERRO: %s\n' "$*" >&2; exit 1; }

# ---------------------------------------------------------------- argumentos
DRY_RUN=0
IDS=()
for arg in "$@"; do
  case "$arg" in
    --dry-run) DRY_RUN=1 ;;
    ''|*[!0-9]*) fail "argumento inválido: '$arg' (use --dry-run e/ou IDs numéricos)" ;;
    *) IDS+=("$arg") ;;
  esac
done
if [ "${#IDS[@]}" -eq 0 ]; then
  IDS=("${DEFAULT_IDS[@]}")
fi

# ---------------------------------------------------------------- auxiliares
# A resposta do Catalog traz o recurso em "data", como objeto ou como texto JSON.
resource_json() { jq '.data | if type == "string" then fromjson else . end'; }

# Tipo da máquina, lido do próprio dataset (coluna 3), sem inventar informação.
machine_type_of() {
  awk -F',' -v id="$1" 'NR > 1 && $2 == id { gsub(/["\r]/, "", $3); print $3; exit }' "$DATASET_CSV"
}

# UUID já mapeado para o machine_id, ignorando comentários e linhas vazias.
uuid_in_mapping() {
  awk -v id="$1" '!/^[[:space:]]*(#|$)/ && $1 == id { print $2; exit }' "$MAPPING_FILE"
}

description_of_uuid() {
  curl -sS --fail --max-time 20 "$INTERSCITY_URL/catalog/resources/$1" | resource_json | jq -r '.description'
}

# Recursos com descrição exatamente igual (o filtro do servidor é refeito no cliente).
search_by_description() {
  curl -sS --fail --max-time 20 -G \
       --data-urlencode "description=$1" --data-urlencode "per_page=100" \
       "$INTERSCITY_URL/catalog/resources/search" \
  | jq -c --arg d "$1" '[.resources[] | select(.description == $d)]'
}

# ---------------------------------------------------------------- verificações
for cmd in curl jq awk grep; do
  command -v "$cmd" >/dev/null 2>&1 || fail "comando ausente: $cmd"
done
[ -f "$MAPPING_FILE" ] || fail "mapeamento não encontrado: $MAPPING_FILE (execute na raiz do repositório)"
[ -f "$DATASET_CSV" ]  || fail "dataset não encontrado: $DATASET_CSV (execute na raiz do repositório)"
if grep -q $'\r' "$MAPPING_FILE"; then
  fail "$MAPPING_FILE contém quebras de linha CRLF; converta para LF antes de continuar"
fi

for cap in "${CAPABILITIES[@]}"; do
  code=$(curl -sS -o /dev/null -w '%{http_code}' --max-time 20 \
         "$INTERSCITY_URL/catalog/capabilities/$cap") || fail "falha de rede ao consultar a capability $cap"
  [ "$code" = "200" ] || fail "capability $cap não encontrada na instância (HTTP $code)"
done

# Garante que um novo par seja gravado em linha própria (nunca altera nada em --dry-run).
if [ "$DRY_RUN" -eq 0 ] && [ -s "$MAPPING_FILE" ] && [ -n "$(tail -c 1 "$MAPPING_FILE")" ]; then
  printf '\n' >> "$MAPPING_FILE"
fi

if [ "$DRY_RUN" -eq 1 ]; then
  log "Modo --dry-run: nenhuma alteração será feita na instância nem no mapeamento."
fi
log "Instância: $INTERSCITY_URL"
log "Máquinas:  ${IDS[*]}"
log ""

# ---------------------------------------------------------------- execução
verified=0; recovered=0; created=0; planned=0

for id in "${IDS[@]}"; do
  type=$(machine_type_of "$id")
  [ -n "$type" ] || fail "machine_id $id não encontrado em $DATASET_CSV"
  desc="Smart Industry Monitor - $type - machine_id $id"

  # 1. Já está no mapeamento: apenas confere com a instância.
  uuid=$(uuid_in_mapping "$id")
  if [ -n "$uuid" ]; then
    remote_desc=$(description_of_uuid "$uuid") || fail "não foi possível consultar o UUID $uuid (machine_id $id)"
    [ "$remote_desc" = "$desc" ] \
      || fail "machine_id $id: o UUID mapeado tem a descrição '$remote_desc'; esperado '$desc'"
    log "[OK]         machine_id=$id já mapeada e conferida ($uuid)"
    verified=$((verified + 1))
    continue
  fi

  # 2. Não está no mapeamento: procura na instância antes de qualquer criação.
  matches=$(search_by_description "$desc") || fail "falha ao buscar '$desc' na instância"
  n=$(jq 'length' <<< "$matches")

  if [ "$n" -gt 1 ]; then
    fail "machine_id $id: $n recursos com a descrição '$desc' (duplicata); verifique manualmente"
  fi

  if [ "$n" -eq 1 ]; then
    uuid=$(jq -r '.[0].uuid' <<< "$matches")
    if [ "$DRY_RUN" -eq 1 ]; then
      log "[RECUPERAR]  machine_id=$id existe na instância ($uuid), mas falta no mapeamento"
      planned=$((planned + 1))
    else
      printf '%s %s\n' "$id" "$uuid" >> "$MAPPING_FILE"
      log "[RECUPERADO] machine_id=$id já existia na instância; mapeamento atualizado ($uuid)"
      recovered=$((recovered + 1))
    fi
    continue
  fi

  # 3. Não existe em lugar nenhum: cadastra pelo Catalog (decisão documentada).
  if [ "$DRY_RUN" -eq 1 ]; then
    log "[CRIAR]      machine_id=$id -> '$desc'"
    planned=$((planned + 1))
    continue
  fi

  body=$(jq -n \
    --arg desc "$desc" \
    --arg uri "smart-industry-monitor/machines/$id" \
    --argjson lat "$LAT" --argjson lon "$LON" --argjson ci "$COLLECT_INTERVAL" \
    '{data: {description: $desc, uri: $uri, lat: $lat, lon: $lon, status: "active",
             collect_interval: $ci, capabilities: $ARGS.positional}}' \
    --args "${CAPABILITIES[@]}")

  resp=$(mktemp)
  code=$(curl -sS -o "$resp" -w '%{http_code}' --max-time 30 \
         -H 'Content-Type: application/json' -X POST --data "$body" \
         "$INTERSCITY_URL/catalog/resources") || code="000"

  if [ "$code" != "201" ]; then
    {
      printf 'Cadastro de machine_id=%s retornou HTTP %s. Resposta:\n' "$id" "$code"
      head -c 500 "$resp"; printf '\n'
    } >&2
    rm -f "$resp"
    fail "interrompido sem repetir. O recurso pode ter sido criado mesmo assim: aguarde um minuto e rode com --dry-run, que consulta a instância antes de qualquer criação."
  fi

  uuid=$(resource_json < "$resp" | jq -r '.uuid')
  rm -f "$resp"
  [[ "$uuid" =~ ^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$ ]] \
    || fail "UUID inválido na resposta de machine_id $id: '$uuid'"

  printf '%s %s\n' "$id" "$uuid" >> "$MAPPING_FILE"
  log "[CRIADO]     machine_id=$id -> $uuid"
  created=$((created + 1))
done

log ""
if [ "$DRY_RUN" -eq 1 ]; then
  log "Resumo (--dry-run): $verified conferidas, $planned ações planejadas."
else
  log "Resumo: $verified conferidas, $recovered recuperadas, $created criadas."
fi
