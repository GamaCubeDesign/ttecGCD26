#!/bin/sh
# flight/ttcd/gama-link without root nor systemd: a temporary unit directory
# and a systemctl that records its calls and answers is-active from a file.
#
#   sh test_gama_link.sh <path to gama-link>

set -eu
link=$1
t=$(mktemp -d)
trap 'rm -rf "$t"' EXIT
cat > "$t/systemctl" <<EOF
#!/bin/sh
echo "\$*" >> "$t/calls"
if [ "\$1" = is-active ]; then cat "$t/active"; fi
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
restart ttcd
is-active ttcd" ] || fail "only ttcd restarts: $(cat "$t/calls")"
[ "$(sh "$link" status | head -n 1)" = "LINK ssh" ] || fail "status on ssh"

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
