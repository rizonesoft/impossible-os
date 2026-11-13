#!/usr/bin/env bash
# Install the CI toolchain into the pinned CI image. Runs as root, once, at image
# build time (scripts/ci/image/Dockerfile); no CI job runs it. The package list is
# scripts/ci/toolchain-packages.txt.
#
# Until 2026-11-13 every job installed this with apt on every run, and nine Build
# runs in a row on 2026-11-11 failed in that step because the runner could not
# resolve the Ubuntu mirror. A mirror outage now fails an image build, which is
# retried, and never a job. The install still goes through scripts/ci/apt-install.sh,
# so a stalled mirror is abandoned and retried instead of holding the build.
set -euo pipefail

LLVM_VERSION=19
RUFF_VERSION=0.17.4
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LIST="$HERE/toolchain-packages.txt"

[ "$(id -u)" = 0 ] || { echo "toolchain.sh: run as root (image build only)" >&2; exit 1; }
export DEBIAN_FRONTEND=noninteractive
APT=(-o Acquire::Retries=3 -o Acquire::http::Timeout=30)

# Ubuntu 26.04 defaults to uutils coreutils, whose `timeout -s KILL` leaves a
# stalled child running, so the identity gate's time bounds stop holding and
# apt-install.sh's own bounds with them. The swap removes an Essential package,
# so it goes first, on its own, with plain apt-get.
apt-get update "${APT[@]}"
apt-get install -y --allow-remove-essential "${APT[@]}" coreutils-from-gnu coreutils-from-uutils-
timeout --version | grep -q 'GNU coreutils'

mapfile -t PACKAGES < <(sed -e 's/#.*//' -e "s/@LLVM@/$LLVM_VERSION/g" "$LIST" | awk 'NF {print $1}')
[ "${#PACKAGES[@]}" -gt 0 ] || { echo "toolchain.sh: no packages in $LIST" >&2; exit 1; }
# The whole toolchain is about 1 GB of packages and 4 GB unpacked, so each phase
# gets more than a job's few packages need; the worst case still fits the
# publishing job (test_ci_toolchain_install.py recomputes it).
APT_DOWNLOAD_SECONDS=600 APT_INSTALL_SECONDS=900 bash "$HERE/apt-install.sh" "${PACKAGES[@]}"

# ruff lints the Python tooling (lint Check 35); pinned, installed for every user.
PIPX_HOME=/opt/pipx PIPX_BIN_DIR=/usr/local/bin pipx install "ruff==$RUFF_VERSION"
ruff --version | grep -q "$RUFF_VERSION"

apt-get clean
rm -rf /var/lib/apt/lists/*
