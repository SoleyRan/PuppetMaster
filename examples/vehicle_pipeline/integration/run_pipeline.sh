#!/usr/bin/env bash
# System integration: checks every node of system.json together, then starts
# them in manifest order over shared memory, stops them after a few seconds,
# and checks that the monitor received states without loss.
#
#     ./run_pipeline.sh <install-prefix> <puppet_master_config> [seconds]
#
# <install-prefix> holds every package, as installed by build_all.sh. The
# node list, search paths, and deployment all come from system.json; nothing
# here names a node's files.
set -euo pipefail

PREFIX=$(cd "${1:?usage: run_pipeline.sh <install-prefix> <puppet_master_config> [seconds]}" && pwd)
CONFIG_TOOL=${2:?usage: run_pipeline.sh <install-prefix> <puppet_master_config> [seconds]}
SECONDS_TO_RUN=${3:-3}
MANIFEST=$(cd "$(dirname "$0")" && pwd)/system.json
export VEHICLE_PREFIX=$PREFIX
LOGS=$(mktemp -d)
trap 'kill $(jobs -p) 2>/dev/null || true; rm -rf "$LOGS"' EXIT

# One check for the whole system: no node can see the others' files.
"$CONFIG_TOOL" system validate "$MANIFEST"

# Each node: its own --validate checks component types, then it starts.
# Lines are name<TAB>command...<TAB>#delay_ms=N, in start order.
NAMES=()
PIDS=()
while IFS=$'\t' read -r -a fields; do
    name=${fields[0]}
    delay=${fields[${#fields[@]}-1]#\#delay_ms=}
    command=("${fields[@]:1:${#fields[@]}-2}")
    node_config=${command[${#command[@]}-1]}
    "${command[@]:0:${#command[@]}-1}" --validate "$node_config" > /dev/null
    "${command[@]}" > "$LOGS/$name.log" 2>&1 &
    NAMES+=("$name")
    PIDS+=($!)
    sleep "$(awk "BEGIN {print $delay / 1000}")"
done < <("$CONFIG_TOOL" system commands "$MANIFEST")

sleep "$SECONDS_TO_RUN"

# Reverse start order, so producers stop first and downstream nodes drain
# what is in flight.
status=0
for ((index = ${#PIDS[@]} - 1; index >= 0; --index)); do
    kill -INT "${PIDS[$index]}" 2>/dev/null || true
    wait "${PIDS[$index]}" || status=1
    sleep 0.2
done

for name in "${NAMES[@]}"; do
    echo "--- $name"
    grep -v "<info>" "$LOGS/$name.log" || true
done

summary=$(grep "monitor summary" "$LOGS/monitor.log" || true)
received=$(echo "$summary" | sed -n 's/.*received \([0-9]*\).*/\1/p')
lost=$(echo "$summary" | sed -n 's/.*lost \([0-9]*\).*/\1/p')
out_of_range=$(echo "$summary" | sed -n 's/.*out of range \([0-9]*\).*/\1/p')

if [ "$status" -ne 0 ] || [ -z "$received" ] || [ "$received" -lt 50 ] || [ "$lost" -ne 0 ] || [ "$out_of_range" -ne 0 ]; then
    echo "FAILED: exit status $status, received ${received:-none}, lost ${lost:-?}, out of range ${out_of_range:-?}"
    exit 1
fi
echo "OK: received $received states across processes, none lost"
