# Zink evidence gate (WS8, re-opened for Phase 2)

Status: **closed for the GE8322 with vendor DDK 1.18@6267915 — now for two
independent reasons.**

This note records why desktop OpenGL on this phone is delivered by GL4ES rather
than by Mesa's Zink.

The gate was originally closed because Zink refused to create a screen without
`VK_EXT_robustness2.nullDescriptor`. Phase 2 removed that blocker and Zink now
*starts*. It still cannot be used, because:

1. the vendor Vulkan driver has no `VK_EXT_transform_feedback`, which Mesa
   requires for both GL 3.0 and GLES 3.0, so the screen is capped at **GL 2.1**
   (the same ceiling GL4ES already delivers); and
2. the vendor driver **crashes on the first draw** Zink emits, so the 2.1
   screen is not usable for rendering either.

## Device as seen by Vulkan

```
apiVersion    = 1.3.216 (4206808)
driverVersion = 1.506.1035 (6267915)
deviceName    = PowerVR Rogue GE8322
driverID      = DRIVER_ID_IMAGINATION_PROPRIETARY
driverInfo    = 1.18@6267915
geometryShader                  = false
maxGeometryShaderInvocations    = 0
```

The device exposes 93 vendor device extensions plus the one this project's ICD
adds (`VK_EXT_robustness2`), and 15 instance extensions from the X11 WSI layer.

## Phase 1 of the gate (WS8): the nullDescriptor blocker

Zink 26/27 makes robustness2 a hard gate in
`src/gallium/drivers/zink/zink_screen.c`:

```c
if (!screen->info.rb2_feats.nullDescriptor) {
   mesa_loge("Zink requires the nullDescriptor feature of KHR/EXT robustness2.");
   goto fail;
}
```

The vendor exposes no `VK_EXT_robustness2`, so the feature was zero and screen
creation failed with exit status 139.

`libsysvk` now supplies the missing extension (Phase 2a, see
[../bridge/README.md](../bridge/README.md)): with `SYSVK_ENABLE_RB2=1` it
appends `VK_EXT_robustness2` to `vkEnumerateDeviceExtensionProperties`, reports
`nullDescriptor = 1` through `vkGetPhysicalDeviceFeatures2`, and strips the name
again in `vkCreateDevice` because the vendor would reject it. `vulkaninfo` then
lists 94 device extensions including `VK_EXT_robustness2`.

The shim is opt-in so that no other Vulkan client is told it has a capability
the vendor lacks; the Zink checks in the harness set the variable explicitly.
Without it the vendor's 93 extensions are passed through unchanged.

Caveat: the feature is *advertised*, not translated. The vendor has no
robustness2 support, so an application that actually samples a null descriptor
would still reach code that assumes a real resource. Zink only relies on it for
unused bindings at this version.

## Phase 2 of the gate: a vendor `vkQueueSubmit` bug

With the screen created, the first Zink run crashed inside the vendor's
`vkQueueSubmit` (`vulkan.sp9863a.so`, file offset `0x4f6f8`, `ldr r0,[r0,#8]`
with `r0 = 0`), before any descriptor set had been allocated.

Tracing the submit path in `libsysvk` showed the trigger:

```
QueueSubmit submits=2 fence=(nil)
  submit[0] cmds=1 waits=0 signals=0 pNext=(nil)
  submit[1] cmds=0 waits=0 signals=1 pNext=...   <-- crashes the vendor
```

Zink submits a second batch that only signals a timeline semaphore. That is
legal Vulkan, but the vendor dereferences the first command buffer of every
submit entry. Stripping `pNext` did not help; skipping the empty entry did, so
the fault is the zero `commandBufferCount`, not the timeline chain.

`libsysvk` now substitutes a recorded empty command buffer
(`VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT`) allocated from a per-queue-family
pool, so semaphore and fence semantics are preserved instead of dropped. Queue
families are recorded from `vkGetDeviceQueue`/`vkGetDeviceQueue2`. This is a
general ICD correctness fix: any Vulkan client that submits a
semaphore-signalling batch without work would have crashed the vendor driver.

Result: `glxinfo -B` exits 0 and reports

```
OpenGL renderer string: zink Vulkan 1.3(PowerVR Rogue GE8322 (IMAGINATION_PROPRIETARY))
OpenGL version string: 2.1 Mesa 26.0.8-1
Preferred profile: compat (0x2)
Max core profile version: 0.0
Max compat profile version: 2.1
Max GLES[23] profile version: 2.0
```

## Why the ceiling is GL 2.1: no transform feedback

Mesa computes the advertised version from a fixed requirement list in
`src/mesa/main/version.c` (`compute_version`). GL 3.0 requires 22 extensions
plus `GLSLVersion >= 130`, four colour attachments and four samples. Zink's
screen was checked against that list:

| Requirement | Status |
|---|---|
| `GL_ARB_shadow`, `ARB_vertex_shader`, `ARB_fragment_shader` | present |
| `GL_EXT_blend_equation_separate`, `EXT_stencil_two_side`, `EXT_texture_sRGB` | present |
| `GL_ARB_color_buffer_float`, `ARB_depth_buffer_float`, `ARB_half_float_vertex` | present |
| `GL_ARB_map_buffer_range`, `ARB_shader_texture_lod`, `ARB_texture_float` | present |
| `GL_ARB_texture_rg`, `ARB_texture_compression_rgtc`, `EXT_draw_buffers2` | present |
| `GL_ARB_framebuffer_object`, `EXT_framebuffer_sRGB`, `EXT_packed_float` | present |
| `GL_EXT_texture_array`, `EXT_texture_integer`, `EXT_texture_shared_exponent` | present |
| `GL_NV_conditional_render` | present |
| **`GL_EXT_transform_feedback`** | **missing** |

