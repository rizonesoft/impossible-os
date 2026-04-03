#!/usr/bin/env bash
# push-shim-review.sh -- Push the shim-review submission branch to GitHub.
#
# Prerequisites:
#   1. Fork https://github.com/rhboot/shim-review into rizonesoft/shim-review on GitHub (manual, one-time)
#   2. Run this script from the repo root
#
# Steps this script performs:
#   - Clones rhboot/shim-review if not already at /tmp/shim-review-rizonesoft
#   - Creates branch rizonesoft-shim-x86_64-20260314
#   - Adds shimx64.efi, MOK.cer, Dockerfile, and README
#   - Pushes to rizonesoft/shim-review

set -euo pipefail

BRANCH="rizonesoft-shim-x86_64-20260314"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="/tmp/shim-review-rizonesoft"
REMOTE="https://github.com/rizonesoft/shim-review.git"

echo "[SHIM-REVIEW] Preparing submission branch: $BRANCH"

if [ ! -d "$BUILD_DIR" ]; then
    git clone --depth=1 https://github.com/rhboot/shim-review.git "$BUILD_DIR"
fi

cd "$BUILD_DIR"
git checkout main 2>/dev/null || git checkout master
git pull origin main 2>/dev/null || git pull origin master

# Create or reset submission branch
git checkout -B "$BRANCH"

# Copy required submission files
cp "$REPO_ROOT/shim/shimx64.efi" .
cp "$REPO_ROOT/keys/MOK.cer" .
cp "$REPO_ROOT/shim-review/Dockerfile" .
cp "$REPO_ROOT/shim-review/README-submission.md" README.md

# Commit
git config user.name "rizonesoft"
git config user.email "derick@rizonetech.com"
git add shimx64.efi MOK.cer Dockerfile README.md
git commit -m "rizonesoft: Impossible OS shim submission (x86_64, 2026-03-14)" --allow-empty

# Push
echo "[SHIM-REVIEW] Pushing to $REMOTE ..."
git remote add rizonesoft "$REMOTE" 2>/dev/null || git remote set-url rizonesoft "$REMOTE"
git push rizonesoft "$BRANCH" --force

echo ""
echo "[SHIM-REVIEW] Done!"
echo "  Branch: https://github.com/rizonesoft/shim-review/tree/$BRANCH"
echo "  SHA256 (shimx64.efi): d7e21770b1c8f2b977db1d533f7bba3d0de3d212e83ffd35c2509de970d6bd2f"
echo ""
echo "Next step: Open an issue on https://github.com/rhboot/shim-review using the"
echo "  ISSUE_TEMPLATE.md and fill in the branch URL and SHA256 above."
