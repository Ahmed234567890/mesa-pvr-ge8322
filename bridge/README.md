# PowerVR GE8322 Vulkan bridge

This is the vendor-HAL route for hardware Vulkan on the Samsung Galaxy A03
Core (SM-A032F, SC9863A, PowerVR GE8322). It is separate from the Mesa `pvr`
research in the rest of this repository.

> **Status:** verified on the phone (SM-A032F, Android 14, stock bionic).
> `pvr-smoke` enumerates the PowerVR Rogue GE8322 through the vendor HAL and
> reads back a rendered clear. `vkcube --c 120` renders and presents through
> Termux:X11 using X11 DRI3/Present.

## Stack

```text
Vulkan application (vkcube, glmark2, ...)
        |
  Khronos loader 1.4 (Kali armhf)
        |
  libVkLayer_window_system_integration.so   X11 DRI3/Present + headless
        |
  libsysvk.so (ICD shim, advertises Vulkan 1.1)
        |
  libhybris-common + q.so (Android 14 linker)
        |
  /vendor/lib/hw/vulkan.sp9863a.so (PowerVR HAL)
        |
  libsrv_um.so / libusc.so / libufwriter.so
        |
  /dev/dri/renderD128 + pvrsrvkm
```

The WSI layer owns swapchains. It asks the vendor HAL for an Android hardware
buffer (`vkGetMemoryAndroidHardwareBufferANDROID`), exports it as a dma-buf
file descriptor, and hands it to X11 through DRI3/Present. The
`AHardwareBuffer_*` entry points are resolved through libhybris from
`libnativewindow.so` by `libahb-wrapper.so`, which the layer loads itself
(`0004-resolve-ahb-wrapper.patch`). A session-wide `LD_PRELOAD` is therefore
not required, which matters because preloading a glibc library breaks Android
binaries started from desktop terminals. `pvr-env` still preloads the wrapper
for compatibility with older layer builds. The vendor HAL has
no `VK_KHR_xcb_surface` or `VK_EXT_headless_surface`, so the layer synthesizes
its own opaque `VkSurfaceKHR` handles and never calls the ICD's surface entry
points.

The layer is installed as a Vulkan **implicit** layer. Applications such as
`vkcube` query `VK_KHR_surface` while creating the instance, before they could
enable an explicit layer by name.

## Why this route

- Mesa `pvr` expects the mainline `drm/imagination` UAPI. This kernel exposes
  the proprietary `pvrsrvkm` UAPI, so the Mesa driver cannot bind to it.
- The vendor Vulkan HAL already exposes every device extension the WSI layer
  requires: external memory/fence/semaphore FD, AHardwareBuffer,
  `VK_EXT_queue_family_foreign`, `VK_KHR_maintenance1`, `VK_KHR_bind_memory2`,
  `VK_KHR_sampler_ycbcr_conversion`, and the memory-requirements extensions.
- libhybris lets a glibc armhf process load the 32-bit Android HAL and its
  bionic dependencies.

## Repository layout

| Path | Purpose |
| --- | --- |
| `src/sysvk/` | ICD shim that opens the vendor `vulkan` HAL through `hw_get_module()` |
| `src/ahb-wrapper/` | `AHardwareBuffer_*` entry points for the WSI layer, resolved through libhybris |
| `src/pvr-smoke.c` | display-independent device, clear, and readback test |
| `src/pvr-mount-private.c` | static `MS_PRIVATE\|MS_REC` helper for chroot bind mounts |
| `cross/wsi-layer/` | patches against `xMeM/vulkan-wsi-layer@d5624d4` |
| `cross/libhybris/` | bionic TLS compatibility patch for libhybris |
| `cross/armhf-toolchain.cmake` | Arm GNU Toolchain cross-compilation definition |
| `cross/stage.sh` | assembles the `/opt/pvr-bridge` bundle |
| `cross/phone-test-device.sh` | installs the bundle and bind-mounts the Android trees |
| `cross/phone-cleanup-device.sh` | detaches the bind mounts without touching the host |

No binaries, vendor blobs, firmware, or kernel modules are stored here.

## Build prerequisites

- Linux x86_64 host with CMake, make or ninja, pkg-config, `patchelf`,
  autoconf, automake, and libtool.
- [Arm GNU Toolchain 14.3](https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads)
  for `arm-none-linux-gnueabihf`.
