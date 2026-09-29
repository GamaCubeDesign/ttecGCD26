#!/usr/bin/env bash
# adsb_chain.sh — a recorded or simulated mission through the whole chain,
# with no hardware:
#
#   sbs_replay (dump1090) -> adsbd -> ttcd -> UDP radio -> gs_cli (ground)
#       -> ground/host/tracks_to_ndjson.py -> the estimator's NDJSON
#
#   adsb_chain.sh INPUT OUT_DIR [DURATION_S]
#
# INPUT is an NDJSON file (the onboard record, or the output of the ground's
# simulator, simular_trajetorias.py) or an SBS capture (*.sbs). OUT_DIR
# receives every process's log, adsbd's onboard record, and ground.ndjson:
# what the ground rebuilt from the radio, ready for the ground's estimator:
#
#   cd ground/host/ground-aeronaves && python -m ground --input OUT_DIR/ground.ndjson
#
# Real time — a 10-minute mission takes 10 minutes — because adsbd stamps
# every message on arrival (ADR-0009). The ground streams at the flight
# period, 5 s, and sends its clock on contact (gs_cli --auto-time on).
#
# The Makefile runs it: make adsb-chain INPUT=<file>. Ports can be moved
# with SBS_PORT, SAT_PORT and GND_PORT, if the defaults are taken.
set -euo pipefail

input=$1
out=$2
duration=${3:-}
build=${BUILD_DIR:-build}
python=${PYTHON:-python3}
sbs_port=${SBS_PORT:-31003}      # not 30003: a real dump1090 may be running
sat_port=${SAT_PORT:-47101}      # not the bench's 47001/47002
gnd_port=${GND_PORT:-47102}

mkdir -p "$out"
# Not inside OUT_DIR: an AF_UNIX path must fit in 108 bytes.
sock=$(mktemp -u "${TMPDIR:-/tmp}/adsb-chain.XXXXXX.sock")
kind=--ndjson
[[ $input == *.sbs ]] && kind=--sbs

if [[ -z $duration ]]; then
    if [[ $kind == --ndjson ]]; then
        # The mission's own span, plus half a minute for the last snapshots.
        duration=$("$python" -c '
import json, sys
t = [json.loads(l).get("rx_epoch_ns", 0) for l in open(sys.argv[1]) if l.strip()]
t = [x for x in t if x]
print(int((max(t) - min(t)) / 1e9) + 30 if t else 60)' "$input")
    else
        duration=630
    fi
fi

pids=()
cleanup() {
    for p in "${pids[@]}"; do kill "$p" 2>/dev/null || true; done
    for p in "${pids[@]}"; do wait "$p" 2>/dev/null || true; done
    rm -f -- "$sock"
}
trap cleanup EXIT

echo "adsb-chain: $input -> $out, ${duration} s of real time"

"$build"/flight/ttcd -c flight/ttcd/ttcd.conf.example -o radio=udp -o ipc_path="$sock" \
    -o log_path="$out/ttcd.jsonl" -o udp_port="$sat_port" -o udp_peer_port="$gnd_port" &
pids+=($!)
for _ in {1..100}; do [[ -S $sock ]] && break; sleep 0.1; done
[[ -S $sock ]] || { echo "adsb-chain: ttcd did not start — see $out/ttcd.jsonl" >&2; exit 1; }

"$python" tools/sbs_replay/sbs_replay.py "$kind" "$input" --port "$sbs_port" --exit-at-end \
    2> "$out/replay.log" &
pids+=($!)

"$build"/flight/adsbd -c flight/adsbd/adsbd.conf.example -o ipc_path="$sock" \
    -o sbs_port="$sbs_port" -o dump1090_cmd= -o ndjson_path="$out/adsb.ndjson" \
    -o log_path="$out/adsbd.jsonl" &
pids+=($!)

printf 'stream 5\nwait %s\n' "$duration" \
    | "$build"/tools/gs_cli/gs_cli --radio udp --port "$gnd_port" --peer-port "$sat_port" \
          --auto-time on \
    | tee "$out/ground.jsonl" \
    | "$python" ground/host/tracks_to_ndjson.py > "$out/ground.ndjson"

echo
echo "== adsb-chain: what happened"
grep -c . "$out/adsb.ndjson" | sed 's/^/onboard record:        /; s/$/ messages/' || true
grep -c '"ev":"tracks"' "$out/ground.jsonl" | sed 's/^/TM_TRACKS received:    /; s/$/ frames/' || true
grep -o '"ev":"summary".*' "$out/ground.jsonl" | sed 's/^/ground summary:        /' || true
grep -c . "$out/ground.ndjson" | sed 's/^/for the estimator:     /; s/$/ records/' || true
grep -o '"event":"time_anchor".*' "$out/adsbd.jsonl" | head -1 | sed 's/^/adsbd anchored:        /' || true
echo
echo "next, the ground's estimator:"
echo "  cd ground/host/ground-aeronaves && python -m ground --input $(realpath "$out")/ground.ndjson"
