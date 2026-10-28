# Zink evidence gate (WS8)

Status: **closed for the GE8322 with vendor DDK 1.18@6267915.**

This note records why desktop OpenGL on this phone is delivered by GL4ES rather
than by Mesa's Zink. It replaces the earlier assumption that Zink was blocked
because the vendor Vulkan driver lacked "the extensions". Extension coverage is
in fact broad; Zink is blocked by one mandatory *feature*.

## Device as seen by Vulkan

Captured with `vulkaninfo` from the Kali chroot through
`/opt/pvr-bridge/desktop/bin/gpu-env`:

```
apiVersion    = 1.3.216 (4206808)
driverVersion = 1.506.1035 (6267915)
deviceName    = PowerVR Rogue GE8322
driverID      = DRIVER_ID_IMAGINATION_PROPRIETARY
driverName    = PowerVR Rogue Vulkan Driver
driverInfo    = 1.18@6267915
```

The device exposes **85 device extensions** and 15 instance extensions (the
latter include `VK_KHR_surface`, `VK_KHR_xcb_surface`, `VK_KHR_xlib_surface`
and `VK_EXT_surface_maintenance1` from the X11 WSI layer). This is not a
"Vulkan 1.1" driver: the instance reports API 1.3.

## Method

Three runs, all inside the Kali chroot on Termux:X11 `:1`:

1. Zink against the **vendor** ICD (`sysvk_icd.json`).
2. Zink against **lavapipe** (`lvp_icd.json`) as a control, to prove Zink itself
   initialises in this chroot.
3. Mesa's **pvr** ICD (`powervr_mesa_icd.json`) through GLX, to separate the
   Mesa `pvr` kernel-interface problem from the Zink problem.

```sh
E="HOME=/root PATH=/usr/bin:/bin DISPLAY=:1 \
   LD_LIBRARY_PATH=/opt/pvr-bridge/lib:/usr/lib/arm-linux-gnueabihf \
   HYBRIS_LINKER_DIR=/opt/pvr-bridge/libhybris/linker \
   HYBRIS_ANDROID_SDK_VERSION=34 \
   HYBRIS_LD_LIBRARY_PATH=/system/lib:/vendor/lib:/vendor/lib/hw:/odm/lib:/apex/com.android.runtime/lib \
   VK_ICD_FILENAMES=/opt/pvr-bridge/share/vulkan/icd.d/sysvk_icd.json \
   VK_ADD_IMPLICIT_LAYER_PATH=/opt/pvr-bridge/share/vulkan/implicit_layer.d \
   VK_LOADER_LAYERS_DISABLE=VK_LAYER_MESA_device_select,VK_LAYER_MESA_anti_lag"

# 1. vendor ICD
chroot $CH /usr/bin/env -i $E \
  MESA_LOADER_DRIVER_OVERRIDE=zink GALLIUM_DRIVER=zink \
  /usr/bin/glxinfo -B

# 2. control
chroot $CH /usr/bin/env -i HOME=/root PATH=/usr/bin:/bin DISPLAY=:1 \
  LD_LIBRARY_PATH=/usr/lib/arm-linux-gnueabihf \
  VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json \
  XDG_RUNTIME_DIR=/tmp/xfce-runtime LIBGL_ALWAYS_SOFTWARE=1 \
  MESA_LOADER_DRIVER_OVERRIDE=zink GALLIUM_DRIVER=zink \
  /usr/bin/glxinfo -B
```

## Results

### 1. Vendor ICD: fails on a mandatory feature

```
MESA: error: Zink requires the nullDescriptor feature of KHR/EXT robustness2.
glx: failed to create drisw screen
failed to load driver: zink
```

Exit status 139. The failure happens after Zink has enumerated and selected the
vendor physical device, so device discovery and instance creation succeed.

Mesa 26/27 makes this a hard gate in
`src/gallium/drivers/zink/zink_screen.c`:

```c
if (!screen->info.rb2_feats.nullDescriptor) {
   mesa_loge("Zink requires the nullDescriptor feature of KHR/EXT robustness2.");
   goto fail;
}
```

`VK_EXT_robustness2` does not appear at all in the vendor device's 85
extensions, so the feature is zero-initialised and the check always fails.
There is no environment variable or driconf switch that bypasses it.

### 2. Control: Zink works in this chroot

```
OpenGL vendor string:   Mesa
OpenGL renderer string: zink Vulkan 1.4(llvmpipe (LLVM 21.1.8, 128 bits) (MESA_LLVMPIPE))
OpenGL version string:  4.6 (Compatibility Profile) Mesa 26.0.8-1
```

Exit status 0. This isolates the vendor failure: the Zink loader, GLX glue,
shader compiler and presentation stack all work here; only the vendor device is
rejected.

### 3. Mesa `pvr` ICD: a separate blocker

```
screen 0 does not appear to be DRI3 capable
MESA: error: ZINK: vkEnumeratePhysicalDevices failed (VK_ERROR_INITIALIZATION_FAILED)
MESA: error: ZINK: failed to choose pdev
OpenGL renderer string: llvmpipe (LLVM 21.1.8, 128 bits)
```

Mesa's `pvr` Vulkan driver cannot initialise because it targets the
`drm/imagination` kernel interface, while this phone runs the vendor
`pvrsrvkm` module. That is independent of Zink and is the same root cause as the
queue-submit `EINVAL` recorded in [../TESTING.md](../TESTING.md).

