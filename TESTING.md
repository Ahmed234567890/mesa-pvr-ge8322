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
| `vkcube` on Termux:X11 | Fail: selects Xlib then segfaults | Pass, previously verified |
| Render/readback | No successful Mesa submission/readback verified | Vendor clear-only render/readback passed; not a Mesa result |

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
rendered output. The next Mesa blocker is this WSI/DRI3 ioctl path; queue submit
is independently blocked by RGXTQ func 3 `EINVAL` in `pvr-smoke`.
