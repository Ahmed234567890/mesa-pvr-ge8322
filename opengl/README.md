# OpenGL ES bring-up status

This work tests the phone's **vendor OpenGL ES implementation through
libhybris**. It is a separate path from Mesa `pvr`, the vendor Vulkan bridge,
and Kali's default Mesa EGL libraries. The phone exposes GLES, not a tested
desktop OpenGL implementation.

> **Current result:** an armhf EGL/GLES2 pbuffer test running from the Kali
> chroot reaches the vendor driver, reports `PowerVR Rogue GE8322` / OpenGL ES
> 3.2, performs a GPU clear, and reads back `64 128 191 255`. The X11
> EGL-window probe now passes end to end on Termux:X11 `:1`: it creates the
> window and surface, makes the context current, reports the GE8322 renderer,
> reads back the same colour, and shows the window on screen. `glmark2-es2-x11`
> renders visible GE8322 scenes through the same path. This is verified **vendor
> GLES on X11**, not desktop OpenGL, GLX, Mesa/Zink, or a fully accelerated
> desktop.

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
| Kali chroot + libhybris X11 EGL window smoke probe | **Pass**; window visible on Termux:X11; GE8322 renderer; readback `64 128 191 255`; exits `PASS` | Vendor GLES renders into a real X11 window through the libhybris X11 platform |
| `glmark2-es2-x11 --validate` + vendor GLES | Reports GE8322; most validation cases pass; process later segfaults | App-level EGL/GLES validation reaches the vendor GPU, but is not a clean exit or timed benchmark |
| `glmark2-es2-x11 --benchmark build` + default swap, SHM enabled | Crashes in the first timed scene | SysV SHM failure in the chroot leaves the XImage destination invalid |
| Same timed `build` scene with `HYBRIS_X11_DISABLE_SHM=1` | **Visible moving scene on Termux:X11**; GE8322 renderer; repeated FPS 154–163 in `--run-forever` | X11 presentation works through XPutImage when MIT-SHM is disabled |
| Timed `texture` scene through `run-x11-gles` | **Visible textured scene**; GE8322 renderer; FPS 168 | Confirms texture upload/shading and repeated swap, not just a clear |
| Same timed scene, one-shot with SHM disabled | Emits FPS / score, then segfaults during process shutdown | Rendering and swap work; teardown still needs repair |
| Same timed scene with `--frame-end none` / `finish` / `readpixels` | Emits scene results, then segfaults at shutdown | Useful diagnostics, not comparable benchmark scores |
| Kali `glmark2 --validate` (GLX) | Most validation cases pass; reports Mesa `llvmpipe` | Software rendering only; not GE8322 acceleration |
| `glx-probe` GLX window through `desktop/bin/gpu-env` | **Pass**; `GL_RENDERER = GL4ES using PowerVR Rogue GE8322`, `GL_VERSION = 2.1 gl4es wrapper 1.1.7`, readback `64 128 191 255`, exit 0 | Desktop OpenGL 2.1 is now hardware-backed through GL4ES on the vendor GLES driver |
| `glmark2 --benchmark build` (GLX) through `desktop/bin/gpu-env` | **Pass**; GE8322 renderer, score 139 at 480x320, exit 0 | Same scene on Mesa llvmpipe scores 39, so the GL4ES path is ~3.6x faster and uses the GPU |
| Vendor `glmark2-es2-x11` with no `LD_PRELOAD` | **Pass**; GE8322, score 212 | The ctype repair inside `libEGL` is sufficient; no preload is needed for a whole session |
| Kali `glmark2-es2 --off-screen --validate` (default Mesa EGL) | Most validation cases pass; reports Mesa `llvmpipe` | Software GLES only; not GE8322 acceleration |
| Mesa `pvr` OpenGL/Zink | Not tested | Mesa Vulkan submit and WSI failures remain; vendor GLES results do not imply Mesa support |
| Kali desktop acceleration | Not tested / not working evidence | XFCE runs on Termux:X11, but draws through X11/CPU; no compositor or desktop app has been verified on the vendor GLES path |

The X11 probe crash had two independent causes, both now fixed.

