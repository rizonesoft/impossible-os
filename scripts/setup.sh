#!/usr/bin/env bash
# ============================================================================
# setup.sh -- One-command development environment setup
#
# Clone the repo, run this script, and you're ready to build.
#
# Usage:
#   bash scripts/setup.sh             Install deps + verification build
#   bash scripts/setup.sh --verify    Read-only hard gate: presence + version floors (no install)
#   bash scripts/setup.sh --check-versions  Read-only required-tool version-floor gate only
#   bash scripts/setup.sh --versions  Advisory version report vs floors (always exits 0)
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
    "qemu-img:qemu-img (release VM-format conversions: VHDX/VDI/qcow2; ships in qemu-utils)"
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
    "qemu-img:qemu-img:7.0:qemu-img --version 2>&1 | head -1 | grep -oE '[0-9]+\.[0-9]+' | head -1"
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
  bash scripts/setup.sh --verify    Hard contract: required tools present AND meeting their version floors (read-only; no install, no build).
  bash scripts/setup.sh --check-versions  Hard required-tool version-floor gate only (read-only).
  bash scripts/setup.sh --versions  Advisory report of installed versions vs floors (always exits 0).
  bash scripts/setup.sh --help      Show this help.

Required host tools (see "Host Bootstrap Contract" in canonical doc):
  clang-19, ld.lld-19, llvm-objcopy-19, llvm-ar-19, llvm-nm-19,
  nasm, gcc, python3, qemu-system-x86_64, qemu-img (qemu-utils),
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
    else
        echo -e "  ${RED}$missing required tool(s) missing.${NC}"
        echo -e "  Fix: ${CYAN}bash scripts/setup.sh${NC}  (installs packages for the detected distro)"
    fi

    # OPTIONAL-tier report: LSP-MCP bridge dependencies (TODO-07 in
    # 00-infrastructure). Each one is gated graceful-skip at the
    # bridge level; missing-here just means that language's LSP
    # tools won't be available through the MCP bridge. Status is
    # advisory only -- never affects --verify exit code.
    #
    # We use the bridge's own is_available() probes (which include
    # runtime --version checks for npm-shim'd binaries like
    # bash-language-server and pyright-langserver) rather than a
    # shallow `command -v`. A broken Node shim would otherwise be
    # reported as OK here while the bridge SKIPs it at runtime;
    # Codex post-implementation review of the LSP-MCP integration
    # flagged the mismatch.
    echo ""
    echo -e "${CYAN}==================================================${NC}"
    echo -e "${CYAN}  Optional host tools (LSP-MCP bridge -- TODO-07)${NC}"
    echo -e "${CYAN}==================================================${NC}"
    echo ""
    # Run the bridge's is_available() probes via python3. Output is
    # `<lang>:<status>:<binary-path>` per line. status is OK / MISS.
    # If python3 is missing or the bridge module fails to import,
    # fall back to advisory `command -v` checks.
    local opt_report
    opt_report=$(python3 - <<'PY' 2>/dev/null
import sys
sys.path.insert(0, 'scripts/lsp-mcp')
try:
    from servers import bash_server, python_server, asm_server, powershell_server
    import shutil
    probes = [
        ('bash-language-server (shell LSP)', bash_server.is_available, bash_server.BASH_LSP_BIN),
        ('pyright (Python LSP, sibling pyright CLI probed for runnability)', python_server.is_available, python_server.PYRIGHT_BIN),
        ('asm-lsp (NASM LSP)', asm_server.is_available, asm_server.ASM_LSP_BIN),
        ('pwsh + PSES (PowerShell 7.x; PSES module probed)', powershell_server.is_available, 'pwsh'),
    ]
    for label, probe, binname in probes:
        ok = probe()
        path = shutil.which(binname) or ''
        print(f'{label}|{"OK" if ok else "MISS"}|{path}')
except Exception as exc:
    print(f'__error__|{exc}')
PY
)
    if [ -n "$opt_report" ] && [[ "$opt_report" != __error__* ]]; then
        while IFS='|' read -r opt_label opt_status opt_path; do
            [ -z "$opt_label" ] && continue
            if [ "$opt_status" = "OK" ]; then
                if [ -n "$opt_path" ]; then
                    echo -e "  ${GREEN}OK${NC}   $opt_label  ($opt_path)"
                else
                    echo -e "  ${GREEN}OK${NC}   $opt_label"
                fi
            else
                if [ -n "$opt_path" ]; then
                    echo -e "  ${YELLOW}optional -- not runnable${NC}  $opt_label  (binary present at $opt_path but probe failed)"
                else
                    echo -e "  ${YELLOW}optional -- not installed${NC}  $opt_label"
                fi
            fi
        done <<< "$opt_report"
    else
        # Bridge module failed to import -- fall back to shallow check.
        local opt_label opt_check
        for entry in \
            "bash-language-server:bash-language-server" \
            "pyright-langserver:pyright (Python LSP)" \
            "asm-lsp:asm-lsp (NASM LSP)" \
            "pwsh:pwsh (PowerShell 7.x; PSES module probed at bridge spawn)"
        do
            opt_check="${entry%%:*}"
            opt_label="${entry#*:}"
            if command -v "$opt_check" >/dev/null 2>&1; then
                echo -e "  ${GREEN}OK${NC}   $opt_label  ($(command -v "$opt_check"))"
            else
                echo -e "  ${YELLOW}optional -- not installed${NC}  $opt_label"
            fi
        done
    fi
    echo ""
    echo -e "  Optional installs: ${CYAN}bash scripts/setup-deps.sh${NC}"
    echo -e "  See: ${CYAN}docs/infrastructure/development-tooling.md${NC} (LSP MCP Bridge subsection)"

    if [ "$missing" -eq 0 ]; then
        return 0
    else
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

