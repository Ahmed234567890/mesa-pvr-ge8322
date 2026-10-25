# mesa-pvr-ge8322

Unofficial, experimental Mesa PowerVR (`pvr`) research for the PowerVR Rogue
GE8322 (BVNC `22.86.104.218`), targeting the Samsung Galaxy A03 Core
(`SM-A032F`).

> **Status:** research only. There is no working GE8322 Mesa driver yet.
> The vendor kernel exposes a proprietary `pvrsrvkm` UAPI, while Mesa `pvr`
> expects mainline `drm/imagination`. That kernel/firmware path is the hard
> blocker.

## Start here

- [GE8322.md](GE8322.md) — hardware details, current findings, blockers, roadmap
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
