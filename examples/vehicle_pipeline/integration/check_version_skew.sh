#!/usr/bin/env bash
# The sensor team releases WheelSpeed v3, but the estimator in production was
# built against v2. Both safety nets must catch it:
#   1. the system check, before anything is deployed;
#   2. the shared-memory segment, if the check is skipped and both run.
#
#     ./check_version_skew.sh <puppetmaster-prefix> <work-dir> <puppet_master_config>
set -euo pipefail

PM_PREFIX=${1:?usage: check_version_skew.sh <puppetmaster-prefix> <work-dir> <puppet_master_config>}
WORK=${2:?usage: check_version_skew.sh <puppetmaster-prefix> <work-dir> <puppet_master_config>}
CONFIG_TOOL=${3:?usage: check_version_skew.sh <puppetmaster-prefix> <work-dir> <puppet_master_config>}
HERE=$(cd "$(dirname "$0")" && pwd)
DEPLOY=$HERE/deploy.json

# Current release (v2) of everything, and a newer sensor release (v3) built
# in its own prefix, as another team's CI would.
BUILD_ROOT=$WORK/build-v2 "$HERE/build_all.sh" "$PM_PREFIX" "$WORK/v2" > /dev/null
BUILD_ROOT=$WORK/build-v3 "$HERE/build_all.sh" "$PM_PREFIX" "$WORK/v3" -DWHEEL_SPEED_VERSION=3 > /dev/null

V2=$WORK/v2
V3=$WORK/v3

# 1. Mixed system: v3 sensors with the v2 estimator. Each file is valid on its
# own; the system check sees the disagreement.
SENSORS_V3=$V3/share/sensor_node/sensors.json
ESTIMATOR_V2=$V2/share/estimator_node/estimator.json
MONITOR_V2=$V2/share/monitor_node/monitor.json
if PUPPETMASTER_CONFIG_PATH=$V3/share "$CONFIG_TOOL" validate --deployment "$DEPLOY" "$SENSORS_V3" > /dev/null \
    && PUPPETMASTER_CONFIG_PATH=$V2/share "$CONFIG_TOOL" validate --deployment "$DEPLOY" "$ESTIMATOR_V2" > /dev/null; then
    echo "each file validates on its own"
else
    echo "FAILED: single-file validation should pass"
    exit 1
fi

# The integrator checks the files each team shipped, each resolved against
# its own release: assemble them in one tree the way a system image would.
MIXED=$WORK/mixed
rm -rf "$MIXED"
mkdir -p "$MIXED/share"
cp -r "$V3/share/sensor_interfaces" "$MIXED/share/sensor_interfaces_v3"
cp -r "$V2/share/sensor_interfaces" "$V2/share/estimation_interfaces" "$MIXED/share/"
sed 's#"sensor_interfaces/topics.json"#"sensor_interfaces_v3/topics.json"#' "$SENSORS_V3" > "$MIXED/sensors.json"
if output=$(PUPPETMASTER_CONFIG_PATH=$MIXED/share "$CONFIG_TOOL" validate --system --deployment "$DEPLOY" \
    "$MIXED/sensors.json" "$ESTIMATOR_V2" "$MONITOR_V2" 2>&1); then
    echo "FAILED: system check accepted mixed versions"
    echo "$output"
    exit 1
fi
echo "$output" | grep -q "vehicle.WheelSpeed v3" && echo "$output" | grep -q "vehicle.WheelSpeed v2" || {
    echo "FAILED: system check did not name both versions"
    echo "$output"
    exit 1
}
echo "system check: $(echo "$output" | grep '^system: topic')"

# 2. Runtime: start the v2 estimator, then the v3 sensors. The sensor process
# must fail to open the wheel topic instead of publishing into it.
LOGS=$(mktemp -d)
trap 'kill $(jobs -p) 2>/dev/null || true; rm -rf "$LOGS"' EXIT
PUPPETMASTER_CONFIG_PATH=$V2/share "$V2/bin/estimator_node" --deployment "$DEPLOY" "$ESTIMATOR_V2" \
    > "$LOGS/estimator.log" 2>&1 &
ESTIMATOR=$!
sleep 0.5
if PUPPETMASTER_CONFIG_PATH=$V3/share timeout 10 "$V3/bin/sensor_node" --deployment "$DEPLOY" "$SENSORS_V3" \
    > "$LOGS/sensors.log" 2>&1; then
    echo "FAILED: v3 sensors started against a v2 segment"
    exit 1
fi
kill -INT "$ESTIMATOR"
wait "$ESTIMATOR" || true
grep -q "vehicle.WheelSpeed v2" "$LOGS/sensors.log" && grep -q "vehicle.WheelSpeed v3" "$LOGS/sensors.log" || {
    echo "FAILED: runtime error did not name both versions"
    cat "$LOGS/sensors.log"
    exit 1
}
echo "runtime: $(grep -o 'shm topic /vehicle/wheel_speed carries[^(]*([^)]*), not [^(]*([^)]*)' "$LOGS/sensors.log" | head -1)"
echo "OK: version skew caught by the system check and by shared memory"
