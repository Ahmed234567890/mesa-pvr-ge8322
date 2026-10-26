#!/bin/sh
set -eu

: "${ARMHF_TOOLCHAIN:?Set ARMHF_TOOLCHAIN to the Arm GNU toolchain root}"
: "${ARMHF_SYSROOT:?Set ARMHF_SYSROOT to the armhf sysroot}"
: "${LIBHYBRIS_BUILD:?Set LIBHYBRIS_BUILD to the configured libhybris build tree}"
: "${OUT_DIR:?Set OUT_DIR to a private output directory outside this repository}"

CC="$ARMHF_TOOLCHAIN/bin/arm-none-linux-gnueabihf-gcc"
EGL_DIR="$LIBHYBRIS_BUILD/hybris/egl/.libs"
GLES_DIR="$LIBHYBRIS_BUILD/hybris/glesv2/.libs"
COMMON_DIR="$LIBHYBRIS_BUILD/hybris/common/.libs"
HARDWARE_DIR="$LIBHYBRIS_BUILD/hybris/hardware/.libs"
SYSROOT_LIB="$ARMHF_SYSROOT/usr/lib/arm-linux-gnueabihf"
REPO_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

mkdir -p "$OUT_DIR"
COMMON_FLAGS="--sysroot=$ARMHF_SYSROOT -O2 -Wall -Wextra -I$LIBHYBRIS_BUILD/hybris/include"
LINK_FLAGS="-L$EGL_DIR -L$GLES_DIR -L$SYSROOT_LIB -Wl,-rpath-link,$SYSROOT_LIB -Wl,-rpath-link,$COMMON_DIR -Wl,-rpath-link,$HARDWARE_DIR -lEGL -lGLESv2"

# Keep flag splitting intentional: toolchain/sysroot paths must not contain spaces.
# shellcheck disable=SC2086
"$CC" $COMMON_FLAGS -o "$OUT_DIR/egl-gles2-smoke" \
    "$REPO_ROOT/opengl/egl-gles2-smoke.c" $LINK_FLAGS

# shellcheck disable=SC2086
"$CC" $COMMON_FLAGS -o "$OUT_DIR/egl-gles2-x11-smoke" \
    "$REPO_ROOT/opengl/egl-gles2-x11-smoke.c" $LINK_FLAGS -lX11 -lxcb

# shellcheck disable=SC2086
"$CC" $COMMON_FLAGS -o "$OUT_DIR/egl-loader-smoke" \
    "$REPO_ROOT/opengl/egl-loader-smoke.c" -ldl

file "$OUT_DIR/egl-gles2-smoke" "$OUT_DIR/egl-gles2-x11-smoke" \
     "$OUT_DIR/egl-loader-smoke"
