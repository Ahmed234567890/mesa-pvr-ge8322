#!/bin/bash
# Assemble the PowerVR GE8322 Vulkan bridge bundle.
#
# Expected build tree ($BUILD):
#   libhybris/                  Linux-on-droid libhybris build
#   build-ahb/                  ahb-wrapper build
#   build-sysvk/                sysvk build
#   build-wsi/                  WSI layer build
#   pvr-smoke                   cross-compiled smoke test
#   pvr-mount-private           static MS_PRIVATE|MS_REC helper
#
# Target location on the phone: /opt/pvr-bridge inside the Kali chroot.
set -euo pipefail

: "${BUILD:?set BUILD to the bridge build tree}"
STAGE=${STAGE:-"$BUILD/stage"}
PREFIX=${PREFIX:-/opt/pvr-bridge}

required=(
    "$BUILD/libhybris/hybris/common/.libs/libhybris-common.so.1.0.0"
    "$BUILD/libhybris/hybris/hardware/.libs/libhardware.so.2.0.0"
    "$BUILD/libhybris/hybris/properties/.libs/libandroid-properties.so.1.0.0"
    "$BUILD/libhybris/hybris/common/q/.libs/q.so"
    "$BUILD/libhybris/hybris/common/o/.libs/o.so"
    "$BUILD/libhybris/hybris/common/n/.libs/n.so"
    "$BUILD/libhybris/hybris/common/mm/.libs/mm.so"
    "$BUILD/libhybris/hybris/common/jb/.libs/jb.so"
    "$BUILD/build-ahb/libahb-wrapper.so.1.0.0"
    "$BUILD/build-sysvk/libsysvk.so"
    "$BUILD/build-wsi/libVkLayer_window_system_integration.so"
    "$BUILD/pvr-smoke"
    "$BUILD/pvr-mount-private"
)
for f in "${required[@]}"; do
    [ -e "$f" ] || { echo "stage: missing $f" >&2; exit 1; }
done

rm -rf "$STAGE"
install -d "$STAGE/lib" "$STAGE/libhybris/linker" \
           "$STAGE/share/vulkan/icd.d" "$STAGE/share/vulkan/implicit_layer.d" \
           "$STAGE/bin"

# --- libraries -------------------------------------------------------------
install -m 755 "$BUILD/libhybris/hybris/common/.libs/libhybris-common.so.1.0.0" "$STAGE/lib/"
ln -sfn libhybris-common.so.1.0.0 "$STAGE/lib/libhybris-common.so.1"

install -m 755 "$BUILD/libhybris/hybris/hardware/.libs/libhardware.so.2.0.0" "$STAGE/lib/"
ln -sfn libhardware.so.2.0.0 "$STAGE/lib/libhardware.so.2"

install -m 755 "$BUILD/libhybris/hybris/properties/.libs/libandroid-properties.so.1.0.0" "$STAGE/lib/"
ln -sfn libandroid-properties.so.1.0.0 "$STAGE/lib/libandroid-properties.so.1"

install -m 755 "$BUILD/build-ahb/libahb-wrapper.so.1.0.0" "$STAGE/lib/"
ln -sfn libahb-wrapper.so.1.0.0 "$STAGE/lib/libahb-wrapper.so.1"
ln -sfn libahb-wrapper.so.1 "$STAGE/lib/libahb-wrapper.so"

install -m 755 "$BUILD/build-sysvk/libsysvk.so" "$STAGE/lib/"
install -m 755 "$BUILD/build-wsi/libVkLayer_window_system_integration.so" "$STAGE/lib/"

# --- Android linker plugins ------------------------------------------------
for m in q o n mm jb; do
    install -m 755 "$BUILD/libhybris/hybris/common/$m/.libs/$m.so" "$STAGE/libhybris/linker/$m.so"
done

# --- RPATH: make every bundled library find its siblings -------------------
while IFS= read -r -d '' f; do
    patchelf --set-rpath '$ORIGIN' "$f"
done < <(find "$STAGE/lib" -maxdepth 1 -type f -name '*.so*' -print0)
while IFS= read -r -d '' f; do
    patchelf --set-rpath '$ORIGIN/../../lib' "$f"
done < <(find "$STAGE/libhybris/linker" -maxdepth 1 -type f -name '*.so' -print0)

# --- Vulkan ICD manifest ---------------------------------------------------
cat > "$STAGE/share/vulkan/icd.d/sysvk_icd.json" <<EOF
{
    "file_format_version": "1.0.0",
    "ICD": {
        "api_version": "1.1.0",
        "library_path": "$PREFIX/lib/libsysvk.so"
    }
}
EOF

