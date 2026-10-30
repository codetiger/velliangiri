# Workspace Development Rules

## 1. Repository Scope

This repository hosts only:
1. **Mainline Linux 6.12 Kernel Code & Patches** (`arch/arm/`, `drivers/`).
2. **Clean-Room Linux Hardware Drivers** (`S5L8947X` SoC support), licensed under `GPL-2.0-only`.
3. **Open-Source Build & Microcontroller Tooling** (`scripts/`).

## 2. Source Tree Hygiene

- Only freshly authored, open-source (`GPL-2.0-only`) text source files may be staged or committed.
- Never commit binary blobs, precompiled objects, archives, raw memory dumps, cryptographic keys, or third-party proprietary code/documentation.
- Never mention proprietary vendor terminology or internal bootloader/key names in public documentation, comments, or commit messages.
- Keep all local research notes and non-distributable testing materials strictly inside the git-ignored `research/` directory.
- Run `./scripts/check-no-proprietary.sh --all` before concluding any task that creates or modifies files, and never bypass git hooks (`--no-verify`).

## 3. Commits

- Never add `Co-authored-by:` (or any other co-author / AI attribution) trailers to commit messages. Commits carry only the author's own identity.
