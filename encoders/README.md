# Hardware video encoders (SPRD / Unisoc)

This is the hardware-encoding counterpart to the vendor Vulkan/GLES work. It
drives the phone's SPRD OpenMAX IL components through libhybris inside the Kali
chroot, so Kali applications can encode H.264 or HEVC on the dedicated video
sub-processor instead of the CPU.

> **Status:** verified on the phone (SM-A032F, Android 14, armhf Kali).
> `OMX.sprd.h264.encoder` and `OMX.sprd.h265.encoder` produce elementary
> streams that `ffmpeg` decodes cleanly at 320x240, 640x480 and 1280x720,
> from NV12 or I420 input, in file and pipe modes. The software
> `OMX.sprd.mpeg4.encoder` and `OMX.sprd.h263.encoder` also work at 640x480.
> Decoding through OpenMAX, VP8 encoding, and rate-control tuning are **not**
> verified.

## What the vendor exposes

`libsprd_omx_core.so` registers these encoders:

| Component | Library | Runs on |
| --- | --- | --- |
| `OMX.sprd.h264.encoder` | `libomx_avcenc_hw_sprd.so` | `/dev/sprd_vsp` |
| `OMX.sprd.h265.encoder` | `libomx_hevcenc_hw_sprd.so` | `/dev/sprd_vsp` |
| `OMX.sprd.mpeg4.encoder` | `libomx_m4vh263enc_sw_sprd.so` | CPU |
| `OMX.sprd.h263.encoder` | `libomx_m4vh263enc_sw_sprd.so` | CPU |

There is **no VP8 encoder component**. The core registers
`OMX.sprd.vpx.decoder` only, and while `/vendor/lib/libomx_vpxenc_hw_sprd.so`
exists, it is a bare VSP library: it exports no `OMX_ComponentInit` and no
OpenMAX symbols at all, so it cannot be driven through OpenMAX IL.
`omx-enc -c vp8` therefore fails with an explicit message instead of guessing.

The MPEG-4/H.263 pair is the vendor's software path. The core first probes for
a hardware variant (`libomx_m4vh263enc_hw_sprd.so`), which does not exist on
this build, prints `library "libomx_m4vh263enc_hw_sprd.so" not found`, then
falls back to the software library. That warning is expected and harmless.

The H.264/HEVC components allocate through `/dev/ion` and run on the VSP.
There is no `/dev/video*` node and no VA-API/V4L2-M2M stack, so FFmpeg's
`h264_v4l2m2m` and `h264_vaapi` cannot reach the hardware directly.

## Stack

```text
omx-enc (glibc, Kali armhf)
  |  dlopen libhybris-common
  |  hybris_dlopen / hybris_dlsym
  v
libhybris (Android 14 q linker)
  |
  +-- libgbm-shim.so   freestanding, DF_1_GLOBAL, loaded first
  |     - __cfi_slowpath / __cfi_slowpath_diag / __cfi_init no-ops
  |     - GraphicBufferMapper::lock/unlock (old vendor overloads)
  |     - amessage_post_sync()
  |
  +-- libsprd_omx_core.so
        +-- libstagefrighthw.so   SprdSimpleOMXComponent
              +-- libomx_avcenc_hw_sprd.so / libomx_hevcenc_hw_sprd.so
              |     +-- /dev/sprd_vsp, /dev/ion
              +-- libomx_m4vh263enc_sw_sprd.so   (MPEG-4 / H.263, CPU)
```

## Why the shim and the two patches

The vendor blobs were built for an older Android, and the Android 14 system
libraries they link against no longer match. Three separate problems had to be
solved; each is narrow and documented in the source.

### 1. Missing/renamed symbols and CFI

`libstagefrighthw.so` references the old
`GraphicBufferMapper::lock(handle, usage, Rect, void**, int*, int*)` and
`unlock(handle)` overloads. Android 14's `libui.so` only exports the 4-argument
`lock()` and `unlock(handle, unique_fd*)`. Bionic binds lazily and these calls
are normally never made for OMX-allocated buffers, but libhybris resolves
eagerly, so the load aborts.