# --- Vulkan implicit layer manifest ----------------------------------------
# The layer is implicit: vkcube and other applications query VK_KHR_surface
# during instance creation, before they would enable an explicit layer by
# name.  Only advertise what this build actually implements:
#   X11 + headless WSI, non-experimental (no present timing / maintenance1 /
#   image compression control swapchain).
cat > "$STAGE/share/vulkan/implicit_layer.d/VkLayer_window_system_integration.json" <<EOF
{
    "file_format_version": "1.1.2",
    "layer": {
        "name": "VK_LAYER_window_system_integration",
        "type": "GLOBAL",
        "is_implicit": true,
        "library_path": "$PREFIX/lib/libVkLayer_window_system_integration.so",
        "api_version": "1.3.216",
        "implementation_version": "1",
        "description": "Window system integration layer (X11 DRI3/Present + headless)",
        "functions": {
            "vkNegotiateLoaderLayerInterfaceVersion": "wsi_layer_vkNegotiateLoaderLayerInterfaceVersion"
        },
        "instance_extensions": [
            {"name": "VK_KHR_surface", "spec_version": "25"},
            {"name": "VK_KHR_xcb_surface", "spec_version": "6"},
            {"name": "VK_KHR_xlib_surface", "spec_version": "6"},
            {"name": "VK_EXT_headless_surface", "spec_version": "1"},
            {"name": "VK_KHR_get_surface_capabilities2", "spec_version": "1"},
            {"name": "VK_EXT_surface_maintenance1", "spec_version": "1"}
        ],
        "device_extensions": [
            {"name": "VK_KHR_shared_presentable_image", "spec_version": "1", "entrypoints": ["vkGetSwapchainStatusKHR"]},
            {
                "name": "VK_KHR_swapchain",
                "spec_version": "70",
                "entrypoints": [
                    "vkAcquireNextImageKHR",
                    "vkCreateSwapchainKHR",
                    "vkDestroySwapchainKHR",
                    "vkGetSwapchainImagesKHR",
                    "vkQueuePresentKHR",
                    "vkAcquireNextImage2KHR",
                    "vkGetDeviceGroupPresentCapabilitiesKHR",
                    "vkGetDeviceGroupSurfacePresentModesKHR",
                    "vkGetPhysicalDevicePresentRectanglesKHR"
                ]
            },
            {"name": "VK_KHR_present_id", "spec_version": "1"}
        ],
        "disable_environment": {
            "DISABLE_WSI_LAYER": "1"
        }
    }
}
EOF

# --- runtime environment ---------------------------------------------------
cat > "$STAGE/bin/pvr-env" <<'EOF'
# Source this file: . /opt/pvr-bridge/bin/pvr-env
# Configures the PowerVR Vulkan bridge inside the Kali chroot.
PVR_BRIDGE="${PVR_BRIDGE:-/opt/pvr-bridge}"

export PATH="$PVR_BRIDGE/bin${PATH:+:$PATH}"
export LD_LIBRARY_PATH="$PVR_BRIDGE/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

# libhybris uses the Android linker plugin named after the device SDK.
export HYBRIS_LINKER_DIR="$PVR_BRIDGE/libhybris/linker"
export HYBRIS_ANDROID_SDK_VERSION="${HYBRIS_ANDROID_SDK_VERSION:-34}"
export HYBRIS_LD_LIBRARY_PATH="/system/lib:/vendor/lib:/vendor/lib/hw:/odm/lib:/apex/com.android.runtime/lib"

# Force our ICD and, unless PVR_NO_LAYER=1, auto-enable our WSI layer.
# The layer is installed as implicit: applications such as vkcube ask for
# VK_KHR_surface during instance creation, before an explicit layer could be
# enabled by name.
export VK_ICD_FILENAMES="$PVR_BRIDGE/share/vulkan/icd.d/sysvk_icd.json"
unset VK_INSTANCE_LAYERS VK_LAYER_PATH
if [ "${PVR_NO_LAYER:-0}" = "1" ]; then
    unset VK_ADD_IMPLICIT_LAYER_PATH
else
    export VK_ADD_IMPLICIT_LAYER_PATH="$PVR_BRIDGE/share/vulkan/implicit_layer.d"
fi

# Mesa's device-select and anti-lag implicit layers can intercept the vendor
# ICD's device selection and presentation.  Do not use "~implicit~" here: it
# would also disable the WSI layer above.
export VK_LOADER_LAYERS_DISABLE="${VK_LOADER_LAYERS_DISABLE:-VK_LAYER_MESA_device_select,VK_LAYER_MESA_anti_lag}"

