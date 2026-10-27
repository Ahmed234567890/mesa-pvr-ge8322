#!/system/bin/sh
# Safely unmount the Android bind mounts from the Kali NetHunter chroot.
#
# This never removes files: it only detaches /system, /vendor, /apex, /dev,
# /proc and /sys from the chroot, innermost first.  The chroot itself and
# /opt/pvr-bridge (Prism stage, GPU bridge, encoder tools) are untouched.
set -u
CHROOT=/data/local/nhsystem/kali-armhf

if [ -z "$CHROOT" ] || [ "$CHROOT" = "/" ]; then
    echo "refusing to unmount with CHROOT='$CHROOT'" >&2
    exit 1
fi
if [ ! -d "$CHROOT" ]; then
    echo "chroot not found: $CHROOT" >&2
    exit 1
fi

for d in sys proc dev apex vendor system; do
    target="$CHROOT/$d"
    if grep -q " $target " /proc/mounts 2>/dev/null; then
        umount -R "$target" 2>/dev/null \
            || umount -l -R "$target" 2>/dev/null \
            || umount -l "$target" 2>/dev/null \
            || true
        if grep -q " $target " /proc/mounts 2>/dev/null; then
            echo "still mounted (busy?): $target" >&2
        else
            echo "unmounted: $target"
        fi
    fi
done