**1. bionic `TLS_SLOT_OPENGL` collides with glibc `__ctype_b`.** glibc keeps
its thread-local `__ctype_b` pointer at `thread pointer + 16`; that is exactly
bionic's `TLS_SLOT_OPENGL` (slot 4). Android's `/system/lib/libEGL.so`
constructor stores a dispatch-table pointer there (`mrc p15,0,r1,c13,c0,3;
str r0,[r1,#16]`), so Xlib's locale parser (`_XlcResolveLocaleName`, called via
`_XrmInitParseInfo` → `_XOpenLC`) read a bionic pointer as a ctype table. Bit
13 (`_ISspace`) was set for every byte, so the path splitter scanned past the
end of its buffer and faulted at `libX11.so.6.4.0 + 0x39886`. The stripped
library hid the function, and an earlier `_XimDecodeICATTRIBUTE` label was
wrong because GDB was given the ELF load bias as the `.text` address.
`setlocale()` does not repair the pointer because the locale is already `C` and
the call is a no-op. The staged `libEGL` now saves the glibc value around
`android_dlopen()` of the vendor libraries and writes it back afterwards
(`patches/libhybris-egl-ctype-restore.patch`), so no preload is required and
Android binaries started from the desktop are unaffected.
`run-x11-gles` still preloads `libhybris-ctype-fix.so` for unpatched
`libEGL` builds; that helper interposes `__ctype_b_loc`,
`__ctype_tolower_loc`, and `__ctype_toupper_loc` and reinstalls a valid
C-locale table whenever the slot no longer validates.

**2. Depth-32 EGL visual on a depth-24 root.** The EGL config exposes visual
`0x44` (depth 32, TrueColor) while the Termux:X11 root is depth 24.
`XCreateWindow` then fails with `BadMatch` unless the window sets an explicit
`border_pixel = 0` and `background_pixmap = None` with
`CWBackPixmap | CWBorderPixel`, matching glmark2's canvas attributes. Both the
`BadMatch` and the follow-on `BadWindow` errors disappear with that change.

Presentation also needed a colour fix: libhybris advertises
`HAL_PIXEL_FORMAT_BGRA_8888` on the non-drihybris blit path, but the vendor
driver writes RGBA bytes, so `XPutImage` swapped red and blue on depth 32.
`patches/libhybris-x11-swap-red-blue.patch` adds the byte swap in
`copyToX11()`. The vendor `eglSwapBuffers` also sometimes returns `EGL_FALSE`
with `eglGetError() == EGL_SUCCESS`; the frame is already presented in
`ws_finishSwap()`, so the probe treats that combination as success.

The earlier core dumps remain private because they contain process memory and
are stored under `$HOME/pvr-reverse/cases/ge8322-opengl/evidence/`.

The pbuffer run emits non-fatal lookup warnings for `libui_compat_layer.so`
and `/system/vendor/lib/egl/libGL_powervr.so`, but still completes with the
PowerVR renderer and correct readback. The same warnings appear on the X11 path
and do not affect rendering.

Static import/export triage shows `libEGL_powervr.so` exports standard `egl*`
entry points that delegate to `IMGegl*` symbols in `libIMGegl.so`. GLES modules
reference the DDK's `KEGL*`, `PVRSRV*`, and `RGX*` interfaces; the GLESv2 module
also depends on the vendor GLSL compiler and USC compiler libraries. This is an
initial linkage map, not a full function-level reconstruction. The full local
symbol survey is `evidence/vendor-symbol-survey.txt`.

The installed Kali `glmark2` executable uses GLX (`libGL.so` / `glX*`) and
reports llvmpipe. The `glmark2-es2-x11` executable was downloaded and extracted
to private staging only (not installed into Kali). With the X11 platform and
`HYBRIS_X11_DISABLE_SHM=1`, it reports the GE8322 and renders a moving scene
visibly on Termux:X11, including in a decorated XFCE window over the Kali
desktop. A long-running process remains active and swaps frames; one-shot
process shutdown still segfaults, so teardown and full-suite stability remain
open. XFCE itself draws through X11, not this GLES path.

Dynamic-loading applications initially failed because the built
`libhybris-common.so.1.0.0` requests an executable stack and glibc refuses to
`dlopen()` it. Clearing the executable-stack flag on an isolated staged copy
made `dlopen(libEGL.so)` and `dlopen(libGLESv2.so)` succeed; the libhybris build
tree and Kali's default libraries were left unchanged. This is a staging
workaround, not yet a general packaging fix. It also matters for Vulkan: the
loader `dlopen()`s the ICD, so the `libhybris-common.so.1` reachable from
`/opt/pvr-bridge/lib` must be the cleared copy, otherwise instance creation
fails with `cannot enable executable stack`. Reproduce the private copy with:

