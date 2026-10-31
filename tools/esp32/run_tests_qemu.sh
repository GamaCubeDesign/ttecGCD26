#!/usr/bin/env bash
# run_tests_qemu.sh — the portable test suites on an emulated ESP32 (TTC-16).
#
#   tools/esp32/run_tests_qemu.sh
#
# Builds ground/esp32-tests, merges bootloader, partition table and app into
# one 4 MB flash image, boots it in the QEMU of the ESP-IDF Docker image
# (Espressif's, which emulates the ESP32's Xtensa CPU), and prints the suites'
# results. Exits 0 only on the sentinel "ESP32 SUITES: N run, 0 failed".
# On the DevKit the same image runs for real (make esp32-tests-flash).
set -euo pipefail

root=$(cd "$(dirname "$0")/../.." && pwd)
image=${IDF_IMAGE:-espressif/idf:release-v5.3}
"$root/tools/esp32/idf.sh" ground/esp32-tests build >/dev/null

out=$(docker run --rm -v "$root:/project" -w /project/ground/esp32-tests/build \
    -u "$(id -u):$(id -g)" -e HOME=/tmp "$image" bash -c '
        esptool.py --chip esp32 merge_bin --fill-flash-size 4MB -o flash.bin @flash_args >/dev/null || exit 1
        # The app idles forever after the sentinel: stop QEMU as soon as it
        # appears, or after 3 minutes.
        qemu-system-xtensa -nographic -machine esp32 -nic none \
            -drive file=flash.bin,if=mtd,format=raw > qemu.out 2>&1 &
        pid=$!
        for _ in $(seq 360); do
            grep -qE "ESP32 SUITES: [0-9]+ run" qemu.out && break
            sleep 0.5
        done
        kill "$pid" 2>/dev/null
        sed -n "/ESP32 SUITES: start/,/ESP32 SUITES: [0-9]* run/p" qemu.out' 2>/dev/null) || true

printf '%s\n' "$out" | grep -E '^(==|ESP32 SUITES|  FAIL)|checks' || true
if printf '%s\n' "$out" | grep -qE '^ESP32 SUITES: [0-9]+ run, 0 failed'; then
    exit 0
fi
echo "run_tests_qemu: the suites did not all pass (or QEMU did not reach the end)" >&2
exit 1
