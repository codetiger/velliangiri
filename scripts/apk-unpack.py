#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""
Unpack Alpine .apk packages into a target directory and resolve shared-library
dependencies via APKINDEX.tar.gz and readelf without executing target binaries.
"""

import argparse
import glob
import gzip
import io
import os
import re
import shutil
import subprocess
import sys
import tarfile
import urllib.request

_ELF_NEEDS_CACHE: dict[tuple[int, int], list[str]] = {}


def log(msg: str) -> None:
    print(f"    {msg}", flush=True)


def fetch(url: str, cache_dir: str | None = None, cache_name: str | None = None) -> bytes:
    if cache_dir and cache_name:
        os.makedirs(cache_dir, exist_ok=True)
        cpath = os.path.join(cache_dir, cache_name)
        if os.path.isfile(cpath) and os.path.getsize(cpath) > 0:
            with open(cpath, "rb") as f:
                return f.read()
        with urllib.request.urlopen(url, timeout=60) as r:
            data = r.read()
        tmp = cpath + ".part"
        with open(tmp, "wb") as f:
            f.write(data)
        os.replace(tmp, cpath)
        return data
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
        st = os.stat(path)
        key = (st.st_ino, st.st_size)
        if key in _ELF_NEEDS_CACHE:
            return _ELF_NEEDS_CACHE[key]
        out = subprocess.run(
            ["readelf", "-dl", path], capture_output=True, text=True, timeout=30
        ).stdout
    except Exception:
        return []
    needs = re.findall(r"Shared library: \[(.+?)\]", out)
    interp = re.findall(r"program interpreter: (.+?)\]", out)
    res = needs + interp
    _ELF_NEEDS_CACHE[key] = res
    return res


def apply_prune_list(dest: str, prune_file: str | None) -> None:
    if not prune_file or not os.path.isfile(prune_file):
        return
    with open(prune_file, "r", encoding="utf-8") as f:
        for raw in f:
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            pat = os.path.join(dest, line.lstrip("/"))
            for match in glob.glob(pat):
                if os.path.isdir(match) and not os.path.islink(match):
                    shutil.rmtree(match, ignore_errors=True)
                else:
                    try:
                        os.unlink(match)
                    except OSError:
                        pass


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
    ap.add_argument("--cache", default=None)
    ap.add_argument("--prune", default=None)
    ap.add_argument("--max-rounds", type=int, default=10)
    ap.add_argument("packages", nargs="+")
    args = ap.parse_args()

    if os.path.isdir(args.dest):
        shutil.rmtree(args.dest)
    os.makedirs(args.dest)

    print(f"==> index: {args.repo}/APKINDEX.tar.gz", flush=True)
    idx_blob = fetch(
        f"{args.repo}/APKINDEX.tar.gz", args.cache, "APKINDEX.tar.gz"
    )
    by_name, by_soname = parse_index(idx_blob)
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
            apk_blob = fetch(f"{args.repo}/{fn}", args.cache, fn)
            for n in untar_apk(apk_blob, args.dest):
                log(n)
            done.add(pkg)

        apply_prune_list(args.dest, args.prune)

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
