#!/usr/bin/env bash
# idf.sh — runs idf.py on one of this repository's ESP-IDF projects, inside
# the ESP-IDF Docker image: nothing to install but Docker.
#
#   tools/esp32/idf.sh PROJECT_DIR [--port DEVICE] IDF_PY_ARGS...
#   tools/esp32/idf.sh ground/esp32 build
#   tools/esp32/idf.sh ground/esp32 --port /dev/ttyUSB0 -p /dev/ttyUSB0 flash
#
# The whole repository is mounted, because the firmware compiles common/,
# flight/radio/sx1278.c and ground/console/ from where they are. The build
# runs as the calling user, so build/ belongs to them. IDF_IMAGE picks
# another image (the default is the one ADR-0018 was built with).
set -euo pipefail

root=$(cd "$(dirname "$0")/../.." && pwd)
image=${IDF_IMAGE:-espressif/idf:release-v5.3}
project=${1:?usage: idf.sh PROJECT_DIR [--port DEVICE] IDF_PY_ARGS...}
shift

device=()
if [[ ${1:-} == --port ]]; then
    port=${2:?--port needs a device}
    shift 2
    [[ -e $port ]] || { echo "idf.sh: $port does not exist (is the board plugged in?)" >&2; exit 1; }
    device=(--device "$port" --group-add "$(stat -c %g "$port")")
fi
tty=()
if [[ -t 0 && -t 1 ]]; then
    tty=(-it)
fi

exec docker run --rm "${tty[@]}" "${device[@]}" \
    -v "$root:/project" -w "/project/$project" \
    -u "$(id -u):$(id -g)" -e HOME=/tmp \
    -e IDF_GIT_SAFE_DIR=/project:/opt/esp/idf -e IDF_COMPONENT_MANAGER=0 \
    "$image" idf.py "$@"
