#!/bin/sh
# Hardware check for the CoreAudio open-failure hardening (2026-09-05):
#   A) exclusive bit-perfect requested on the built-in output -> must fall back to shared output
#   B) device that does not exist -> process must stay alive and retry with backoff (no exit(1))
#   C) same as A with 'strict' -> no fallback, stays alive, retries with backoff
# Runs three throwaway instances with their own MACs/names; installs nothing, plays nothing.
# Usage (on the Mac that runs LMS, with the freshly built binary next to this script's repo):
#   sh tools/audit-device-fallback.sh /path/to/apple-squeezer-intel [builtin-device-name]
set -u
BIN=${1:?path to apple-squeezer-intel}
# resolve to an absolute path before the cd below, otherwise a relative path stops working
case "$BIN" in /*) ;; *) BIN="$(cd "$(dirname "$BIN")" && pwd)/$(basename "$BIN")" ;; esac
[ -x "$BIN" ] || { echo "not executable: $BIN"; exit 2; }
BUILTIN=${2:-"AppleHDAEngineOutput:1B,0,1,1:0"}
DIR=${TMPDIR:-/tmp}/apple-squeezer-audit; mkdir -p "$DIR"; cd "$DIR" || exit 1
rm -f a.log b.log c.log
"$BIN" -s 127.0.0.1 -n AuditFallback -m 02:41:53:49:4e:55 -o "$BUILTIN" -a mode=native:exclusive:bitperfect:profile=safe -f "$DIR/a.log" -d all=info & PA=$!
"$BIN" -s 127.0.0.1 -n AuditMissing  -m 02:41:53:49:4e:56 -o "NoSuchDevice"  -a mode=native:profile=safe -f "$DIR/b.log" -d all=info & PB=$!
"$BIN" -s 127.0.0.1 -n AuditStrict   -m 02:41:53:49:4e:57 -o "$BUILTIN" -a mode=native:exclusive:bitperfect:profile=safe:strict -f "$DIR/c.log" -d all=info & PC=$!
sleep 45
fail=0
alive() { kill -0 "$1" 2>/dev/null; }
echo "== A (fallback expected) =="; alive $PA && echo "alive: yes" || { echo "alive: NO"; fail=1; }
grep -q "falling back to shared" a.log && grep -q "opened CoreAudio device" a.log && echo "fallback + open: yes" || { echo "fallback + open: NO"; fail=1; }
grep ca_monitor a.log | tail -1 | grep -q "fallback=shared" && echo "telemetry fallback=shared: yes" || { echo "telemetry fallback=shared: NO"; fail=1; }
echo "== B (missing device: stay alive, backoff) =="; alive $PB && echo "alive: yes" || { echo "alive: NO"; fail=1; }
grep -q "not available at startup" b.log && echo "startup message: yes" || { echo "startup message: NO"; fail=1; }
n=$(grep -c "unable to open CoreAudio" b.log); echo "open attempts in 45 s: $n (expected about 6, was hundreds before)"; [ "$n" -le 10 ] || fail=1
grep -q "slimproto.*connected" b.log && echo "connected to LMS without a device: yes" || { echo "connected to LMS: NO"; fail=1; }
echo "== C (strict: no fallback, alive) =="; alive $PC && echo "alive: yes" || { echo "alive: NO"; fail=1; }
grep -q "falling back" c.log && { echo "fallback happened despite strict: NO"; fail=1; } || echo "no fallback: yes"
kill $PA $PB $PC 2>/dev/null; sleep 1
echo "logs in $DIR"; [ $fail -eq 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit $fail
