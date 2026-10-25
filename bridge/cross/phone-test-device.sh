#!/system/bin/sh
# Device-side half of the PowerVR bridge test. Run as root via adb.
#
# Installs the staged bundle, bind-mounts the Android trees into the Kali
# chroot, and runs the display-independent smoke test.  Every recursive bind
# clone is made private with the bundled static helper: toybox mount does not
# implement --make-rprivate and silently leaves the clone in the host's shared
# peer group, where unmounting it can detach /dev (including binderfs).
set -eu

CHROOT=/data/local/nhsystem/kali-armhf
BRIDGE="$CHROOT/opt/pvr-bridge"
TARBALL=/data/local/tmp/pvr-bridge.tar
PRIVATE=/opt/pvr-bridge/bin/pvr-mount-private

echo "== installing bundle =="
rm -rf "$BRIDGE"
mkdir -p "$BRIDGE"
tar -xf "$TARBALL" -C "$BRIDGE"

[ -x "$CHROOT$PRIVATE" ] || {
    echo "missing $CHROOT$PRIVATE" >&2
    exit 1
}

echo "== creating mountpoints =="
for d in system vendor apex dev proc sys; do
    mkdir -p "$CHROOT/$d"
done

make_private() {
    # $1 is a host path under $CHROOT; the helper runs inside the chroot and
    # therefore needs the path relative to the chroot root.
    chroot "$CHROOT" "$PRIVATE" "${1#"$CHROOT"}"
}

mount_one() {
    src=$1
    dst=$2
    if grep -q " $dst " /proc/mounts; then
        # Never unmount a shared clone: propagation would detach the
        # corresponding mount (including binderfs) from the host namespace.
        make_private "$dst"
        umount -R "$dst" 2>/dev/null || umount -l "$dst" 2>/dev/null || true
    fi
    # Recursive bind: /apex and /dev contain nested mountpoints.
    mount --rbind "$src" "$dst"
    make_private "$dst"
    echo "mounted: $src -> $dst"
}

echo "== bind-mounting Android trees (no /linkerconfig) =="
mount_one /system "$CHROOT/system"
mount_one /vendor "$CHROOT/vendor"
mount_one /apex "$CHROOT/apex"
mount_one /dev "$CHROOT/dev"
mount_one /proc "$CHROOT/proc"
mount_one /sys "$CHROOT/sys"

[ -e "$CHROOT/apex/com.android.runtime/lib/bionic/libc.so" ] || {
    echo "missing bionic libc under the chroot; apex recursive bind failed" >&2
    exit 1
}

echo "== pvr-smoke =="
chroot "$CHROOT" /usr/bin/env -i \
    HOME=/root \
    PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
    PVR_NO_LAYER="${PVR_NO_LAYER:-0}" \
    PVR_DEBUG="${PVR_DEBUG:-0}" \
    /bin/bash -lc '. /opt/pvr-bridge/bin/pvr-env; pvr-smoke'
