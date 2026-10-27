# GE8322 bring-up test status

This page separates tests of the local Mesa `pvr` ICD from tests of the
independent vendor-HAL bridge. Results are from an SM-A032F (SC9863A), Android
14, 32-bit ARM userspace, GE8322 BVNC `22.86.104.218`, vendor DDK
`1.18@6267915`. They describe an experimental bring-up, not production
readiness or Vulkan conformance.

## Test results

| Test | Mesa pvr ICD | Vendor-HAL bridge |
|---|---|---|
| ICD load / physical-device enumeration | Pass; identifies PowerVR Rogue GE8322 | Pass |
| `vkCreateDevice` | Pass | Pass |
| Dummy HWRT dataset, RGXTA3D func 16 | Pass; kernel returns handles `0x53` and `0x54` in the captured run | Vendor func 16 captures succeeded |
| Queue submit | Fail: RGXTQ func 3 ioctl returns `-1`, `errno=22` (`EINVAL`); `pvr-smoke` reports `vkQueueSubmit` failure `-4` | Vendor bridge `pvr-smoke` has passed on its supported path |
| `vkcube` on Termux:X11 `:1` | Fail: swapchain ioctl ID 11 / func 2 returns `EINVAL`; aborts before verified rendering | Pass; visible cube verified |
| Render/readback | No successful Mesa submission/readback verified | Vendor clear-only render/readback passed; not a Mesa result |
| Full `vkmark` suite | Not run: Mesa queue-submit and swapchain paths are blocked | Pass; score 60, each scene 60 FPS with FIFO/vsync |
| OpenGL ES pbuffer | Mesa Zink/OpenGL not tested | Pass via libhybris + vendor EGL/GLES; GE8322 renderer, GLES 3.2, readback `64 128 191 255` |
| OpenGL ES X11 smoke probe | Mesa path not tested | Pass via libhybris + vendor EGL/GLES; GE8322 renderer, X window visible, readback `64 128 191 255` |
| `glmark2-es2-x11 --validate` via vendor GLES | Mesa path not tested | Reports GE8322 and validates most scenes, but exits with SIGSEGV |
| `glmark2-es2-x11` timed scene via vendor GLES | Mesa path not tested | Renders visible GE8322 scenes; `--run-forever` is stable, one-shot teardown still crashes |
| `glmark2-es2 --off-screen --validate` via default EGL | Reports Mesa llvmpipe | Most validation cases pass; software GLES, not GE8322 |

`vulkaninfo --summary` reported API `1.2.363`, device `PowerVR Rogue GE8322`,
and conformance version `1.4.3.3`. The latter is a driver-reported property,
not a CTS result. `vkCreateDevice` and HWRT initialization demonstrate
initialization only; they do not establish GPU rendering or OpenGL support.

## Evidence and reproduction

The original device logs are local research artifacts and are not included in
this repository. Relevant host-side logs from the test session were:

- `/tmp/opencode/mesa-hwrt2-retry.log` — Mesa `vulkaninfo --summary`; shows
  dummy HWRT creation and device enumeration.
- `/tmp/opencode/pvr-smoke-mesa.log` — Mesa `pvr-smoke`; shows successful device
  creation followed by RGXTQ func 3 `EINVAL` and failed queue submission.
- `/tmp/opencode/vkcube-mesa2.log` — Mesa `vkcube`; selects Xlib then segfaults.
- `/tmp/opencode/pvr-renderpass-clear-trace.log` — vendor-HAL clear-only
  capture; do not use as evidence of Mesa rendering.

On the test phone, with the Mesa ICD deployed in the approved
`/opt/pvr-bridge` bundle and the documented private chroot mounts active, run
the enumeration test as:

```sh
VK_ICD_FILENAMES=/opt/pvr-bridge/share/vulkan/icd.d/powervr_mesa_ge8322.json \
PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=1 vulkaninfo --summary
```

The Mesa smoke and presentation tests use the same ICD and opt-in override:

