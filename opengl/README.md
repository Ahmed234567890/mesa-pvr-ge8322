# OpenGL ES bring-up status

This work tests the phone's **vendor OpenGL ES implementation through
libhybris**. It is a separate path from Mesa `pvr`, the vendor Vulkan bridge,
and Kali's default Mesa EGL libraries. The phone exposes GLES, not a tested
desktop OpenGL implementation.

> **Current result:** an armhf EGL/GLES2 pbuffer test running from the Kali
> chroot reaches the vendor driver, reports `PowerVR Rogue GE8322` / OpenGL ES
> 3.2, performs a GPU clear, and reads back `64 128 191 255`. The X11
> EGL-window test reaches `eglCreateWindowSurface` and then segfaults. This
> does **not** establish working OpenGL for X11 applications or a GPU-rendered
> Kali desktop.

## Device and driver inventory

On the SM-A032F running Android 14 / SDK 34 with 32-bit ARM userspace,
SurfaceFlinger reports:

```text
EGL implementation : 1.5 Android META-EGL
GLES: Imagination Technologies, PowerVR Rogue GE8322,
      OpenGL ES 3.2 build 1.18@6267915
```

The vendor EGL/GLES entry points are under `/vendor/lib/egl/`:

- `libEGL_powervr.so`
- `libGLESv1_CM_powervr.so`
- `libGLESv2_powervr.so`

Initial ELF dependency inspection shows the GLESv2 driver depends on
`libIMGegl.so`, `libsrv_um.so`, `libglslcompiler.so`, and `libusc.so`. `libIMGegl`
also depends on Android `libnativewindow`, `libhardware`, and `libsync` APIs.
This makes the Android/bionic runtime and native-window ABI part of the
integration problem; the vendor libraries are not drop-in glibc/Kali drivers.

The stripped vendor `.so` files were pulled for local analysis only. They and
their hashes are kept outside this Git repository in a permission-restricted
case directory:

```text
$HOME/pvr-reverse/cases/ge8322-opengl/raw/
$HOME/pvr-reverse/cases/ge8322-opengl/evidence/
```

No vendor binaries, firmware, kernel modules, or derived vendor code are
published here.

## What has been tested

| Path | Result | Meaning |
|---|---|---|
| Android SurfaceFlinger GLES | Reports Imagination / GE8322 / GLES 3.2 | Confirms Android's system compositor uses the hardware GLES driver |
| Kali chroot + libhybris EGL/GLES + null platform + pbuffer | **Pass**; renderer is GE8322; clear/readback is `64 128 191 255` | Vendor GPU GLES rendering works offscreen from a glibc armhf process |
| Kali chroot + libhybris X11 EGL window | **Fail**; segfault during `eglCreateWindowSurface` after X display, EGL init/config selection, and X window creation succeeded | The X11 native-window bridge is currently the blocker for visible GLES windows |
| Mesa `pvr` OpenGL/Zink | Not tested | Mesa Vulkan submit and WSI failures remain; vendor GLES results do not imply Mesa support |
| Kali desktop acceleration | Not tested / not working evidence | No desktop or compositor has been verified using this GLES path |

The X11 crash has been reproduced with core dumps enabled and the PC symbolized
against the matching armhf Kali Xlib:

```text
SIGSEGV at 0xf3039886
libX11.so.6.4.0: _XimDecodeICATTRIBUTE + 262
Build ID: 8e27db7a5a6d2390f8ce81f94234de30428efb17
```

The core's backtrace ends immediately after this frame with a corrupt/invalid
caller frame (`lr=0`); thus it identifies the faulting Xlib routine, but not the
exact caller or whether Xlib received malformed state from the shim. It is not
yet evidence of a PowerVR driver crash. The core is private because it contains
process memory; it is stored under `$HOME/pvr-reverse/cases/ge8322-opengl/evidence/`.

The pbuffer run emitted non-fatal lookup warnings for `libui_compat_layer.so`
and `/system/vendor/lib/egl/libGL_powervr.so`, but still completed with the
PowerVR renderer and correct readback. The X11 window failure also reproduced
with `HYBRIS_X11_DISABLE_SHM=1` and with `LC_ALL=C XMODIFIERS=@im=none`; neither
disabling MIT-SHM nor changing locale/input-method environment variables
avoided the crash.

Static import/export triage shows `libEGL_powervr.so` exports standard `egl*`
entry points that delegate to `IMGegl*` symbols in `libIMGegl.so`. GLES modules
reference the DDK's `KEGL*`, `PVRSRV*`, and `RGX*` interfaces; the GLESv2 module
also depends on the vendor GLSL compiler and USC compiler libraries. This is an
initial linkage map, not a full function-level reconstruction. The full local
symbol survey is `evidence/vendor-symbol-survey.txt`.

The installed Kali `glmark2` executable was inspected and found to use GLX
(`libGL.so` / `glX*` symbols). It is not a direct EGL/GLES test for this
libhybris path. A GLES benchmark should wait until X11 EGL window creation and
presentation work.

## Build the diagnostic probes

The probes use the existing host-side libhybris build and armhf sysroot; they
do not install packages in Kali. Set paths for the current host build, then
build into a private directory outside the repository:

```sh
export ARMHF_TOOLCHAIN="$HOME/toolchains/arm-gnu-toolchain-14.3.rel1-x86_64-arm-none-linux-gnueabihf"
export ARMHF_SYSROOT="$HOME/armhf-sysroot"
export LIBHYBRIS_BUILD="$HOME/mesa-pvr-build/libhybris"
export OUT_DIR="$HOME/pvr-reverse/cases/ge8322-opengl/build"
./opengl/build-probes.sh
```

