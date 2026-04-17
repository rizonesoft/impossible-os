#!/usr/bin/env bash
# ============================================================================
# setup.sh -- One-command development environment setup
#
# Clone the repo, run this script, and you're ready to build.
#
# Usage:
#   bash scripts/setup.sh            Install deps + verification build
#   bash scripts/setup.sh --verify   Read-only sentinel check (no install)
#   bash scripts/setup.sh --help     Print usage and exit
#
# Canonical host bootstrap contract: docs/infrastructure/development-tooling.md
# TODO owner: todo/00-infrastructure/TODO-01-developer-tooling-stack.md (section 1)
# ============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# ---- Colors ----
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

# ---- Required tools (host bootstrap contract, TODO-01 section 1) ----
# Sentinel format: "check:label"; "check" is a command name, the marker __OVMF_CODE__,
# or the marker __OVMF_VARS__. Kept in one array so install, verify, and the doc
# stay aligned. Every entry below maps to an exact Makefile command or path.
REQUIRED_SENTINELS=(
    "clang-19:clang-19 (Makefile CC)"
    "ld.lld-19:ld.lld-19 (Makefile LD)"
    "llvm-objcopy-19:llvm-objcopy-19 (Makefile OBJCOPY)"
    "llvm-ar-19:llvm-ar-19 (Makefile AR, userland archives)"
    "llvm-nm-19:llvm-nm-19 (kernel.map generation, Makefile line 187)"
    "nasm:nasm (Makefile AS)"
    "gcc:gcc (Makefile HOST_CC, builds irespack/jpg2raw/mkfs-ixfs host tools)"
    "python3:python3 (asset pipeline + convert_symmap.py)"
    "qemu-system-x86_64:qemu-system-x86_64 (Makefile QEMU)"
    "mcopy:mtools mcopy (FAT image population)"
    "mmd:mtools mmd (BlackBox FAT directory creation)"
    "mkfs.fat:dosfstools (FAT32 partition formatting)"
    "__OVMF_CODE__:OVMF_CODE_4M.fd (Makefile OVMF_CODE)"
    "__OVMF_VARS__:OVMF_VARS_4M.fd (Makefile OVMF_VARS)"
)

# OVMF paths are hardcoded in the Makefile. Fallbacks here are strictly for
# distros that ship OVMF under a different prefix; the repo build still requires
# the canonical Debian-style 4M paths to be present. Fedora/Arch currently need
# manual symlinks until TODO-01 section 2 lands the supported-host profile matrix.
OVMF_CODE_CANDIDATES=(
    "/usr/share/OVMF/OVMF_CODE_4M.fd"
)
OVMF_VARS_CANDIDATES=(
    "/usr/share/OVMF/OVMF_VARS_4M.fd"
)

print_help() {
    cat <<'EOF'
Impossible OS -- development environment setup

Usage:
  bash scripts/setup.sh            Install dependencies, then run a verification build.
  bash scripts/setup.sh --verify   Check required tools (read-only; no install, no build).
  bash scripts/setup.sh --help     Show this help.

Required host tools (TODO-01 section 1):
  clang-19, ld.lld-19, llvm-objcopy-19, llvm-ar-19, llvm-nm-19,
  nasm, gcc, python3, qemu-system-x86_64,
  mtools (mcopy + mmd), dosfstools (mkfs.fat),
  /usr/share/OVMF/OVMF_CODE_4M.fd, /usr/share/OVMF/OVMF_VARS_4M.fd

Supported distros:
  Ubuntu/Debian is the out-of-the-box path today. Fedora and Arch are detected
  by setup-deps.sh and install the right package set, but the repo's Makefile
  hardcodes version-suffixed LLVM tool names (clang-19, ld.lld-19, etc.) and
  the Debian-style OVMF_4M.fd paths; Fedora/Arch hosts currently need manual
  symlinks. The full supported-host profile matrix (Ubuntu/Debian/Fedora/Arch/
  WSL2/CI) is owned by TODO-01 section 2. See
  docs/infrastructure/development-tooling.md#host-bootstrap-contract.

Boundary:
  Repo-local developer bootstrap only. Cross-compiler toolchain and SDK build-system
  work is owned by todo/14-host-tools/TODO-01-sdk-build-system.md (D14 T01 sections 1-3).

EOF
}

