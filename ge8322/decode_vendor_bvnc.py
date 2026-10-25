#!/usr/bin/env python3
"""
Decode PowerVR GE8322 feature and errata data from a vendor pvrsrvkm.ko.

The offsets below are specific to the Samsung Galaxy A03 Core vendor module:

    Rogue_DDK_Android rogueddk 1.18@6267915 (rogue) sharkl3_android

Usage:
    python3 decode_vendor_bvnc.py /path/to/pvrsrvkm.ko
"""

import argparse
import struct

# File offsets in the vendor module.  These are not generic ELF offsets.
RODATA_OFF = 0x23DA10
GA_FEATURES = 0xEA00
GA_ERNS_BRNS = 0xEE28
FEATURE_BITPOS = 0xF078
FEATURE_MASKS = 0xF0B8
FEATURE_MAX_INDEXES = 0xF1A8
FEATURE_VALUES = 0xF1E8
RELA_RODATA = 0x24D6D0
RELA_RODATA_COUNT = 3758

FEATURE_NAMES = [
    "CDM_CONTROL_STREAM_FORMAT",
    "ECC_RAMS",
    "FBCDC",
    "FBCDC_ALGORITHM",
    "FBCDC_ARCHITECTURE",
    "FBC_MAX_DEFAULT_DESCRIPTORS",
    "FBC_MAX_LARGE_DESCRIPTORS",
    "LAYOUT_MARS",
    "META",
    "META_COREMEM_BANKS",
    "META_COREMEM_SIZE",
    "META_DMA_CHANNEL_COUNT",
    "NUM_CLUSTERS",
    "NUM_ISP_IPP_PIPES",
    "NUM_OSIDS",
    "NUM_RASTER_PIPES",
    "PHYS_BUS_WIDTH",
    "SCALABLE_TE_ARCH",
    "SCALABLE_VCE",
    "SIMPLE_PARAMETER_FORMAT_VERSION",
    "SLC_BANKS",
    "SLC_CACHE_LINE_SIZE_BITS",
    "SLC_SIZE_IN_KILOBYTES",
    "TILE_SIZE_X",
    "TILE_SIZE_Y",
    "VIRTUAL_ADDRESS_SPACE_BITS",
    "XE_ARCHITECTURE",
    "XPU_MAX_REGBANKS_ADDR_WIDTH",
    "XPU_MAX_SLAVES",
    "XPU_REGISTER_BROADCAST",
]


def u16(data, off, count):
    return struct.unpack_from("<%dH" % count, data, off)


def u32(data, off, count):
    return struct.unpack_from("<%dI" % count, data, off)


def u64(data, off, count):
    return struct.unpack_from("<%dQ" % count, data, off)


def bnc_key(b, n, c):
    # The vendor feature table keys ignore the V field.
    return (b << 48) | (n << 16) | c


def bvnc_key(b, v, n, c):
    # The vendor errata table keys include the full BVNC.
    return (b << 48) | (v << 32) | (n << 16) | c


def decode_features(data, bnc):
    feature_bitpos = u16(data, RODATA_OFF + FEATURE_BITPOS, 30)
    feature_masks = u64(data, RODATA_OFF + FEATURE_MASKS, 30)
    max_indexes = u16(data, RODATA_OFF + FEATURE_MAX_INDEXES, 30)

    # gaFeaturesValues is an array of 30 relocated pointers to uint16 arrays.
    value_arrays = {}
    for i in range(RELA_RODATA_COUNT):
        off, info, addend = struct.unpack_from(
            "<QQq", data, RELA_RODATA + i * 24
        )
        if FEATURE_VALUES <= off < FEATURE_VALUES + 30 * 8:
            idx = (off - FEATURE_VALUES) // 8
            value_arrays[idx] = addend

    if len(value_arrays) != 30:
        raise RuntimeError(
            "expected 30 feature value relocations, found %d"
            % len(value_arrays)
        )

    for i in range(33):
        entry = RODATA_OFF + GA_FEATURES + i * 32
        key, feature_mask, value_bits, _reserved = struct.unpack_from(
            "<QQQQ", data, entry
        )
        if key != bnc:
            continue

        print("feature table entry %d" % i)
        print("  key:         0x%016x" % key)
        print("  feature mask: 0x%016x" % feature_mask)
        print("  value bits:   0x%016x" % value_bits)
        print()
        print("  %-36s %s" % ("feature", "value"))
        print("  %-36s %s" % ("-" * 36, "-" * 12))
        for j, name in enumerate(FEATURE_NAMES):
            raw = (value_bits & feature_masks[j]) >> feature_bitpos[j]
            max_index = max_indexes[j]
            if raw >= max_index:
                value = "INVALID(raw=%d,max=%d)" % (raw, max_index)
            else:
                arr = u16(
                    data,
                    RODATA_OFF + value_arrays[j],
                    max_index,
                )
                value = arr[raw]
                if value == 0xFFFF:
                    value = "not present"
            print("  %-36s %s" % (name, value))
        return

    raise RuntimeError("feature entry for BNC 0x%016x not found" % bnc)


def decode_errata(data, bnc):
    for i in range(37):
        entry = RODATA_OFF + GA_ERNS_BRNS + i * 16
        key, mask = struct.unpack_from("<QQ", data, entry)
        if key != bnc:
            continue
        bits = [bit for bit in range(64) if mask & (1 << bit)]
        print()
        print("errata table entry %d" % i)
        print("  key:  0x%016x" % key)
        print("  mask: 0x%016x" % mask)
        print("  bits: %s" % ", ".join(str(bit) for bit in bits))
        return

    raise RuntimeError("errata entry for BNC 0x%016x not found" % bnc)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("module", help="path to vendor pvrsrvkm.ko")
    args = parser.parse_args()

    with open(args.module, "rb") as f:
        data = f.read()

    print("GE8322 (BNC 22.104.218)")
    decode_features(data, bnc_key(22, 104, 218))
    decode_errata(data, bvnc_key(22, 86, 104, 218))

    print()
    print("GE8300 22.102.54.38 (closest existing Mesa entry)")
    decode_features(data, bnc_key(22, 54, 38))
    decode_errata(data, bvnc_key(22, 102, 54, 38))


if __name__ == "__main__":
    main()
