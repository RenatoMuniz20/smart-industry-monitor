#!/usr/bin/env bash
# SIM-11: cria no InterSCity (instância LSDi/UFMA) as capabilities de vibração e
# rotação usadas pelo Gateway. As quatro capabilities da SIM-05 não são tocadas.
#
# Uso (a partir da raiz do repositório):
#   bash iot-gateway/scripts/create-interscity-capabilities.sh --dry-run   # só consulta
#   bash iot-gateway/scripts/create-interscity-capabilities.sh             # cria o que falta
#
# Variável de ambiente opcional:
#   INTERSCITY_URL  (padrão: https://cidadesinteligentes.lsdi.ufma.br/interscity_lh)
#
# Regras de segurança (instância compartilhada):
#   - Consulta cada capability antes; só cria quando a consulta responde 404.
#   - Qualquer resposta inesperada ou timeout interrompe o script sem repetir.
#   - Nunca remove nada. Capabilities não devem ser apagadas na instância.
# As máquinas cadastradas não precisam ser alteradas: o Data Collector associa a
# capability ao recurso no primeiro envio (docs/sim-05-interscity.md).
# Detalhes: docs/sim-11-vibracao-rpm.md

set -euo pipefail

INTERSCITY_URL="${INTERSCITY_URL:-https://cidadesinteligentes.lsdi.ufma.br/interscity_lh}"

NAMES=(sim_vibration_rms sim_rpm)
DESCRIPTIONS=(
  "Smart Industry Monitor - vibração RMS do motor (vibration_rms do dataset)"
  "Smart Industry Monitor - rotação do motor em rpm (rpm do dataset)"
)

log()  { printf '%s\n' "$*"; }
fail() { printf 'ERRO: %s\n' "$*" >&2; exit 1; }

DRY_RUN=0
for arg in "$@"; do
  case "$arg" in
    --dry-run) DRY_RUN=1 ;;
    *) fail "argumento inválido: '$arg' (use apenas --dry-run)" ;;
  esac
done

for cmd in curl jq; do
  command -v "$cmd" >/dev/null 2>&1 || fail "comando ausente: $cmd"
done

[ "$DRY_RUN" -eq 1 ] && log "Modo --dry-run: nada será criado."
log "Instância: $INTERSCITY_URL"
log ""

existing=0; created=0; planned=0
for i in "${!NAMES[@]}"; do
  name="${NAMES[$i]}"
  description="${DESCRIPTIONS[$i]}"

  code=$(curl -sS -o /dev/null -w '%{http_code}' --max-time 20 \
         "$INTERSCITY_URL/catalog/capabilities/$name") || fail "falha de rede ao consultar $name"

  if [ "$code" = "200" ]; then
    log "[OK]      $name já existe"
    existing=$((existing + 1))
    continue
  fi
  [ "$code" = "404" ] || fail "consulta de $name respondeu HTTP $code; nada foi criado"

  if [ "$DRY_RUN" -eq 1 ]; then
    log "[CRIAR]   $name -> '$description'"
    planned=$((planned + 1))
    continue
  fi

  body=$(jq -n --arg name "$name" --arg description "$description" \
         '{name: $name, description: $description, capability_type: "sensor"}')
  resp=$(mktemp)
  code=$(curl -sS -o "$resp" -w '%{http_code}' --max-time 30 \
         -H 'Content-Type: application/json' -X POST --data "$body" \
         "$INTERSCITY_URL/catalog/capabilities") || code="000"
  if [ "$code" != "201" ]; then
    {
      printf 'Criação de %s retornou HTTP %s. Resposta:\n' "$name" "$code"
      head -c 500 "$resp"; printf '\n'
    } >&2
    rm -f "$resp"
    fail "interrompido sem repetir. A capability pode ter sido criada mesmo assim: rode com --dry-run para conferir."
  fi
  rm -f "$resp"
  log "[CRIADA]  $name"
  created=$((created + 1))
done

log ""
if [ "$DRY_RUN" -eq 1 ]; then
  log "Resumo (--dry-run): $existing existentes, $planned a criar."
else
  log "Resumo: $existing existentes, $created criadas."
fi