- An armhf sysroot with the Kali development packages for glibc, libstdc++,
  X11, xcb (including `xcb-present`, `xcb-dri3`, `xcb-xfixes`), `libx11-xcb`,
  Vulkan headers/loader, and libdrm.
- Android platform headers. The reference build used the
  `android-headers-30` tree from the `android-vulkan-bridge` project.
- [Linux-on-droid/libhybris](https://github.com/Linux-on-droid/libhybris).
- [xMeM/vulkan-wsi-layer](https://github.com/xMeM/vulkan-wsi-layer) at commit
  `d5624d42d8b2debbd910ad25662a05c751eb38b7`.

The examples below assume:

```sh
BUILD=$HOME/mesa-pvr-build
SYSROOT=$HOME/armhf-sysroot
TC=$HOME/toolchains/arm-gnu-toolchain-14.3.rel1-x86_64-arm-none-linux-gnueabihf
```

## Build

### 1. libhybris

Build the Linux-on-droid fork for armhf. The Vulkan wrapper's header guard
must be relaxed for current Vulkan headers:

```sh
cd "$BUILD/libhybris"
sed -i 's/#if VK_HEADER_VERSION >= 269/#if 0/g' hybris/vulkan/vulkan.c
git apply /path/to/mesa-pvr-ge8322/bridge/cross/libhybris/0001-bionic-ctype-tls-hook.patch

./autogen.sh \
  --host=arm-linux-gnueabihf --build=x86_64-pc-linux-gnu \
  --with-android-headers="$BUILD/android-headers-30" \
  --enable-arch=arm --enable-property-cache --enable-experimental \
  --disable-glvnd --disable-clicd --prefix=/usr \
  CC="$TC/bin/arm-none-linux-gnueabihf-gcc" \
  CXX="$TC/bin/arm-none-linux-gnueabihf-g++" \
  CPPFLAGS="--sysroot=$SYSROOT" \
  CFLAGS="--sysroot=$SYSROOT -O2 -fPIC" \
  CXXFLAGS="--sysroot=$SYSROOT -O2 -fPIC" \
  LDFLAGS="--sysroot=$SYSROOT -L$SYSROOT/usr/lib/arm-linux-gnueabihf -Wl,-rpath-link,$SYSROOT/usr/lib/arm-linux-gnueabihf" \
  PKG_CONFIG_LIBDIR="$SYSROOT/usr/lib/arm-linux-gnueabihf/pkgconfig:$SYSROOT/usr/share/pkgconfig"
make -j"$(nproc)"
```

This produces `libhybris-common`, `libhardware`, `libandroid-properties`, the
Vulkan wrapper, and the linker plugins (`q.so`, `o.so`, `n.so`, `mm.so`,
`jb.so`).

The patch adds a hook for bionic's `__ctype_get_mb_cur_max()`. Bionic reads
`libc_globals` from its own TLS area, which libhybris does not initialize when
it loads `libc.so` into a glibc process. The first locale lookup therefore
dereferences `NULL + 0x410`. The hook returns the C locale's `MB_CUR_MAX`
value (`1`), which is the only locale the Android side uses here. It avoids
needing a patched arm32 bionic `libc.so`.

### 2. WSI layer

```sh
git clone https://github.com/xMeM/vulkan-wsi-layer.git "$BUILD/vulkan-wsi-layer"
cd "$BUILD/vulkan-wsi-layer"
git checkout d5624d42d8b2debbd910ad25662a05c751eb38b7
git apply /path/to/mesa-pvr-ge8322/bridge/cross/wsi-layer/0001-build-cross-compile.patch
git apply /path/to/mesa-pvr-ge8322/bridge/cross/wsi-layer/0002-layer-owned-surfaces.patch
git apply /path/to/mesa-pvr-ge8322/bridge/cross/wsi-layer/0003-init-memory-properties.patch
git apply /path/to/mesa-pvr-ge8322/bridge/cross/wsi-layer/0004-resolve-ahb-wrapper.patch

cmake -S . -B "$BUILD/build-wsi" \
  -DCMAKE_TOOLCHAIN_FILE=/path/to/mesa-pvr-ge8322/bridge/cross/armhf-toolchain.cmake \
  -DARMHF_TOOLCHAIN="$TC" -DARMHF_SYSROOT="$SYSROOT" \
  -DANDROID_HEADERS_DIR="$BUILD/android-headers-30" \
  -DBUILD_WSI_X11=ON -DBUILD_WSI_HEADLESS=ON \
  -DBUILD_WSI_WAYLAND=OFF -DBUILD_WSI_DISPLAY=OFF \
  -DVULKAN_WSI_LAYER_EXPERIMENTAL=OFF \
  -DBUILD_WSI_IMAGE_COMPRESSION_CONTROL_SWAPCHAIN=OFF \
  -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD/build-wsi" -j"$(nproc)"
```

The first patch adds the Android headers include path, removes the nonexistent
`android` link target, and fixes the X11 designated-initializer order. The
second makes X11 and headless surface creation layer-owned. The third
initializes `VkPhysicalDeviceMemoryProperties2` before passing it to
`vkGetPhysicalDeviceMemoryProperties2KHR`; the vendor HAL walks the struct's
`pNext` unconditionally, so an uninitialized stack value crashes it even
though Mesa ignores `pNext`. The fourth lets the layer load
`libahb-wrapper.so` itself instead of depending on a session-wide
`LD_PRELOAD`, so Vulkan presentation works from the global desktop
environment.

### 3. sysvk

```sh
cmake -S /path/to/mesa-pvr-ge8322/bridge/src/sysvk -B "$BUILD/build-sysvk" \
  -DCMAKE_TOOLCHAIN_FILE=/path/to/mesa-pvr-ge8322/bridge/cross/armhf-toolchain.cmake \
  -DARMHF_TOOLCHAIN="$TC" -DARMHF_SYSROOT="$SYSROOT" \
  -DANDROID_HEADERS_DIR="$BUILD/android-headers-30" \
  -DHYBRIS_LIBRARY_DIR="$BUILD/libhybris/hybris/hardware/.libs" \
  -DHYBRIS_COMMON_LIBRARY_DIR="$BUILD/libhybris/hybris/common/.libs" \
  -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD/build-sysvk" -j"$(nproc)"
```

### 4. AHardwareBuffer shim

```sh
cmake -S /path/to/mesa-pvr-ge8322/bridge/src/ahb-wrapper -B "$BUILD/build-ahb" \
  -DCMAKE_TOOLCHAIN_FILE=/path/to/mesa-pvr-ge8322/bridge/cross/armhf-toolchain.cmake \
  -DARMHF_TOOLCHAIN="$TC" -DARMHF_SYSROOT="$SYSROOT" \
  -DHYBRIS_INCLUDE_DIR="$BUILD/libhybris/hybris/include/hybris" \
  -DHYBRIS_LIBRARY_DIR="$BUILD/libhybris/hybris/common/.libs" \
  -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD/build-ahb" -j"$(nproc)"
```

The shim tries `libnativewindow.so` first and only falls back to
`libandroid.so`. On Android 14 `libandroid.so` is the framework hub: loading
it pulls in `libandroid_runtime`, `libhwui`, and `libcrypto`, and the process
aborts inside `libcrypto` before the AHB entry points can be used.
`libnativewindow.so` exports all four `AHardwareBuffer_*` functions and loads
cleanly through libhybris.

### 5. Smoke test

```sh
"$TC/bin/arm-none-linux-gnueabihf-gcc" \
  --sysroot="$SYSROOT" -O2 -o "$BUILD/pvr-smoke" \
  /path/to/mesa-pvr-ge8322/bridge/src/pvr-smoke.c \
  -L"$SYSROOT/usr/lib/arm-linux-gnueabihf" \
  -Wl,-rpath-link,"$SYSROOT/usr/lib/arm-linux-gnueabihf" -lvulkan
```

### 6. Mount helper

Android's toybox `mount` does not implement `--make-rprivate` and fails
silently, leaving recursive bind clones in the host's shared peer group.
Build the static helper so the device scripts can call `mount(2)` directly:

```sh
"$TC/bin/arm-none-linux-gnueabihf-gcc" \
  --sysroot="$SYSROOT" -static -O2 -o "$BUILD/pvr-mount-private" \
  /path/to/mesa-pvr-ge8322/bridge/src/pvr-mount-private.c
```

### 7. Bundle

```sh
BUILD="$BUILD" PREFIX=/opt/pvr-bridge \
  /path/to/mesa-pvr-ge8322/bridge/cross/stage.sh
```

The result is `$BUILD/stage`, about 8 MB unstripped. It contains only the
bridge libraries, linker plugins, manifests, environment scripts, and the
smoke test.

## Runtime layout

Push `stage/` to `/opt/pvr-bridge` inside the Kali chroot. The chroot also
needs these Android trees visible read-only:

- `/system`
- `/vendor`
- `/apex`
- `/dev` (property service, `/dev/pvr_sync`, `/dev/dri/*`)
- `/proc`
- `/sys`

Do **not** bind-mount `/linkerconfig`. Without it, the Q linker falls back to
permissive default search paths; Android's namespace configuration would
block `/vendor/lib/hw`.

Use `cross/phone-test-device.sh` to install the bundle and create the bind
mounts. It calls `bin/pvr-mount-private` after every recursive bind, because
toybox's `mount --make-rprivate` is unimplemented and silently leaves the
clone in the host's shared peer group. Unmounting such a clone propagates to
the host and can detach `/dev`, including binderfs. `phone-cleanup-device.sh`
uses the same helper before detaching each tree.

## Test order

```sh
chroot /data/local/nhsystem/kali-armhf /usr/bin/env -i \
  HOME=/root PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
  /bin/bash -lc '. /opt/pvr-bridge/bin/pvr-env; pvr-smoke'
```

`pvr-smoke` should print the PowerVR device name, queue families, and a
readback of `64 128 191 255` (the clear color `0.25, 0.50, 0.75, 1.0`).

Verified on the SM-A032F with the default environment:

```text
physical devices: 1
device 0: PowerVR Rogue GE8322
  api 1.3.216 driver 0x005fa40b vendor 0x1010 device 0x22104218 type 1
queue family 0: flags 0x7 count 2
readback first: 64 128 191 255
readback last:  64 128 191 255
pvr-smoke: OK
```

If it fails:

1. `PVR_NO_LAYER=1` — isolates the ICD from the WSI layer.
2. `PVR_DEBUG=1` — enables loader and libhybris diagnostics.
3. If loading fails before the device is enumerated, confirm the ctype patch
   was applied to `hybris/common/hooks.c`. Do not skip bionic property
   initialization: the vendor ICD needs it to find its libraries.

After the smoke test passes:

```sh
/opt/pvr-bridge/bin/pvr-run vulkaninfo --summary
```

Then start Termux:X11, bind its socket directory onto `/tmp/.X11-unix`, and
run `vkcube`:

```sh
chroot /data/local/nhsystem/kali-armhf /usr/bin/env -i \
  HOME=/root PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
  DISPLAY=:0 /bin/bash -lc '. /opt/pvr-bridge/bin/pvr-env; vkcube --c 120'
```

Expected output:

```text
Selected WSI platform: xcb
Selected GPU 0: PowerVR Rogue GE8322, type: IntegratedGpu
```

`vkcube` exits 0 after 120 frames and the cube is visible in the Termux:X11
window. The environment script already sets `VK_ADD_IMPLICIT_LAYER_PATH` and
the Mesa layer disable list; do not set `VK_INSTANCE_LAYERS` or `VK_LAYER_PATH`
as well.

## Known limitations

- **Termux:X11 only.** The layer presents through X11 DRI3/Present with
  Android hardware buffers and requires DRI3 1.2. Termux:X11 provides it;
  Xtigervnc/KeX report DRI3 1.0 and need a readback/shared-memory WSI path.
- **`libandroid.so` is unusable for AHB.** On Android 14 it pulls the whole
  framework into the glibc process and aborts in `libcrypto`. The shim uses
  `libnativewindow.so` instead and falls back to `libandroid.so` only if the
  former lacks the symbols.
- **Vendor HAL only.** This does not make Mesa's `pvr` driver work. The
  `pvrsrvkm` versus `drm/imagination` UAPI mismatch remains.
- **32-bit armhf userspace.** The vendor HAL is 32-bit; the bridge is built
  for the same ABI.
- **bionic TLS.** Stock Android 14 bionic works with the ctype hook above.
  Without it, the first locale lookup crashes because bionic's TLS area is not
  initialized in the glibc process. No patched `libc.so` is required.

## Licensing

The bridge sources in this directory are MIT-licensed. The WSI layer patches
apply to Arm's MIT-licensed `vulkan-wsi-layer`. libhybris is LGPL-2.1. The
AHardwareBuffer shim here is an independent implementation; the GPL-2.0
`ahb-wrapper` from the reference project is not copied or required.
