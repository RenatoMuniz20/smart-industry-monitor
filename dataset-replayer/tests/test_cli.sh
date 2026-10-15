#!/bin/sh
set -eu

test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT
replayer=./build/dataset-replayer

# Interleaved machines ensure --limit counts selected rows, not CSV rows.
# Machine 2 has a non-binary flag: filtering must precede normalization.
cat > "$test_dir/input.csv" <<'CSV'
timestamp,machine_id,machine_type,vibration_rms,temperature_motor,current_phase_avg,pressure_level,rpm,operating_mode,hours_since_maintenance,ambient_temp,rul_hours,failure_within_24h,failure_type,estimated_repair_cost
2024-01-01 00:00:00,2,Pump,1,90,8,4,5,peak,6,7,8,2,none,0
2024-01-01 00:01:00,1,Robotic Arm,1,49.7,3.5,4,5,idle,6,7,8,0,none,0
2024-01-01 00:02:00,2,Pump,1,90,8,4,5,peak,6,7,8,0,none,0
2024-01-01 00:03:00,1,Robotic Arm,1,,,4,5,normal,6,7,8,1,none,0
2024-01-01 00:04:00,1,Robotic Arm,1,50,4,4,5,peak,6,7,8,0,none,0
2024-01-01 00:05:00,1,Robotic Arm,1,51,5,4,5,other,6,7,8,1,none,0
2024-01-01 00:06:00,1,Robotic Arm,1,52,6,4,5,,6,7,8,0,none,0
CSV
cat > "$test_dir/rows" <<'EXPECTED'
timestamp=2024-01-01 00:01:00 machine_id=1 machine_type=Robotic Arm temperature=49.7 current=3.5 power=N/A state=IDLE failure_within_24h=0
timestamp=2024-01-01 00:03:00 machine_id=1 machine_type=Robotic Arm temperature=N/A current=N/A power=N/A state=NORMAL failure_within_24h=1
timestamp=2024-01-01 00:04:00 machine_id=1 machine_type=Robotic Arm temperature=50 current=4 power=N/A state=PEAK failure_within_24h=0
timestamp=2024-01-01 00:05:00 machine_id=1 machine_type=Robotic Arm temperature=51 current=5 power=N/A state=UNKNOWN failure_within_24h=1
timestamp=2024-01-01 00:06:00 machine_id=1 machine_type=Robotic Arm temperature=52 current=6 power=N/A state=UNKNOWN failure_within_24h=0
EXPECTED
cp "$test_dir/rows" "$test_dir/expected"
echo 'Registros selecionados: 5' >> "$test_dir/expected"
"$replayer" "$test_dir/input.csv" --machine-id 1 > "$test_dir/actual"
diff -u "$test_dir/expected" "$test_dir/actual"

head -n 2 "$test_dir/rows" > "$test_dir/expected"
echo 'Registros selecionados: 2' >> "$test_dir/expected"
"$replayer" "$test_dir/input.csv" --limit 2 --machine-id 1 > "$test_dir/actual"
diff -u "$test_dir/expected" "$test_dir/actual"

cat > "$test_dir/expected" <<'EXPECTED'
Nenhum registro encontrado para machine_id 99.
Registros selecionados: 0
EXPECTED
"$replayer" "$test_dir/input.csv" --machine-id 99 > "$test_dir/actual"
diff -u "$test_dir/expected" "$test_dir/actual"

expect_error()
{
    if "$replayer" "$@" > "$test_dir/actual" 2> "$test_dir/error"; then
        echo 'A CLI deveria ter retornado erro.' >&2
        exit 1
    fi
    test -s "$test_dir/error"
}

# A normalization error stops before any output or success summary.
expect_error "$test_dir/input.csv" --machine-id 2
test ! -s "$test_dir/actual"
echo 'Erro ao processar o CSV na linha 2.' > "$test_dir/expected"
diff -u "$test_dir/expected" "$test_dir/error"

