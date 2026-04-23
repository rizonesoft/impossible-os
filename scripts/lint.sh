#!/usr/bin/env bash
# ============================================================================
# lint.sh -- Code style linter for Impossible OS
#
# Checks all C source and header files against the project's coding standards.
# Uses grep and awk for pattern matching (no external dependencies).
#
# Usage:
#   bash scripts/lint.sh              # Lint all files
#   bash scripts/lint.sh src/kernel/  # Lint specific directory
#
# Exit codes:
#   0 = clean (no violations)
#   1 = violations found
# ============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# ---- Help ----
case "${1:-}" in
    -h|--help)
        cat <<'EOF'
Impossible OS -- code style linter

Usage:
  bash scripts/lint.sh                    Lint full repo (default scope)
  bash scripts/lint.sh src/kernel/mm/     Lint a specific path (skips xref check)
  bash scripts/lint.sh --help             Show this help

Checks (5):
  1. #pragma once or include guard in every .h
  2. Lines <= 120 characters (excludes comment lines)
  3. No trailing whitespace
  4. No numeric TODO shorthand (TODO-NN sectionN, DNN TNN) outside todo/**
     except a baseline allowlist of pre-existing files (see the developer
     tooling stack roadmap -- Tooling Doctor section -- for the legacy-XREF
     sweep).
  5. No bare "section sign + number" (§N) in code comments outside todo/**
     unless the same line carries an external-spec qualifier (UEFI, Intel
     SDM, NTFS, ACPI, RFC, etc.). Pre-existing files are warn-listed;
     anything new is an error. Code comments must name the FEATURE, not
     the TODO section number, because numbers drift silently on renumber.

Removed 2026-04-17: "functions > 50 lines" and "lowercase #define" warnings
(warn-only, never triaged; duplicated Codex + domain code-quality coverage).
Removed 2026-04-18: camelCase-function warning -- unsafe for a Win11-native
OS where NT/Rtl/Ob/Se/Ke/Mm/Io/Ex API names are deliberately CamelCase.
Style review belongs with Codex adversarial review + domain code-quality
skills; lint stays focused on drift-catching structural checks.

Exit codes:
  0 = clean
  1 = errors found
EOF
        exit 0
        ;;
esac

# ---- Colors ----
RED='\033[0;31m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
DIM='\033[0;90m'
NC='\033[0m'

# ---- Counters ----
ERRORS=0
WARNINGS=0

error() {
    local file=$1 line=$2 msg=$3
    echo -e "${RED}error${NC}: ${file}:${line}: ${msg}"
    ERRORS=$((ERRORS + 1))
}

warn() {
    local file=$1 line=$2 msg=$3
    echo -e "${YELLOW}warn${NC}: ${file}:${line}: ${msg}"
    WARNINGS=$((WARNINGS + 1))
}

# ---- Determine files to lint ----
SEARCH_PATH="${1:-$REPO_ROOT/src $REPO_ROOT/include}"

# Exclude third-party and auto-generated files. Third-party libraries
# carry their upstream style and line-length conventions -- we do not own
# them, so we do not gate on them. Auto-generated headers are pure data
# dumps with mechanical formatting.
EXCLUDE_PATTERNS=(
    "stb_truetype"
    "stb_image"
    "cJSON"
    "build_info.h"
    "os_logo.h"
    "bsod_icon.h"
    "boot_splash_font_data.h"
)

build_exclude_args() {
    local args=""
    for pat in "${EXCLUDE_PATTERNS[@]}"; do
        args="$args ! -name '*${pat}*'"
    done
    echo "$args"
}

# Find all .c and .h files
mapfile -t FILES < <(eval "find $SEARCH_PATH -type f \\( -name '*.c' -o -name '*.h' \\) $(build_exclude_args)" 2>/dev/null | sort)

if [ ${#FILES[@]} -eq 0 ]; then
    echo -e "${RED}No source files found.${NC}"
    exit 1
fi

echo -e "${CYAN}══════════════════════════════════════════════════${NC}"
echo -e "${CYAN}  Impossible OS -- Code Style Linter${NC}"
echo -e "${CYAN}══════════════════════════════════════════════════${NC}"
echo -e "  ${DIM}Checking ${#FILES[@]} files...${NC}"
echo ""

# ============================================================================
# Check 1: #pragma once or include guard in .h files
# ============================================================================
for file in "${FILES[@]}"; do
    [[ "$file" != *.h ]] && continue
    relpath="${file#"$REPO_ROOT"/}"

    if ! grep -q '#pragma once' "$file" 2>/dev/null; then
        # Check for traditional include guards
        if ! grep -qE '#ifndef\s+\w+_H' "$file" 2>/dev/null; then
            error "$relpath" 1 "missing #pragma once or include guard"
        fi
    fi
done

# ============================================================================
# Check 2: Lines > 120 characters
# ============================================================================
for file in "${FILES[@]}"; do
    relpath="${file#"$REPO_ROOT"/}"
    awk -v file="$relpath" '
        length($0) > 120 {
            # Skip comments and long string literals
            if ($0 ~ /^\s*\*/ || $0 ~ /^\s*\/\*/ || $0 ~ /^\s*\/\//) next
            printf "'"${RED}"'error'"${NC}"': %s:%d: line exceeds 120 characters (%d chars)\n", file, NR, length($0)
            errors++
        }
        END { exit errors > 0 ? 1 : 0 }
    ' "$file" 2>/dev/null && true
    # Count errors from awk output
    long_lines=$(awk 'length($0) > 120 && $0 !~ /^\s*\*/ && $0 !~ /^\s*\/\*/ && $0 !~ /^\s*\/\//' "$file" 2>/dev/null | wc -l)
    ERRORS=$((ERRORS + long_lines))
