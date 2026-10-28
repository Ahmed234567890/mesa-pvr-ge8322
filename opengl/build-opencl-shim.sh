#!/bin/sh
# Build the libOpenCL forwarding shim for the vendor PowerVR OpenCL driver.
#
# The shim exports the full OpenCL API and resolves each entry point from
# /vendor/lib/libPVROCL.so through libhybris on first use, so ordinary
# applications can link -lOpenCL and run on the GE8322.
#
# Requires a copy of the vendor library only to read its exported symbol list.
# The blob is not stored in this repository; pull it from the device first:
#   adb pull /vendor/lib/libPVROCL.so "$HOME/pvr-reverse/blobs/libPVROCL.so"
#
# Usage:
#   VENDOR_LIB=... ARMHF_TOOLCHAIN=... ARMHF_SYSROOT=... LIBHYBRIS_BUILD=... \
#   OUT_DIR=... opengl/build-opencl-shim.sh
set -eu

: "${VENDOR_LIB:?Set VENDOR_LIB to a copy of the vendor libPVROCL.so}"
: "${ARMHF_TOOLCHAIN:?Set ARMHF_TOOLCHAIN to the Arm GNU toolchain root}"
: "${ARMHF_SYSROOT:?Set ARMHF_SYSROOT to the armhf sysroot}"
: "${LIBHYBRIS_BUILD:?Set LIBHYBRIS_BUILD to the configured libhybris build tree}"
: "${OUT_DIR:?Set OUT_DIR to a private output directory outside this repository}"

CC="$ARMHF_TOOLCHAIN/bin/arm-none-linux-gnueabihf-gcc"
COMMON_DIR="$LIBHYBRIS_BUILD/hybris/common/.libs"
INCLUDE_DIR="$LIBHYBRIS_BUILD/hybris/include"
SYSROOT_LIB="$ARMHF_SYSROOT/usr/lib/arm-linux-gnueabihf"
REPO_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

mkdir -p "$OUT_DIR"

# Only the real OpenCL entry points; bionic also exports versioned libc symbols
# such as close@LIBC, which also start with "cl".
readelf --dyn-syms -W "$VENDOR_LIB" |
    awk '$4 == "FUNC" && $5 == "GLOBAL" && $6 == "DEFAULT" { print $8 }' |
    grep -E '^cl[A-Z][A-Za-z0-9_]*$' | sort -u > "$OUT_DIR/cl-symbols.txt"

if [ ! -s "$OUT_DIR/cl-symbols.txt" ]; then
    echo "no cl* symbols found in $VENDOR_LIB" >&2
    exit 1
fi

python3 "$REPO_ROOT/opengl/gen-opencl-shim.py" \
    --header CL/cl.h \
    --header-path "$INCLUDE_DIR/CL/cl.h" \
    --symbols "$OUT_DIR/cl-symbols.txt" \
    --output "$OUT_DIR/opencl-shim.c"

"$CC" --sysroot="$ARMHF_SYSROOT" -O2 -Wall -Wextra -fPIC -shared \
    -I"$INCLUDE_DIR" \
    -o "$OUT_DIR/libOpenCL.so.1.0.0" "$OUT_DIR/opencl-shim.c" \
    -L"$COMMON_DIR" -lhybris-common \
    -L"$SYSROOT_LIB" -ldl \
    -Wl,-rpath-link,"$COMMON_DIR" -Wl,-soname,libOpenCL.so.1

ln -sf libOpenCL.so.1.0.0 "$OUT_DIR/libOpenCL.so.1"
ln -sf libOpenCL.so.1 "$OUT_DIR/libOpenCL.so"

file "$OUT_DIR/libOpenCL.so.1.0.0"
echo "symbols: $(wc -l < "$OUT_DIR/cl-symbols.txt")"
