# Hardware video encoders and decoders (SPRD / Unisoc)

This is the video counterpart to the vendor Vulkan/GLES work. It drives the
phone's SPRD OpenMAX IL components through libhybris inside the Kali chroot, so
Kali applications can encode and decode on the dedicated video sub-processor
instead of the CPU.

> **Status:** verified on the phone (SM-A032F, Android 14, armhf Kali).
>
> *Encoding:* `OMX.sprd.h264.encoder` and `OMX.sprd.h265.encoder` produce
> elementary streams that `ffmpeg` decodes cleanly at 320x240, 640x480 and
> 1280x720, from NV12 or I420 input, in file and pipe modes. The software
> `OMX.sprd.mpeg4.encoder` and `OMX.sprd.h263.encoder` also work.
>
> *Decoding:* `omx-dec` decodes H.264, HEVC, MPEG-4, H.263 and VP9 in hardware.
> H.264, HEVC and VP9 are bit-exact against FFmpeg's software decoder at
> 320x240, 640x480 and 1280x720; MPEG-4 matches luma exactly and chroma within
> ±1; H.263 is bit-exact. NV12 and I420 output are both verified.
>
> VP8 encoding, AV1 decoding, and rate-control tuning are **not** verified.

## What the vendor exposes

`libsprd_omx_core.so` registers these encoders:

| Component | Library | Runs on |
| --- | --- | --- |
| `OMX.sprd.h264.encoder` | `libomx_avcenc_hw_sprd.so` | `/dev/sprd_vsp` |
| `OMX.sprd.h265.encoder` | `libomx_hevcenc_hw_sprd.so` | `/dev/sprd_vsp` |
| `OMX.sprd.mpeg4.encoder` | `libomx_m4vh263enc_sw_sprd.so` | CPU |
| `OMX.sprd.h263.encoder` | `libomx_m4vh263enc_sw_sprd.so` | CPU |

and these decoders:

| Component | Library | Runs on | Verified |
| --- | --- | --- | --- |
| `OMX.sprd.h264.decoder` | `libomx_avcdec_hw_sprd.so` | `/dev/sprd_vsp` | yes |
| `OMX.sprd.hevc.decoder` | `libomx_hevcdec_hw_sprd.so` | `/dev/sprd_vsp` | yes |
| `OMX.sprd.mpeg4.decoder` | `libomx_m4vh263dec_hw_sprd.so` | `/dev/sprd_vsp` | yes |
| `OMX.sprd.h263.decoder` | `libomx_m4vh263dec_hw_sprd.so` | `/dev/sprd_vsp` | yes |
| `OMX.sprd.vp9.decoder` | `libomx_vp9dec_hw_sprd.so` | `/dev/sprd_vsp` | yes |
| `OMX.sprd.vpx.decoder` | `libomx_vpxdec_hw_sprd.so` | `/dev/sprd_vsp` | not driven |
| `OMX.sprd.av1.decoder` | `libomx_av1dec_sw_sprd.so` | CPU | no (headers lack the enum) |

`/vendor/etc/media_codecs.xml` marks the MPEG-4/H.263 decoders with
`requires-allocate-on-output-ports` and `needs-flush-before-disable`, and
limits them to 1920x2032. The H.264/HEVC decoders are limited to 1920x3840.

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
omx-enc / omx-dec (glibc, Kali armhf)
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
              |
              +-- encoders
              |     +-- libomx_avcenc_hw_sprd.so
              |     +-- libomx_hevcenc_hw_sprd.so
              |     |     +-- /dev/sprd_vsp, /dev/ion
              |     +-- libomx_m4vh263enc_sw_sprd.so   (MPEG-4 / H.263, CPU)
              |
              +-- decoders
                    +-- libomx_avcdec_hw_sprd.so
                    +-- libomx_hevcdec_hw_sprd.so
                    +-- libomx_m4vh263dec_hw_sprd.so    (MPEG-4 / H.263)
                    +-- libomx_vp9dec_hw_sprd.so
                          +-- /dev/sprd_vsp, /dev/ion
```

`omx-dec` reuses the encoder's libhybris bootstrap, `libgbm-shim.so`, and the
`libstagefrighthw.so` handler/`AMessage::post` patches unchanged.

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

The shim is cross-compiled on the host; `omx-enc` and `omx-dec` are compiled
inside the armhf chroot. The OpenMAX headers are standard AOSP headers
(Apache-2.0) and are fetched at build time, not vendored.

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
/opt/pvr-bridge/encoders/bin/omx-dec
/opt/pvr-bridge/encoders/bin/omx-dec-env
/opt/pvr-bridge/encoders/lib/libgbm-shim.so
/opt/pvr-bridge/encoders/include/OMX_*.h
```

`omx-enc-env` and `omx-dec-env` set the libhybris environment and exec the
matching tool, so they can be called directly from a Kali shell once the
Android bind mounts are present.

## Encoder usage

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

## Encoder verified results

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

## Decoder usage

```text
omx-dec -c h264|h265|h263|mpeg4|vp9 [-p nv12|i420]
        [-i IN|-] [-o OUT|-] [-n FRAMES] [-q] [-v] [--info]
```

