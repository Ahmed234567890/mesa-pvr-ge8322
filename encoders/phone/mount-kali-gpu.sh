#!/system/bin/sh
set -eu
CHROOT=/data/local/nhsystem/kali-armhf
PRIVATE=/opt/pvr-bridge/bin/pvr-mount-private
[ -x "$CHROOT$PRIVATE" ] || { echo "missing $CHROOT$PRIVATE" >&2; exit 1; }
for d in system vendor apex dev proc sys; do mkdir -p "$CHROOT/$d"; done
make_private() { chroot "$CHROOT" "$PRIVATE" "$1"; }
mount_one() {
  src=$1; dst=$2; rel=$3
  if grep -q " $dst " /proc/mounts; then
    make_private "$rel" || true
    umount -R "$dst" 2>/dev/null || umount -l "$dst" 2>/dev/null || true
  fi
  mount --rbind "$src" "$dst"
  make_private "$rel"
  echo "mounted: $src -> $dst"
}
mount_one /system "$CHROOT/system" /system
mount_one /vendor "$CHROOT/vendor" /vendor
mount_one /apex "$CHROOT/apex" /apex
mount_one /dev "$CHROOT/dev" /dev
mount_one /proc "$CHROOT/proc" /proc
mount_one /sys "$CHROOT/sys" /sys
echo "== device check =="
chroot "$CHROOT" /usr/bin/env -i HOME=/root PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin /bin/sh -c 'ls -l /dev/dri /dev/pvr_sync /dev/ion /dev/sprd_vsp /dev/binder /dev/ashmem 2>/dev/null; echo; head -1 /proc/version'