# Stop after one successful row when a later selected row cannot normalize.
head -n 2 "$test_dir/input.csv" > "$test_dir/invalid.csv"
sed -n '3p' "$test_dir/input.csv" >> "$test_dir/invalid.csv"
sed -n '3p' "$test_dir/input.csv" | sed 's/,0,none,0$/,2,none,0/' >> "$test_dir/invalid.csv"
expect_error "$test_dir/invalid.csv" --machine-id 1
head -n 1 "$test_dir/rows" > "$test_dir/expected"
diff -u "$test_dir/expected" "$test_dir/actual"
echo 'Erro ao processar o CSV na linha 4.' > "$test_dir/expected"
diff -u "$test_dir/expected" "$test_dir/error"

# Early stop must not inspect the later invalid selected row.
"$replayer" "$test_dir/invalid.csv" --machine-id 1 --limit 1 > "$test_dir/actual"
head -n 1 "$test_dir/rows" > "$test_dir/expected"
echo 'Registros selecionados: 1' >> "$test_dir/expected"
diff -u "$test_dir/expected" "$test_dir/actual"

expect_error "$test_dir/input.csv"
expect_error "$test_dir/input.csv" --machine-id 0
expect_error "$test_dir/input.csv" --machine-id bad
expect_error "$test_dir/input.csv" --machine-id 1 --limit 0
expect_error "$test_dir/input.csv" --machine-id 1 --machine-id 2
expect_error "$test_dir/input.csv" --machine-id 1 --unknown 2
expect_error "$test_dir/missing.csv" --machine-id 1
expect_error tests/fixtures/invalid-header.csv --machine-id 1
expect_error tests/fixtures/invalid-row.csv --machine-id 1

expect_usage()
{
    expect_error "$@"
    test ! -s "$test_dir/actual"
    # Invalid publication arguments must be rejected before any connection.
    grep '^Uso:' "$test_dir/error" > /dev/null
}

expect_usage "$test_dir/input.csv" --publish
expect_usage "$test_dir/input.csv" --publish --limit 1
expect_usage "$test_dir/input.csv" --machine-id 1 --publish --publish
expect_usage "$test_dir/input.csv" --machine-id 1 --publish true
expect_usage "$test_dir/input.csv" --machine-id 1 --publish --unknown 1
for option in --mqtt-port --interval-ms; do
    for value in 0 -1 +1 1.5 1x abc '' ' 1' 999999999999999999999999999999999999; do
        expect_usage "$test_dir/input.csv" --machine-id 1 --publish "$option" "$value"
    done
    expect_usage "$test_dir/input.csv" --machine-id 1 --publish "$option"
    expect_usage "$test_dir/input.csv" --machine-id 1 "$option" 1
    expect_usage "$test_dir/input.csv" --machine-id 1 --publish "$option" 1 "$option" 2
    expect_usage "$test_dir/input.csv" --machine-id 1 "$option" --publish
done
expect_usage "$test_dir/input.csv" --machine-id 1 --publish --mqtt-port 65536
expect_usage "$test_dir/input.csv" --machine-id 1 --publish --interval-ms 60001
expect_usage "$test_dir/input.csv" --machine-id 1 --mqtt-host localhost
expect_usage "$test_dir/input.csv" --machine-id 1 --publish --mqtt-host
expect_usage "$test_dir/input.csv" --machine-id 1 --publish --mqtt-host ''
expect_usage "$test_dir/input.csv" --machine-id 1 --mqtt-host --publish
expect_usage "$test_dir/input.csv" --machine-id 1 --publish --mqtt-host a --mqtt-host b
expect_usage "$test_dir/input.csv" --publish --machine-id 0
expect_usage "$test_dir/input.csv" --publish --machine-id 1 --limit 0
expect_usage "$test_dir/input.csv" --publish --machine-id 1 --limit
expect_usage "$test_dir/input.csv" --publish --machine-id 1 --machine-id 2

echo 'Todos os testes da CLI passaram.'
