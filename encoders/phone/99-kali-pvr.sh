#!/system/bin/sh
# Bind the Android /system, /vendor, /apex, /dev, /proc and /sys trees into the
# Kali NetHunter chroot at boot, so the PowerVR GE8322 GPU and the SPRD
# hardware video encoders are usable inside Kali.
#
# This runs from Magisk's service.d (late_start), after /data is decrypted and
# the framework is up.  It is safe to run again by hand at any time; the mount
# helper unmounts stale entries first.
LOG=/data/local/tmp/kali-pvr-mount.log

(
    i=0
    while [ "$(getprop sys.boot_completed)" != "1" ] && [ $i -lt 150 ]; do
        sleep 2
        i=$((i + 1))
    done
    # Give NetHunter's own boot scripts a moment to settle.
    sleep 5

    echo "=== $(date) boot mount ===" >> "$LOG"
    if [ -x /data/local/tmp/mount-kali-gpu.sh ]; then
        /data/local/tmp/mount-kali-gpu.sh >> "$LOG" 2>&1
        echo "mount rc=$?" >> "$LOG"
    else
        echo "missing /data/local/tmp/mount-kali-gpu.sh" >> "$LOG"
    fi
) &
