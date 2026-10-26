#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""
Generate a Dropbear ed25519 host key file at build time so the initramfs
does not need to spend CPU cycles generating one on first boot.
"""

import os
import struct
import sys
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric import ed25519


def ssh_string(payload: bytes) -> bytes:
    return struct.pack(">I", len(payload)) + payload


def main() -> int:
    if len(sys.argv) != 2:
        print(f"Usage: {sys.argv[0]} <output_dropbear_ed25519_host_key>", file=sys.stderr)
        return 1

    out_path = sys.argv[1]
    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)

    priv_key = ed25519.Ed25519PrivateKey.generate()
    pub_key = priv_key.public_key()

    priv_bytes = priv_key.private_bytes(
        encoding=serialization.Encoding.Raw,
        format=serialization.PrivateFormat.Raw,
        encryption_algorithm=serialization.NoEncryption(),
    )
    pub_bytes = pub_key.public_bytes(
        encoding=serialization.Encoding.Raw,
        format=serialization.PublicFormat.Raw,
    )

    blob = (
        ssh_string(b"ssh-ed25519")
        + ssh_string(pub_bytes)
        + ssh_string(priv_bytes + pub_bytes)
    )

    with open(out_path, "wb") as f:
        f.write(blob)
    os.chmod(out_path, 0o600)
    return 0


if __name__ == "__main__":
    sys.exit(main())
