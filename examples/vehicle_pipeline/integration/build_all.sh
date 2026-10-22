#!/usr/bin/env bash
# Builds every package of the example the way separate teams would: each in
# its own build tree, installed into one shared prefix, finding the others
# only through that prefix.
#
#     ./build_all.sh <puppetmaster-prefix> <install-prefix> [extra CMake args for sensor_interfaces]
#
# Extra arguments let a test build a mismatched release, for example
# -DWHEEL_SPEED_VERSION=3 for a sensor team that bumped a message.
set -euo pipefail

PM_PREFIX=$(cd "${1:?usage: build_all.sh <puppetmaster-prefix> <install-prefix>}" && pwd)
PREFIX=${2:?usage: build_all.sh <puppetmaster-prefix> <install-prefix>}
shift 2
mkdir -p "$PREFIX"
PREFIX=$(cd "$PREFIX" && pwd)
HERE=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${BUILD_ROOT:-$PREFIX/../build}
mkdir -p "$BUILD"

build() {
    local package=$1
    shift
    cmake -S "$HERE/$package" -B "$BUILD/$package" -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_PREFIX_PATH="$PREFIX;$PM_PREFIX${EXTRA_PREFIX_PATH:+;$EXTRA_PREFIX_PATH}" \
        -DCMAKE_INSTALL_PREFIX="$PREFIX" "$@" > "$BUILD/$package.configure.log"
    cmake --build "$BUILD/$package" --parallel > "$BUILD/$package.build.log"
    cmake --install "$BUILD/$package" > /dev/null
    echo "built $package"
}

# Interface packages first, then nodes; the order is the dependency order.
build sensor_interfaces "$@"
build estimation_interfaces
build sensor_node
build estimator_node
build monitor_node
