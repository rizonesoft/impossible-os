#!/bin/bash
# =============================================================================
# install-hooks.sh -- Install or remove git hooks
#
# Usage:
#   bash scripts/install-hooks.sh           # install hooks
#   bash scripts/install-hooks.sh --remove  # remove hooks
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT="$(dirname "$SCRIPT_DIR")"
HOOKS_SRC="$SCRIPT_DIR/hooks"
HOOKS_DST="$PROJECT/.git/hooks"

if [ ! -d "$HOOKS_DST" ]; then
    echo "ERROR: .git/hooks not found. Are you in a git repository?"
    exit 1
fi

if [ "${1:-}" = "--remove" ]; then
    for hook in "$HOOKS_SRC"/*; do
        name="$(basename "$hook")"
        target="$HOOKS_DST/$name"
        if [ -L "$target" ]; then
            rm "$target"
            echo "Removed: $name"
        fi
    done
    echo "Git hooks removed."
    exit 0
fi

# Install: symlink each hook
for hook in "$HOOKS_SRC"/*; do
    name="$(basename "$hook")"
    target="$HOOKS_DST/$name"

    # Don't overwrite non-symlink hooks (user's own hooks)
    if [ -e "$target" ] && [ ! -L "$target" ]; then
        echo "SKIP: $name (existing non-symlink hook -- won't overwrite)"
        continue
    fi

    ln -sf "$hook" "$target"
    echo "Installed: $name -> scripts/hooks/$name"
done

echo ""
echo "Git hooks installed. To remove: bash scripts/install-hooks.sh --remove"