# ---- Optional tools (advisory floor only; never hard-fail) ----
# Keyed by the VERSION_SPECS check-command. Everything else in VERSION_SPECS is
# REQUIRED and is hard-gated by check_versions. Explicit set (not a display-name
# substring) so a future rename cannot silently flip a tool's enforcement tier.
OPTIONAL_VERSION_TOOLS=(
    "bear"
)

is_optional_version_tool() {
    local c="$1" t
    for t in "${OPTIONAL_VERSION_TOOLS[@]}"; do
        [ "$c" = "$t" ] && return 0
    done
    return 1
}

# Hard required-tool version-floor gate. Every REQUIRED tool in VERSION_SPECS
# must be present, have a parseable version, AND meet its documented minimum.
# Fails closed: MISSING, UNKNOWN (unparseable), and BELOW-floor required tools
# all return non-zero -- a floor that cannot be proven is a failure. Optional
# tools (OPTIONAL_VERSION_TOOLS) are reported but never affect the exit code.
# Also drift-guards that every REQUIRED non-firmware sentinel carries a floor in
# VERSION_SPECS, so a required tool can never silently escape the gate. Composed
# into --verify (presence + floors = the hard contract) and exposed standalone
# as --check-versions; --versions stays advisory.
check_versions() {
    local failed=0 spec
    echo -e "${CYAN}==================================================${NC}"
    echo -e "${CYAN}  Required-tool version floors (hard gate)${NC}"
    echo -e "${CYAN}==================================================${NC}"
    echo ""
    for spec in "${VERSION_SPECS[@]}"; do
        local check="${spec%%:*}"
        local rest="${spec#*:}"
        local name="${rest%%:*}"
        rest="${rest#*:}"
        local minimum="${rest%%:*}"
        local probe="${rest#*:}"
        local optional=0
        is_optional_version_tool "$check" && optional=1
        local have
        if ! command -v "$check" >/dev/null 2>&1; then
            if [ "$optional" -eq 1 ]; then
                echo -e "  ${YELLOW}skip${NC}  $name (optional, not installed)"
            else
                echo -e "  ${RED}FAIL${NC}  $name: required tool missing"
                echo -e "        fix: bash scripts/setup.sh"
                failed=1
            fi
            continue
        fi
        have=$(eval "$probe" 2>/dev/null || echo "")
        if [ -z "$have" ]; then
            if [ "$optional" -eq 1 ]; then
                echo -e "  ${YELLOW}warn${NC}  $name (optional): version unparseable"
            else
                echo -e "  ${RED}FAIL${NC}  $name: version unparseable, floor >= $minimum cannot be proven"
                echo -e "        fix: run '$check --version'; update the probe in scripts/setup.sh if its output changed"
                failed=1
            fi
            continue
        fi
        if version_ge "$have" "$minimum"; then
            echo -e "  ${GREEN}OK${NC}    $name $have (>= $minimum)"
        elif [ "$optional" -eq 1 ]; then
            echo -e "  ${YELLOW}warn${NC}  $name $have (optional; below $minimum)"
        else
            echo -e "  ${RED}FAIL${NC}  $name $have below floor $minimum"
            echo -e "        fix: upgrade $name to >= $minimum (bash scripts/setup.sh)"
            failed=1
        fi
    done
    # Drift guard: every REQUIRED non-firmware sentinel must carry a floor, else
    # a required tool (e.g. qemu-img) escapes the loop above unchecked.
    local sent check
    for sent in "${REQUIRED_SENTINELS[@]}"; do
        check="${sent%%:*}"
        case "$check" in __OVMF_CODE__|__OVMF_VARS__) continue ;; esac
        local found=0 vs
        for vs in "${VERSION_SPECS[@]}"; do
            [ "${vs%%:*}" = "$check" ] && { found=1; break; }
        done
        if [ "$found" -eq 0 ]; then
            echo -e "  ${RED}FAIL${NC}  $check: required sentinel has no VERSION_SPECS floor (set drift)"
            echo -e "        fix: add a $check version spec to VERSION_SPECS in scripts/setup.sh"
            failed=1
        fi
    done
    echo ""
    if [ "$failed" -ne 0 ]; then
        echo -e "  ${RED}One or more required tools are below the documented floor.${NC}"
        echo -e "  Advisory report: ${CYAN}bash scripts/setup.sh --versions${NC}."
        return 1
    fi
    echo -e "  ${GREEN}All required tools meet their minimum version floors.${NC}"
    return 0
}

