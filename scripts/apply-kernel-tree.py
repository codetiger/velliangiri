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
RECORD_PREFIX = ".velliangiri-applied-"
MANIFEST = ".velliangiri-overlay-files"


def fail(msg: str) -> None:
    print(f"[apply-kernel-tree] ERROR: {msg}", file=sys.stderr)
    raise SystemExit(1)


def read_bytes(path: str) -> bytes:
    with open(path, "rb") as f:
        return f.read()


def overlay_files() -> list[str]:
    files = []
    for top in OVERLAY_DIRS:
        for dirpath, _, filenames in os.walk(os.path.join(ROOT, top)):
            for fname in filenames:
                if fname.startswith("."):
                    continue
                files.append(os.path.relpath(os.path.join(dirpath, fname), ROOT))
    return sorted(files)


def sync_overlay_files(tree: str, revert: bool) -> int:
    """Copy the overlay into the tree and drop files it no longer carries.

    The installed file list is kept in MANIFEST so a file that is moved or
    deleted in this repository is also removed from the kernel tree.
    """
    manifest = os.path.join(tree, MANIFEST)
    if os.path.exists(manifest):
        with open(manifest) as f:
            previous = set(f.read().split())
    else:
        # Trees overlaid before the manifest existed
        previous = set(overlay_files()) if revert else set()
    current = [] if revert else overlay_files()

    changed = 0
    for rel in sorted(previous.difference(current)):
        dst = os.path.join(tree, rel)
        if os.path.exists(dst):
            os.remove(dst)
            changed += 1
            print(f"  removed {rel}")

    for rel in current:
        src = os.path.join(ROOT, rel)
        dst = os.path.join(tree, rel)
        if os.path.exists(dst) and read_bytes(dst) == read_bytes(src):
            continue
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copyfile(src, dst)
        changed += 1
        print(f"  installed {rel}")

    if current:
        with open(manifest, "w") as f:
            f.write("\n".join(current) + "\n")
    elif os.path.exists(manifest):
        os.remove(manifest)
    return changed


def run_git_apply(tree: str, args: list[str]) -> bool:
    res = subprocess.run(
        ["git", "-C", tree, "apply", *args],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    return res.returncode == 0


def apply_patches(tree: str, revert: bool) -> int:
    """Bring the tree's patch set in line with scripts/patches/.

    Each applied patch is recorded as a copy in the tree. When the recorded
    set differs from the current one (a patch was edited, renamed or
    removed), every recorded patch is reverted, newest first, and the
    current series is applied from scratch.
    """
    patches = sorted(glob.glob(os.path.join(PATCH_DIR, "*.patch")))
    records = sorted(glob.glob(os.path.join(tree, RECORD_PREFIX + "*.patch")))
    current = {os.path.basename(p): read_bytes(p) for p in patches}
    recorded = {os.path.basename(r)[len(RECORD_PREFIX):]: read_bytes(r) for r in records}

    if not revert and recorded == current:
        print(f"  {len(patches)} patch(es) already applied")
        return 0

    changed = 0
    for r in reversed(records):
        name = os.path.basename(r)[len(RECORD_PREFIX):]
        if not run_git_apply(tree, ["--reverse", "--check", r]):
            fail(f"applied patch {name} no longer reverts cleanly in {tree}")
        subprocess.check_call(["git", "-C", tree, "apply", "--reverse", r])
        os.remove(r)
        changed += 1
        print(f"  reverted {name}")
    if revert:
        return changed

    for p in patches:
        name = os.path.basename(p)
        if run_git_apply(tree, ["--check", p]):
            subprocess.check_call(["git", "-C", tree, "apply", p])
            changed += 1
            print(f"  applied {name}")
        elif run_git_apply(tree, ["--reverse", "--check", p]):
            print(f"  already applied: {name}")
        else:
            subprocess.run(["git", "-C", tree, "apply", "--check", p])
            fail(f"patch {name} did not apply cleanly to {tree}")
        shutil.copyfile(p, os.path.join(tree, RECORD_PREFIX + name))
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
