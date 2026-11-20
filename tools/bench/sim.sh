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
# after its own adsb_timeout (600 s, OBC-6), as in flight; MISSION_S=<s>
# shortens it, and tells the page the same.
#
# INPUT is an NDJSON file (tools/live_traffic, the onboard record, or the
# ground's simulator) or an SBS capture (*.sbs). OUT_DIR receives every
# process's log; the ground's own session goes under OUT_DIR/sessions.
# Ctrl-C stops everything.
#
# The Makefile runs it: make sim INPUT=<file>. WEB_HOST, WEB_PORT, SBS_PORT,
# SAT_PORT, GND_PORT and AVR_PORT move the ports if the defaults are taken;
# AVR=0 leaves the category port off (ADR-0023).

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
avr_port=${AVR_PORT:-31012}
mission_s=${MISSION_S:-600}
[[ $mission_s =~ ^[0-9]+$ && $mission_s -gt 0 ]] || { echo "sim: MISSION_S must be a positive number of seconds" >&2; exit 1; }

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
# dump1090's raw (AVR) port, where adsbd reads the emitter category
# (ADR-0023): on here, with an NDJSON input that carries one; AVR=0 plays
# the flight's default, without it, for comparison.
avr=()
replay_avr=()
if [[ ${AVR:-1} == 1 && $kind == --ndjson ]]; then
    avr=(-o avr_port="$avr_port")
    replay_avr=(--avr-port "$avr_port")
fi

# The route-pair method (ADR-0019) is off, as in the ground's default
# (ADR-0022); PAIRS=1 turns it on, with the locally generated database.
pairs=()
if [[ ${PAIRS:-0} == 1 ]]; then
    pairs=(--pairs --pairs-db ground/host/adsb/data/pares/pares_aeroportos.db)
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
# It runs in the background of its loop, so that the TERM of cleanup reaches
# it: as a foreground child it kept the loop deaf to the signal while it
# waited for a client, and Ctrl-C left sim.sh hung with the replay holding
# its ports (an asynchronous command ignores SIGINT, so the replay never
# saw the Ctrl-C itself).
( c=
  trap '[[ -n $c ]] && kill "$c" 2>/dev/null; exit 0' TERM
  while true; do
      "$python" tools/sbs_replay/sbs_replay.py "$kind" "$input" --port "$sbs_port" "${replay_avr[@]}" \
          2>> "$out/replay.log" &
      c=$!
      wait "$c" || sleep 1
  done ) &
pids+=($!)

"$build"/flight/adsbd -c flight/adsbd/adsbd.conf.example -o ipc_path="$sock" \
    -o sbs_port="$sbs_port" -o dump1090_cmd= -o ndjson_path="$out/adsb.ndjson" \
    -o log_path="$out/adsbd.jsonl" "${avr[@]}" &
pids+=($!)

"$build"/flight/obcd -c flight/obcd/obcd.conf.example -o ipc_path="$sock" \
    -o log_path="$out/obcd.jsonl" -o adsb_timeout_ms=$(( mission_s * 1000 )) -o i2c_dev= -o w1_path= \
    -o burnwire_line=none -o heater_line=none &
pids+=($!)

echo "sim: $input — logs in $out"
"$python" ground/host/gs_web.py --host "$web_host" --port "$web_port" --adsb "${pairs[@]}" \
    --mission-s "$mission_s" \
    --session-dir "$out/sessions" \
    --exec "$build"/tools/gs_cli/gs_cli --radio udp --port "$gnd_port" --peer-port "$sat_port" \
        --auto-time on &
pids+=($!)
wait "${pids[-1]}"