```sh
./opengl/stage-dlopen-safe-common.sh \
  "$LIBHYBRIS_BUILD/hybris/common/.libs/libhybris-common.so.1.0.0" \
  "$HOME/pvr-reverse/cases/ge8322-opengl/build/opengl-stage/lib/libhybris-common.so.1.0.0"
```

## Run an X11 GLES application

After staging the libhybris runtime and application under
`/opt/pvr-bridge/opengl`, launch it inside the Kali chroot through the wrapper:

```sh
chroot /data/local/nhsystem/kali-armhf \
  /opt/pvr-bridge/opengl/bin/run-x11-gles \
  /opt/pvr-bridge/opengl/bin/glmark2-es2-bin \
  --run-forever --benchmark build --data-path /usr/share/glmark2
```

The wrapper defaults to `DISPLAY=:1`, selects the libhybris X11 platform, sets
the Android linker paths, and disables MIT-SHM. The latter is important in this
chroot: the current libhybris code does not check a failed `shmat()` before
copying to the returned address. Disabling SHM selects the `XPutImage` fallback.
It also sets `LD_PRELOAD=/opt/pvr-bridge/opengl/lib/libhybris-ctype-fix.so`,
which repairs the glibc ctype TLS slot for libhybris builds that do not carry
`libhybris-egl-ctype-restore.patch`; the staged `libEGL` restores the slot
itself, so the preload is only a fallback. Paths supplied by the caller come
first in `LD_LIBRARY_PATH`, which is what lets `desktop/bin/gpu-env` put GL4ES
ahead of Mesa.
Set `DISPLAY` before invocation to target a different X server. This confirms a
visible GLES path for this benchmark, not desktop OpenGL or Minecraft
compatibility.

The working libhybris build carries four local patches under `patches/`:

- `libhybris-egl-terminate-order.patch` calls the vendor `eglTerminate()`
  before `ws_Terminate()` unloads the platform module. The vendor driver keeps
  callbacks into `eglplatform_x11.so` and calls them while terminating the
  display, so unloading first sent those calls into unmapped memory and made
  every GLES application die with `SIGSEGV` at exit.
- `libhybris-egl-ctype-restore.patch` saves glibc's `__ctype_b` TLS pointer
  across `android_dlopen()` of the vendor EGL/GLES libraries and restores it
  afterwards, because bionic's `TLS_SLOT_OPENGL` aliases that slot. This makes
  the fix part of the library instead of requiring `LD_PRELOAD` in every
  process.
- `libhybris-x11-destroy-buffers.patch` removes invalid list iterator use from
  native-window buffer cleanup.
- `libhybris-x11-swap-red-blue.patch` swaps red and blue in `copyToX11()` for
  depth-32 windows, because the vendor driver writes RGBA while libhybris
  advertises BGRA on the non-drihybris path.

The terminate-order patch fixes the one-shot shutdown crash: `glmark2-es2-x11`
now exits 0 both windowed and off-screen, and `FULL=1 scripts/verify-all.sh`
reports `PASS glmark2-oneshot`. The two X11 platform patches are still required
for correct colours and for buffer cleanup.

To build the patched module, apply the patches to a libhybris tree and rebuild
only the X11 platform, then stage `eglplatform_x11.so` under
`/opt/pvr-bridge/opengl/eglplatform/` (back up the previous copy first):

```sh
make -C "$LIBHYBRIS_BUILD/hybris/egl/platforms/x11" -j4 \
  CPPFLAGS="--sysroot=$ARMHF_SYSROOT \
    -I$HOME/pvr-reverse/cases/ge8322-opengl/build/include \
    -I$HOME/mesa-pvr-build/android-headers-30 \
    -I$HOME/mesa-pvr-build/android-headers-30/libnfc-nxp"
```

The terminate-order patch lives in the EGL core rather than the X11 platform,
so rebuild and stage `libEGL` for it (back up the previous copy first):

```sh
make -C "$LIBHYBRIS_BUILD/hybris/egl" -j4 \
  CPPFLAGS="--sysroot=$ARMHF_SYSROOT \
    -I$HOME/pvr-reverse/cases/ge8322-opengl/build/include \
    -I$HOME/mesa-pvr-build/android-headers-30 \
    -I$HOME/mesa-pvr-build/android-headers-30/libnfc-nxp"
```

