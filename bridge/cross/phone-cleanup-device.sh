#!/system/bin/sh
# Unmount the Android trees from the Kali chroot. The bundle is kept by
# default; pass --remove-bundle to delete it too.
#
# The static helper makes each clone private before unmounting.  Without it,
# unmounting a clone that is still in the host's shared peer group propagates
# to the host and can remove /dev entries, including binderfs.
set -eu

CHROOT=/data/local/nhsystem/kali-armhf
BRIDGE="$CHROOT/opt/pvr-bridge"
PRIVATE=/opt/pvr-bridge/bin/pvr-mount-private
FALLBACK=/data/local/tmp/pvr-mount-private
REMOVE_BUNDLE=0
[ "${1:-}" = "--remove-bundle" ] && REMOVE_BUNDLE=1

if [ -x "$CHROOT$PRIVATE" ]; then
    HELPER=$PRIVATE
elif [ -x "$FALLBACK" ]; then
    HELPER=$FALLBACK
else
    echo "no pvr-mount-private helper; refusing to unmount shared clones" >&2
    echo "expected $CHROOT$PRIVATE or $FALLBACK" >&2
    exit 1
fi

for d in sys proc dev apex vendor system; do
    if grep -q " $CHROOT/$d " /proc/mounts; then
        # Detach only the chroot clone, after making it private.
        if [ "$HELPER" = "$PRIVATE" ]; then
            chroot "$CHROOT" "$PRIVATE" "/$d"
        else
            "$HELPER" "$CHROOT/$d"
        fi
        umount -R "$CHROOT/$d" 2>/dev/null || umount -l "$CHROOT/$d"
        echo "unmounted: $CHROOT/$d"
    fi
done

if [ "$REMOVE_BUNDLE" = "1" ]; then
    rm -rf "$BRIDGE"
    echo "removed: $BRIDGE"
fi