```sh
VK_ICD_FILENAMES=/opt/pvr-bridge/share/vulkan/icd.d/powervr_mesa_ge8322.json \
PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=1 pvr-smoke

DISPLAY=:0 \
VK_ICD_FILENAMES=/opt/pvr-bridge/share/vulkan/icd.d/powervr_mesa_ge8322.json \
PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=1 vkcube --c 120
```

Run these inside the Kali chroot with Android device nodes, `/sys`, runtime
libraries, and `/opt/pvr-bridge` configured as described in
[bridge/README.md](bridge/README.md). Use a clean environment to avoid
accidentally selecting the vendor ICD or preloading the bridge layer.

## What is missing

1. **RGXTQ func 3 ABI:** recover exact DDK 1.18 input/output sizes, field order,
   pointer widths, nested user-pointer copying, and returned fence semantics.
   Compare a successful vendor submission against Mesa. The existing vendor
   graphics-pipeline crash may prevent obtaining a comparable draw submission;
   find a minimal safe vendor workload before relying on it.
2. **Safe submission validation:** after the ABI is independently checked,
   test the smallest no-op or clear workload first, capture ioctl arguments and
   return values, then verify completion/fence behavior. Stop on a device reboot
   and collect pstore before another attempt.
3. **Mesa rendering and readback:** demonstrate successful queue submission,
   offscreen clear, synchronization, and readback before testing WSI.
4. **X11 WSI:** diagnose Mesa `vkcube` after queue submission works; test
   Termux:X11 separately from KeX/Xtigervnc.
5. **OpenGL:** build/test Zink on Mesa Vulkan only after Vulkan execution is
   sound. The vendor-HAL Zink path previously lacked `nullDescriptor`.
6. **Build reproducibility:** automate target-ABI usclib generation, rebuild
   from documented sources, remove temporary `PVRDBG` logging, and retest the
   vendor `sysvk` baseline.
7. **Coverage:** verify cleanup/lifetimes, display-buffer map/unmap, transfer
   and compute submit ABIs, validation layers, repeated runs, and stress only
   after basic rendering is stable.

Do not retry a kernel call known to panic on error until its ABI and payload are
verified offline. Keep proprietary blobs, firmware, kernel modules, and derived
vendor code out of published artifacts. Do not describe this as working Mesa
OpenGL or a first-of-its-kind result without sufficient evidence.

## Latest phone retest

On 2026-10-26, the checks were repeated over ADB. The vendor `pvr-smoke`
passed and read back `64 128 191 255`. Mesa `vulkaninfo --summary` exited
successfully (with an `XDG_RUNTIME_DIR` warning) and identified the GE8322.
Mesa `pvr-smoke` again failed at RGXTQ func 3 with `EINVAL`.

Termux:X11 display `:0` was stale and refused new clients. Starting a fresh
display `:1` from the Termux terminal made `xdpyinfo` succeed. The chroot also
needed its existing `/dev/binderfs` directory bind-mounted from Android; this
resolved the vendor WSI error `Binder driver '/dev/binder' could not be
opened`. With these two fixes, vendor `vkcube --c 120` exited 0, and a live
vendor cube was observed on the phone screen. This is the vendor-HAL control,
not Mesa rendering.

On display `:1`, Mesa `vkcube --c 120` created the device and dummy HWRT, then
failed swapchain setup: bridge ioctl ID 11 / func 2 returned `EINVAL` twice,
and `vkcube` aborted at `demo_prepare_swapchain`. Mesa still has no verified
rendered output. Queue submit is independently blocked by RGXTQ func 3 `EINVAL`
in `pvr-smoke`.

The same day, `vkmark` 2025.01-1 (ARMHF) was staged with its runtime libraries
under the phone's `/opt/pvr-bridge/vkmark` directory; no packages were
installed into Kali and no benchmark binaries or vendor files were added to
this repository. With Termux:X11 `:1`, the vendor-HAL ICD, XCB winsys, a
480x800 window, and FIFO presentation, the complete default suite completed:

