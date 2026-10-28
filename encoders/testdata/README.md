# Decoder test fixtures

Small, generated elementary streams used by the device verification matrix
(`scripts/verify-all-device.sh`).

| File | Codec | Geometry | Frames | Notes |
| --- | --- | --- | --- | --- |
| `vp9-320x240-3f.ivf` | VP9 | 320x240 | 3 | IVF container; generated with `ffmpeg -c:v libvpx-vp9` from flat NV12 frames. |

VP9 frames carry no byte-level delimiter, so `omx-dec -c vp9` reads IVF rather
than an Annex B stream.  The other codecs are exercised by encoding a flat
frame on the device with `omx-enc` and decoding the result with `omx-dec`, so
they need no fixture.

Everything here is synthetic (flat grey/black frames); no user or game data.
