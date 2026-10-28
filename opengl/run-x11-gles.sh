#!/bin/sh
# Run an EGL/GLES application in the isolated GE8322/libhybris environment.
set -eu

if [ "$#" -eq 0 ]; then
    echo "usage: $0 APPLICATION [ARG ...]" >&2
    exit 2
fi

DISPLAY=${DISPLAY:-:1}
ANDROID_ROOT=${ANDROID_ROOT:-/system}
ANDROID_DATA=${ANDROID_DATA:-/data}
HYBRIS_LINKER_DIR=${HYBRIS_LINKER_DIR:-/opt/pvr-bridge/libhybris/linker}
HYBRIS_ANDROID_SDK_VERSION=${HYBRIS_ANDROID_SDK_VERSION:-34}
HYBRIS_LD_LIBRARY_PATH=${HYBRIS_LD_LIBRARY_PATH:-/system/lib:/vendor/lib:/vendor/lib/hw:/odm/lib:/apex/com.android.runtime/lib}
HYBRIS_EGLPLATFORM=x11
HYBRIS_EGLPLATFORM_DIR=${HYBRIS_EGLPLATFORM_DIR:-/opt/pvr-bridge/opengl/eglplatform}
# SysV SHM can fail from the Kali chroot; libhybris's X11 path otherwise
# passes shmat's (void *)-1 result to memcpy. XPutImage is the safe fallback.
HYBRIS_X11_DISABLE_SHM=${HYBRIS_X11_DISABLE_SHM:-1}
# Caller-supplied paths win, so an alternative libGL (GL4ES) can be selected.
LD_LIBRARY_PATH="${LD_LIBRARY_PATH:+$LD_LIBRARY_PATH:}/opt/pvr-bridge/opengl/lib:/opt/pvr-bridge/lib:/usr/lib/arm-linux-gnueabihf"
# Android's libEGL constructor writes bionic TLS slot 4 (thread pointer + 16),
# which is where glibc keeps its thread-local __ctype_b pointer. Without the
# fix, Xlib's locale parser reads a bogus ctype table and walks off the stack
# inside eglCreateWindowSurface.
LD_PRELOAD="/opt/pvr-bridge/opengl/lib/libhybris-ctype-fix.so${LD_PRELOAD:+:$LD_PRELOAD}"

export DISPLAY ANDROID_ROOT ANDROID_DATA HYBRIS_LINKER_DIR
export HYBRIS_ANDROID_SDK_VERSION HYBRIS_LD_LIBRARY_PATH
export HYBRIS_EGLPLATFORM HYBRIS_EGLPLATFORM_DIR HYBRIS_X11_DISABLE_SHM
export LD_LIBRARY_PATH LD_PRELOAD

exec "$@"
