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
    echo "    xorriso mtools qemu-system-x86 qemu-utils ovmf"
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
            "gcc:build-essential"
            "xorriso:xorriso"
            "mtools:mtools"
            "qemu-system-x86_64:qemu-system-x86"
            "qemu-img:qemu-utils"
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
            "gcc:gcc"
            "xorriso:xorriso"
            "mtools:mtools"
            "qemu-system-x86_64:qemu-system-x86"
            "qemu-img:qemu-img"
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
            "gcc:gcc"
            "xorriso:libisoburn"
            "mtools:mtools"
            "qemu-system-x86_64:qemu-system-x86"
            "qemu-img:qemu-img"
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

# PyYAML (required by scripts/tooling-doctor.sh + scripts/test-tooling.sh
# for .github/workflows/*.yml parse).
if python3 -c "import yaml" 2>/dev/null; then
    skip "PyYAML (python3-yaml)"
else
    case "$DISTRO" in
        debian) echo -e "  ${YELLOW}Installing python3-yaml...${NC}"; sudo apt-get install -y python3-yaml ;;
        fedora) echo -e "  ${YELLOW}Installing python3-pyyaml...${NC}"; sudo dnf install -y python3-pyyaml ;;
        arch)   echo -e "  ${YELLOW}Installing python-yaml...${NC}"; sudo pacman -S --noconfirm python-yaml ;;
        *) echo -e "  ${YELLOW}Install PyYAML manually for your distro${NC}" ;;
    esac
    if python3 -c "import yaml" 2>/dev/null; then
        ok "PyYAML installed"
        INSTALLED=$((INSTALLED + 1))
    else
        echo -e "  ${YELLOW}warn:${NC} PyYAML still missing after install; tooling-doctor.sh --json and workflow YAML checks will fall back to warn-only."
    fi
fi

# python3-jsonschema (required by scripts/todo-graph/tests/test_build.sh
# sub-test 6d to validate docs/infrastructure/todo-metadata.schema.json
# as a Draft 2020-12 schema and to validate the authoritative example
# from docs/infrastructure/todo-metadata.md against it).
if python3 -c "import jsonschema" 2>/dev/null; then
    skip "jsonschema (python3-jsonschema)"
else
    case "$DISTRO" in
        debian) echo -e "  ${YELLOW}Installing python3-jsonschema...${NC}"; sudo apt-get install -y python3-jsonschema ;;
        fedora) echo -e "  ${YELLOW}Installing python3-jsonschema...${NC}"; sudo dnf install -y python3-jsonschema ;;
        arch)   echo -e "  ${YELLOW}Installing python-jsonschema...${NC}"; sudo pacman -S --noconfirm python-jsonschema ;;
        *) echo -e "  ${YELLOW}Install python3-jsonschema manually for your distro${NC}" ;;
    esac
    if python3 -c "import jsonschema" 2>/dev/null; then
        ok "jsonschema installed"
        INSTALLED=$((INSTALLED + 1))
    else
        echo -e "  ${YELLOW}warn:${NC} jsonschema still missing after install; the schema sidecar regression sub-test (test_build.sh 6d) will FAIL until installed."
    fi
fi

# python3-mcp (OPTIONAL -- required only by scripts/todo-graph/mcp_server.py
# which ships the TODO metadata layer's AI-agent integration over the
# query surface). Skipped cleanly when unavailable so setup-deps never
# blocks on it.
if python3 -c "import mcp" 2>/dev/null; then
    skip "mcp (python3-mcp)"
else
    echo -e "  ${YELLOW}note:${NC} mcp SDK not installed; AI-agent integration (todo-graph MCP server) unavailable."
    echo -e "        Optional install: ${GREEN}pip install --user mcp${NC} (or use a venv)."
fi

# LSP-MCP bridge optional dependencies (TODO-07 in 00-infrastructure).
# clangd-19 is already in REQUIRED via scripts/setup.sh. The four
# language servers below are OPTIONAL: each is gated graceful-skip in
# scripts/lsp-mcp/servers/<lang>_server.py via is_available(); a
# missing dep produces a SKIP banner in --self-test, never blocks.
# We DO NOT autoinstall any of these because they require system-wide
# npm / cargo / pwsh writes that should be the user's explicit choice.

