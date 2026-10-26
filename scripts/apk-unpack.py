#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""
Unpack Alpine .apk packages into a target directory and resolve shared-library
dependencies via APKINDEX.tar.gz and readelf without executing target binaries.
"""

import argparse
import gzip
import io
import os
import re
import shutil
import subprocess
import sys
import tarfile
import urllib.request


def log(msg: str) -> None:
    print(f"    {msg}", flush=True)


def fetch(url: str) -> bytes:
    with urllib.request.urlopen(url, timeout=60) as r:
        return r.read()


def untar_apk(blob: bytes, dest: str) -> list[str]:
    names = []
    raw = gzip.decompress(blob)
    with tarfile.open(fileobj=io.BytesIO(raw), mode="r:", ignore_zeros=True) as tf:
        for m in tf:
            if m.name.startswith(".") or m.name.startswith("/"):
                continue
            if ".." in m.name.split("/"):
                continue
            tf.extract(m, dest, set_attrs=False)
            if m.isfile():
                os.chmod(os.path.join(dest, m.name), m.mode)
            if m.isfile() or m.issym():
                names.append(m.name)
    return names


def parse_index(blob: bytes):
    index = None
    with tarfile.open(
        fileobj=io.BytesIO(gzip.decompress(blob)), mode="r:", ignore_zeros=True
    ) as tf:
        for m in tf:
            if m.name == "APKINDEX":
                extracted = tf.extractfile(m)
                if extracted is not None:
                    index = extracted.read().decode("utf-8", "replace")
                break
    if index is None:
        sys.exit("error: APKINDEX not found inside APKINDEX.tar.gz")

    by_name, by_soname = {}, {}
    for record in index.split("\n\n"):
        f = {}
        for line in record.splitlines():
            if len(line) > 2 and line[1] == ":":
                f.setdefault(line[0], []).append(line[2:])
        name = f.get("P", [None])[0]
        ver = f.get("V", [None])[0]
        if not name or not ver:
            continue
        fn = f"{name}-{ver}.apk"
        by_name[name] = fn
        for prov in " ".join(f.get("p", [])).split():
            if prov.startswith("so:"):
                by_soname[prov[3:].split("=")[0]] = fn
    return by_name, by_soname


def elf_needs(path: str) -> list[str]:
    try:
        out = subprocess.run(
            ["readelf", "-dl", path], capture_output=True, text=True, timeout=30
        ).stdout
    except Exception:
        return []
    needs = re.findall(r"Shared library: \[(.+?)\]", out)
    interp = re.findall(r"program interpreter: (.+?)\]", out)
    return needs + interp


def is_elf(path: str) -> bool:
    try:
        with open(path, "rb") as fh:
            return fh.read(4) == b"\x7fELF"
    except OSError:
        return False


def satisfied(soname: str, roots: list[str]) -> bool:
    for root in roots:
        if soname.startswith("/"):
            if os.path.exists(os.path.join(root, soname.lstrip("/"))):
                return True
        else:
            for d in ("lib", "usr/lib"):
                if os.path.exists(os.path.join(root, d, soname)):
                    return True
    return False


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo", required=True)
    ap.add_argument("--dest", required=True)
    ap.add_argument("--rootfs", required=True)
    ap.add_argument("--max-rounds", type=int, default=10)
    ap.add_argument("packages", nargs="+")
    args = ap.parse_args()

    if os.path.isdir(args.dest):
        shutil.rmtree(args.dest)
    os.makedirs(args.dest)

    print(f"==> index: {args.repo}/APKINDEX.tar.gz", flush=True)
    by_name, by_soname = parse_index(fetch(f"{args.repo}/APKINDEX.tar.gz"))
    log(f"{len(by_name)} packages, {len(by_soname)} sonames")

    wanted, done = list(args.packages), set()

    for _ in range(args.max_rounds):
        for pkg in wanted:
            if pkg in done:
                continue
            fn = by_name.get(pkg)
            if not fn:
                sys.exit(f"error: no package named {pkg!r} in the index")
            print(f"==> {fn}", flush=True)
            for n in untar_apk(fetch(f"{args.repo}/{fn}"), args.dest):
                log(n)
            done.add(pkg)

        roots = [args.dest, args.rootfs]
        missing = set()
        for dirpath, _, files in os.walk(args.dest):
            for f in files:
                p = os.path.join(dirpath, f)
                if os.path.islink(p) or not is_elf(p):
                    continue
                for need in elf_needs(p):
                    if not satisfied(need, roots):
                        missing.add(need)

        if not missing:
            print("==> all shared-library dependencies satisfied", flush=True)
            return 0

        print(f"==> unsatisfied: {' '.join(sorted(missing))}", flush=True)
        wanted = []
        for so in sorted(missing):
            fn = by_soname.get(so)
            if not fn:
                sys.exit(f"error: nothing in the index provides {so!r}")
            pkg = fn.rsplit("-", 2)[0]
            if pkg in done:
                sys.exit(f"error: {pkg} is already unpacked but {so} is still missing")
            log(f"{so} -> {pkg}")
            wanted.append(pkg)

    sys.exit(f"error: dependencies still unresolved after {args.max_rounds} rounds")


if __name__ == "__main__":
    sys.exit(main())