Then copy `hybris/egl/.libs/libEGL.so.1.0.0` to
`/opt/pvr-bridge/opengl/lib/` inside the chroot and re-run
`scripts/verify-all.sh`.

## Desktop OpenGL through GL4ES

Mesa cannot present vendor frames to Termux:X11 (`screen 0 does not appear to be
DRI3 capable`), and Zink needs Vulkan extensions the 1.1 DDK does not have, so
desktop OpenGL is provided by GL4ES translating GL 2.1 to vendor GLES. GL4ES is
built for armhf with `HYBRIS=ON` and `GLX_STUBS=ON`:

```sh
cmake -S "$GL4ES_SRC" -B "$HOME/mesa-pvr-build/build-gl4es" \
  -DCMAKE_TOOLCHAIN_FILE="$HOME/mesa-pvr-build/armhf-toolchain.cmake" \
  -DHYBRIS=ON -DGLX_STUBS=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo
make -C "$HOME/mesa-pvr-build/build-gl4es" -j4
```

The build writes `lib/libGL.so.1` into the GL4ES source tree, not the build
directory. Stage it as `/opt/pvr-bridge/gl4es/lib/libGL.so.1` (plus a
`libGL.so` symlink) inside the chroot.

`patches/gl4es-hybris-eglplatform.patch` is required. Upstream GL4ES asks for
`EGL_PLATFORM_ANDROID_KHR` whenever `HYBRIS` is defined, in both
`GetHardwareExtensions()` and `init_display()`. libhybris maps that platform to
its passthrough ("null") winsys, and because libhybris allows only one winsys
per process, the extension probe locked out the X11 winsys for the rest of the
run. `eglCreateWindowSurface()` then failed its
`ANDROID_NATIVE_WINDOW_MAGIC` assertion on a plain X11 `Window`. With the patch,
GL4ES honours `HYBRIS_EGLPLATFORM` and calls `eglGetDisplay()` so the X11
winsys is loaded.

Verify the path with the GLX probe or with `glmark2`:

```sh
chroot /data/local/nhsystem/kali-armhf /usr/bin/env -i HOME=/root \
  PATH=/usr/bin:/bin DISPLAY=:1 \
  /opt/pvr-bridge/desktop/bin/gpu-env /tmp/glx-probe
```

Expected output:

```
glx-probe: client vendor  = ptitSeb
glx-probe: client version = 1.4 GL4ES
glx-probe: GL_VENDOR    = GL4ES wrapping Imagination Technologies
glx-probe: GL_RENDERER  = GL4ES using PowerVR Rogue GE8322
glx-probe: GL_VERSION   = 2.1 gl4es wrapper 1.1.7
glx-probe: center pixel  = 64 128 191 255
glx-probe: PASS
```

Measured on this device at 480x320, `glmark2 --benchmark build` scores 139
through GL4ES and 39 on Mesa llvmpipe. `scripts/verify-all.sh` reports
`PASS desktop-gl-gl4es` with the renderer string as evidence.

`desktop/gpu-env.sh` is the single place that sets the whole stack
(`LD_LIBRARY_PATH` with GL4ES, EGL/GLES and the OpenCL shim, `LIBGL_EGL`,
`LIBGL_GLES`, `HYBRIS_*`, `ANDROID_ROOT`/`ANDROID_DATA`, and the Vulkan ICD,
implicit-layer path and loader exclusions). `desktop/start-xfce.sh` sources it
before `startxfce4`, so every XFCE child inherits the GPU environment. No
`LD_PRELOAD` is used: the ctype repair lives in `libEGL` and the WSI layer
loads `libahb-wrapper` itself, which keeps Android binaries launched from
desktop terminals working. Set `PVR_DESKTOP_GL=mesa` to get Mesa llvmpipe
(GL 4.5) for a session that needs a newer GL version.

## OpenCL

The vendor driver at `/vendor/lib/libPVROCL.so` works from the Kali chroot
through libhybris. It reports **OpenCL 3.0** on platform and device (OpenCL C
1.2), driver `1.18@6267915`, 1 compute unit, and 1846 MiB of global memory. A
kernel is compiled from source, enqueued, and read back with correct results;
three consecutive runs exited 0.

Two layers are provided:

