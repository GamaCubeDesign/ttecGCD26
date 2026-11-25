#!/bin/sh
# flight/ttcd/gama-link without root nor systemd: a temporary unit directory
# and a systemctl that records its calls and answers is-active from a file.
# With the file "limit" present it plays systemd's start limit: a restart
# fails and leaves ttcd failed until reset-failed removes the file.
#
#   sh test_gama_link.sh <path to gama-link>

set -eu
link=$1
t=$(mktemp -d)
trap 'rm -rf "$t"' EXIT
cat > "$t/systemctl" <<EOF
#!/bin/sh
echo "\$*" >> "$t/calls"
case \$1 in
is-active) cat "$t/active" ;;
reset-failed) rm -f "$t/limit" ;;
restart)
    if [ -e "$t/limit" ]; then
        echo failed > "$t/active"
        echo "Job for ttcd.service failed because start of the service was attempted too often." >&2
        exit 1
    fi ;;
esac
EOF
chmod +x "$t/systemctl"
export GAMA_LINK_DIR="$t/ttcd.service.d" GAMA_LINK_SYSTEMCTL="$t/systemctl"
fail() { echo "FAIL: $*"; exit 1; }

echo active > "$t/active"
out=$(sh "$link" status)
[ "$out" = "LINK lora
TTCD active" ] || fail "status on the radio: $out"

: > "$t/calls"
out=$(sh "$link" ssh) || fail "ssh exit $?"
[ "$out" = "RESULT ssh active" ] || fail "ssh: $out"
grep -q '^ExecStart=$' "$GAMA_LINK_DIR/link-ssh.conf" || fail "the drop-in does not clear ExecStart"
grep -q -- '-o radio=udp -o udp_bind=127.0.0.1 -o udp_port=47001' "$GAMA_LINK_DIR/link-ssh.conf" || fail "the drop-in's line"
[ "$(cat "$t/calls")" = "daemon-reload
reset-failed ttcd
restart ttcd
is-active ttcd" ] || fail "only ttcd restarts: $(cat "$t/calls")"
[ "$(sh "$link" status | head -n 1)" = "LINK ssh" ] || fail "status on ssh"

# From an RA-02 that does not answer (25/11): ttcd restarting every 2 s, at
# systemd's start limit. The move to ssh must still take at the first try.
rm -f "$GAMA_LINK_DIR/link-ssh.conf"
touch "$t/limit"
echo active > "$t/active"
out=$(sh "$link" ssh 2>&1) || fail "ssh at the start limit: exit $?, $out"
[ "$out" = "RESULT ssh active" ] || fail "ssh at the start limit: $out"

# Back on an RA-02 that does not answer: the link changes, ttcd restarts in vain.
echo activating > "$t/active"
: > "$t/calls"
set +e
out=$(sh "$link" lora); code=$?
set -e
[ $code -eq 1 ] || fail "lora with ttcd not active: exit $code"
[ "$out" = "RESULT lora activating" ] || fail "lora: $out"
[ ! -e "$GAMA_LINK_DIR/link-ssh.conf" ] || fail "the drop-in is still there"

set +e
sh "$link" sideways 2>/dev/null; code=$?
set -e
[ $code -eq 2 ] || fail "bad usage: exit $code"
echo "gama-link: ok"
