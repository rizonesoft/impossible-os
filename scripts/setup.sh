#!/usr/bin/env bash
# ============================================================================
# setup.sh -- One-command development environment setup
#
# Clone the repo, run this script, and you're ready to build.
#
# Usage:
#   bash scripts/setup.sh             Install deps + verification build
#   bash scripts/setup.sh --verify    Read-only sentinel check (no install)
#   bash scripts/setup.sh --versions  Print actual tool versions with minimum floors (advisory)
#   bash scripts/setup.sh --help      Print usage and exit
#
# Canonical contract: docs/infrastructure/development-tooling.md
#   - "Host Bootstrap Contract" section defines the required-tool sentinel set,
#     supported distros, idempotence, and scope boundary.
#   - "Supported Host Profiles and Reproducible Environments" section defines
#     the profile matrix, minimum version floors, and the .devcontainer profile.
# Referenced from anchor URLs (stable against TODO renumbering).
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

# ---- Required tools (see "Host Bootstrap Contract" in the canonical doc) ----
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
# manual symlinks (see docs/infrastructure/development-tooling.md, Fedora/Arch
# Shim Procedure section) until full-support promotion lands.
OVMF_CODE_CANDIDATES=(
    "/usr/share/OVMF/OVMF_CODE_4M.fd"
)
OVMF_VARS_CANDIDATES=(
    "/usr/share/OVMF/OVMF_VARS_4M.fd"
)

# ---- Minimum tool versions (see "Supported Host Profiles" in canonical doc) ----
# Entry format: "check_cmd:display_name:minimum:version_probe"
# version_probe is a small awk/sed pipeline that extracts a dotted version
# from the tool's output. Keeping these inline so docs and script stay aligned.
VERSION_SPECS=(
    "clang-19:clang-19:19.1.0:clang-19 --version 2>&1 | head -1 | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1"
    "ld.lld-19:ld.lld-19:19.1.0:ld.lld-19 --version 2>&1 | head -1 | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1"
    "llvm-objcopy-19:llvm-objcopy-19:19.1.0:llvm-objcopy-19 --version 2>&1 | head -3 | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1"
    "llvm-ar-19:llvm-ar-19:19.1.0:llvm-ar-19 --version 2>&1 | head -3 | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1"
    "llvm-nm-19:llvm-nm-19:19.1.0:llvm-nm-19 --version 2>&1 | head -3 | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1"
    "nasm:nasm:2.15.05:nasm -v 2>&1 | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1"
    "gcc:gcc:11.0:gcc --version 2>&1 | head -1 | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1"
    "python3:python3:3.8:python3 --version 2>&1 | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1"
    "qemu-system-x86_64:qemu-system-x86_64:7.0:qemu-system-x86_64 --version 2>&1 | head -1 | grep -oE '[0-9]+\.[0-9]+' | head -1"
    "mcopy:mtools mcopy:4.0:mcopy -V 2>&1 | head -1 | grep -oE '[0-9]+\.[0-9]+' | head -1"
    "mmd:mtools mmd:4.0:mmd -V 2>&1 | head -1 | grep -oE '[0-9]+\.[0-9]+' | head -1"
    "mkfs.fat:dosfstools mkfs.fat:4.0:mkfs.fat --help 2>&1 | tail -1 | grep -oE '[0-9]+\.[0-9]+' | head -1"
    "bear:bear (optional):3.0:bear --version 2>&1 | head -1 | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1"
)

# Firmware files have no version strings. print_versions also walks these
# for presence + size so `--versions` coverage matches --verify's sentinel set.
FIRMWARE_CHECKS=(
    "OVMF_CODE_4M.fd:/usr/share/OVMF/OVMF_CODE_4M.fd"
    "OVMF_VARS_4M.fd:/usr/share/OVMF/OVMF_VARS_4M.fd"
)

# Compare dotted versions: returns 0 if $1 >= $2, else 1.
version_ge() {
    local have="$1"
    local need="$2"
    [ -z "$have" ] && return 1
    [ -z "$need" ] && return 0
    local highest
    highest=$(printf '%s\n%s\n' "$have" "$need" | sort -V | tail -n 1)
    [ "$highest" = "$have" ]
}

