# PowerVR GE8322 Vulkan bridge

This is the vendor-HAL route for hardware Vulkan on the Samsung Galaxy A03
Core (SM-A032F, SC9863A, PowerVR GE8322). It is separate from the Mesa `pvr`
research in the rest of this repository.

> **Status:** host build and bundle validation are complete. The bundle has
> **not** been run on the phone yet. The first on-device step is the
> display-independent `pvr-smoke` test; X11 presentation comes after that.

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

The WSI layer owns swapchains. It allocates Android hardware buffers, exports
them as dma-buf file descriptors, and hands them to X11 through DRI3/Present.
The vendor HAL has no `VK_KHR_xcb_surface` or `VK_EXT_headless_surface`, so
the layer synthesizes its own opaque `VkSurfaceKHR` handles and never calls
the ICD's surface entry points.

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
| `build/wsi-layer/` | patches against `xMeM/vulkan-wsi-layer@d5624d4` |
| `build/armhf-toolchain.cmake` | Arm GNU Toolchain cross-compilation definition |
| `build/stage.sh` | assembles the `/opt/pvr-bridge` bundle |

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

### 2. WSI layer

```sh
git clone https://github.com/xMeM/vulkan-wsi-layer.git "$BUILD/vulkan-wsi-layer"
cd "$BUILD/vulkan-wsi-layer"
git checkout d5624d42d8b2debbd910ad25662a05c751eb38b7
git apply /path/to/mesa-pvr-ge8322/bridge/cross/wsi-layer/0001-build-cross-compile.patch
git apply /path/to/mesa-pvr-ge8322/bridge/cross/wsi-layer/0002-layer-owned-surfaces.patch

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
second makes X11 and headless surface creation layer-owned.

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

### 5. Smoke test

```sh
"$TC/bin/arm-none-linux-gnueabihf-gcc" \
  --sysroot="$SYSROOT" -O2 -o "$BUILD/pvr-smoke" \
  /path/to/mesa-pvr-ge8322/bridge/src/pvr-smoke.c \
  -L"$SYSROOT/usr/lib/arm-linux-gnueabihf" \
  -Wl,-rpath-link,"$SYSROOT/usr/lib/arm-linux-gnueabihf" -lvulkan
```

### 6. Bundle

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

## Test order

```sh
chroot /data/local/nhsystem/kali-armhf /usr/bin/env -i \
  HOME=/root PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
  /bin/bash -lc '. /opt/pvr-bridge/bin/pvr-env; pvr-smoke'
```

`pvr-smoke` should print the PowerVR device name, queue families, and a
readback of `64 128 191 255` (the clear color `0.25, 0.50, 0.75, 1.0`).

If it fails:

1. `PVR_NO_LAYER=1` — isolates the ICD from the WSI layer.
2. `PVR_DEBUG=1` — enables loader and libhybris diagnostics.
3. If the vendor HAL cannot load against stock Android 14 bionic, an arm32
   patched libc is the next thing to investigate. The reference project only
   publishes an arm64 patched libc.

After the smoke test passes:

```sh
/opt/pvr-bridge/bin/pvr-run vulkaninfo --summary
```

Then start Termux:X11, bind its socket directory onto `/tmp/.X11-unix`, and
run `vkcube`.

## Known limitations

- **Termux:X11 only.** The layer presents through X11 DRI3/Present with
  Android hardware buffers. It does not present into Xtigervnc or NetHunter
  KeX; those need a readback/shared-memory WSI path.
- **Vendor HAL only.** This does not make Mesa's `pvr` driver work. The
  `pvrsrvkm` versus `drm/imagination` UAPI mismatch remains.
- **32-bit armhf userspace.** The vendor HAL is 32-bit; the bridge is built
  for the same ABI.
- **Stock bionic is untested on device.** The Q linker and the vendor HAL are
  expected to work with Android 14's bionic, but this has not been confirmed
  on the phone yet.

## Licensing

The bridge sources in this directory are MIT-licensed. The WSI layer patches
apply to Arm's MIT-licensed `vulkan-wsi-layer`. libhybris is LGPL-2.1. The
AHardwareBuffer shim here is an independent implementation; the GPL-2.0
`ahb-wrapper` from the reference project is not copied or required.