- `opengl/opencl-probe.c` loads the vendor library directly with
  `hybris_dlopen()` and exercises the raw API. Use it to prove the driver
  itself works.
- `opengl/gen-opencl-shim.py` plus `opengl/build-opencl-shim.sh` build a
  `libOpenCL.so.1` that exports the full 114-entry-point API and resolves each
  function from the vendor library through libhybris on first use. With that
  shim first in `LD_LIBRARY_PATH`, unmodified applications that link `-lOpenCL`
  reach the GPU; `opengl/opencl-shim-smoke.c` is an ordinary client and passes.

Build the shim (the vendor blob is read only for its symbol list and is not
stored in this repository):

```sh
VENDOR_LIB="$HOME/pvr-reverse/blobs/libPVROCL.so" \
ARMHF_TOOLCHAIN="$HOME/toolchains/arm-gnu-toolchain-14.3.rel1-x86_64-arm-none-linux-gnueabihf" \
ARMHF_SYSROOT="$HOME/armhf-sysroot" \
LIBHYBRIS_BUILD="$HOME/mesa-pvr-build/libhybris" \
OUT_DIR="$HOME/mesa-pvr-build/build-opencl" \
  opengl/build-opencl-shim.sh
```

Stage `libOpenCL.so.1.0.0` plus the `libOpenCL.so.1` and `libOpenCL.so`
symlinks under `/opt/pvr-bridge/opencl/lib/`. `desktop/gpu-env.sh` already puts
that directory ahead of Kali's ICD loader, so the whole session gets it.

Run the probe:

```sh
chroot /data/local/nhsystem/kali-armhf /usr/bin/env -i HOME=/root \
  PATH=/usr/bin:/bin \
  LD_LIBRARY_PATH=/opt/pvr-bridge/lib:/usr/lib/arm-linux-gnueabihf \
  ANDROID_ROOT=/system ANDROID_DATA=/data \
  HYBRIS_LINKER_DIR=/opt/pvr-bridge/libhybris/linker \
  HYBRIS_ANDROID_SDK_VERSION=34 \
  HYBRIS_LD_LIBRARY_PATH=/system/lib:/vendor/lib:/vendor/lib/hw:/odm/lib:/apex/com.android.runtime/lib \
  /opt/pvr-bridge/opengl/bin/opencl-probe
```

Device extensions include `cl_khr_fp16`, `cl_khr_spir`, `cl_khr_il_program`,
`cl_khr_3d_image_writes`, `cl_khr_mipmap_image`, `cl_khr_integer_dot_product`,
`cl_arm_import_memory_dma_buf`, `cl_img_use_gralloc_ptr`, and the semaphore and
external-semaphore families. `clIcdGetPlatformIDsKHR` is not exported, so this
is a direct forwarding library rather than an ICD: applications that enumerate
`/etc/OpenCL/vendors` will not see it, while anything that links or `dlopen`s
`libOpenCL.so.1` will. The shim resolves entry points lazily without a lock;
concurrent first calls from several threads are benign because every thread
resolves the same address.

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

The probe binaries are intentionally not stored in Git. `build-probes.sh` also
builds `libhybris-ctype-fix.so`, the preload shim that keeps glibc's ctype
tables usable in a process hosting Android libraries. The X11 probe is expected
to print `egl-gles2-x11-smoke: PASS` once the ctype shim is preloaded and the
patched `eglplatform_x11.so` is staged.

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
`egl-gles2-x11-smoke`. Run it through `run-x11-gles` so the ctype shim and the
SHM setting are applied:

```sh
chroot /data/local/nhsystem/kali-armhf /usr/bin/env -i \
  HOME=/root PATH=/usr/bin:/bin DISPLAY=:1 \
  /opt/pvr-bridge/opengl/bin/run-x11-gles \
  /opt/pvr-bridge/opengl/bin/egl-gles2-x11-smoke
```

Expected key output:

```text
[x11-smoke] EGL visual 0x44: depth 32, class 4, root depth 24
[x11-smoke] X window created and mapped
[x11-smoke] eglCreateWindowSurface succeeded
[x11-smoke] eglSwapBuffers returned EGL_FALSE with EGL_SUCCESS (frame presented)
GL renderer: PowerVR Rogue GE8322
center pixel: 64 128 191 255 (glError=0x0000)
egl-gles2-x11-smoke: PASS
```

