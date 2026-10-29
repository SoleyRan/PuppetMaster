#!/usr/bin/env bash
# System integration: checks every node of system.json together, starts the
# system over shared memory with puppet_master_launch, stops it after a few
# seconds, and checks that the monitor received states without loss.
#
#     ./run_pipeline.sh <install-prefix> <puppet_master_config> [seconds]
#
# <install-prefix> holds every package, as installed by build_all.sh.
# puppet_master_launch is taken from next to <puppet_master_config>. The node
# list, search paths, and deployment all come from system.json; nothing here
# names a node's files.
set -euo pipefail

PREFIX=$(cd "${1:?usage: run_pipeline.sh <install-prefix> <puppet_master_config> [seconds]}" && pwd)
CONFIG_TOOL=${2:?usage: run_pipeline.sh <install-prefix> <puppet_master_config> [seconds]}
SECONDS_TO_RUN=${3:-3}
LAUNCH=$(dirname "$CONFIG_TOOL")/puppet_master_launch
MANIFEST=$(cd "$(dirname "$0")" && pwd)/system.json
export VEHICLE_PREFIX=$PREFIX
LOGS=$(mktemp -d)
trap 'kill $(jobs -p) 2>/dev/null || true; rm -rf "$LOGS"' EXIT

# Each node's own --validate checks its component types, which only the
# node binary knows. Lines are name<TAB>command...<TAB>#delay_ms=N.
while IFS=$'\t' read -r -a fields; do
    command=("${fields[@]:1:${#fields[@]}-2}")
    node_config=${command[${#command[@]}-1]}
    "${command[@]:0:${#command[@]}-1}" --validate "$node_config" > /dev/null
done < <("$CONFIG_TOOL" system commands "$MANIFEST")

# The launcher checks the nodes together, starts them in manifest order,
# and on SIGINT stops them in reverse order, so producers stop first and
# downstream nodes drain what is in flight.
"$LAUNCH" --log-dir "$LOGS" "$MANIFEST" 2> "$LOGS/launch.txt" &
launcher=$!
sleep "$SECONDS_TO_RUN"
kill -INT "$launcher"
status=0
wait "$launcher" || status=$?

cat "$LOGS/launch.txt"
for log in "$LOGS"/*.log; do
    echo "--- $(basename "$log" .log)"
    grep -v "<info>" "$log" || true
done

nodes=$("$CONFIG_TOOL" system commands "$MANIFEST" | wc -l)
clean_stops=$(grep -c ": stopped (exit code 0)" "$LOGS/launch.txt" || true)

summary=$(grep "monitor summary" "$LOGS/monitor.log" || true)
received=$(echo "$summary" | sed -n 's/.*received \([0-9]*\).*/\1/p')
lost=$(echo "$summary" | sed -n 's/.*lost \([0-9]*\).*/\1/p')
out_of_range=$(echo "$summary" | sed -n 's/.*out of range \([0-9]*\).*/\1/p')

if [ "$status" -ne 0 ] || [ "$clean_stops" -ne "$nodes" ] || [ -z "$received" ] || [ "$received" -lt 50 ] ||
    [ "$lost" -ne 0 ] || [ "$out_of_range" -ne 0 ]; then
    echo "FAILED: launcher exit $status, $clean_stops of $nodes nodes stopped cleanly," \
        "received ${received:-none}, lost ${lost:-?}, out of range ${out_of_range:-?}"
    exit 1
fi
echo "OK: received $received states across processes, none lost"
