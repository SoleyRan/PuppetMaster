#!/usr/bin/env bash
# Runs the pipeline as three processes over shared memory, stops them after
# a few seconds, and checks that the monitor received states without loss.
#
#     ./run_pipeline.sh <path/to/vehicle_node> [seconds]
set -euo pipefail

NODE=${1:?usage: run_pipeline.sh <vehicle_node> [seconds]}
SECONDS_TO_RUN=${2:-3}
CONFIG=$(cd "$(dirname "$0")/config" && pwd)
LOGS=$(mktemp -d)
trap 'kill $(jobs -p) 2>/dev/null || true; rm -rf "$LOGS"' EXIT

for node in monitor estimator sensors; do
    "$NODE" --validate "$CONFIG/$node.json" > /dev/null
done

# Consumers first, so they see the first message the sensors write.
"$NODE" "$CONFIG/monitor.json" > "$LOGS/monitor.log" 2>&1 &
MONITOR=$!
"$NODE" "$CONFIG/estimator.json" > "$LOGS/estimator.log" 2>&1 &
ESTIMATOR=$!
sleep 0.5
"$NODE" "$CONFIG/sensors.json" > "$LOGS/sensors.log" 2>&1 &
SENSORS=$!

sleep "$SECONDS_TO_RUN"

# Producers first, so downstream nodes drain what is in flight.
status=0
for pid in $SENSORS $ESTIMATOR $MONITOR; do
    kill -INT "$pid"
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