print_help() {
    cat <<'EOF'
Impossible OS -- development environment setup

Usage:
  bash scripts/setup.sh             Install dependencies, then run a verification build.
  bash scripts/setup.sh --verify    Check required tools (read-only; no install, no build).
  bash scripts/setup.sh --versions  Report installed tool versions vs documented minimum floors.
  bash scripts/setup.sh --help      Show this help.

Required host tools (see "Host Bootstrap Contract" in canonical doc):
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
  WSL2/CI) and the .devcontainer reproducible profile are documented in
  docs/infrastructure/development-tooling.md (sections "Host Bootstrap Contract"
  and "Supported Host Profiles and Reproducible Environments").

Boundary:
  Repo-local developer bootstrap only. Cross-compiler toolchain and SDK build-system
  work belongs to the SDK build-system TODO. See the "Scope Boundary" table in
  the canonical doc for the current owner and deep link.

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

print_versions() {
    # Advisory report: ALWAYS exits 0 so docs-claimed-advisory contract is honored.
    # Hard pass/fail remains `--verify`. Exit 1 would conflate optional tools
    # (bear) and patch-level floors with actual host breakage.
    echo -e "${CYAN}==================================================${NC}"
    echo -e "${CYAN}  Host tool versions vs documented minimums (advisory)${NC}"
    echo -e "${CYAN}==================================================${NC}"
    echo ""
    printf "  %-24s %-12s %-12s %s\n" "Tool" "Installed" "Minimum" "Status"
    printf "  %-24s %-12s %-12s %s\n" "----" "---------" "-------" "------"
    for spec in "${VERSION_SPECS[@]}"; do
        local check="${spec%%:*}"
        local rest="${spec#*:}"
        local name="${rest%%:*}"
        rest="${rest#*:}"
        local minimum="${rest%%:*}"
        local probe="${rest#*:}"
        local have
        if ! command -v "$check" >/dev/null 2>&1; then
            printf "  %-24s ${RED}%-12s${NC} %-12s ${RED}MISSING${NC}\n" "$name" "--" "$minimum"
            continue
        fi
        have=$(eval "$probe" 2>/dev/null || echo "")
        if [ -z "$have" ]; then
            printf "  %-24s ${YELLOW}%-12s${NC} %-12s ${YELLOW}UNKNOWN${NC}\n" "$name" "?" "$minimum"
            continue
        fi
        if version_ge "$have" "$minimum"; then
            printf "  %-24s ${GREEN}%-12s${NC} %-12s ${GREEN}OK${NC}\n" "$name" "$have" "$minimum"
        else
            printf "  %-24s ${YELLOW}%-12s${NC} %-12s ${YELLOW}BELOW${NC}\n" "$name" "$have" "$minimum"
        fi
    done
    echo ""
    echo -e "  ${CYAN}Firmware files (no version string; presence check):${NC}"
    for entry in "${FIRMWARE_CHECKS[@]}"; do
        local name="${entry%%:*}"
        local path="${entry#*:}"
        if [ -f "$path" ]; then
            local size
            size=$(stat -c %s "$path" 2>/dev/null || echo "?")
            printf "  %-24s ${GREEN}%-12s${NC} present      ${GREEN}OK${NC}  (%s)\n" "$name" "${size} B" "$path"
        else
            printf "  %-24s ${RED}%-12s${NC} present      ${RED}MISSING${NC} (%s)\n" "$name" "--" "$path"
        fi
    done
    echo ""
    echo -e "  ${CYAN}Advisory:${NC} minimums are documented floors, not hard gates."
    echo -e "  Hard contract: ${CYAN}bash scripts/setup.sh --verify${NC}."
    return 0
}

# ---- Arg parsing ----
MODE="install"
while [ $# -gt 0 ]; do
    case "$1" in
        --help|-h) MODE="help" ;;
        --verify) MODE="verify" ;;
        --versions) MODE="versions" ;;
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
    versions)
        print_versions
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
