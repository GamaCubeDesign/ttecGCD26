#!/bin/sh
# flight/obcd/gama-deploy against a real obcd with no hardware: the signal
# becomes the BASIC_INTERMEDIATE pulse and the way back to PRE_TEST, and
# the script prints obcd's lines and the RESULT line that gama deploy and
# the ground's page read. Usage: test_gama_deploy.sh <obcd> <gama-deploy>
set -u
obcd=$1
deploy=$2
dir=$(mktemp -d /tmp/gama-deploy-XXXXXX)
pid=
cleanup() {
    [ -n "$pid" ] && kill "$pid" 2>/dev/null && wait "$pid" 2>/dev/null
    rm -rf "$dir"
}
trap cleanup EXIT
fail() { echo "FAIL: $*"; echo "--- output"; cat "$dir/out" 2>/dev/null; exit 1; }

cat > "$dir/obcd.conf" <<CONF
ipc_path = $dir/no-ttcd.sock
log_path = $dir/obcd.jsonl
retry_ms = 200
w1_path = $dir/no-w1
i2c_dev = $dir/no-i2c
i2c_lease_path = $dir/obcd-i2c.lease
gpio_chip = $dir/no-gpiochip
burnwire_ms = 300
CONF

"$obcd" -c "$dir/obcd.conf" > "$dir/obcd.txt" 2>&1 &
pid=$!
i=0
until grep -q '"event":"start"' "$dir/obcd.jsonl" 2>/dev/null; do
    i=$((i + 1)); [ $i -lt 50 ] || fail "obcd did not start"; sleep 0.1
done
sleep 0.3

GAMA_DEPLOY_PID=$pid "$deploy" > "$dir/out" 2>&1
code=$?
[ $code -eq 0 ] || fail "exit $code, expected 0"
grep -q '"event":"local_command","signal":10' "$dir/out" || fail "no local_command line"
grep -q '"to":"BASIC_INTERMEDIATE","event":"TC_BASIC_INTER","by":"signal"' "$dir/out" || fail "no entry into BASIC_INTERMEDIATE"
grep -q '"event":"burnwire","on":true,"why":"deploy"' "$dir/out" || fail "no burn-wire pulse"
grep -q '"to":"PRE_TEST"' "$dir/out" || fail "not back in PRE_TEST"
tail -n 1 "$dir/out" | grep -q "^RESULT done pid=$pid log=$dir/obcd.jsonl\$" || fail "bad RESULT line"
[ "$(grep -c '"event":"start"' "$dir/out")" -eq 0 ] || fail "printed lines from before the signal"
kill -0 "$pid" || fail "obcd is gone"

GAMA_DEPLOY_PID=999999999 "$deploy" > "$dir/out" 2>&1
[ $? -eq 3 ] || fail "a missing process should exit 3"
echo "test_gama_deploy: ok"
