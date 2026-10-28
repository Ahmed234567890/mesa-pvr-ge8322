#!/bin/bash
# Build the SPRD hardware encoder tool and install it into the Kali chroot.
#
# Host requirements:
#   - adb with the phone connected (set SERIAL to pick a device)
#   - an ARM cross compiler for the freestanding shim (set CXX)
#   - OMX headers fetched with ./fetch-omx-headers.sh
#
# Phone requirements:
#   - root (Magisk/KernelSU) and the Kali chroot at /data/local/nhsystem/kali-armhf
#
# Environment:
#   CXX      cross compiler (default: arm-none-linux-gnueabihf-g++)
#   ADB      adb binary     (default: adb)
#   SERIAL   device serial  (default: adb's only device)
#   HEADERS  OMX header dir (default: ./.omx-headers)
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
CXX=${CXX:-arm-none-linux-gnueabihf-g++}
ADB=${ADB:-adb}
CHROOT=/data/local/nhsystem/kali-armhf
HEADERS=${HEADERS:-"$HERE/.omx-headers"}
BUILD=${BUILD:-"$HERE/build"}

adb_cmd() {
    if [ -n "${SERIAL:-}" ]; then
        "$ADB" -s "$SERIAL" "$@"
    else
        "$ADB" "$@"
    fi
}

if [ ! -f "$HEADERS/OMX_Core.h" ]; then
    echo "missing OMX headers in $HEADERS; run $HERE/fetch-omx-headers.sh" >&2
    exit 1
fi

mkdir -p "$BUILD"

echo "== building libgbm-shim.so =="
"$CXX" -O2 -fPIC -shared -nostdlib -fno-exceptions -fno-rtti \
    -Wl,--version-script="$HERE/src/gbm-shim.map" \
    -Wl,-z,global -Wl,-soname,libgbm-shim.so \
    -o "$BUILD/libgbm-shim.so" "$HERE/src/gbm-shim.cpp"

echo "== pushing sources =="
adb_cmd shell "mkdir -p /data/local/tmp/omx-headers"
adb_cmd push "$HEADERS/." /data/local/tmp/omx-headers/
adb_cmd push "$BUILD/libgbm-shim.so" /data/local/tmp/libgbm-shim.so
adb_cmd push "$HERE/src/omx-enc.c" /data/local/tmp/omx-enc.c
adb_cmd push "$HERE/src/omx-dec.c" /data/local/tmp/omx-dec.c
adb_cmd push "$HERE/phone/build-omx-enc.sh" /data/local/tmp/build-omx-enc.sh

echo "== building inside the chroot =="
adb_cmd shell "su -c 'sh /data/local/tmp/build-omx-enc.sh'"

echo "== installing wrappers =="
adb_cmd shell "su -c 'mkdir -p $CHROOT/opt/pvr-bridge/encoders/bin'"
adb_cmd push "$HERE/omx-enc-env" "$CHROOT/opt/pvr-bridge/encoders/bin/omx-enc-env"
adb_cmd push "$HERE/omx-dec-env" "$CHROOT/opt/pvr-bridge/encoders/bin/omx-dec-env"
adb_cmd shell "su -c 'chmod 755 $CHROOT/opt/pvr-bridge/encoders/bin/omx-enc-env $CHROOT/opt/pvr-bridge/encoders/bin/omx-dec-env'"

echo "done: /opt/pvr-bridge/encoders/bin/{omx-enc,omx-dec} (inside Kali)"