# ---- Arg parsing ----
MODE="install"
while [ $# -gt 0 ]; do
    case "$1" in
        --help|-h) MODE="help" ;;
        --verify) MODE="verify" ;;
        --check-versions) MODE="check-versions" ;;
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
        # Hard contract = presence AND required version floors. Presence first
        # (a missing tool has no version to check); floors second.
        verify_tools || exit 1
        echo ""
        check_versions
        exit $?
        ;;
    check-versions)
        check_versions
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
echo -e "${CYAN}[1/4]${NC} Installing system dependencies..."
echo ""
bash "$SCRIPT_DIR/setup-deps.sh"

# Step 2: Sentinel check -- fail early if any required tool is missing.
echo ""
echo -e "${CYAN}[2/4]${NC} Verifying required tools..."
echo ""
if ! verify_tools; then
    echo ""
    echo -e "  ${RED}Host bootstrap incomplete.${NC}"
    echo -e "  See: ${CYAN}docs/infrastructure/development-tooling.md${NC} (Host Bootstrap Contract)"
    exit 1
fi

# Step 3: Version-floor gate -- fail clearly on a below-floor required tool
# BEFORE the expensive build, instead of letting it surface as an opaque
# build/boot failure later.
echo ""
echo -e "${CYAN}[3/4]${NC} Checking required-tool version floors..."
echo ""
if ! check_versions; then
    echo ""
    echo -e "  ${RED}Host bootstrap incomplete (tool below documented floor).${NC}"
    echo -e "  See: ${CYAN}docs/infrastructure/development-tooling.md${NC} (Supported Host Profiles)"
    exit 1
fi

# Step 4: Verification build
echo ""
echo -e "${CYAN}[4/4]${NC} Verification build..."
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