verify_tools() {
    local missing=0
    echo -e "${CYAN}==================================================${NC}"
    echo -e "${CYAN}  Host bootstrap verification (required tools)${NC}"
    echo -e "${CYAN}==================================================${NC}"
    echo ""
    for entry in "${REQUIRED_SENTINELS[@]}"; do
        local check="${entry%%:*}"
        local label="${entry#*:}"
        case "$check" in
            __OVMF_CODE__)
                local found=""
                for cand in "${OVMF_CODE_CANDIDATES[@]}"; do
                    if [ -f "$cand" ]; then
                        found="$cand"
                        break
                    fi
                done
                if [ -n "$found" ]; then
                    echo -e "  ${GREEN}OK${NC}   $label  ($found)"
                else
                    echo -e "  ${RED}MISS${NC} $label  (expected at ${OVMF_CODE_CANDIDATES[0]})"
                    missing=$((missing + 1))
                fi
                ;;
            __OVMF_VARS__)
                local found=""
                for cand in "${OVMF_VARS_CANDIDATES[@]}"; do
                    if [ -f "$cand" ]; then
                        found="$cand"
                        break
                    fi
                done
                if [ -n "$found" ]; then
                    echo -e "  ${GREEN}OK${NC}   $label  ($found)"
                else
                    echo -e "  ${RED}MISS${NC} $label  (expected at ${OVMF_VARS_CANDIDATES[0]})"
                    missing=$((missing + 1))
                fi
                ;;
            *)
                if command -v "$check" >/dev/null 2>&1; then
                    echo -e "  ${GREEN}OK${NC}   $label  ($(command -v "$check"))"
                else
                    echo -e "  ${RED}MISS${NC} $label  (not found on PATH)"
                    missing=$((missing + 1))
                fi
                ;;
        esac
    done
    echo ""
    if [ "$missing" -eq 0 ]; then
        echo -e "  ${GREEN}All required host tools present.${NC}"
        return 0
    else
        echo -e "  ${RED}$missing required tool(s) missing.${NC}"
        echo -e "  Fix: ${CYAN}bash scripts/setup.sh${NC}  (installs packages for the detected distro)"
        return 1
    fi
}

# ---- Arg parsing ----
MODE="install"
while [ $# -gt 0 ]; do
    case "$1" in
        --help|-h) MODE="help" ;;
        --verify) MODE="verify" ;;
        *)
            echo "Unknown argument: $1" >&2
            echo ""
            print_help
            exit 2
            ;;
    esac
    shift
done

case "$MODE" in
    help)
        print_help
        exit 0
        ;;
    verify)
        verify_tools
        exit $?
        ;;
esac

echo -e "${CYAN}==================================================${NC}"
echo -e "${CYAN}  Impossible OS -- Development Environment Setup${NC}"
echo -e "${CYAN}==================================================${NC}"
echo ""

# Step 1: Install system dependencies
echo -e "${CYAN}[1/3]${NC} Installing system dependencies..."
echo ""
bash "$SCRIPT_DIR/setup-deps.sh"

# Step 2: Sentinel check -- fail early if any required tool is missing.
echo ""
echo -e "${CYAN}[2/3]${NC} Verifying required tools..."
echo ""
if ! verify_tools; then
    echo ""
    echo -e "  ${RED}Host bootstrap incomplete.${NC}"
    echo -e "  See: ${CYAN}docs/infrastructure/development-tooling.md${NC} (Host Bootstrap Contract)"
    exit 1
fi

# Step 3: Verification build
echo ""
echo -e "${CYAN}[3/3]${NC} Verification build..."
echo ""
cd "$REPO_ROOT"
bash scripts/build.sh clean

BUILD_RESULT=$(tail -1 build/build.log 2>/dev/null || echo "UNKNOWN")

echo ""
echo -e "${CYAN}==================================================${NC}"
if [ "$BUILD_RESULT" = "=== BUILD OK ===" ]; then
    echo -e "  ${GREEN}Setup complete! Build verified.${NC}"
    echo ""
    echo "  Next steps:"
    echo "    bash scripts/build.sh run    # Boot in QEMU"
    echo "    bash scripts/build.sh        # Incremental build"
    echo "    bash scripts/build.sh clean  # Clean build"
    echo "    bash scripts/setup.sh --verify   # Re-check host tools any time"
else
    echo -e "  ${RED}Build failed. Check build/build.log for details.${NC}"
    exit 1
fi
echo -e "${CYAN}==================================================${NC}"
echo ""