| Option | Meaning |
| --- | --- |
| `-c` | `h264`, `h265`, `h263`, `mpeg4` (Annex B / start-code streams) or `vp9` (IVF); required |
| `-p` | output layout: `nv12` (default) or `i420` |
| `-i`, `-o` | input/output path, `-` for stdin/stdout |
| `-n` | stop after N output frames (default: until EOS) |
| `-q`, `-v` | quiet / per-frame logging |
| `--info` | instantiate the component, print both port definitions, exit |

Output is tightly packed at the visible (cropped) size: `W*H*3/2` bytes per
frame, no stride padding. H.264/HEVC crop is honoured; for I420 the U and V
planes are written separately.

### Examples

```sh
# H.264 elementary stream -> NV12
omx-dec-env -c h264 -i in.h264 -o out.nv12

# HEVC -> planar I420
omx-dec-env -c h265 -p i420 -i in.h265 -o out.yuv

# VP9 must be IVF (DKIF); raw VP9 has no byte-level frame delimiter
omx-dec-env -c vp9 -i in.ivf -o out.nv12

# Inspect a component without decoding
omx-dec-env -c mpeg4 --info
```

## Decoder quirks

The SPRD decoder components are not fully standard-compliant. `omx-dec`
implements the sequences that actually work; the source comments record the
reasoning.

### Output-port reconfiguration uses the Loaded reset

`OMX_EventPortSettingsChanged` is handled with the same teardown Android uses
at end of playback:

```text
Executing -> Idle -> Loaded
  free all input and output buffers
  read the new port definitions
  allocate in Loaded
Loaded -> Idle -> Executing
  prime the output buffers
```

Two traps make the usual Android `PortDisable / free / PortEnable / allocate`
sequence unusable here:

* `OMX_CommandPortDisable` is accepted but **never completes**. The port is
  left in `PortInfo::DISABLING` (`mTransition = 1`), and the matching
  `OMX_CommandPortEnable` then hits
  `SprdSimpleOMXComponent.cpp:621 CHECK_EQ(port->mTransition, PortInfo::NONE)`.
* `FreeBuffer()` asserts unless the port is disabled or the component is in
  `Loaded` (`SprdSimpleOMXComponent.cpp:249`).

The `Loaded` reset satisfies both: freeing is legal in `Loaded`, and no port
command is needed.

### Reconfiguration rewinds submission

After a `Loaded` reset every input buffer is gone, so `omx-dec` restarts the
stream:

* H.264 / HEVC: from the most recent SPS (NAL type 7 / 33).
* MPEG-4 / H.263 / VP9: from byte 0, because their headers are not repeated.

This is why HEVC at 320x240 and MPEG-4/H.263 (which start at the component's
352x288 default and shrink to the real size) need the reset even though H.264
at the same resolution does not.

### Input and output are interleaved

Hardware decoders stop consuming input once every output buffer is full.
`omx-dec` therefore drains and re-primes output buffers *while* it submits
input, instead of submitting the whole stream first.

### Per-codec framing

| Codec | Unit submitted per buffer |
| --- | --- |
| H.264 | one Annex B NAL unit |
| HEVC | one Annex B NAL unit |
| MPEG-4 | one VOP (`00 00 01 b6`), with VOS/VO/VOL folded into the first frame |
| H.263 | one picture, split on the 22-bit PSC (`00 00 10xxxxxx`) |
| VP9 | one IVF frame |

MPEG-4 folding matches Android's `MPEG4Source`, which prepends the VOL header
to the first sample. H.263 does not use `00 00 01` start codes at all, which is
why a generic Annex B splitter submits nothing for it.

### Teardown

Like the encoder, `omx-dec` does not call `OMX_FreeHandle()`/`OMX_Deinit()`:
unloading the vendor codec aborts inside libhybris's
`unregister_tls_module()`. The process exits and the kernel reclaims the
mappings.

## Decoder verified results

Each stream was decoded on the phone, pulled to a desktop, and compared with
FFmpeg's software decoder for the same stream. "Bit-exact" means every byte of
every frame matches.

| Codec | Size | Frames | Output | Comparison |
| --- | --- | --- | --- | --- |
| H.264 | 320x240 | 30 | NV12 | bit-exact |
| H.264 | 320x240 | 30 | I420 | bit-exact |
| H.264 | 640x480 | 30 | NV12 | bit-exact |
| H.264 | 1280x720 | 10 | NV12 | bit-exact |
| HEVC | 320x240 | 30 | NV12 | bit-exact |
| HEVC | 640x480 | 30 | NV12 | bit-exact |
| HEVC | 1280x720 | 10 | NV12 | bit-exact |
| MPEG-4 | 320x240 | 30 | NV12 | luma bit-exact, chroma ±1 |
| H.263 | 320x240 | 30 | NV12 | bit-exact |
| VP9 | 320x240 | 30 | NV12 | bit-exact |

The MPEG-4 ±1 is a chroma rounding convention difference: the encoder input was
neutral 128, the hardware decoder returns 128, and FFmpeg's MPEG-4 decoder
returns 127. Luma is identical.

The device harness (`scripts/verify-all-device.sh`) checks each codec end to
end: it encodes three flat frames with `omx-enc`, decodes them with `omx-dec`,
and requires exactly `3 * 115200` bytes. VP9 uses the committed IVF fixture in
`testdata/` because there is no VP9 encoder on the device.

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

`testdata/` holds one synthetic 143-byte VP9 IVF fixture (flat grey frames,
generated with FFmpeg) so the device harness can exercise the VP9 decoder. It
contains no user, game, or device data.