The current libhybris build needed host Wayland headers in a private include
overlay to finish its EGL/X11 targets. Those headers are not committed. The
verified build command was:

```sh
mkdir -p "$HOME/pvr-reverse/cases/ge8322-opengl/build/include"
cp /usr/include/wayland-client.h \
   /usr/include/wayland-client-core.h \
   /usr/include/wayland-client-protocol.h \
   /usr/include/wayland-egl.h \
   /usr/include/wayland-egl-core.h \
   /usr/include/wayland-util.h \
   /usr/include/wayland-version.h \
   "$HOME/pvr-reverse/cases/ge8322-opengl/build/include/"

make -C "$LIBHYBRIS_BUILD/hybris" -j4 \
  CPPFLAGS="--sysroot=$ARMHF_SYSROOT \
    -I$HOME/pvr-reverse/cases/ge8322-opengl/build/include \
    -I$HOME/mesa-pvr-build/android-headers-30 \
    -I$HOME/mesa-pvr-build/android-headers-30/libnfc-nxp"
```

The probe binaries are intentionally not stored in Git. The X11 probe test is
expected to reproduce the current `eglCreateWindowSurface` crash until the
native-window path is fixed.

## Run the pbuffer smoke test on the phone

The tests use an isolated staging directory under `/opt/pvr-bridge/opengl`; the
existing vendor Vulkan ICD and Kali default libraries remain unchanged. With
Android `/system`, `/vendor`, `/apex`, and device mounts available inside the
chroot, run:

```sh
chroot /data/local/nhsystem/kali-armhf /usr/bin/env -i \
  HOME=/root PATH=/usr/bin:/bin \
  ANDROID_ROOT=/system ANDROID_DATA=/data \
  HYBRIS_LINKER_DIR=/opt/pvr-bridge/libhybris/linker \
  HYBRIS_ANDROID_SDK_VERSION=34 \
  HYBRIS_LD_LIBRARY_PATH=/system/lib:/vendor/lib:/vendor/lib/hw:/odm/lib:/apex/com.android.runtime/lib \
  HYBRIS_EGLPLATFORM=null \
  HYBRIS_EGLPLATFORM_DIR=/opt/pvr-bridge/opengl/eglplatform \
  LD_LIBRARY_PATH=/opt/pvr-bridge/opengl/lib:/opt/pvr-bridge/lib:/usr/lib/arm-linux-gnueabihf \
  /opt/pvr-bridge/opengl/bin/egl-gles2-smoke
```

Expected key output:

```text
GL vendor: Imagination Technologies
GL renderer: PowerVR Rogue GE8322
GL version: OpenGL ES 3.2 build 1.18@6267915
center pixel: 64 128 191 255 (glError=0x0000)
egl-gles2-smoke: PASS
```

The X11 variant uses `HYBRIS_EGLPLATFORM=x11`, `DISPLAY=:1`, and
`egl-gles2-x11-smoke`. Its present failure is recorded in the local case
evidence at `evidence/egl-gles2-x11-smoke.log`.

## Blockers and next work

1. **X11 surface crash:** the PC resolves to Xlib's `_XimDecodeICATTRIBUTE`,
   and the core's remaining caller chain is corrupt. Add stage logging or
   breakpoints around libhybris `X11NativeWindow` creation (window attributes,
   GC, Present event registration, and buffer setup) to identify which Xlib
   operation reaches XIM decoding. Compare a minimal Xlib-only reproducer and
   inspect Xlib locale/input-method configuration before changing the vendor
   ABI.
2. **Native-window compatibility:** compare Android's real `ANativeWindow`
   contract and gralloc/mapper behavior with libhybris's X11 shim. Test a
   safer bridge (for example, rendering to an Android-compatible buffer and
   presenting it through X11 DRI3) if direct window surfaces are not viable.
3. **GLES benchmark:** after X11 surfaces work, run a GLES2 benchmark and
   verify both `GL_RENDERER` and visible output. Do not use the current GLX
   `glmark2` binary as evidence for this path.
4. **Desktop:** test a small X11 application, then a lightweight desktop with
   compositing disabled. Verify renderer strings per process; avoid claiming
   acceleration if apps fall back to `llvmpipe`/`softpipe`.
5. **Mesa OpenGL:** keep this separate. Mesa `pvr` is Vulkan-focused and its
   queue submit/swapchain paths still fail. Do not replace Kali's default GL or
   Vulkan drivers; retain the vendor paths as fallback.

## Local evidence

The private case folder contains the pulled ELF files, SHA-256 hashes,
dependency summaries, the exact pbuffer and X11 test outputs, and the
libhybris build log. Relevant files include:

- `evidence/phone-inventory.txt`
- `evidence/vendor-elf-summary.txt`
- `evidence/sha256.txt`
- `evidence/egl-gles2-smoke-null.log`
- `evidence/egl-gles2-smoke-repeat.log`
- `evidence/egl-gles2-x11-smoke.log`
- `evidence/egl-gles2-x11-disable-shm.log`
- `evidence/egl-gles2-x11-locale-none.log`
- `evidence/x11-smoke.core` (private process-memory dump)
- `evidence/libhybris-egl-build.log`

Only source probes and non-proprietary findings are tracked in this repository.
