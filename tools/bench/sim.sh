#!/usr/bin/env bash
# sim.sh — the whole satellite on this PC, operated from the ground's page:
#
#   sbs_replay (dump1090, from a capture) -> adsbd -> ttcd -> UDP radio
#       -> gs_cli -> gs_web.py --adsb (http://WEB_HOST:WEB_PORT)
#   obcd (the OBC, with no board: no sensors, no outputs) <-> ttcd
#
#   sim.sh INPUT OUT_DIR
#
# The flight daemons are the real ones, with the bench configurations; only
# the radio is UDP and dump1090 is the replay. Nothing is scripted: the
# operator drives the satellite from the page, as with the ESP32. Each
# MISSION_ADSB starts the capture again from its beginning: adsbd connects to
# the replay when the mode starts it (ADR-0014), and the replay serves one
# connection, so it is started again for the next one. obcd ends the mission
# after its own adsb_timeout (600 s, OBC-6), as in flight.
#
# INPUT is an NDJSON file (tools/live_traffic, the onboard record, or the
# ground's simulator) or an SBS capture (*.sbs). OUT_DIR receives every
# process's log; the ground's own session goes under OUT_DIR/sessions.
# Ctrl-C stops everything.
#
# The Makefile runs it: make sim INPUT=<file>. WEB_HOST, WEB_PORT, SBS_PORT,
# SAT_PORT and GND_PORT move the ports if the defaults are taken.

set -euo pipefail

input=${1:?usage: sim.sh INPUT OUT_DIR}
out=${2:?usage: sim.sh INPUT OUT_DIR}
build=${BUILD_DIR:-build}
python=${PYTHON:-python3}
web_host=${WEB_HOST:-127.0.0.1}
web_port=${WEB_PORT:-8080}
sbs_port=${SBS_PORT:-31013}
sat_port=${SAT_PORT:-47011}
gnd_port=${GND_PORT:-47012}

for b in ttcd adsbd obcd; do
    [[ -x $build/flight/$b ]] || { echo "sim: $build/flight/$b is missing; run make first" >&2; exit 1; }
done
[[ -x $build/tools/gs_cli/gs_cli ]] || { echo "sim: $build/tools/gs_cli/gs_cli is missing; run make first" >&2; exit 1; }
[[ -f $input ]] || { echo "sim: no input $input" >&2; exit 1; }
mkdir -p "$out"
# Not inside OUT_DIR: an AF_UNIX path must fit in 108 bytes.
sock=$(mktemp -u "${TMPDIR:-/tmp}/sim.XXXXXX.sock")
kind=--ndjson
[[ $input == *.sbs ]] && kind=--sbs

# The second strategy needs the 710 MB pair database (ADR-0019); without it
# the ground runs the local method alone, and says so.
pairs=(--no-pairs)
if [[ -f ground/host/pares/pares_aeroportos.db ]]; then
    pairs=(--pairs-db ground/host/pares/pares_aeroportos.db)
fi

pids=()
cleanup() {
    trap - EXIT INT TERM
    for p in "${pids[@]}"; do kill "$p" 2>/dev/null || true; done
    for p in "${pids[@]}"; do wait "$p" 2>/dev/null || true; done
    rm -f -- "$sock"
}
trap cleanup EXIT
trap 'exit 130' INT TERM

"$build"/flight/ttcd -c flight/ttcd/ttcd.conf.example -o radio=udp -o ipc_path="$sock" \
    -o log_path="$out/ttcd.jsonl" -o udp_port="$sat_port" -o udp_peer_port="$gnd_port" &
pids+=($!)
for _ in {1..100}; do [[ -S $sock ]] && break; sleep 0.1; done
[[ -S $sock ]] || { echo "sim: ttcd did not start — see $out/ttcd.jsonl" >&2; exit 1; }

# One replay per mission: a new one waits as soon as the last client leaves.
( trap 'exit 0' TERM
  while true; do
      "$python" tools/sbs_replay/sbs_replay.py "$kind" "$input" --port "$sbs_port" \
          2>> "$out/replay.log" || sleep 1
  done ) &
pids+=($!)

"$build"/flight/adsbd -c flight/adsbd/adsbd.conf.example -o ipc_path="$sock" \
    -o sbs_port="$sbs_port" -o dump1090_cmd= -o ndjson_path="$out/adsb.ndjson" \
    -o log_path="$out/adsbd.jsonl" &
pids+=($!)

"$build"/flight/obcd -c flight/obcd/obcd.conf.example -o ipc_path="$sock" \
    -o log_path="$out/obcd.jsonl" -o i2c_dev= -o w1_path= \
    -o burnwire_line=none -o heater_line=none &
pids+=($!)

echo "sim: $input — logs in $out"
"$python" ground/host/gs_web.py --host "$web_host" --port "$web_port" --adsb "${pairs[@]}" \
    --session-dir "$out/sessions" \
    --exec "$build"/tools/gs_cli/gs_cli --radio udp --port "$gnd_port" --peer-port "$sat_port" \
        --auto-time on &
pids+=($!)
wait "${pids[-1]}"
