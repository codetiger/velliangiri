#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only

set -euo pipefail

MODE="${1:---all}"
REPO_ROOT="$(git rev-parse --show-toplevel)"
cd "$REPO_ROOT"

python3 -S - "$MODE" <<'PYEOF'
import os
import re
import subprocess
import sys

mode = sys.argv[1]
SELF_PATH = "scripts/check-no-proprietary.sh"

# Permitted source-tree file extensions and exact filenames
ALLOWED_EXTENSIONS = {
    ".c", ".h", ".s", ".S", ".rs",
    ".dts", ".dtsi", ".yaml", ".yml",
    ".sh", ".py", ".pl", ".awk",
    ".md", ".txt", ".rst", ".json", ".toml",
    ".cfg", ".conf", ".config", ".patch", ".diff",
    ".ld", ".lds", ".cmake", ".mk",
}

ALLOWED_EXACT_NAMES = {
    "LICENSE", "COPYING", "Makefile", "Kbuild", "Kconfig",
    ".gitignore", ".gitattributes", ".clang-format", ".editorconfig",
    "pre-commit", "pre-push", "CMakeLists.txt",
}

BLOCKED_DIRS = (
    "research/", "build/", "out/",
)

FORBIDDEN_TEXT_PATTERNS = [
    (
        re.compile(r"INTERNAL\s+USE\s+ONLY|Confidential\s+and\s+Proprietary", re.IGNORECASE),
        "Restricted confidentiality marking",
    ),
    (
        re.compile(r"(-iv|--iv)\s+[0-9a-fA-F]{32}\s+(-k|--key)\s+[0-9a-fA-F]{64}"),
        "Hardcoded cryptographic key/IV parameters",
    ),
]

EXTRA_BASENAME_PATTERNS = []
EXTRA_MAGIC_SIGNATURES = []

# Load optional local rules if present in ignored workspace
local_rules_path = os.path.join("research", ".guardrail-rules.py")
if os.path.isfile(local_rules_path):
    ns = {}
    with open(local_rules_path, "r", encoding="utf-8") as lf:
        exec(lf.read(), ns)
    EXTRA_BASENAME_PATTERNS.extend(ns.get("EXTRA_BASENAME_PATTERNS", []))
    EXTRA_MAGIC_SIGNATURES.extend(ns.get("EXTRA_MAGIC_SIGNATURES", []))
    FORBIDDEN_TEXT_PATTERNS.extend(ns.get("EXTRA_TEXT_PATTERNS", []))

HEX_ARRAY_LINE_RE = re.compile(r"^\s*(0x[0-9a-fA-F]{2}\s*,\s*){8,}")


def get_files_to_check(scan_mode: str):
    items = []
    if scan_mode == "--staged":
        out = subprocess.check_output(
            ["git", "diff", "--cached", "--name-only", "--diff-filter=ACMR", "-z"],
            text=False,
        )
        for p in out.split(b"\x00"):
            if p:
                items.append((p.decode("utf-8", errors="surrogateescape"), "staged"))
    else:
        tracked = subprocess.check_output(["git", "ls-files", "-z"], text=False)
        untracked = subprocess.check_output(
            ["git", "ls-files", "--others", "--exclude-standard", "-z"], text=False
        )
        seen = set()
        for raw in tracked.split(b"\x00") + untracked.split(b"\x00"):
            if raw:
                path_str = raw.decode("utf-8", errors="surrogateescape")
                if path_str not in seen and os.path.exists(path_str):
                    seen.add(path_str)
                    items.append((path_str, "worktree"))
    return items


def read_content(path: str, source_type: str) -> bytes:
    if source_type == "staged":
        return subprocess.check_output(["git", "show", f":{path}"], text=False)
    with open(path, "rb") as f:
        return f.read()


violations = []
files = get_files_to_check(mode)

for rel_path, src_type in files:
    norm_path = rel_path.replace("\\", "/")
    basename = os.path.basename(norm_path)
    _, ext = os.path.splitext(basename)

    if norm_path.lower().startswith(BLOCKED_DIRS):
        violations.append(
            f"[BLOCKED_DIR] '{norm_path}': Ignored workspace directory '{norm_path.split('/')[0]}/' must not be tracked."
        )
        continue

    if basename not in ALLOWED_EXACT_NAMES and not basename.startswith("Kconfig.") and not basename.startswith("Makefile."):
        if ext.lower() not in ALLOWED_EXTENSIONS:
            violations.append(
                f"[DISALLOWED_FILE] '{norm_path}': Only permitted source/documentation text files are allowed."
            )
            continue

    for pat in EXTRA_BASENAME_PATTERNS:
        if re.match(pat, basename, re.IGNORECASE):
            violations.append(
                f"[DISALLOWED_NAME] '{norm_path}': Filename matches disallowed pattern."
            )
            break

    try:
        data = read_content(rel_path, src_type)
    except Exception as exc:
        violations.append(f"[READ_ERROR] '{norm_path}': {exc}")
        continue

    header_window = data[:4096]
    for sig, desc in EXTRA_MAGIC_SIGNATURES:
        if data.startswith(sig) or (len(sig) >= 4 and sig in header_window[:64]):
            violations.append(f"[BINARY_HEADER] '{norm_path}': Detected {desc}.")
            break

    if b"\x00" in data[:8192]:
        violations.append(
            f"[BINARY_DATA] '{norm_path}': Binary files are not permitted in the source tree."
        )
        continue

    if norm_path == SELF_PATH:
        continue

    text = data.decode("utf-8", errors="replace")
    for regex, reason in FORBIDDEN_TEXT_PATTERNS:
        match = regex.search(text)
        if match:
            line_num = text[: match.start()].count("\n") + 1
            violations.append(
                f"[CONTENT_CHECK] '{norm_path}:{line_num}': {reason} ('{match.group(0)[:60]}')."
            )

    consec_hex_lines = 0
    for idx, line in enumerate(text.splitlines(), start=1):
        if HEX_ARRAY_LINE_RE.match(line):
            consec_hex_lines += 1
            if consec_hex_lines == 33:
                violations.append(
                    f"[RAW_HEX_ARRAY] '{norm_path}:{idx}': Large embedded hex byte arrays (>32 lines) are not permitted."
                )
                break
        else:
            consec_hex_lines = 0

if violations:
    print("Source tree check failed:", file=sys.stderr)
    for v in violations:
        print(f"  - {v}", file=sys.stderr)
    sys.exit(1)

print(f"[OK] Verified {len(files)} file(s) ({mode}).")
PYEOF