```text
[vertex] device-local=true: FPS: 60 FrameTime: 16.667 ms
[vertex] device-local=false: FPS: 60 FrameTime: 16.667 ms
[texture] anisotropy=0: FPS: 60 FrameTime: 16.667 ms
[texture] anisotropy=16: FPS: 60 FrameTime: 16.667 ms
[shading] shading=gouraud: FPS: 60 FrameTime: 16.667 ms
[shading] shading=blinn-phong-inf: FPS: 60 FrameTime: 16.667 ms
[shading] shading=phong: FPS: 60 FrameTime: 16.667 ms
[shading] shading=cel: FPS: 60 FrameTime: 16.667 ms
[effect2d] kernel=edge: FPS: 60 FrameTime: 16.667 ms
[effect2d] kernel=blur: FPS: 60 FrameTime: 16.667 ms
[desktop] <default>: FPS: 60 FrameTime: 16.667 ms
[cube] <default>: FPS: 60 FrameTime: 16.667 ms
[clear] <default>: FPS: 60 FrameTime: 16.667 ms
                                   vkmark Score: 60
```

This vendor score is capped by the selected FIFO/vsync mode and 60 Hz display;
it is not a peak-throughput measurement and is not a Mesa result. The host log
is `/tmp/opencode/phone-vendor-vkmark-full.log` and is not part of the repo.

## What works, what does not, and what remains unverified

### Verified working

- Vendor-HAL device enumeration and a clear/readback (`64 128 191 255`) via
  `pvr-smoke`.
- Vendor-HAL `vkcube` presentation on Termux:X11 display `:1`.
- The full vendor-HAL `vkmark` default scene suite on the GE8322, with the
  vsync-capped result above.
- The SPRD hardware H.264 and HEVC encoders through libhybris: 320x240,
  640x480 and 1280x720 streams produced from NV12/I420 input decode cleanly in
  `ffmpeg`, in file and pipe modes. The vendor software MPEG-4 and H.263
  encoders also produce decodable 640x480 streams. There is no VP8 encoder
  component on this build. See [encoders/README.md](encoders/README.md).
- Mesa ICD enumeration, device creation, and dummy HWRT initialization only.

### Known failures and blockers

- Mesa RGXTQ func 3 queue-submit ioctl fails with `EINVAL`. Its exact DDK 1.18
  32-bit-user/64-bit-kernel ABI, payload layout, pointer handling, and fence
  semantics are not yet established.
- Mesa swapchain setup on X11 fails at bridge ioctl ID 11 / func 2 with
  `EINVAL`; Mesa `vkcube` aborts before any verified frame is presented.
- No successful Mesa command submission, render, synchronization, or readback
  has been demonstrated. Mesa is not currently a usable renderer on this phone.
- No Mesa `vkmark` benchmark can be claimed until submission and WSI work.
- Kali's desktop has not been validated as GPU-rendered. The visible cube and
  benchmark use the separate vendor HAL, not Mesa.
- OpenGL/Zink on this Mesa path has not been brought up or validated. Do not
  infer OpenGL acceleration from Vulkan device enumeration or vendor results.
- Mainline Mesa `pvr` expects the `drm/imagination` kernel UAPI; this phone
  exposes proprietary `pvrsrvkm`. A compatible kernel DRM path or another
  maintainable interface is still a major prerequisite for upstream Mesa.
- GE8322 compiler/device-info and errata correctness, target-ABI usclib/NIR
  generation, validation, Vulkan CTS/conformance, stability, and performance
  remain incomplete or unverified.

### Next steps

1. Reconstruct and independently validate RGXTQ func 3 offline before making
   further kernel submissions; do not guess at payloads on the phone.
2. Fix and verify Mesa offscreen submit, clear, synchronization, and readback.
3. Diagnose the Mesa swapchain ioctl only after rendering works; then retry
   Mesa `vkcube` and a Mesa `vkmark` run.
4. Validate Zink/OpenGL separately, checking the renderer string for the PowerVR
   GPU rather than `llvmpipe`/`softpipe`.
5. Only after those pass, test a lightweight desktop with compositing disabled
   first. Keep the working vendor ICD as fallback and do not replace global
   Vulkan/OpenGL libraries during bring-up.

