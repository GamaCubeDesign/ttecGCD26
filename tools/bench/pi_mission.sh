#!/usr/bin/env bash
# pi_mission.sh — the satellite on the bench Pi, for an ADS-B mission
# operated from the ground's page (gs_web.py on the PC, the ESP32 ground):
#
#   [sbs_replay | dump1090-fa + SDR] -> adsbd -> ttcd -> radio A  ~~LoRa~~>
#       radio B -> ESP32 -> gs_web.py --adsb --mission-s MISSION_S
#   obcd <-> ttcd, with MISSION_ADSB lasting MISSION_S seconds
#
#   pi_mission.sh OUT_DIR
#
# The commands of test 1 of docs/vv/bancada/2026-11-19/roteiro.md, in one
# place. The daemons are the installed ones' configurations
# (/etc/gama/*.conf: the board's pins), with what a bench run changes on top:
#   MISSION_S  MISSION_ADSB's length, s (default 600, the flight's OBC-6);
#              obcd gets adsb_timeout_ms = MISSION_S * 1000. Tell the page
#              the same: ./gama ground --secs <same>.
#   INPUT      a capture played as dump1090 (NDJSON or *.sbs); empty: the
#              real SDR, with the dump1090_cmd of adsbd.conf.
#   AVR=1      the emitter category on the AVR port (ADR-0023, Etapa B); off
#              by default, as in flight. With an NDJSON INPUT the replay
#              serves it (test 1 of docs/vv/bancada/2026-11-20/roteiro.md);
#              with the SDR, dump1090-fa opens --net-ro-port 30002 and adsbd
#              reads it (test 2), for this run only: /etc/gama is not touched.
#   TX_POWER   radio A's power, dBm (default 2: the bench's short range).
# OUT_DIR receives every process's log. Ctrl-C stops everything.
#
# The Makefile runs it: make mission-sat on the Pi, which ./gama mission starts from
# the PC (which also copies INPUT and brings the logs back).

set -euo pipefail

out=${1:?usage: pi_mission.sh OUT_DIR}
build=${BUILD_DIR:-build}
python=${PYTHON:-python3}
mission_s=${MISSION_S:-600}
input=${INPUT:-}
tx_power=${TX_POWER:-2}
conf=${CONF_ETC:-/etc/gama}
sbs_port=${SBS_PORT:-31003}
avr_port=${AVR_PORT:-31004}

[[ $mission_s =~ ^[0-9]+$ && $mission_s -gt 0 ]] || { echo "mission: MISSION_S must be a positive number of seconds" >&2; exit 1; }
for b in ttcd adsbd obcd; do
    [[ -x $build/flight/$b ]] || { echo "mission: $build/flight/$b is missing; run make first" >&2; exit 1; }
    [[ -f $conf/$b.conf ]] || { echo "mission: no $conf/$b.conf; make install CONF_DIR=flight/conf/obc-v2 first" >&2; exit 1; }
done
if [[ -n $input && ! -f $input ]]; then
    echo "mission: no input $input" >&2; exit 1
fi
# One radio, one SDR, one I2C bus: a service or a leftover from a manual run
# would fight these for them.
if pgrep -a -x 'ttcd|obcd|adsbd' >&2; then
    echo "mission: the daemons above are running; stop them first (./gama pi stop from the PC, or kill the manual ones)" >&2
    exit 1
fi
mkdir -p "$out"
sock=$(mktemp -u "${TMPDIR:-/tmp}/mission.XXXXXX.sock")

