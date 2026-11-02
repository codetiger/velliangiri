#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Generate a 256-byte Velliangiri NAND boot manifest (VLBT v1)."""

import argparse
import binascii
import pathlib
import struct

BOOT_MANIFEST_MAGIC = 0x564C4254  # "VLBT"
BOOT_MANIFEST_VERSION = 1
FMI_PAGE_SIZE = 16384
FMI_SLC_PAGES = 128


def slc_row(peb: int, page: int = 0) -> int:
    """Convert interleaved boot-partition PEB index (0..31) to FMI row."""
    cau = peb % 2
    block = 1 + (peb // 2)
    return (1 << 20) | (cau << 19) | (block << 8) | page


def pack_slot(image_bytes: bytes, dtb_bytes: bytes, start_peb: int) -> bytes:
    """Pack a 92-byte struct boot_slot starting at start_peb."""
    img_size = len(image_bytes)
    img_crc = binascii.crc32(image_bytes) & 0xFFFFFFFF
    dtb_size = len(dtb_bytes)
    dtb_crc = binascii.crc32(dtb_bytes) & 0xFFFFFFFF

    total_img_pages = (img_size + FMI_PAGE_SIZE - 1) // FMI_PAGE_SIZE
    extents = []
    rem = total_img_pages
    peb = start_peb
    while rem > 0 and len(extents) < 8:
        cnt = min(rem, FMI_SLC_PAGES)
        extents.append((slc_row(peb, 0), cnt))
        rem -= cnt
        peb += 1
    while len(extents) < 8:
        extents.append((0, 0))

    dtb_pages = (dtb_size + FMI_PAGE_SIZE - 1) // FMI_PAGE_SIZE
    # DTB is placed in the last PEB of the 14-PEB slot (start_peb + 13)
    dtb_extent = (slc_row(start_peb + 13, 0), dtb_pages)

    out = struct.pack("<IIII", img_size, img_crc, dtb_size, dtb_crc)
    for r, c in extents:
        out += struct.pack("<II", r, c)
    out += struct.pack("<II", dtb_extent[0], dtb_extent[1])
    assert len(out) == 88
    return out


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--image", type=pathlib.Path, required=True, help="Path to uncompressed ARM Linux Image")
    ap.add_argument("--dtb", type=pathlib.Path, help="Default DTB path for both slots")
    ap.add_argument("--dtb-a", type=pathlib.Path, help="Slot A DTB path")
    ap.add_argument("--dtb-b", type=pathlib.Path, help="Slot B DTB path")
    ap.add_argument("--output", "-o", type=pathlib.Path, required=True, help="Output 256-byte manifest file")
    ap.add_argument("--seq", type=int, default=1, help="Manifest sequence number")
    ap.add_argument("--active-slot", type=int, choices=(0, 1), default=0)
    ap.add_argument("--pending-slot", type=int, default=0xFF)
    ap.add_argument("--tries", type=int, default=0)
    args = ap.parse_args()

    img = args.image.read_bytes()
    dtb_a = (args.dtb_a or args.dtb).read_bytes()
    dtb_b = (args.dtb_b or args.dtb).read_bytes()

    slot_a = pack_slot(img, dtb_a, start_peb=2)   # PEBs 2..15
    slot_b = pack_slot(img, dtb_b, start_peb=16)  # PEBs 16..29

    hdr = struct.pack(
        "<IIIBBBB",
        BOOT_MANIFEST_MAGIC,
        BOOT_MANIFEST_VERSION,
        args.seq,
        args.active_slot,
        args.pending_slot & 0xFF,
        args.tries & 0xFF,
        0,
    )
    body = hdr + slot_a + slot_b + (b"\x00" * 60)
    assert len(body) == 252
    crc = binascii.crc32(body) & 0xFFFFFFFF
    manifest = body + struct.pack("<I", crc)
    assert len(manifest) == 256
    args.output.write_bytes(manifest)
    print(f"[mkmanifest] Wrote {args.output} (256 bytes, seq={args.seq}, crc={crc:#010x})")


if __name__ == "__main__":
    main()