Android 14 libraries are also built with CFI; their indirect calls go through
`__cfi_slowpath`, which needs a linker-populated CFI shadow that libhybris's
linker never creates. The first check dereferences NULL.

`libgbm-shim.so` supplies no-op CFI hooks and the old overloads, forwarding to
the real libui entry points. It must be loaded before any Android library and
built with `DF_1_GLOBAL` (`-Wl,-z,global`) so its symbols win global lookup.

### 2. AHandler layout mismatch

`SprdSimpleOMXComponent` allocates its `AHandlerReflector` with `new(56)`
(`libstagefrighthw.so+0xab82`). Android 14's `AHandler::deliverMessage()` uses
`mLock` at `+52`, `mDeliveryInProgress` at `+56`, `mStatus` at `+60` and
`mTimestampUs` at `+64/+68`. Those stores run past the end of the 56-byte
object and corrupt the neighbouring heap chunk — in practice the glibc mutex
libhybris allocated for `mLock`, which left the looper thread futex-waiting
forever.

`omx-enc` rewrites that single instruction to `movs r0, #120`, giving the
handler real slack.

### 3. Broken looper delivery

Even with the allocation widened, the vendor `AHandler`/`RefBase` control block
is still laid out for the old Android and the looper path frees the mutex
between `deliverMessage()`'s unlock and re-lock. Symbol interposition cannot
fix this: libhybris resolves a library's own `DT_NEEDED` group before the global
scope, so `AMessage::post` binds to the vendor group first.

`omx-enc` therefore rewrites the `AMessage::post` GOT slot
(`libstagefrighthw.so+0x10dbc`) to `amessage_post_sync()`, which calls the
handler's `onMessageReceived()` (vtable slot 6) directly. That is exactly what
the looper would have done, without the incompatible queue machinery.
`sendCommand()`, `emptyThisBuffer()` and `fillThisBuffer()` all go through this
path, so state transitions and buffer callbacks become synchronous.

### Teardown

`omx-enc` deliberately does **not** call `OMX_FreeHandle()`/`OMX_Deinit()`.
Unloading the vendor codec aborts inside libhybris's
`unregister_tls_module()` (`CHECK(mod.static_offset == SIZE_MAX)`). The process
is about to exit, so the kernel reclaims the mappings. This is fine for a
one-shot CLI; a long-lived server would need a linker-side fix.

## Build and install

The shim is cross-compiled on the host; `omx-enc` itself is compiled inside the
armhf chroot. The OpenMAX headers are standard AOSP headers (Apache-2.0) and
are fetched at build time, not vendored.

```sh
cd encoders
./fetch-omx-headers.sh
CXX=/path/to/arm-none-linux-gnueabihf-g++ \
SERIAL=192.168.1.16:46767 \
./build-omx-enc.sh
```

That installs, inside the Kali chroot:

```text
/opt/pvr-bridge/encoders/bin/omx-enc
/opt/pvr-bridge/encoders/bin/omx-enc-env
/opt/pvr-bridge/encoders/lib/libgbm-shim.so
/opt/pvr-bridge/encoders/include/OMX_*.h
```

`omx-enc-env` sets the libhybris environment and execs `omx-enc`, so it can be
called directly from a Kali shell once the Android bind mounts are present.

## Usage

```text
omx-enc -c h264|h265|mpeg4|h263 [-w W] [-h H] [-r FPS] [-b BPS]
        [-p nv12|i420] [-i IN|-] [-o OUT|-] [-n FRAMES] [-q] [-v]
```

| Option | Meaning |
| --- | --- |
| `-c` | `h264`, `h265` (hardware VSP) or `mpeg4`, `h263` (software); required |
| `-w`, `-h` | frame size, must be even (default 1280x720) |
| `-r` | frame rate (default 30) |
| `-b` | target bitrate in bits/s (default 4000000) |
| `-p` | input pixel format: `nv12` (default) or `i420` |
| `-i`, `-o` | input/output path, `-` for stdin/stdout |
| `-n` | stop after N frames (default: until EOF) |
| `-q`, `-v` | quiet / per-frame logging |

