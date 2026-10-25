# GE8322 vendor feature data

Source: `/tmp/opencode/pvr/pvrsrvkm.ko` (vendor Rogue DDK 1.18@6267915)
Device: PowerVR Rogue GE8322, BVNC 22.86.104.218

The vendor module contains a `gaFeatures` table keyed by B/N/C (the V field is
not part of the key).  For BNC 22.104.218 the entry is:

- feature mask: `0x000000c5844b3025`
- feature value bitfield: `0x0558944cc402a591`
- reserved: `0`

Decoded feature values:

| Feature | Value |
|---|---:|
| CDM_CONTROL_STREAM_FORMAT | 1 |
| ECC_RAMS | not present |
| FBCDC | 3 |
| FBCDC_ALGORITHM | 3 |
| FBCDC_ARCHITECTURE | 1 |
| FBC_MAX_DEFAULT_DESCRIPTORS | 0 |
| FBC_MAX_LARGE_DESCRIPTORS | 0 |
| LAYOUT_MARS | 0 |
| META | not present |
| META_COREMEM_BANKS | not present |
| META_COREMEM_SIZE | not present |
| META_DMA_CHANNEL_COUNT | not present |
| NUM_CLUSTERS | 1 |
| NUM_ISP_IPP_PIPES | 7 |
| NUM_OSIDS | 8 |
| NUM_RASTER_PIPES | 1 |
| PHYS_BUS_WIDTH | 36 |
| SCALABLE_TE_ARCH | not present |
| SCALABLE_VCE | not present |
| SIMPLE_PARAMETER_FORMAT_VERSION | 1 |
| SLC_BANKS | 1 |
| SLC_CACHE_LINE_SIZE_BITS | 512 |
| SLC_SIZE_IN_KILOBYTES | 64 |
| TILE_SIZE_X | 16 |
| TILE_SIZE_Y | 16 |
| VIRTUAL_ADDRESS_SPACE_BITS | 40 |
| XE_ARCHITECTURE | 1 |
| XPU_MAX_REGBANKS_ADDR_WIDTH | not present |
| XPU_MAX_SLAVES | not present |
| XPU_REGISTER_BROADCAST | not present |

Errata table (`gaErnsBrns`) for BNC 22.104.218:

- mask: `0x000000000008e408`
- set bits: 3, 10, 13, 14, 15, 19

Closest Mesa device info is GE8300 22.102.54.38:

- feature mask: `0x000000c5844b3025` (same as GE8322)
- errata mask: `0x0000000000086508`
- differences in decoded values: GE8322 has FBCDC/FBCDC_ALGORITHM/FBCDC_ARCHITECTURE
  present, 7 ISP IPP pipes instead of 4, and one different errata bit.

This is data for a GE8322 device-info entry.  It is not a Mesa patch.  Mesa's
`src/imagination/README.md` contribution policy declines LLM-generated changes
to the PowerVR driver, so the actual Mesa code must be written and submitted by
the user.