# bash-language-server (OPTIONAL -- shell LSP for the LSP-MCP bridge)
if command -v bash-language-server >/dev/null 2>&1; then
    skip "bash-language-server"
else
    echo -e "  ${YELLOW}note:${NC} bash-language-server not installed; LSP-MCP bridge .sh integration unavailable."
    echo -e "        Optional install: ${GREEN}npm install -g bash-language-server${NC} (also apt install shellcheck for full diagnostics)."
fi

# pyright (OPTIONAL -- Python LSP for the LSP-MCP bridge)
if command -v pyright-langserver >/dev/null 2>&1; then
    skip "pyright (pyright-langserver)"
else
    echo -e "  ${YELLOW}note:${NC} pyright-langserver not installed; LSP-MCP bridge .py integration unavailable."
    echo -e "        Optional install: ${GREEN}npm install -g pyright${NC} (requires node >= 14)."
fi

# asm-lsp (OPTIONAL -- NASM LSP for the LSP-MCP bridge)
if command -v asm-lsp >/dev/null 2>&1; then
    skip "asm-lsp"
else
    echo -e "  ${YELLOW}note:${NC} asm-lsp not installed; LSP-MCP bridge .asm integration unavailable."
    echo -e "        Optional install: ${GREEN}cargo install asm-lsp${NC} (requires Rust toolchain)."
fi

# pwsh + PowerShellEditorServices (OPTIONAL -- PowerShell LSP for the
# bridge). PSES is a pwsh module, not a standalone binary, so we
# delegate to the bridge's own powershell_server.is_available() probe
# which validates pwsh on PATH AND pwsh >= 7.0 AND PSES discoverable
# (both Get-Module -ListAvailable AND the VS Code extension glob
# fallback). A host with pwsh installed but PSES missing reports here
# as "PSES missing" and gets the Install-Module hint -- without this,
# `command -v pwsh` would report it as installed and the user would
# be surprised when --self-test --lang=ps1 SKIPs at runtime.
PWSH_STATE=$(python3 - <<'PY' 2>/dev/null
import sys
sys.path.insert(0, 'scripts/lsp-mcp')
try:
    from servers import powershell_server
    state = powershell_server._probe()
    print(state.failure_reason or 'OK')
except Exception as exc:
    print(f'__error__:{exc}')
PY
)
case "$PWSH_STATE" in
    OK)
        skip "pwsh + PowerShellEditorServices"
        ;;
    pwsh-missing)
        echo -e "  ${YELLOW}note:${NC} pwsh not installed; LSP-MCP bridge .ps1 integration unavailable."
        echo -e "        Optional install: ${GREEN}apt install powershell${NC} or download from https://aka.ms/powershell-release."
        echo -e "        Then install PSES module: ${GREEN}pwsh -NoProfile -Command 'Install-Module PowerShellEditorServices -Scope CurrentUser -Force'${NC}."
        ;;
    pwsh-too-old|pwsh-broken|pwsh-version-unparseable)
        echo -e "  ${YELLOW}note:${NC} pwsh present but not runnable as 7.x; LSP-MCP bridge .ps1 integration unavailable."
        echo -e "        Reinstall pwsh 7.x from ${GREEN}https://aka.ms/powershell-release${NC}."
        ;;
    pses-missing|pses-entrypoint-missing)
        echo -e "  ${YELLOW}note:${NC} pwsh present but PowerShellEditorServices module is missing; LSP-MCP bridge .ps1 integration unavailable."
        echo -e "        Optional install: ${GREEN}pwsh -NoProfile -Command 'Install-Module PowerShellEditorServices -Scope CurrentUser -Force'${NC}."
        ;;
    *)
        # Fallback for unrecognized state OR python3 import failure.
        if command -v pwsh >/dev/null 2>&1; then
            skip "pwsh (PSES probe failed; install via Install-Module PowerShellEditorServices)"
        else
            echo -e "  ${YELLOW}note:${NC} pwsh not installed; LSP-MCP bridge .ps1 integration unavailable."
            echo -e "        Optional install: ${GREEN}apt install powershell${NC}."
        fi
        ;;
esac

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