# The WSI layer resolves AHardwareBuffer_* with dlsym(RTLD_DEFAULT, ...).
export LD_PRELOAD="$PVR_BRIDGE/lib/libahb-wrapper.so${LD_PRELOAD:+:$LD_PRELOAD}"

if [ "${PVR_DEBUG:-0}" = "1" ]; then
    export VK_LOADER_DEBUG="${VK_LOADER_DEBUG:-error,warn,info}"
    export HYBRIS_LD_DEBUG="${HYBRIS_LD_DEBUG:-1}"
fi
EOF

cat > "$STAGE/bin/pvr-run" <<'EOF'
#!/bin/sh
# Run a command with the PowerVR Vulkan bridge environment.
set -eu
PVR_BRIDGE="${PVR_BRIDGE:-/opt/pvr-bridge}"
. "$PVR_BRIDGE/bin/pvr-env"
exec "$@"
EOF
install -m 755 "$BUILD/pvr-smoke" "$STAGE/bin/pvr-smoke"
install -m 755 "$BUILD/pvr-mount-private" "$STAGE/bin/pvr-mount-private"
chmod 755 "$STAGE/bin/pvr-run"
chmod 644 "$STAGE/bin/pvr-env"

# --- documentation ---------------------------------------------------------
cat > "$STAGE/README.md" <<'EOF'
# PowerVR GE8322 Vulkan bridge

Target: `/opt/pvr-bridge` inside the Kali NetHunter armhf chroot on the
Samsung Galaxy A03 Core (SM-A032F, SC9863A, PowerVR GE8322).

## What is here

| Path | Purpose |
| --- | --- |
| `lib/libsysvk.so` | Vulkan ICD shim; opens `vulkan.sp9863a.so` through libhybris |
| `lib/libVkLayer_window_system_integration.so` | Arm WSI layer (X11 DRI3/Present + headless) |
| `lib/libahb-wrapper.so` | `AHardwareBuffer_*` shim; resolves through `libnativewindow.so` |
| `lib/libhybris-common.so.1` | libhybris core |
| `lib/libhardware.so.2` | `hw_get_module()` bridge |
| `lib/libandroid-properties.so.1` | Android property bridge |
| `libhybris/linker/q.so` | Android 14 (SDK 34) linker plugin |
| `share/vulkan/icd.d/sysvk_icd.json` | ICD manifest |
| `share/vulkan/implicit_layer.d/...json` | implicit WSI layer manifest |
| `bin/pvr-env` | sourceable environment |
| `bin/pvr-run` | run one command with the environment |
| `bin/pvr-smoke` | display-independent device + clear/readback test |
| `bin/pvr-mount-private` | static `MS_PRIVATE\|MS_REC` helper for chroot bind mounts |

## Runtime prerequisites

Inside the chroot, these Android trees must be visible read-only:

* `/system`
* `/vendor`
* `/apex`
* `/dev` (property service socket, `/dev/pvr_sync`, `/dev/dri/*`)
* `/proc`
* `/sys`

Do **not** bind-mount `/linkerconfig`: without it the Q linker falls back to
its permissive default search paths, which is what the vendor HAL needs.

## Usage

```sh
. /opt/pvr-bridge/bin/pvr-env
pvr-smoke                 # device + clear/readback, no display needed
vulkaninfo --summary
vkcube --c 120            # requires Termux:X11
```

or, scoped to one command:

```sh
/opt/pvr-bridge/bin/pvr-run pvr-smoke
/opt/pvr-bridge/bin/pvr-run vulkaninfo --summary
```

For the X11 path, Termux:X11 must be running and its socket must be visible
inside the chroot (bind-mount
`/data/data/com.termux/files/usr/tmp/.X11-unix` onto `/tmp/.X11-unix`).
The current layer does not present into Xtigervnc/KeX; that needs a
readback/SHM WSI path.

`pvr-env` installs the WSI layer as an implicit layer through
`VK_ADD_IMPLICIT_LAYER_PATH`, and disables only Mesa's device-select and
anti-lag implicit layers so they cannot intercept presentation. Set
`PVR_DEBUG=1` before sourcing it to enable loader and libhybris debug output.
Set `PVR_NO_LAYER=1` to test the ICD without the WSI layer.

## Notes

* The vendor HAL is 32-bit softfp, but Vulkan entry points use
  `pcs("aapcs-vfp")` on both sides, so the hard-float build is correct.
* Stock Android 14 bionic is used first. If the vendor HAL refuses to load,
  an arm32 patched libc is the next step; no prebuilt arm32 patched libc is
  currently published by the reference project.
EOF

# --- report ----------------------------------------------------------------
echo "staged: $STAGE"
find "$STAGE" -printf '%y %P\n' | sort
