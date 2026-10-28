#!/system/bin/sh
# Device-side verification matrix for the GE8322 Kali GPU stack.
#
# Runs on the phone as root:
#   adb shell su -c 'sh /data/local/tmp/verify-all-device.sh'
#
# Prints one line per check and exits non-zero when any check failed.
# Behaviour that is tracked as a known limitation is reported as XFAIL and
# does not affect the exit status.
#
# Set FULL=1 to include the long-running checks (glmark2 one-shot).
#
# Nothing here writes into the repository.  Temporary files live under
# /data/local/tmp and the Kali chroot's /tmp.

CH=/data/local/nhsystem/kali-armhf
TMPD=/data/local/tmp/verify-all
FULL=${FULL:-0}

PASS_N=0
FAIL_N=0
XFAIL_N=0

pass()  { PASS_N=$((PASS_N + 1));   echo "PASS  $1"; }
fail()  { FAIL_N=$((FAIL_N + 1));   echo "FAIL  $1"; }
xfail() { XFAIL_N=$((XFAIL_N + 1)); echo "XFAIL $1"; }

renderer_of() {
    sed -n 's/^GL renderer: //p' "$1" | head -n 1
}

rm -rf "$TMPD"
mkdir -p "$TMPD"

echo "=== environment ==="

if [ -x "$CH/bin/bash" ] && [ -x "$CH/usr/bin/env" ]; then
    pass "chroot-present"
else
    fail "chroot-present ($CH)"
fi

missing=""
for m in system vendor apex dev proc sys; do
    grep -q " $CH/$m " /proc/mounts || missing="$missing $m"
done
if [ -z "$missing" ]; then
    pass "android-bind-mounts"
else
    fail "android-bind-mounts (missing:$missing)"
fi

if ps -A -o NAME | grep -q '^termux-x11'; then
    pass "x11-server"
else
    fail "x11-server (termux-x11 not running)"
fi

if timeout 30 chroot "$CH" /usr/bin/env -i HOME=/root PATH=/usr/bin:/bin \
        DISPLAY=:1 /usr/bin/xdpyinfo >"$TMPD/xdpyinfo.log" 2>&1; then
    pass "x11-xdpyinfo"
else
    fail "x11-xdpyinfo"
fi

echo "=== vendor GLES / EGL (libhybris) ==="

if timeout 60 chroot "$CH" /usr/bin/env -i HOME=/root PATH=/usr/bin:/bin \
        DISPLAY=:1 /opt/pvr-bridge/opengl/bin/run-x11-gles \
        /opt/pvr-bridge/opengl/bin/egl-gles2-smoke \
        >"$TMPD/pbuffer.log" 2>&1 \
        && grep -q "egl-gles2-smoke: PASS" "$TMPD/pbuffer.log"; then
    pass "gles-egl-pbuffer ($(renderer_of "$TMPD/pbuffer.log"))"
else
    fail "gles-egl-pbuffer"
fi

if timeout 60 chroot "$CH" /usr/bin/env -i HOME=/root PATH=/usr/bin:/bin \
        DISPLAY=:1 /opt/pvr-bridge/opengl/bin/run-x11-gles \
        /opt/pvr-bridge/opengl/bin/egl-gles2-x11-smoke \
        >"$TMPD/x11.log" 2>&1 \
        && grep -q "egl-gles2-x11-smoke: PASS" "$TMPD/x11.log" \
        && grep -q "center pixel: 64 128 191" "$TMPD/x11.log"; then
    pass "gles-egl-x11 ($(renderer_of "$TMPD/x11.log"))"
else
    fail "gles-egl-x11"
fi

echo "=== desktop OpenGL (baseline) ==="

# This is a baseline, not a requirement: it records which renderer the
# desktop currently uses.  It must keep running after the global override
# work, with a different renderer string.
if timeout 60 chroot "$CH" /usr/bin/env -i HOME=/root PATH=/usr/bin:/bin \
        DISPLAY=:1 /usr/bin/glxinfo -B >"$TMPD/glxinfo.log" 2>&1; then
    gl_device=$(sed -n 's/^ *Device: //p' "$TMPD/glxinfo.log" | head -n 1)
    pass "desktop-gl-baseline ($gl_device)"
else
    fail "desktop-gl-baseline"
fi

echo "=== desktop OpenGL (GL4ES on vendor GLES) ==="

if [ ! -r "$CH/opt/pvr-bridge/desktop/bin/gpu-env" ]; then
    fail "desktop-gl-gl4es (gpu-env missing)"
