#!/usr/bin/env bash
# ============================================================================
# setup.sh -- One-command development environment setup
#
# Clone the repo, run this script, and you're ready to build.
#
# Usage:  bash scripts/setup.sh
# ============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# ---- Colors ----
GREEN='\033[0;32m'
CYAN='\033[0;36m'
NC='\033[0m'

echo -e "${CYAN}══════════════════════════════════════════════════${NC}"
echo -e "${CYAN}  Impossible OS -- Development Environment Setup${NC}"
echo -e "${CYAN}══════════════════════════════════════════════════${NC}"
echo ""

# Step 1: Install system dependencies
echo -e "${CYAN}[1/2]${NC} Installing system dependencies..."
echo ""
bash "$SCRIPT_DIR/setup-deps.sh"

# Step 2: Verify build
echo ""
echo -e "${CYAN}[2/2]${NC} Verifying build..."
echo ""
cd "$REPO_ROOT"
bash scripts/build.sh clean

BUILD_RESULT=$(tail -1 build/build.log 2>/dev/null || echo "UNKNOWN")

echo ""
echo -e "${CYAN}══════════════════════════════════════════════════${NC}"
if [ "$BUILD_RESULT" = "=== BUILD OK ===" ]; then
    echo -e "  ${GREEN}✓ Setup complete! Build verified.${NC}"
    echo ""
    echo "  Next steps:"
    echo "    bash scripts/build.sh run    # Boot in QEMU"
    echo "    bash scripts/build.sh        # Incremental build"
    echo "    bash scripts/build.sh clean  # Clean build"
else
    echo -e "  ✗ Build failed. Check build/build.log for details."
fi
echo -e "${CYAN}══════════════════════════════════════════════════${NC}"
echo ""