`-c vp8` is rejected: this device has no VP8 encoder component.

Input is tightly packed (no stride padding). The tool copies each frame into
the encoder's reported stride and slice height.

### File mode

```sh
omx-enc-env -c h264 -w 1280 -h 720 -r 30 -b 4000000 \
  -i frames.nv12 -o out.h264
```

### Pipe mode with FFmpeg

Decode with FFmpeg (software or any working path), scale to the encoder size,
and pipe NV12 into the hardware encoder:

```sh
ffmpeg -i input.mp4 -vf scale=1280:720 -r 30 \
       -f rawvideo -pix_fmt nv12 - \
  | omx-enc-env -c h264 -w 1280 -h 720 -r 30 -b 4000000 -i - -o - -q \
  | ffmpeg -f h264 -i - -c copy output.mp4
```

For HEVC, use `-c h265` and `-f hevc` on the muxer side.

### Verify an output

```sh
ffprobe -v error -count_frames -select_streams v:0 \
  -show_entries stream=codec_name,width,height,nb_read_frames \
  -of default=nw=1 out.h264
ffmpeg -v error -i out.h264 -f null -
```

## Verified results

All runs used the phone's vendor components through libhybris in the Kali
chroot, then the stream was pulled to a desktop and checked with FFmpeg.

| Codec | Size | Input | Frames | Result |
| --- | --- | --- | --- | --- |
| H.264 | 320x240 | NV12 | 30 | decodes, 30 frames |
| H.264 | 640x480 | I420 | 25 | decodes, 25 frames |
| H.264 | 1280x720 | NV12 | 30 | decodes, 30 frames, ~470 KB |
| HEVC | 320x240 | NV12 | 30 | decodes, 30 frames |
| HEVC | 640x480 | I420 | 25 | decodes, 25 frames |
| HEVC | 1280x720 | NV12 | 30 | decodes, 30 frames, ~468 KB |
| MPEG-4 | 640x480 | I420 | 25 | decodes, 25 frames (software) |
| H.263 | 640x480 | I420 | 25 | decodes, 25 frames (software) |

The H.264 output is Constrained Baseline (no B-frames) and the HEVC output is
Main profile, both from the component's defaults plus the explicit settings in
`omx-enc`.

The MPEG-4 and H.263 streams are raw elementary streams. FFmpeg needs the
container hinted:

```sh
ffmpeg -f m4v  -i out.m4v  -f null -   # MPEG-4 part 2
ffmpeg -f h263 -i out.h263 -f null -   # H.263
```

Without `-f m4v` FFmpeg may still probe MPEG-4 correctly, but the raw stream
carries no timestamps, so decoding to the null muxer can print a
"non monotonically increasing dts" warning. The frame count is still correct.

## Persisting the bind mounts

The encoders need `/system`, `/vendor`, `/apex`, `/dev`, `/proc` and `/sys`
inside the chroot. `phone/mount-kali-gpu.sh` sets those up, and
`phone/99-kali-pvr.sh` runs it from Magisk's `service.d` after boot:

```sh
adb push encoders/phone/mount-kali-gpu.sh  /data/local/tmp/
adb push encoders/phone/umount-kali-gpu.sh /data/local/tmp/
adb push encoders/phone/99-kali-pvr.sh     /data/adb/service.d/
adb shell su -c 'chmod 755 /data/local/tmp/mount-kali-gpu.sh \
  /data/local/tmp/umount-kali-gpu.sh /data/adb/service.d/99-kali-pvr.sh'
```

The boot log is `/data/local/tmp/kali-pvr-mount.log`. To detach everything
without deleting anything, run:

```sh
adb shell su -c 'sh /data/local/tmp/umount-kali-gpu.sh'
```

Both scripts only touch the chroot's Android mount points; the chroot itself,
`/opt/pvr-bridge`, the Prism stage and the encoder tools are never removed.

## Privacy boundary

This directory contains only our own source and setup scripts. It does not
include vendor libraries, firmware, OMX headers, captured video, or device
logs. `.omx-headers/` and `build/` are generated locally and ignored by git.
