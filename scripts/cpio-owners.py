#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""
Normalize SVR4 newc cpio header ownership (c_uid=0, c_grp=0) and set
setuid bit (04755) on sbin/mount.nfs when built inside an unprivileged container.
"""

import sys

SETUID_PATHS = {b"sbin/mount.nfs", b"sbin/mount.nfs4"}


def align4(n: int) -> int:
    return (n + 3) & ~3


def process_cpio(data: bytes) -> bytes:
    out = bytearray()
    pos = 0
    n = len(data)

    while pos < n:
        if pos + 110 > n:
            out.extend(data[pos:])
            break

        hdr = bytearray(data[pos : pos + 110])
        magic = bytes(hdr[0:6])
        if magic not in (b"070701", b"070702"):
            raise RuntimeError(f"Unexpected cpio magic {magic!r} at offset {pos}")

        c_mode = int(hdr[14:22], 16)
        c_filesize = int(hdr[54:62], 16)
        c_namesize = int(hdr[94:102], 16)

        name_start = pos + 110
        name_end = name_start + c_namesize
        data_start = align4(name_end)
        data_end = data_start + c_filesize
        next_pos = align4(data_end)

        raw_name = data[name_start : name_end - 1].lstrip(b"./")
        if raw_name == b"TRAILER!!!":
            out.extend(hdr)
            out.extend(data[name_start:])
            break

        # Force root ownership (uid=0, grp=0)
        hdr[22:30] = b"00000000"
        hdr[30:38] = b"00000000"
        if raw_name in SETUID_PATHS:
            c_mode |= 0o4000
            hdr[14:22] = f"{c_mode:08X}".encode("ascii")

        out.extend(hdr)
        out.extend(data[name_start:next_pos])
        pos = next_pos

    return bytes(out)


def main() -> int:
    if len(sys.argv) != 2:
        print(f"Usage: {sys.argv[0]} <archive.cpio>", file=sys.stderr)
        return 1
    path = sys.argv[1]
    with open(path, "rb") as f:
        raw = f.read()
    fixed = process_cpio(raw)
    with open(path, "wb") as f:
        f.write(fixed)
    return 0


if __name__ == "__main__":
    sys.exit(main())
