#!/usr/bin/env bash
# run_step.sh — one step of the bench procedure (flight/ttcd/README.md).
#
#   run_step.sh COMMANDS PREFIX
#
# Starts ttcd in the background, feeds gs_cli the command file, stops ttcd,
# and prints what decides the step. Leaves PREFIX-ttcd.jsonl and
# PREFIX-ground.jsonl behind: the step's test evidence.
#
# ttcd starts afresh for every step so that each begins in the same state —
# both ends on NOMINAL, counters at zero — however long the pause before it.
# A satellite kept running between steps falls to SAFE after two minutes
# without hearing the ground (contact_timeout_ms), and the next step would
# open with the link recovering instead of with its measurement.
#
# The Makefile runs it (make bench-<step>) and sets, in the environment:
#   TTCD_CMD  ttcd and its options, without log_path
#   GS_CMD    gs_cli and its options
#   SOCK      the ipc_path given in TTCD_CMD
#   TTCD_SSH  optional: user@host. ttcd then runs there, in TTCD_DIR (default
#             ttec, relative to the home), while the ground stays on this
#             machine — the ground on the PC, ttcd on the Pi (make bench
#             SPLIT=1). Its log is copied back to PREFIX-ttcd.jsonl at the end.

set -euo pipefail

commands=$1
prefix=$2
sat=$prefix-ttcd.jsonl
gnd=$prefix-ground.jsonl
read -r -a ttcd <<< "$TTCD_CMD"
read -r -a gs <<< "$GS_CMD"
mkdir -p "$(dirname "$prefix")"

remote=${TTCD_SSH:-}
rdir=${TTCD_DIR:-ttec}
rsh() {
    ssh -o BatchMode=yes -o ControlMaster=auto -o ControlPath="/tmp/ttec-bench-%C" \
        -o ControlPersist=60 "$remote" "$@"
}
sock_ready() {
    if [[ -n $remote ]]; then rsh "test -S '$SOCK'"; else [[ -S $SOCK ]]; fi
}
stop_ttcd() {
    if [[ -n $remote ]]; then rsh 'pkill -TERM -x ttcd' || true
    else kill -TERM "$pid" 2>/dev/null || true; fi
}

# A socket left by a ttcd that did not exit cleanly would pass for readiness.
if [[ -n $remote ]]; then
    if rsh 'pgrep -x ttcd' >/dev/null; then
        echo "bench: a ttcd is already running on $remote and holds radio A" >&2
        exit 1
    fi
    rsh "rm -f -- '$SOCK'"
elif [[ -S $SOCK ]]; then
    rm -f -- "$SOCK"
fi

if [[ -n $remote ]]; then
    printf -v rcmd '%q ' "${ttcd[@]}" -o "log_path=$sat"
    rsh "cd '$rdir' && mkdir -p '$(dirname "$sat")' && exec $rcmd" &
else
    "${ttcd[@]}" -o log_path="$sat" &
fi
pid=$!
trap 'stop_ttcd; wait "$pid" 2>/dev/null || true' EXIT

# ttcd opens its socket once the radio has answered and is receiving.
for _ in {1..100}; do
    sock_ready && break
    if ! kill -0 "$pid" 2>/dev/null; then
        echo "bench: ttcd did not start — see $sat" >&2
        exit 1
    fi
    sleep 0.1
done
if ! sock_ready; then
    echo "bench: ttcd did not open $SOCK in time — see $sat" >&2
    exit 1
fi

echo "bench: ${remote:+$remote: }${ttcd[*]} -o log_path=$sat"
echo "bench: ${gs[*]} < $commands | tee -a $gnd"
"${gs[@]}" < "$commands" | tee -a "$gnd"

stop_ttcd
status=0
wait "$pid" || status=$?
trap - EXIT
if [[ -n $remote ]]; then
    rsh "cd '$rdir' && cat '$sat'" > "$sat" || echo "bench: could not copy $sat from $remote" >&2
fi

# What decides the step: the ground's results and every rate, contact or
# failure event on either side; then how strong the link was, and a count of
# everything ttcd logged.
echo
echo "== $(basename "$prefix")"
grep -E '"ev":"(ack|ping_summary|per|rate|contact|tc_failed|error|summary)"' "$gnd" \
    | grep -v '"cmd":"PING"' || true
grep -E '"event":"(rate|rate_commit|contact_lost|tx_watchdog|radio_error|log_full)"' "$sat" || true
grep -oE '"rssi":-?[0-9]+,"snr":-?[0-9]+' "$gnd" | sed -E 's/[^0-9-]+/ /g' | awk '
    { n++; r += $1; s += $2
      if (n == 1 || $1 < r0) r0 = $1
      if (n == 1 || $1 > r1) r1 = $1
      if (n == 1 || $2 < s0) s0 = $2
      if (n == 1 || $2 > s1) s1 = $2 }
    END { if (n) printf "ground heard %d frames: RSSI %d to %d dBm (mean %.1f), SNR %d to %d dB (mean %.1f)\n",
                        n, r0, r1, r / n, s0, s1, s / n
          else print "ground heard no frame at all" }' || true
grep -oE '"event":"[a-z_]+"' "$sat" | cut -d'"' -f4 | sort | uniq -c | sort -rn \
    | awk '{ printf "%s %s %s", NR == 1 ? "ttcd logged" : ",", $1, $2 } END { if (NR) print "" }' || true

if (( status != 0 )); then
    echo "bench: ttcd exited with status $status — see $sat" >&2
    exit 1
fi
