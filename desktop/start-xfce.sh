#!/bin/sh
# Start XFCE on Termux:X11 with a working session bus.
#
# Run inside the Kali chroot.  Two environment details matter:
#
#   SHELL   The Android root shell that starts the chroot usually exports
#           SHELL=/system/bin/sh.  If that leaks into XFCE, every terminal
#           tries to start Android's /system/bin/sh instead of a Kali shell.
#
#   LD_PRELOAD
#           The SELinux shim is only needed by dbus-daemon.  It is a glibc
#           library, so exporting it to the whole session makes any Android
#           binary started from the desktop fail in Android's linker with
#           "library libc.so.6 not found".  Scope it to dbus-daemon only.
set -eu

DISPLAY=${DISPLAY:-:1}
HOME=${HOME:-/root}
SHELL=/bin/bash
TMPDIR=/tmp
XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/tmp/xfce-runtime}
SELINUX_SHIM=${SELINUX_SHIM:-/opt/pvr-bridge/desktop/lib/libselinux-off.so}
GPU_ENV=${GPU_ENV:-/opt/pvr-bridge/desktop/bin/gpu-env}

export DISPLAY HOME SHELL TMPDIR XDG_RUNTIME_DIR
unset LD_PRELOAD 2>/dev/null || true

# Give the whole session the vendor GPU stack (EGL/GLES/GL4ES). No preload is
# involved: the ctype/TLS collision is handled inside the staged libEGL.
if [ -r "$GPU_ENV" ]; then
    set --
    . "$GPU_ENV"
fi

mkdir -p "$XDG_RUNTIME_DIR"
chmod 700 "$XDG_RUNTIME_DIR"

if [ -z "${DBUS_SESSION_BUS_ADDRESS:-}" ]; then
    DBUS_SESSION_BUS_ADDRESS=$(
        LD_PRELOAD="$SELINUX_SHIM" dbus-daemon --session --print-address --fork
    )
    export DBUS_SESSION_BUS_ADDRESS
fi

exec startxfce4