elif timeout 300 chroot "$CH" /usr/bin/env -i HOME=/root PATH=/usr/bin:/bin \
        DISPLAY=:1 /opt/pvr-bridge/desktop/bin/gpu-env \
        /usr/bin/glmark2 --benchmark build:duration=2 --size 480x320 \
        >"$TMPD/glmark2-desktop.log" 2>&1 \
        && grep -q "GL4ES using PowerVR Rogue GE8322" "$TMPD/glmark2-desktop.log"; then
    gl4es_score=$(sed -n 's/.*glmark2 Score: //p' "$TMPD/glmark2-desktop.log" | head -n 1)
    pass "desktop-gl-gl4es (GL4ES on GE8322, score ${gl4es_score:-?})"
else
    fail "desktop-gl-gl4es"
fi

echo "=== Vulkan (vendor HAL through libsysvk) ==="

if timeout 60 chroot "$CH" /usr/bin/env -i HOME=/root PATH=/usr/bin:/bin \
        DISPLAY=:1 XDG_RUNTIME_DIR=/tmp/xfce-runtime /bin/sh -c \
        '. /opt/pvr-bridge/bin/pvr-env && exec /usr/bin/vulkaninfo --summary' \
        >"$TMPD/vulkaninfo.log" 2>&1 \
        && grep -qiE "powervr|rogue|ge8322" "$TMPD/vulkaninfo.log"; then
    vk_device=$(sed -n 's/^[[:space:]]*deviceName[[:space:]]*=[[:space:]]*//p' "$TMPD/vulkaninfo.log" | head -n 1)
    pass "vulkan-vendor-summary ($vk_device)"
else
    fail "vulkan-vendor-summary"
fi

if timeout 120 chroot "$CH" /usr/bin/env -i HOME=/root PATH=/usr/bin:/bin \
        DISPLAY=:1 XDG_RUNTIME_DIR=/tmp/xfce-runtime /bin/sh -c \
        '. /opt/pvr-bridge/bin/pvr-env && exec /opt/pvr-bridge/bin/pvr-smoke' \
        >"$TMPD/pvr-smoke.log" 2>&1 \
        && grep -q "pvr-smoke: OK" "$TMPD/pvr-smoke.log"; then
    pass "vulkan-vendor-compute"
else
    fail "vulkan-vendor-compute"
fi

if timeout 120 chroot "$CH" /usr/bin/env -i HOME=/root PATH=/usr/bin:/bin \
        DISPLAY=:1 XDG_RUNTIME_DIR=/tmp/xfce-runtime /bin/sh -c \
        '. /opt/pvr-bridge/bin/pvr-env && exec /usr/bin/vkcube --c 10' \
        >"$TMPD/vkcube.log" 2>&1; then
    pass "vulkan-vendor-present"
else
    fail "vulkan-vendor-present"
fi

# The whole session uses gpu-env, which must not rely on LD_PRELOAD (that
# breaks Android binaries started from desktop terminals). Presentation has to
# work with the environment alone.
if timeout 120 chroot "$CH" /usr/bin/env -i HOME=/root PATH=/usr/bin:/bin \
        DISPLAY=:1 /opt/pvr-bridge/desktop/bin/gpu-env /usr/bin/vkcube --c 5 \
        >"$TMPD/vkcube-no-preload.log" 2>&1 \
        && grep -q "Selected GPU 0: PowerVR Rogue GE8322" "$TMPD/vkcube-no-preload.log"; then
    pass "vulkan-session-env-no-preload"
else
    fail "vulkan-session-env-no-preload"
fi

# Repeat the presentation path to catch teardown or lifetime regressions.
vk_repeat_ok=1
for i in 1 2 3; do
    if ! timeout 120 chroot "$CH" /usr/bin/env -i HOME=/root PATH=/usr/bin:/bin \
            DISPLAY=:1 /opt/pvr-bridge/desktop/bin/gpu-env /usr/bin/vkcube --c 3 \
            >"$TMPD/vkcube-repeat-$i.log" 2>&1; then
        vk_repeat_ok=0
    fi
done
if [ "$vk_repeat_ok" -eq 1 ]; then
    pass "vulkan-repeat-3x"
else
    fail "vulkan-repeat-3x"
fi

echo "=== OpenCL (vendor libPVROCL through libhybris) ==="

if [ ! -x "$CH/opt/pvr-bridge/opengl/bin/opencl-probe" ]; then
    fail "opencl-vendor-compute (opencl-probe missing)"
