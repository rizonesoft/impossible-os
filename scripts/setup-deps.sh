#!/usr/bin/env bash
# ============================================================================
# setup-deps.sh -- Install all system dependencies for building Impossible OS
#
# Detects the Linux distribution and installs the required packages.
# Idempotent: running twice changes nothing.
#
# Usage:  bash scripts/setup-deps.sh
# ============================================================================

set -euo pipefail

# ---- Colors ----
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

ok()   { echo -e "  ${GREEN}✓${NC} $1"; }
skip() { echo -e "  ${CYAN}·${NC} $1 ${CYAN}(already installed)${NC}"; }
warn() { echo -e "  ${YELLOW}!${NC} $1"; }
fail() { echo -e "  ${RED}✗${NC} $1"; }

# ---- Distro detection ----
detect_distro() {
    if [ -f /etc/os-release ]; then
        . /etc/os-release
        case "$ID" in
            ubuntu|debian|linuxmint|pop) echo "debian" ;;
            fedora|rhel|centos|rocky|alma) echo "fedora" ;;
            arch|manjaro|endeavouros) echo "arch" ;;
            *) echo "unknown" ;;
        esac
    else
        echo "unknown"
    fi
}

DISTRO=$(detect_distro)
echo -e "${CYAN}══════════════════════════════════════════════════${NC}"
echo -e "${CYAN}  Impossible OS -- Dependency Installer${NC}"
echo -e "${CYAN}══════════════════════════════════════════════════${NC}"
echo ""

if [ "$DISTRO" = "unknown" ]; then
    fail "Unsupported Linux distribution."
    echo "  Supported: Ubuntu/Debian, Fedora/RHEL, Arch/Manjaro"
    echo "  Install these packages manually:"
    echo "    nasm clang-19 lld-19 llvm-19 clangd-19 bear"
    echo "    xorriso mtools qemu-system-x86 ovmf"
    echo "    python3 python3-pil dosfstools parted cppcheck"
    exit 1
fi

echo -e "  Detected: ${GREEN}${DISTRO}${NC}"
echo ""

# ---- Package lists per distro ----
# Format: "command_to_check:package_name"
# If command_to_check is empty, use dpkg/rpm/pacman query instead

case "$DISTRO" in
    debian)
        PACKAGES=(
            "nasm:nasm"
            "clang-19:clang-19"
            "ld.lld-19:lld-19"
            "llvm-objcopy-19:llvm-19"
            "clangd-19:clangd-19"
            "bear:bear"
            "xorriso:xorriso"
            "mtools:mtools"
            "qemu-system-x86_64:qemu-system-x86"
            # OVMF is a data package -- check for file instead
            "/usr/share/OVMF/OVMF_CODE_4M.fd:ovmf"
            "python3:python3"
            "pip:python3-pip"
            "mkfs.fat:dosfstools"
            "parted:parted"
            "cppcheck:cppcheck"
        )
        INSTALL_CMD="sudo apt-get install -y"
        UPDATE_CMD="sudo apt-get update"
        PIL_INSTALL="pip3 install --break-system-packages Pillow 2>/dev/null || pip3 install Pillow"
        ;;
    fedora)
        PACKAGES=(
            "nasm:nasm"
            "clang-19:clang"
            "ld.lld-19:lld"
            "llvm-objcopy-19:llvm"
            "clangd-19:clang-tools-extra"
            "bear:bear"
            "xorriso:xorriso"
            "mtools:mtools"
            "qemu-system-x86_64:qemu-system-x86"
            "/usr/share/OVMF/OVMF_CODE.fd:edk2-ovmf"
            "python3:python3"
            "pip3:python3-pip"
            "mkfs.fat:dosfstools"
            "parted:parted"
            "cppcheck:cppcheck"
        )
        INSTALL_CMD="sudo dnf install -y"
        UPDATE_CMD="true"
        PIL_INSTALL="pip3 install Pillow"
        ;;
    arch)
        PACKAGES=(
            "nasm:nasm"
            "clang:clang"
            "ld.lld:lld"
            "llvm-objcopy:llvm"
            "clangd:clang"
            "bear:bear"
            "xorriso:libisoburn"
            "mtools:mtools"
            "qemu-system-x86_64:qemu-system-x86"
            "/usr/share/OVMF/OVMF_CODE.fd:edk2-ovmf"
            "python3:python3"
            "pip3:python-pip"
            "mkfs.fat:dosfstools"
            "parted:parted"
            "cppcheck:cppcheck"
        )
        INSTALL_CMD="sudo pacman -S --noconfirm --needed"
        UPDATE_CMD="true"
        PIL_INSTALL="pip3 install Pillow"
        ;;
esac

# ---- Install missing packages ----
MISSING=()
INSTALLED=0
SKIPPED=0

echo "  Checking packages..."
echo ""

for entry in "${PACKAGES[@]}"; do
    check="${entry%%:*}"
    pkg="${entry##*:}"

    # Check if already available
    found=0
    if [[ "$check" == /* ]]; then
        # File path check (e.g., OVMF)
        [ -f "$check" ] && found=1
    else
        # Command check
        command -v "$check" >/dev/null 2>&1 && found=1
    fi

    if [ "$found" -eq 1 ]; then
        skip "$pkg"
        SKIPPED=$((SKIPPED + 1))
    else
        MISSING+=("$pkg")
    fi
done

echo ""

if [ ${#MISSING[@]} -gt 0 ]; then
    echo -e "  ${YELLOW}Installing ${#MISSING[@]} package(s):${NC} ${MISSING[*]}"
    echo ""

    # Update package index if needed
    if [ "$UPDATE_CMD" != "true" ]; then
        echo -e "  ${CYAN}Updating package index...${NC}"
        $UPDATE_CMD
    fi

    $INSTALL_CMD "${MISSING[@]}"
    INSTALLED=${#MISSING[@]}

    echo ""
    for pkg in "${MISSING[@]}"; do
        ok "$pkg installed"
    done
else
    echo -e "  ${GREEN}All system packages are already installed.${NC}"
fi

# ---- Python Pillow (pip, not system package) ----
echo ""
echo "  Checking Python packages..."
echo ""

if python3 -c "import PIL" 2>/dev/null; then
    skip "Pillow (python3-pil)"
else
    echo -e "  ${YELLOW}Installing Pillow...${NC}"
    eval "$PIL_INSTALL"
    ok "Pillow installed"
    INSTALLED=$((INSTALLED + 1))
fi

# ---- Summary ----
echo ""
echo -e "${CYAN}══════════════════════════════════════════════════${NC}"
if [ "$INSTALLED" -gt 0 ]; then
    echo -e "  ${GREEN}Done!${NC} Installed $INSTALLED package(s), $SKIPPED already present."
else
    echo -e "  ${GREEN}All dependencies installed.${NC} Nothing to do."
fi
echo -e "${CYAN}══════════════════════════════════════════════════${NC}"
echo ""

# ---- Version checks ----
echo "  Version summary:"
nasm -v 2>/dev/null | head -1 | sed 's/^/    /'
clang-19 --version 2>/dev/null | head -1 | sed 's/^/    /' || clang --version 2>/dev/null | head -1 | sed 's/^/    /'
qemu-system-x86_64 --version 2>/dev/null | head -1 | sed 's/^/    /'
python3 --version 2>/dev/null | sed 's/^/    /'
echo ""