The window stays mapped only briefly, and the probe does not redraw on `Expose`
events, so it goes black if the X server clears it after the first present.
That is cosmetic; use `glmark2-es2-x11 --run-forever` for a continuously
redrawn visible test.

## Blockers and next work

1. **SHM safety:** add a checked `shmat()` failure path in libhybris and fall
   back to `XCreateImage`/`XPutImage` automatically, rather than requiring
   `HYBRIS_X11_DISABLE_SHM=1`.
2. **Shutdown crash:** fixed by `libhybris-egl-terminate-order.patch`; one-shot
   glmark2 now exits 0. Keep the `FULL=1` check in the gate so it stays fixed.
3. **TLS collision:** the staged `libEGL` now restores glibc's `__ctype_b` slot
   around the vendor `dlopen()` (`libhybris-egl-ctype-restore.patch`), so no
   preload is needed. Android libraries still write other bionic TLS slots into
   glibc's thread block; audit `TLS_SLOT_STACK_GUARD`, `TLS_SLOT_SANITIZER`,
   and `TLS_SLOT_ART_THREAD_SELF` and prefer libhybris fixes over preloads.
4. **Native-window compatibility:** compare Android's real `ANativeWindow`
   contract and gralloc/mapper behavior with libhybris's X11 shim. Test a
   safer bridge (for example, rendering to an Android-compatible buffer and
   presenting it through X11 DRI3) if direct window surfaces are not viable.
5. **GLES benchmark:** run the full timed GLES2 suite now that shutdown is
   clean; verify `GL_RENDERER` plus visible output. Do not use GLX `glmark2`
   as evidence for the vendor GLES path, and do not use vendor GLES numbers as
   evidence for desktop GL (use `desktop-gl-gl4es` for that).
6. **Desktop / Minecraft:** desktop GL 2.1 now works through GL4ES on the
   vendor GPU. Minecraft 1.12.2 still needs a private ARMHF Java 8, and the
   replacement Prism launcher is not installed yet; verify per-process renderer
   strings before claiming a game runs on the GPU.
7. **Desktop:** the session environment is global (`desktop/gpu-env.sh`).
   Verify renderer strings per process; XFCE's own 2D drawing still uses
   X11/XRender on the CPU.
8. **Mesa OpenGL:** keep this separate. Mesa `pvr` is Vulkan-focused and its
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
- `evidence/egl-gles2-x11-smoke-pass.log` (ctype shim + patched X11 platform)
- `evidence/egl-gles2-x11-smoke-pass-screen.png` (visible X11 window)
- `evidence/egl-gles2-x11-disable-shm.log`
- `evidence/egl-gles2-x11-locale-none.log`
- `evidence/glmark2-default-validate.log` (GLX / llvmpipe)
- `evidence/glmark2-es2-default-offscreen.log` (EGL/GLES / llvmpipe)
- `evidence/glmark2-es2-vendor-x11-validate.log`
- `evidence/glmark2-es2-vendor-build.log`
- `evidence/glmark2-es2-vendor-build-frame-none.log`
- `evidence/glmark2-es2-vendor-build-frame-finish.log`
- `evidence/glmark2-es2-vendor-build-frame-readpixels.log`
- `evidence/glmark2-es2-vendor-build-no-shm.log`
- `evidence/glmark2-es2-vendor-build-fixed.log`
- `evidence/glmark2-es2-vendor-build-x11-fixed.log` (ctype shim + R/B swap)
- `evidence/glmark2-vendor-x11-fixed-screen.png` (GE8322 `build` scene)
- `evidence/glmark2-vendor-xfce-window-2026-10-27.png` (GE8322 scene in an XFCE window)
- `evidence/xfce-desktop-termux-x11-2026-10-27.png` (XFCE desktop on Termux:X11)
- `evidence/glmark2-vendor-wrapper-live.log`
- `evidence/glmark2-vendor-texture-live.log`
- `evidence/glmark2-vendor-live-screen.png` (visible rendered scene)
- `evidence/glmark2-vendor-wrapper-screen.png`
- `evidence/glmark2-vendor-texture-screen.png`
- `evidence/egl-loader-smoke.log` (dynamic-loader failure and isolated workaround)
- `evidence/x11-smoke.core` (private process-memory dump)
- `evidence/libhybris-egl-build.log`

Only source probes and non-proprietary findings are tracked in this repository.