Note on the DRI3 message: `xdpyinfo -queryExtensions` on Termux:X11 `:1`
advertises both `DRI3` (opcode 147) and `Present` (opcode 146). The message is
emitted by Mesa's GLX when *its* DRI3 screen setup fails for a driver, not
because the X server lacks the extension. The earlier statement that Mesa
"cannot present vendor frames because Termux:X11 is not DRI3 capable" is
therefore inaccurate and has been corrected.

## Extension coverage against Zink's needs

| Extension / feature | Present | Zink's use |
|---|---|---|
| `VK_EXT_robustness2` (`nullDescriptor`) | **no** | **Hard requirement**; screen creation fails without it |
| `VK_KHR_dynamic_rendering` | yes (rev 1) | Core rendering path |
| `VK_KHR_synchronization2` | yes (rev 1) | Core synchronisation |
| `VK_KHR_timeline_semaphore` | yes (rev 2) | Required, or Vulkan 1.2 `timelineSemaphore` |
| `VK_EXT_extended_dynamic_state` | yes (rev 1) | Dynamic state |
| `VK_EXT_extended_dynamic_state2` | yes (rev 1) | Dynamic state |
| `VK_KHR_imageless_framebuffer` | yes (rev 1) | Framebuffer objects |
| `VK_KHR_create_renderpass2` | yes (rev 1) | Render passes |
| `VK_KHR_copy_commands2` | yes (rev 1) | Blit/copy paths |
| `VK_KHR_draw_indirect_count` | yes (rev 1) | Indirect draws |
| `VK_KHR_maintenance1` | yes (rev 2) | Base requirement |
| `VK_KHR_maintenance2`/`3`/`4` | yes | Base requirements |
| `VK_EXT_scalar_block_layout` | yes (rev 1) | Base requirement |
| `VK_KHR_swapchain` | yes (rev 70) | Presentation through the WSI layer |
| `VK_ANDROID_external_memory_android_hardware_buffer` | yes (rev 5) | WSI buffer export |
| `VK_KHR_present_id` | yes (rev 1) | Presentation timing |
| `VK_EXT_custom_border_color` | no | Warning only: "some incorrect rendering might occur" |
| `VK_EXT_line_rasterization` | no | Warning only |
| `VK_EXT_transform_feedback` | no | Optional; disables device-address commands |
| `VK_EXT_vertex_input_dynamic_state` | no | Optional; disables device-address commands |
| `VK_KHR_maintenance5`/`6`/`11` | no | Optional; maintenance11 only gates shader objects |
| `VK_EXT_descriptor_buffer` | no | Optional fast path |
| `VK_KHR_unified_image_layouts` | no | Optional layout workaround |
| `VK_EXT_host_image_copy` | no | Optional upload fast path |
| `VK_EXT_shader_object` | no | Optional; gated by maintenance11 |
| `VK_KHR_device_address_commands` | no | Optional |
| `VK_EXT_physical_device_drm` | no | Optional; single-device fallback selects device 0 |
| `VK_KHR_present_wait` | no | Optional presentation feature |

Conclusion: the only item in the mandatory column that is missing is
`nullDescriptor`. If a future DDK adds `VK_EXT_robustness2` (or the check is
relaxed upstream), Zink would have a realistic chance on this device, because
dynamic rendering, synchronisation2, timeline semaphores, imageless
framebuffers and the maintenance family are all present.

## Driver-version note

Mesa also warns about the proprietary Imagination driver:

```c
/* Reject IMG blobs with DDK below 24.1@6554834 if not forced */
if (zink_driverid(screen) == VK_DRIVER_ID_IMAGINATION_PROPRIETARY &&
    screen->info.props.driverVersion < 6554834) {
   debug_printf("zink: Imagination proprietary driver is too old to be supported, expect failure\n");
   if (screen->driver_name_is_inferred)
      goto fail;
}
```

This device reports `1.18@6267915`, below the `24.1@6554834` threshold. Because
`VK_KHR_driver_properties` is exposed, the driver name is not inferred, so this
is a warning rather than a hard failure. It is nevertheless a second signal
that DDK 1.18 predates the driver generation Zink targets.

## Decision

- Desktop OpenGL on this phone stays on **GL4ES over vendor GLES** (GL 2.1),
  documented in [README.md](README.md).
- Do **not** advertise Zink, Mesa `pvr` GL, or OpenGL 3.x/4.x for the GE8322.
- Keep the GL4ES path in the verification harness
  (`desktop-gl-gl4es`) so this decision is continuously evidenced.
- Re-open this gate only if a newer vendor DDK exposes `VK_EXT_robustness2`
  with `nullDescriptor`, or if the `drm/imagination` kernel interface becomes
  available for a Mesa `pvr` path that passes queue submission.

## Evidence files

- `vulkaninfo` full output: captured per run under `/tmp/opencode/`, not
  committed (it contains the device serial-adjacent identifiers).
- Zink failure log: `Zink requires the nullDescriptor feature ...`.
- Control log: `zink Vulkan 1.4(llvmpipe ...)`, GL 4.6.
- Mesa `pvr` log: `vkEnumeratePhysicalDevices failed (VK_ERROR_INITIALIZATION_FAILED)`.
