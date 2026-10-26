#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
set -euo pipefail

REPO_ROOT="$(git rev-parse --show-toplevel)"
cd "$REPO_ROOT"

chmod +x scripts/check-no-proprietary.sh scripts/setup-guardrails.sh .githooks/pre-commit .githooks/pre-push
git config core.hooksPath .githooks

./scripts/check-no-proprietary.sh --all