That is the only gap. Three independent checks confirm it is the whole story:

- `MESA_GLSL_VERSION_OVERRIDE=450` alone does **not** raise the version, so
  `consts->GLSLVersion` is already 460 (Zink sets `caps->glsl_feature_level =
  460` unconditionally in `zink_init_screen_caps`); the GLSL level is not the
  limit.
- Forcing `MESA_GL_VERSION_OVERRIDE=4.5` does produce a real 4.5 core context
  (`GL_VERSION: 4.5 (Core Profile)`, `GLSL: 4.50`), which proves the rest of
  the Mesa/Zink/GLX stack is functional — it is a *capability* gap, not a
  plumbing gap. It is not a usable configuration: it advertises transform
  feedback that the driver cannot implement.
- `compute_version_es2` requires `EXT_transform_feedback` for GLES 3.0 as
  well, which is why the same screen reports GLES 2.0.

Root cause on the Vulkan side: the vendor driver has **no
`VK_EXT_transform_feedback`** (and no `VK_EXT_geometry_shader`), consistent
with `geometryShader = false` and `maxGeometryShaderInvocations = 0` for this
Rogue core. Zink implements GL stream output only through
`VK_EXT_transform_feedback`:

```c
caps->max_stream_output_buffers =
   screen->info.have_EXT_transform_feedback ? ... : 0;
```

so `caps.stream_output` is false and `GL_EXT_transform_feedback` is never
enabled.

Both GL 3.0 and GLES 3.0 make transform feedback core, so **no amount of
extension shimming in the ICD can lift the 2.1 ceiling**. The vendor's own GLES
driver does support GLES 3.2, which means the hardware can emulate transform
feedback in software; the vendor Vulkan driver simply does not expose it.

## Stability: Zink crashes on the first draw

Even at GL 2.1 the path is not usable:

| Program | Result |
|---|---|
| `glxinfo -B` | exit 0, renderer/version as above (never draws) |
| `glxgears` | SIGSEGV (139) after printing `GL_RENDERER`, before the first frame |
| `glmark2 --validate` | SIGSEGV (139), no validation result |
| `glmark2 -b :duration=2.0` | SIGSEGV (139), no score |

`gdb` shows the fault in a driver worker thread (`glmark2:gdrv0`) with
`lr = 0xffffffff` and a corrupted backtrace — the same signature class as the
`vkQueueSubmit` fault, i.e. a vendor call reached with a null or bogus
structure. Zink also warns that the device lacks `VK_EXT_depth_clip_enable`
("Incorrect rendering will happen"), so even a fixed crash would need that
feature for correct clipping.

## Mesa `pvr` remains blocked separately

Mesa's own `pvr` Vulkan driver cannot initialise because it targets the
`drm/imagination` kernel interface, while this phone runs the vendor
`pvrsrvkm` module:

```
MESA: error: ZINK: vkEnumeratePhysicalDevices failed (VK_ERROR_INITIALIZATION_FAILED)
MESA: error: ZINK: failed to choose pdev
```

Note on the DRI3 message: `xdpyinfo -queryExtensions` on Termux:X11 `:1`
advertises both `DRI3` (opcode 147) and `Present` (opcode 146). Mesa's GLX emits
its DRI3 warning when *its* DRI3 screen setup fails for a driver, not because
the X server lacks the extension.

## Decision

- Desktop OpenGL on this phone stays on **GL4ES over vendor GLES** (GL 2.1),
  documented in [README.md](README.md). Zink is **not** enabled in
  `desktop/gpu-env.sh`.
- The `libsysvk` work is kept: the empty-command-buffer fix is a correctness
  fix for the ICD, and the robustness2 shim plus `SYSVK_DEBUG` tracing are the
  foundation for any later Vulkan work.
- Do **not** advertise OpenGL 3.x/4.x for the GE8322 through Zink. The ceiling
  is a missing Vulkan feature that GL 3.0 requires, not a configuration issue.
- The verification harness pins the current state: `desktop-gl-zink-screen`
  asserts that the screen still starts and still reports GL 2.1, and
  `desktop-gl-zink-draw` is an XFAIL recording the first-draw crash.

## Routes to GL 3.x and beyond

1. **Emulate transform feedback** (Zink/Mesa patch, not an ICD patch). Zink
   could lower stream output to SSBO writes with rasteriser discard, which is
   how software GL implementations provide TF. This is upstreamable work in
   `src/gallium/drivers/zink`, and it would lift both the GL and GLES ceilings.
2. **Mesa `pvr`** (Phase 4). Requires either the `drm/imagination` kernel
   interface or a user-space shim over `pvrsrvkm`; this is the long-term route
   and also the one that removes the vendor GLES driver from the picture.
3. **Vendor GLES 3.2 through libhybris** for ES workloads. Already works and is
   unaffected by this gate; it is the correct target for anything that only
   needs GLES.

## Evidence files

- `vulkaninfo` full output: captured per run under `/tmp/opencode/`, not
  committed (it contains device identifiers).
- Zink screen log: `zink Vulkan 1.3(PowerVR Rogue GE8322 ...)`, GL 2.1.
- Requirement matrix: derived from a full `glxinfo` extension dump with Zink.
- Override controls: `MESA_GLSL_VERSION_OVERRIDE=450` (no change),
  `MESA_GL_VERSION_OVERRIDE=4.5` (4.5 core context, proves the stack works).
- Crash logs: `glxgears`, `glmark2 --validate`, `glmark2 -b :duration=2.0`,
  all exit 139; `gdb` backtrace in thread `glmark2:gdrv0`.