done

# ============================================================================
# Check 3: Trailing whitespace
# ============================================================================
for file in "${FILES[@]}"; do
    relpath="${file#"$REPO_ROOT"/}"
    while IFS= read -r match; do
        linenum=$(echo "$match" | cut -d: -f1)
        error "$relpath" "$linenum" "trailing whitespace"
    done < <(grep -n '[[:space:]]$' "$file" 2>/dev/null || true)
done

# ============================================================================
# Check 4: Drift-prone TODO numeric shorthand outside todo/
# ============================================================================
# Numeric XREFs (e.g. TODO-<num> <section-sign><num>, D<num> T<num> <section-sign><num>)
# go stale silently when TODOs renumber. Inside todo/** the create-todo / validate-todo-file skills
# enforce the shorthand deliberately. Outside todo/** we want anchor links to a
# canonical doc or named-capability references instead.
#
# Known-dirty files (pre-existing refs) get a warning; everything else is an
# error so new drift cannot sneak in. Legacy cleanup is tracked in
# todo/00-infrastructure/TODO-01-developer-tooling-stack.md "tooling doctor"
# section.
#
# Only runs on the default lint scope (whole repo). If a path argument was
# given, skip -- per-subsystem runs should not scan outside their path.
if [ "$#" -eq 0 ]; then
    # ERE regex matching all four drift-prone shorthand flavors.
    TODO_XREF_REGEX='(TODO-[0-9]+[[:space:]]*§[0-9]+|TODO-[0-9]+[[:space:]]+section[[:space:]]+[0-9]+|D[0-9]+[[:space:]]*T[0-9]+[[:space:]]*§?[0-9]+|\bT[0-9]+[[:space:]]*§[0-9]+)'

    # Paths with pre-existing legacy refs (warn-only until the sweep lands).
    # Keep sorted alphabetically for easy audit. The legacy sweep landed
    # with the developer-tooling stack Legacy-XREF Sweep item; this list
    # is intentionally empty so any new numeric-TODO shorthand is flagged
    # as an error.
    TODO_XREF_LEGACY_FILES=()

    is_todo_xref_legacy() {
        local path="$1"
        local legacy
        for legacy in "${TODO_XREF_LEGACY_FILES[@]}"; do
            [ "$path" = "$legacy" ] && return 0
        done
        return 1
    }

    # Top-level roots to scan. Keep narrow so unrelated repos (node_modules,
    # build artifacts, plugin marketplaces) are not traversed. `.github/`
    # is included so workflow YAML cannot sneak in shorthand that would
    # then outlive a TODO renumbering. The PULL_REQUEST_TEMPLATE.md skip
    # below preserves the intentional teaching template.
    TODO_XREF_ROOTS=(
        "$REPO_ROOT/src"
        "$REPO_ROOT/include"
        "$REPO_ROOT/scripts"
        "$REPO_ROOT/docs"
        "$REPO_ROOT/user"
        "$REPO_ROOT/tools"
        "$REPO_ROOT/.github"
        "$REPO_ROOT/README.md"
        "$REPO_ROOT/CONTRIBUTING.md"
        "$REPO_ROOT/CLAUDE.md"
        "$REPO_ROOT/Makefile"
        "$REPO_ROOT/CHANGELOG.md"
    )

    # Existing roots only (some may not exist in every checkout).
    TODO_XREF_EXISTING=()
    for root in "${TODO_XREF_ROOTS[@]}"; do
        [ -e "$root" ] && TODO_XREF_EXISTING+=("$root")
    done

    while IFS=: read -r file linenum rest; do
        [ -z "$file" ] && continue
        relpath="${file#"$REPO_ROOT"/}"
        # Skip templates that TEACH the shorthand intentionally, and the
        # AI-workflow regression script whose whole purpose is to enforce
        # the AI Development System roadmap section structure (references
        # are load-bearing -- if those sections renumber, the script must
        # be updated too; the rule against drift does not apply to the
        # file that IS the drift-detector).
        case "$relpath" in
            .github/PULL_REQUEST_TEMPLATE.md) continue ;;
            scripts/test-ai-system.sh) continue ;;
            scripts/todo-graph/*) continue ;;
            *.claude/*) continue ;;
        esac
        if is_todo_xref_legacy "$relpath"; then
            warn "$relpath" "$linenum" "numeric TODO shorthand (legacy; tracked for cleanup)"
        else
            error "$relpath" "$linenum" "numeric TODO shorthand outside todo/ (breaks on TODO renumbering; use anchor link or capability name)"
        fi
    done < <(
        grep -rnHE "$TODO_XREF_REGEX" \
            --include='*.c' --include='*.h' --include='*.asm' --include='*.S' \
            --include='*.sh' --include='*.md' --include='*.json' \
            --include='*.bat' --include='*.ps1' --include='*.py' \
            --include='*.yml' --include='*.yaml' \
            --include='Makefile' --include='*.mk' \
            --exclude-dir='.git' --exclude-dir='build' --exclude-dir='node_modules' \
            "${TODO_XREF_EXISTING[@]}" 2>/dev/null \
        || true
    )
fi

# ============================================================================
# Check 5: Bare "section sign + number" in code comments without external-spec
# qualifier. `§11`, `(§4)`, `§1.3` inside a .c / .h / .asm / .py / .sh file
# almost always refers to a TODO section; those break silently when the
# owning TODO renumbers. External-spec citations (UEFI §X, Intel SDM §Y,
# NTFS §Z, ACPI §W, etc.) stay legal because they point at stable published
# standards.
#
# Allowed tokens on the same line: UEFI, Intel, SDM, AMD, APM, RFC <n>,
# ACPI <n>, NTFS, FAT32/16, NVMe, PCI / PCIe, PE/COFF, PE32, COFF, USB <n>,
# xHCI / EHCI / OHCI / UHCI, VirtIO, SMBIOS, IEEE, NIST, TCG, WHEA, HPET,
# MP Spec, "spec ", "specification".
#
# The legacy allowlist lists every file that already contains bare §N refs
# today (WARN) so the check does not block commits while the cleanup is in
# progress. Any file NOT on the allowlist must not introduce new bare §N --
# that is the permanent drift gate for new code.
#
# Only runs on the default lint scope (whole repo).
if [ "$#" -eq 0 ]; then
    BARE_SECTION_RE='§[0-9]'
    SPEC_TOKENS_RE='(UEFI|Intel|SDM|AMD|APM|RFC [0-9]|ACPI [0-9]|NTFS|FAT[0-9]|NVMe|PCIe?|PE/COFF|PE32|COFF|USB [0-9]|xHCI|EHCI|OHCI|UHCI|VirtIO|SMBIOS|IEEE|NIST|TCG|WHEA|HPET|MP Spec|spec |specification)'

    # Path-based spec-code exemption: files whose WHOLE JOB is implementing
    # an external standard carry §N refs to that standard per-line without
    # repeating the standard's name (the file scope provides the context).
    # Add directories/paths here only when every §N ref in the file truly
    # points at the named external standard.
    is_bare_section_spec_code() {
        local path="$1"
        case "$path" in
            # NTFS on-disk format spec (Linux-NTFS + Microsoft NTFS reference).
            src/kernel/fs/ntfs/*|include/kernel/fs/ntfs.h) return 0 ;;
        esac
        return 1
    }

    # Files with pre-existing bare §N refs (WARN-only; tracked for cleanup
    # in todo/00-infrastructure/TODO-01-developer-tooling-stack.md). Keep
    # sorted alphabetically for easy audit. Any new file must NOT land
    # here -- check the lint output and replace the §N with a feature
    # name or a doc anchor link before adding.
    BARE_SECTION_LEGACY_FILES=(
        include/kernel/atomic.h
        include/kernel/boot_info.h
        include/kernel/boot_version.h
        include/kernel/drivers/nvme.h
        include/kernel/drivers/virtio/blk.h
        include/kernel/drivers/virtio/virtio.h
        include/kernel/elf.h
        include/kernel/exec.h
        include/kernel/fs/vfs.h
        include/kernel/ipc/alpc.h
        include/kernel/ipc/alpc_port.h
        include/kernel/mm/boot_reserved.h
        include/kernel/mm/heap.h
        include/kernel/nt/nt_file.h
        include/kernel/ob/ob.h
        include/kernel/ob/ob_type.h
        include/kernel/pe.h
        include/kernel/sched/task.h
        include/kernel/smp.h
        include/kernel/test/input_record.h
        include/kernel/test/klog_suppress.h
        include/kernel/test/scratch.h
        include/kernel/test/test.h
        include/kernel/time/wall_clock.h
        include/kernel/uefi_runtime.h
        include/registry.h
        scripts/debug/desktop/run-matrix-desktop-tests.bat
        scripts/debug/usermode/run-all-usermode-tests.bat
        scripts/debug/usermode/run-test_faultinject.bat
        scripts/debug/usermode/run-test_harness_smoke.bat
        scripts/debug/usermode/run-test_syscall.bat
        scripts/lint.sh
        scripts/qemu-screenshot.sh
        scripts/test-tooling.sh
        src/boot/uefi/boot_info_mirror.h
        src/boot/uefi/bootx64.c
        src/boot/uefi/efi.h
        src/desktop/wm.c
        src/kernel/acpi.c
        src/kernel/cpu_security.c
        src/kernel/drivers/nvme.c
        src/kernel/drivers/xhci.c
        src/kernel/elf.c
        src/kernel/exec.c
        src/kernel/fs/gpt.c
        src/kernel/fs/partition.c
        src/kernel/fs/vfs.c
        src/kernel/idt.c
        src/kernel/ipc/alpc_port.c
        src/kernel/main/boot_hw.c
        src/kernel/main/boot_payload.c
        src/kernel/main/boot_recovery.c
        src/kernel/main/boot_version.c
        src/kernel/main/compositor.c
        src/kernel/main/main_internal.h
        src/kernel/mm/boot_reserved.c
        src/kernel/mm/heap.c
        src/kernel/mm/pmm.c
        src/kernel/mm/vmm.c
        src/kernel/nt/nt_alpc.c
        src/kernel/nt/nt_registry.c
        src/kernel/nt/nt_sync.c
        src/kernel/nt/nt_syscall.c
        src/kernel/ob/ob.c
        src/kernel/ob/ob_ns.c
        src/kernel/ob/ob_thread.c
        src/kernel/panic.c
        src/kernel/pe.c
        src/kernel/registry.c
        src/kernel/sched/irql.c
        src/kernel/sched/syscall.c
        src/kernel/sched/syscall_entry.asm
        src/kernel/sched/task.c
        src/kernel/sched/transition_ring.c
        src/kernel/security/pku.c
        src/kernel/security/token.c
        src/kernel/smp/smp.c
        src/kernel/spinner.c
        src/kernel/test/input_record.c
        src/kernel/test/race_barrier.c
        src/kernel/test/test_acpi_power.c
        src/kernel/test/test_alpc.c
        src/kernel/test/test_boot_info.c
        src/kernel/test/test_boot_init.c
        src/kernel/test/test_cpu_security.c
        src/kernel/test/test_exec.c
        src/kernel/test/test_harness.c
        src/kernel/test/test_heap.c
        src/kernel/test/test_klog.c
        src/kernel/test/test_nt_types.c
        src/kernel/test/test_ob.c
        src/kernel/test/test_peb_teb.c
        src/kernel/test/test_pmm.c
        src/kernel/test/test_registry.c
        src/kernel/test/test_runner.c
        src/kernel/test/test_security.c
        src/kernel/test/test_usermode.c
        src/kernel/test/test_usermode_launcher.c
        src/kernel/test/test_vfs.c
        src/kernel/test/test_vmm.c
        src/kernel/time/wall_clock.c
        src/kernel/timer.c
        src/kernel/tpm.c
        src/kernel/uefi_runtime.c
        src/libc/string.c
        tools/boot-info-manifest/check-doc-coverage.py
        user/include/test.h
        user/lib/win32.c
        user/test/test_fastpath_fuzz.c
        user/test/test_fileio.c
        user/test/test_harness_smoke.c
        user/test/test_ipc.c
        user/test/test_libc.c
        user/test/test_loader_pe.c
        user/test/test_perf_syscall.c
        user/test/test_process.c
        user/test/test_smoke_boot.c
        user/test/test_stress_libc.c
        user/test/test_syscall.c
        user/test/test_win32.c
    )

    is_bare_section_legacy() {
        local path="$1"
        local legacy
        for legacy in "${BARE_SECTION_LEGACY_FILES[@]}"; do
            [ "$path" = "$legacy" ] && return 0
        done
        return 1
    }

    # Same scan roots as Check 4; reuse the existing TODO_XREF_EXISTING array.
    while IFS=: read -r file linenum rest; do
        [ -z "$file" ] && continue
        # Line passes if it carries an external-spec qualifier.
        if echo "$rest" | grep -qE "$SPEC_TOKENS_RE"; then
            continue
        fi
        relpath="${file#"$REPO_ROOT"/}"
        case "$relpath" in
            .github/PULL_REQUEST_TEMPLATE.md) continue ;;
            scripts/test-ai-system.sh) continue ;;
            scripts/todo-graph/*) continue ;;
            *.claude/*) continue ;;
        esac
        # Path-based spec-code exemption (NTFS etc.).
        if is_bare_section_spec_code "$relpath"; then
            continue
        fi
        if is_bare_section_legacy "$relpath"; then
            warn "$relpath" "$linenum" "bare section ref (legacy; tracked for cleanup)"
        else
            error "$relpath" "$linenum" "bare section ref in code (breaks on TODO renumbering; name the feature or cite external spec)"
        fi
    done < <(
        grep -rnHE "$BARE_SECTION_RE" \
            --include='*.c' --include='*.h' --include='*.asm' --include='*.S' \
            --include='*.sh' --include='*.bat' --include='*.ps1' --include='*.py' \
            --include='*.yml' --include='*.yaml' \
            --include='Makefile' --include='*.mk' \
            --exclude-dir='.git' --exclude-dir='build' --exclude-dir='node_modules' \
            "${TODO_XREF_EXISTING[@]}" 2>/dev/null \
        || true
    )
fi

# ============================================================================
# Summary
# ============================================================================
echo ""
echo -e "${CYAN}══════════════════════════════════════════════════${NC}"
TOTAL=$((ERRORS + WARNINGS))
if [ "$ERRORS" -eq 0 ] && [ "$WARNINGS" -eq 0 ]; then
    echo -e "  ${CYAN}LINT CLEAN${NC} -- no violations found"
elif [ "$ERRORS" -eq 0 ]; then
    echo -e "  ${YELLOW}$WARNINGS warning(s)${NC}, 0 errors"
else
    echo -e "  ${RED}$ERRORS error(s)${NC}, ${YELLOW}$WARNINGS warning(s)${NC}"
fi
echo -e "  ${DIM}${#FILES[@]} files checked${NC}"
echo -e "${CYAN}══════════════════════════════════════════════════${NC}"

if [ "$ERRORS" -gt 0 ]; then
    exit 1
else
    exit 0
fi