The vendor baseline is a useful working fallback, not evidence that the Mesa
driver is fixed, upstreamed, conformant, or capable of rendering the Kali
desktop. No Mesa Vulkan CTS result or full desktop acceleration result is
available.

## OpenGL/GLES progress

The phone reports a vendor OpenGL ES 3.2 driver for the GE8322. A separately
staged libhybris EGL/GLES path in `/opt/pvr-bridge/opengl` can create a pbuffer
inside the Kali chroot, render a clear, and read back `64 128 191 255`; the
reported renderer is `PowerVR Rogue GE8322`. This is verified **vendor GLES**
execution and is not Mesa/Zink or desktop OpenGL.

The custom X11 probe now passes. Two independent faults were found:

1. glibc keeps its thread-local `__ctype_b` pointer at `thread pointer + 16`,
   which is also bionic `TLS_SLOT_OPENGL` (slot 4). Android's
   `/system/lib/libEGL.so` constructor stores a dispatch-table pointer in that
   slot, so Xlib's locale parser (`_XlcResolveLocaleName`) read a bionic pointer
   as a ctype table, treated every byte as `_ISspace`, and walked off the stack
   at `libX11 + 0x39886`. `run-x11-gles` now preloads
   `libhybris-ctype-fix.so`, which interposes `__ctype_b_loc` and restores a
   valid C-locale table.
2. The EGL config exposes a depth-32 visual on a depth-24 root, so
   `XCreateWindow` needs an explicit `border_pixel` and
   `background_pixmap = None` or the server returns `BadMatch`.

With both fixes the probe creates the window and surface, makes the context
current, reports `PowerVR Rogue GE8322` / GLES 3.2, reads back
`64 128 191 255`, and exits `PASS`. libhybris advertises
`HAL_PIXEL_FORMAT_BGRA_8888` on the non-drihybris blit path while the vendor
driver writes RGBA bytes, so `XPutImage` swapped red and blue until
`libhybris-x11-swap-red-blue.patch` added the byte swap. The vendor
`eglSwapBuffers` also returns `EGL_FALSE` with `eglGetError() == EGL_SUCCESS`
on some runs; the frame is already presented in `ws_finishSwap()`, so the probe
treats that combination as success.

The `glmark2-es2-x11` package was downloaded and extracted only into private
staging (not installed into Kali). With the `run-x11-gles` wrapper,
`HYBRIS_X11_DISABLE_SHM=1`, and the vendor GLES path, glmark2 reports GE8322 and
shows moving `build` and textured `texture` scenes visibly on Termux:X11. The
live build loop reports roughly 154–163 FPS; the texture scene reported 168
FPS. This confirms GLES drawing and X11 swaps with the XPutImage fallback.
Without disabling SHM, the first timed scene crashes because the current shim
does not check `shmat()` failure. A one-shot run still segfaults at shutdown;
the long-running test was stopped after recording visible output. The regular
GLX `glmark2 --validate` and default-EGL `glmark2-es2 --off-screen --validate`
report Mesa `llvmpipe`, so those runs remain software-rendered. Proprietary
vendor libraries, the downloaded package, executables, and cores remain outside
this repository. See [opengl/README.md](opengl/README.md) for evidence and next
steps.

## Hardware video encoders

On 2026-10-27 the vendor SPRD OpenMAX IL encoders were brought up inside the
Kali chroot through libhybris. `OMX.sprd.h264.encoder` and
`OMX.sprd.h265.encoder` use `/dev/sprd_vsp` and `/dev/ion`. Three Android 14
incompatibilities had to be handled: missing old `GraphicBufferMapper`
overloads and CFI hooks (supplied by a freestanding shim), an undersized
`AHandler` allocation in `libstagefrighthw.so` (widened from 56 to 120 bytes),
and a broken `AMessage::post` looper path (redirected to a synchronous
`onMessageReceived` call by rewriting one GOT slot).

Verified streams, pulled to a desktop and decoded with `ffmpeg`:

