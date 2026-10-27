#!/system/bin/sh
# Build omx-enc inside the Kali chroot from sources pushed to /data/local/tmp.
# This runs on the phone, as root.  See ../build-omx-enc.sh for the host side.
set -eu
CHROOT=/data/local/nhsystem/kali-armhf
mkdir -p "$CHROOT/tmp/omx-headers"
cp /data/local/tmp/omx-headers/*.h "$CHROOT/tmp/omx-headers/"
cp /data/local/tmp/omx-enc.c "$CHROOT/tmp/"
mkdir -p "$CHROOT/opt/pvr-bridge/encoders/lib"
cp /data/local/tmp/libgbm-shim.so "$CHROOT/opt/pvr-bridge/encoders/lib/"
chroot "$CHROOT" /usr/bin/env -i HOME=/root PATH=/usr/bin:/bin /bin/sh -c '
set -e
mkdir -p /opt/pvr-bridge/encoders/include /opt/pvr-bridge/encoders/bin /opt/pvr-bridge/encoders/src
cp /tmp/omx-headers/*.h /opt/pvr-bridge/encoders/include/
cp /tmp/omx-enc.c /opt/pvr-bridge/encoders/src/
cd /opt/pvr-bridge/encoders
gcc -O2 -g -Wall -Iinclude -o bin/omx-enc src/omx-enc.c -ldl -lpthread
echo "== built =="
ls -l bin/omx-enc lib/libgbm-shim.so
'
