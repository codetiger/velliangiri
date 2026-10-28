#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""
Overlay clean-room S5L8947X files (arch/arm/, drivers/, include/, sound/) onto a
mainline Linux 6.12 tree and apply scripts/patches/*.patch idempotently.
"""

from __future__ import annotations

import argparse
import glob
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
OVERLAY_DIRS = ("arch", "drivers", "include", "sound")
PATCH_DIR = os.path.join(HERE, "patches")


def fail(msg: str) -> None:
    print(f"[apply-kernel-tree] ERROR: {msg}", file=sys.stderr)
    raise SystemExit(1)


def read_bytes(path: str) -> bytes:
    with open(path, "rb") as f:
        return f.read()


def sync_overlay_files(tree: str, revert: bool) -> int:
    changed = 0
    for top in OVERLAY_DIRS:
        src_top = os.path.join(ROOT, top)
        if not os.path.isdir(src_top):
            continue
        for dirpath, _, filenames in os.walk(src_top):
            for fname in sorted(filenames):
                src = os.path.join(dirpath, fname)
                rel = os.path.relpath(src, ROOT)
                dst = os.path.join(tree, rel)
                if revert:
                    if os.path.exists(dst):
                        os.remove(dst)
                        changed += 1
                        print(f"  removed {rel}")
                    continue
                if os.path.exists(dst) and read_bytes(dst) == read_bytes(src):
                    continue
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                shutil.copyfile(src, dst)
                changed += 1
                print(f"  installed {rel}")
    return changed


def run_git_apply(tree: str, args: list[str]) -> bool:
    res = subprocess.run(
        ["git", "-C", tree, "apply", *args],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    return res.returncode == 0


def apply_patches(tree: str, revert: bool) -> int:
    changed = 0
    patches = sorted(glob.glob(os.path.join(PATCH_DIR, "*.patch")))
    for p in patches:
        name = os.path.basename(p)
        record = os.path.join(tree, f".velliangiri-applied-{name}")

        if revert:
            if run_git_apply(tree, ["--reverse", "--check", p]):
                subprocess.check_call(["git", "-C", tree, "apply", "--reverse", p])
                if os.path.exists(record):
                    os.remove(record)
                changed += 1
                print(f"  reverted {name}")
            continue

        if run_git_apply(tree, ["--reverse", "--check", p]):
            shutil.copyfile(p, record)
            print(f"  already applied: {name}")
            continue

        if os.path.exists(record) and run_git_apply(tree, ["--reverse", "--check", record]):
            subprocess.check_call(["git", "-C", tree, "apply", "--reverse", record])
            print(f"  reverted previous revision of {name}")

        if not run_git_apply(tree, ["--check", p]):
            subprocess.run(["git", "-C", tree, "apply", "--check", p])
            fail(f"patch {name} did not apply cleanly to {tree}")

        subprocess.check_call(["git", "-C", tree, "apply", p])
        shutil.copyfile(p, record)
        changed += 1
        print(f"  applied {name}")
    return changed


def main() -> int:
    ap = argparse.ArgumentParser(description="Overlay S5L8947X files and patches onto Linux 6.12")
    ap.add_argument("--tree", default=os.path.join(ROOT, "build", "linux"))
    ap.add_argument("--revert", action="store_true")
    args = ap.parse_args()

    tree = os.path.abspath(args.tree)
    if not os.path.isfile(os.path.join(tree, "Makefile")):
        fail(f"{tree} does not look like a Linux kernel source tree")

    changed = 0
    if args.revert:
        changed += apply_patches(tree, revert=True)
        changed += sync_overlay_files(tree, revert=True)
    else:
        changed += sync_overlay_files(tree, revert=False)
        changed += apply_patches(tree, revert=False)

    print(f"[apply-kernel-tree] Done ({changed} change(s)).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
