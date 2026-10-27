#!/usr/bin/env bash
# Host-side driver for the GE8322 verification matrix.
#
# Usage:
#   ADB_SERIAL=192.168.1.16:46767 scripts/verify-all.sh
#   FULL=1 ADB_SERIAL=... scripts/verify-all.sh
#
# The device writes its own log to /data/local/tmp/verify-all.log and the host
# pulls it afterwards.  That way a Wi-Fi ADB drop under load loses the console
# but not the results.
set -u

: "${ADB_SERIAL:?Set ADB_SERIAL, for example ADB_SERIAL=192.168.1.16:46767}"

ROOT=$(cd -- "$(dirname -- "$0")/.." && pwd)
OUT_DIR=${OUT_DIR:-/tmp/opencode}
FULL=${FULL:-0}
DEV_LOG=/data/local/tmp/verify-all.log
LOG="$OUT_DIR/verify-all-$(date +%Y%m%d-%H%M%S).log"
ADB=(adb -s "$ADB_SERIAL")

mkdir -p "$OUT_DIR"

{
    echo "host: $(uname -srm)"
    echo "repo: $ROOT"
    echo "head: $(git -C "$ROOT" log --oneline -1 2>/dev/null || echo unknown)"
    if [ -n "$(git -C "$ROOT" status --porcelain 2>/dev/null)" ]; then
        echo "warning: working tree is dirty"
    fi
    echo
} | tee "$LOG"

"${ADB[@]}" wait-for-device
"${ADB[@]}" push "$ROOT/scripts/verify-all-device.sh" \
    /data/local/tmp/verify-all-device.sh >/dev/null || {
    echo "push failed" | tee -a "$LOG"
    exit 1
}

# Run detached from the ADB console; the log file is the source of truth.
"${ADB[@]}" shell "su -c 'rm -f $DEV_LOG; FULL=$FULL sh /data/local/tmp/verify-all-device.sh > $DEV_LOG 2>&1; echo rc=\$? >> $DEV_LOG'" \
    >/dev/null 2>&1

# The link can drop while the device is still running the matrix.
"${ADB[@]}" wait-for-device

pulled=0
for _ in $(seq 1 40); do
    if "${ADB[@]}" pull "$DEV_LOG" "$LOG.device" >/dev/null 2>&1; then
        pulled=1
        break
    fi
    sleep 3
done

if [ "$pulled" -ne 1 ]; then
    echo "could not pull $DEV_LOG from the device" | tee -a "$LOG"
    exit 1
fi

cat "$LOG.device" >>"$LOG"
cat "$LOG.device"

rc_line=$(sed -n 's/^rc=//p' "$LOG.device" | tail -n 1)
rm -f "$LOG.device"

echo
echo "log: $LOG"
exit "${rc_line:-1}"