| Codec | Size | Input | Frames | Result |
| --- | --- | --- | --- | --- |
| H.264 | 320x240 | NV12 | 30 | decodes |
| H.264 | 640x480 | I420 | 25 | decodes |
| H.264 | 1280x720 | NV12 | 30 | decodes, ~470 KB |
| HEVC | 320x240 | NV12 | 30 | decodes |
| HEVC | 640x480 | I420 | 25 | decodes |
| HEVC | 1280x720 | NV12 | 30 | decodes, ~468 KB |
| MPEG-4 | 640x480 | I420 | 25 | decodes (software component) |
| H.263 | 640x480 | I420 | 25 | decodes (software component) |

The output is Constrained Baseline H.264 (no B-frames) and Main-profile HEVC.
The tool is `omx-enc` in `/opt/pvr-bridge/encoders/bin`, wrapped by
`omx-enc-env`, and accepts raw NV12/I420 on stdin or a file and writes an
elementary stream to stdout or a file, so it can be piped from/to `ffmpeg`.
`-c mpeg4` and `-c h263` drive the vendor's software components, not the VSP.
There is no VP8 encoder component on this build: the core registers only
`OMX.sprd.vpx.decoder`, and `libomx_vpxenc_hw_sprd.so` exports no OpenMAX
symbols, so `-c vp8` is rejected. Decoding through OpenMAX, rate-control
tuning, and long-lived use in a single process are not verified.
`OMX_FreeHandle`/`OMX_Deinit` are deliberately skipped because unloading the
vendor codec aborts in libhybris's TLS-module teardown; the one-shot CLI exits
instead. See [encoders/README.md](encoders/README.md) for the stack, patches,
build and usage.

## XFCE and Minecraft on Termux:X11

On 2026-10-26, the existing Kali XFCE session was started on Termux:X11 display
`:1` without installing/removing Kali packages. The XFCE panel configuration
had an unavailable `null` plugin; removing that invalid panel entry allowed
the desktop to appear. On 2026-10-27 the session was reproduced: the chroot
inherits Android's SELinux policy, which has no D-Bus rules, so the session bus
denied every call until `desktop/selinux-off.c` was preloaded to make
libselinux report "disabled" for the session. The panel, desktop icons, window
manager, and Thunar then started; applets that need Android's system bus still
fail.

Terminals launched from the desktop closed immediately at first. The `su -c`
shell exported `SHELL=/system/bin/sh` and the glibc SELinux preload into the
session, so QTerminal started Android's shell and Android's linker rejected the
glibc shim (`library "libc.so.6" not found`). `desktop/start-xfce.sh` now sets
`SHELL=/bin/bash`, scopes the preload to `dbus-daemon`, and clears
`LD_PRELOAD` before starting XFCE; QTerminal then opens with a normal Kali bash
prompt. Prism Launcher 11.1.1 was open on the XFCE desktop; on 2026-10-27 only
that launcher was removed so a different Prism version can be installed, while
its private `user-data`, staged runtime and chroot were kept. Its title bar was
initially obscured by the top panel; moving the window below the panel restored
normal dragging.

The Prism `1.12.2` instance reached the **Play Demo** flow with a demo player
name. Launch was tested using the staged ARMHF Java 21 runtime, Prism's Java
compatibility bypass, and a 384 MiB maximum heap. It still exited before
rendering, with a Java security-provider stack trace and process exit code 2;
Prism reports that this instance requires Java 8. Therefore neither the
Minecraft menu nor 1.12.2 rendering on the GE8322 has been verified. See
[TERMUX-X11-DESKTOP.md](TERMUX-X11-DESKTOP.md) for the XFCE startup procedure,
window recovery, privacy boundary, and next steps.

This XFCE result means the desktop session is visible on Termux:X11; it is not
evidence that XFCE is GPU-rendered. With the session running,
`glmark2-es2-x11 --benchmark build --run-forever` rendered the GE8322 build
scene in a decorated XFCE window over the desktop, which combines the verified
vendor GLES path with a real window manager and X11 desktop. The desktop itself
still draws through X11, not the vendor GLES path. The separate vendor GLES and
Mesa results above remain distinct.
