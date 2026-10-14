#!/usr/bin/env bash
# Run from any directory. Override LIMIT, INTERVAL_MS, MQTT_HOST, MQTT_PORT
# or CSV_PATH through environment variables; see README for examples.
# Requires Bash >= 5.1 (wait -n -p). Each child is the real C replayer.
set -u

if ((BASH_VERSINFO[0] < 5 || (BASH_VERSINFO[0] == 5 && BASH_VERSINFO[1] < 1))); then
    echo 'Requer Bash 5.1 ou superior.' >&2
    exit 1
fi
if (($# != 0)); then
    echo 'Uso: [LIMIT=10 INTERVAL_MS=1000 MQTT_HOST=127.0.0.1 MQTT_PORT=1883 CSV_PATH=...] run-ten-machines.sh' >&2
    exit 1
fi
script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd) || exit 1
replayer="$script_dir/../build/dataset-replayer"
csv=${CSV_PATH-"$script_dir/../../data/predictive_maintenance_v3.csv"}
limit=${LIMIT-10}
interval=${INTERVAL_MS-1000}
host=${MQTT_HOST-127.0.0.1}
port=${MQTT_PORT-1883}

positive_in_range() {
    local value=$1 maximum=$2
    [[ $value =~ ^[1-9][0-9]*$ && ${#value} -le ${#maximum} ]] &&
        ((10#$value <= maximum))
}
if ! positive_in_range "$limit" 2147483647 ||
   ! positive_in_range "$interval" 60000 ||
   ! positive_in_range "$port" 65535 ||
   [[ -z $host || $host == --* ]]; then
    echo 'Configuração inválida: LIMIT=1..2147483647, INTERVAL_MS=1..60000, MQTT_PORT=1..65535 e MQTT_HOST não vazio.' >&2
    exit 1
fi
if [[ ! -x $replayer || ! -r $csv ]]; then
    echo 'Executável ou CSV indisponível. Compile com make -C dataset-replayer all e confira CSV_PATH.' >&2
    exit 1
fi

# Only tracked children are signalled. Reap every child before exiting, also
# after errors. Signal handlers set a flag so launch + PID registration finish.
declare -A children=()
signal_status=0
trap 'signal_status=130' INT
trap 'signal_status=143' TERM
cleanup() {
    trap '' INT TERM
    local pid
    for pid in "${!children[@]}"; do
        kill -TERM "$pid" 2>/dev/null || :
    done
    for pid in "${!children[@]}"; do
        wait "$pid" 2>/dev/null || :
    done
}
trap cleanup EXIT

for machine in 1 2 3 6 7 8 9 11 12 13; do
    ((signal_status == 0)) || exit "$signal_status"
    "$replayer" "$csv" --machine-id "$machine" --publish --limit "$limit" \
        --interval-ms "$interval" --mqtt-host "$host" --mqtt-port "$port" &
    pid=$!
    children[$pid]=$machine
    printf 'Máquina %s iniciada: PID %s\n' "$machine" "$pid" >&2
done

while ((${#children[@]} != 0)); do
    ((signal_status == 0)) || exit "$signal_status"
    finished=''
    wait -n -p finished "${!children[@]}"
    status=$?
    ((signal_status == 0)) || exit "$signal_status"
    if [[ -z ${finished-} ]]; then
        echo 'Erro ao aguardar processos do Replayer.' >&2
        exit 1
    fi
    machine=${children[$finished]}
    unset 'children[$finished]'
    if ((status != 0)); then
        printf 'Máquina %s (PID %s) falhou com código %s; encerrando as demais.\n' \
            "$machine" "$finished" "$status" >&2
        exit 1
    fi
done
# Handle a signal delivered just after the last wait.
((signal_status == 0)) || exit "$signal_status"
echo 'Dez máquinas concluídas com sucesso.'
