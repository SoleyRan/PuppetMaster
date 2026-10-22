#!/usr/bin/env bash
# System integration: checks every node's configuration together, then runs
# the three processes over shared memory, stops them after a few seconds,
# and checks that the monitor received states without loss.
#
#     ./run_pipeline.sh <install-prefix> <puppet_master_config> [seconds]
#
# <install-prefix> holds every package, as installed by build_all.sh.
set -euo pipefail

PREFIX=$(cd "${1:?usage: run_pipeline.sh <install-prefix> <puppet_master_config> [seconds]}" && pwd)
CONFIG_TOOL=${2:?usage: run_pipeline.sh <install-prefix> <puppet_master_config> [seconds]}
SECONDS_TO_RUN=${3:-3}
DEPLOY=$(cd "$(dirname "$0")" && pwd)/deploy.json
SHARE=$PREFIX/share
export PUPPETMASTER_CONFIG_PATH=$SHARE
LOGS=$(mktemp -d)
trap 'kill $(jobs -p) 2>/dev/null || true; rm -rf "$LOGS"' EXIT

NODES=("$SHARE/sensor_node/sensors.json" "$SHARE/estimator_node/estimator.json" "$SHARE/monitor_node/monitor.json")

# One check for the whole system: no node can see the others' files.
"$CONFIG_TOOL" validate --system --deployment "$DEPLOY" "${NODES[@]}"

# Each binary checks its own component types.
"$PREFIX/bin/sensor_node" --validate --deployment "$DEPLOY" "${NODES[0]}" > /dev/null
"$PREFIX/bin/estimator_node" --validate --deployment "$DEPLOY" "${NODES[1]}" > /dev/null
"$PREFIX/bin/monitor_node" --validate --deployment "$DEPLOY" "${NODES[2]}" > /dev/null

# Consumers first, so they see the first message the sensors write.
"$PREFIX/bin/monitor_node" --deployment "$DEPLOY" "${NODES[2]}" > "$LOGS/monitor.log" 2>&1 &
MONITOR=$!
"$PREFIX/bin/estimator_node" --deployment "$DEPLOY" "${NODES[1]}" > "$LOGS/estimator.log" 2>&1 &
ESTIMATOR=$!
sleep 0.5
"$PREFIX/bin/sensor_node" --deployment "$DEPLOY" "${NODES[0]}" > "$LOGS/sensors.log" 2>&1 &
SENSORS=$!

sleep "$SECONDS_TO_RUN"

# Producers first, so downstream nodes drain what is in flight.
status=0
for pid in $SENSORS $ESTIMATOR $MONITOR; do
    kill -INT "$pid" 2>/dev/null || true
    wait "$pid" || status=1
    sleep 0.2
done

for node in sensors estimator monitor; do
    echo "--- $node"
    grep -v "<info>" "$LOGS/$node.log" || true
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