src=(-o dump1090_cmd= -o sbs_port="$sbs_port")
replay=()
avr=()
if [[ -z $input ]]; then
    src=()                      # adsbd starts dump1090-fa on the SDR, as in flight
    if [[ ${AVR:-0} == 1 ]]; then
        # The installed dump1090_cmd, with its raw output opened on 30002.
        cmd=$(sed -n 's/^[[:space:]]*dump1090_cmd[[:space:]]*=[[:space:]]*//p' "$conf/adsbd.conf" | tail -n 1)
        cmd=${cmd%%#*}
        [[ $cmd == *--net-ro-port\ [0-9]* ]] || { echo "mission: AVR=1 needs --net-ro-port in dump1090_cmd of $conf/adsbd.conf" >&2; exit 1; }
        cmd=$(sed -E 's/--net-ro-port [0-9]+/--net-ro-port 30002/' <<< "$cmd")
        src=(-o "dump1090_cmd=$cmd")
        avr=(-o avr_port=30002)
    fi
else
    kind=--ndjson
    [[ $input == *.sbs ]] && kind=--sbs
    replay=("$python" tools/sbs_replay/sbs_replay.py "$kind" "$input" --port "$sbs_port")
    if [[ ${AVR:-0} == 1 && $kind == --ndjson ]]; then
        replay+=(--avr-port "$avr_port")
        avr=(-o avr_port="$avr_port")
    fi
fi

# adsbd must accept the run's overrides before anything starts.
"$build"/flight/adsbd -c "$conf/adsbd.conf" "${src[@]}" "${avr[@]}" --check-config > /dev/null ||
    { echo "mission: adsbd refuses this configuration" >&2; exit 1; }

# What decides Etapa B (ADR-0023), from the last stats adsbd logged (every
# report_period_ms): the category arrived and nothing was refused.
summary() {
    "$python" - "$out" <<'EOF' || true
import json, pathlib, sys
out = pathlib.Path(sys.argv[1])
stats = None
for line in (out / "adsbd.jsonl").read_text(errors="replace").splitlines() if (out / "adsbd.jsonl").exists() else []:
    try:
        o = json.loads(line)
    except ValueError:
        continue
    if o.get("event") == "stats":
        stats = o
if stats is None:
    print("mission: no stats from adsbd yet (one per report_period_ms)")
else:
    keys = ("decoded", "aircraft", "avr_lines", "avr_idents", "avr_no_track", "avr_rejected")
    print("mission: adsbd " + ", ".join("%s=%s" % (k, stats[k]) for k in keys if k in stats))
nd = out / "adsb.ndjson"
if nd.exists():
    n = sum('"category"' in l for l in nd.read_text(errors="replace").splitlines())
    print("mission: %d records with a category in adsb.ndjson" % n)
EOF
}

pids=()
cleanup() {
    trap - EXIT INT TERM
    for p in "${pids[@]}"; do kill "$p" 2>/dev/null || true; done
    for p in "${pids[@]}"; do wait "$p" 2>/dev/null || true; done
    rm -f -- "$sock"
    [[ -n ${avr[*]} ]] && summary
    echo "mission: stopped; logs in $out"
}
trap cleanup EXIT
trap 'exit 130' INT TERM

"$build"/flight/ttcd -c "$conf/ttcd.conf" -o ipc_path="$sock" -o tx_power_dbm="$tx_power" \
    -o log_path="$out/ttcd.jsonl" > "$out/ttcd.txt" 2>&1 &
pids+=($!)
for _ in {1..100}; do [[ -S $sock ]] && break; sleep 0.1; done
[[ -S $sock ]] || { echo "mission: ttcd did not start — see $out/ttcd.txt and $out/ttcd.jsonl" >&2; exit 1; }

# One replay per mission: adsbd connects when MISSION_ADSB starts it
# (ADR-0014) and the replay serves one connection, so a new one waits as
# soon as the last client leaves, and every mission starts the capture over.
# The replay runs in the background of its loop, so that the TERM of cleanup
# reaches it: a foreground child would keep the loop deaf to the signal while
# it waits for a connection.
if [[ ${#replay[@]} -gt 0 ]]; then
    ( c=
      trap '[[ -n $c ]] && kill "$c" 2>/dev/null; exit 0' TERM
      while true; do
          "${replay[@]}" 2>> "$out/sbs_replay.txt" &
          c=$!
          wait "$c" || sleep 1
      done ) &
    pids+=($!)
fi

"$build"/flight/obcd -c "$conf/obcd.conf" -o ipc_path="$sock" \
    -o adsb_timeout_ms=$(( mission_s * 1000 )) -o log_path="$out/obcd.jsonl" > "$out/obcd.txt" 2>&1 &
pids+=($!)

"$build"/flight/adsbd -c "$conf/adsbd.conf" -o ipc_path="$sock" "${src[@]}" "${avr[@]}" \
    -o ndjson_path="$out/adsb.ndjson" -o log_path="$out/adsbd.jsonl" > "$out/adsbd.txt" 2>&1 &
pids+=($!)

# The cost on the Pi (test 3 of 20/11): dump1090 and adsbd every 5 s.
top -b -d 5 | grep --line-buffered -E 'dump1090|adsbd' > "$out/cpu.txt" &
pids+=($!)

echo "mission: MISSION_ADSB of ${mission_s} s, ${input:-the SDR} as dump1090, ${tx_power} dBm, AVR ${avr[*]:-off}; logs in $out"
echo "mission: on the PC, ./gama ground --secs $mission_s, then 'Iniciar missão ADS-B'. Ctrl-C here stops the satellite."
# Until Ctrl-C, or until one of them dies: then everything stops, and says so.
wait -n "${pids[@]}" || true
echo "mission: a process ended on its own — see the .txt and .jsonl in $out" >&2
exit 1