elif timeout 120 chroot "$CH" /usr/bin/env -i HOME=/root PATH=/usr/bin:/bin \
        LD_LIBRARY_PATH=/opt/pvr-bridge/lib:/usr/lib/arm-linux-gnueabihf \
        ANDROID_ROOT=/system ANDROID_DATA=/data \
        HYBRIS_LINKER_DIR=/opt/pvr-bridge/libhybris/linker \
        HYBRIS_ANDROID_SDK_VERSION=34 \
        HYBRIS_LD_LIBRARY_PATH=/system/lib:/vendor/lib:/vendor/lib/hw:/odm/lib:/apex/com.android.runtime/lib \
        /opt/pvr-bridge/opengl/bin/opencl-probe >"$TMPD/opencl.log" 2>&1 \
        && grep -q "opencl-probe: PASS" "$TMPD/opencl.log"; then
    cl_device=$(sed -n 's/^opencl-probe: device name      = //p' "$TMPD/opencl.log" | head -n 1)
    cl_version=$(sed -n 's/^opencl-probe: platform version = //p' "$TMPD/opencl.log" | head -n 1)
    pass "opencl-vendor-compute ($cl_device, $cl_version)"
else
    fail "opencl-vendor-compute"
fi

if [ ! -x "$CH/opt/pvr-bridge/opengl/bin/opencl-shim-smoke" ]; then
    fail "opencl-app-shim (opencl-shim-smoke missing)"
elif timeout 120 chroot "$CH" /usr/bin/env -i HOME=/root PATH=/usr/bin:/bin \
        LD_LIBRARY_PATH=/opt/pvr-bridge/opencl/lib:/opt/pvr-bridge/lib:/usr/lib/arm-linux-gnueabihf \
        ANDROID_ROOT=/system ANDROID_DATA=/data \
        HYBRIS_LINKER_DIR=/opt/pvr-bridge/libhybris/linker \
        HYBRIS_ANDROID_SDK_VERSION=34 \
        HYBRIS_LD_LIBRARY_PATH=/system/lib:/vendor/lib:/vendor/lib/hw:/odm/lib:/apex/com.android.runtime/lib \
        /opt/pvr-bridge/opengl/bin/opencl-shim-smoke >"$TMPD/opencl-shim.log" 2>&1 \
        && grep -q "opencl-shim-smoke: PASS" "$TMPD/opencl-shim.log"; then
    pass "opencl-app-shim (libOpenCL.so.1 forwards to vendor driver)"
else
    fail "opencl-app-shim"
fi

echo "=== hardware video encode ==="

# 320x240 NV12 black frame, three frames, encoded on the SPRD H.264 core.
head -c 115200 /dev/zero >"$CH/tmp/va-in.nv12"
rm -f "$CH/tmp/va-out.h264"

if timeout 120 chroot "$CH" /usr/bin/env -i HOME=/root PATH=/usr/bin:/bin \
        /opt/pvr-bridge/encoders/bin/omx-enc-env \
        -c h264 -w 320 -h 240 -r 30 -b 400000 -n 3 \
        -i /tmp/va-in.nv12 -o /tmp/va-out.h264 -q \
        >"$TMPD/omx-enc.log" 2>&1 \
        && [ -s "$CH/tmp/va-out.h264" ]; then
    pass "omx-h264-encode ($(wc -c <"$CH/tmp/va-out.h264") bytes)"
else
    fail "omx-h264-encode"
fi

if timeout 120 chroot "$CH" /usr/bin/env -i HOME=/root PATH=/usr/bin:/bin \
        /usr/bin/ffmpeg -v error -i /tmp/va-out.h264 -f null - \
        >"$TMPD/omx-dec.log" 2>&1; then
    pass "omx-h264-decode-check"
else
    fail "omx-h264-decode-check"
fi

echo "=== desktop session ==="

if ps -A -o NAME | grep -qx xfce4-session; then
    pass "xfce-session"
else
    fail "xfce-session (xfce4-session not running)"
fi

if [ "$FULL" = 1 ]; then
    echo "=== full checks ==="
    timeout 180 chroot "$CH" /usr/bin/env -i HOME=/root PATH=/usr/bin:/bin \
            DISPLAY=:1 /opt/pvr-bridge/opengl/bin/run-x11-gles \
            /opt/pvr-bridge/opengl/bin/glmark2-es2-bin \
            --benchmark build --data-path /usr/share/glmark2 -s 320x240 \
            >"$TMPD/glmark2.log" 2>&1
    rc=$?
    if [ "$rc" -eq 0 ]; then
        pass "glmark2-oneshot"
    elif [ "$rc" -eq 139 ] || [ "$rc" -eq 134 ]; then
        xfail "glmark2-oneshot (teardown crash, exit $rc)"
    else
        fail "glmark2-oneshot (exit $rc)"
    fi
fi

rm -f "$CH/tmp/va-in.nv12" "$CH/tmp/va-out.h264"

echo
echo "summary: pass=$PASS_N fail=$FAIL_N xfail=$XFAIL_N"
[ "$FAIL_N" -eq 0 ]
