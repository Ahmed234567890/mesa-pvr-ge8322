# mesa-pvr-ge8322

Unofficial, experimental Mesa PowerVR (`pvr`) research for the PowerVR Rogue
GE8322 (BVNC `22.86.104.218`), targeting the Samsung Galaxy A03 Core
(`SM-A032F`).

> **Status:** experimental, incomplete Mesa `pvr` bring-up. A local Mesa
> adaptation can load the GE8322 ICD, enumerate the device, create a Vulkan
> device, and create the dummy HWRT dataset through the vendor DDK 1.18
> `pvrsrvkm` bridge. It is **not yet a working renderer**: Mesa queue submit
> fails with `EINVAL`, and Mesa `vkcube` fails during swapchain setup with
> `EINVAL`. Do not interpret the driver's reported conformance version as a
> conformance-test result.
>
> The separate vendor-HAL Vulkan bridge in [bridge/](bridge/) is a distinct,
> working path: its `pvr-smoke` succeeds and its `vkcube` presents through
> Termux:X11. A full vendor-driver `vkmark` run also completed at 60 FPS per
> scene (FIFO/vsync-capped). These results do not establish Mesa rendering or
> accelerated Kali desktop/OpenGL support. Do not confuse them with Mesa ICD
> results. Preliminary vendor GLES via libhybris can render/read back an
> offscreen pbuffer. A custom X11 window probe segfaults. `glmark2-es2-x11`
> identifies the GE8322; with MIT-SHM disabled, moving and textured scenes are
> visible on Termux:X11. One-shot shutdown still crashes. Normal GLX `glmark2`
> runs report llvmpipe rather than hardware acceleration.

## Start here

- [GE8322.md](GE8322.md) — hardware details, current findings, blockers, roadmap
- [TESTING.md](TESTING.md) — verified Mesa/vendor test results, vkmark baseline,
  blockers, and ordered next tests
- [opengl/README.md](opengl/README.md) — vendor GLES/libhybris bring-up, smoke
  tests, X11 blocker, and next work
- [TERMUX-X11-DESKTOP.md](TERMUX-X11-DESKTOP.md) — XFCE startup and Prism/Minecraft
  status on the phone's Termux:X11 display
- [bridge/README.md](bridge/README.md) — vendor Vulkan HAL → libhybris →
  WSI layer → X11 bridge
- [ge8322/vendor-features.md](ge8322/vendor-features.md) — decoded vendor DDK
  feature and errata data
- [ge8322/decode_vendor_bvnc.py](ge8322/decode_vendor_bvnc.py) — reproducer for
  the vendor table decode

## Important policy note

Mesa's PowerVR team declines LLM-generated contributions to `src/imagination`.
This repository contains research notes and tooling only; any Mesa driver
changes must be authored by a human contributor under the project's normal
policy:

- [Mesa PowerVR contribution policy](https://gitlab.freedesktop.org/mesa/mesa/-/blob/main/src/imagination/README.md#contribution-policy)
- [Mesa AGENTS.md](https://gitlab.freedesktop.org/mesa/mesa/-/blob/main/AGENTS.md)

## Mesa upstream

This is an independent research repository, not a Mesa source tree. The Mesa
source is at [gitlab.freedesktop.org/mesa/mesa](https://gitlab.freedesktop.org/mesa/mesa).
Mesa is licensed under the MIT license; see the upstream
[licenses directory](https://gitlab.freedesktop.org/mesa/mesa/-/tree/main/licenses).

Vendor blobs, firmware, and kernel modules are not included.
