#!/usr/bin/env bash
# ============================================================================
# tooling-doctor.sh -- Read-only health check for the developer tooling stack.
#
# Runs the full diagnostic sweep that the developer tooling stack roadmap
# (section 7) codifies: toolchain sentinels, wrapper script executability,
# hook install state, workflow YAML sanity, runtime prerequisites, and
# supported-host profile classification. Prints actionable remediation for
# every non-passing check. Does NOT mutate the repo or run any builds.
#
# Usage:
#   bash scripts/tooling-doctor.sh              # prose report (default)
#   bash scripts/tooling-doctor.sh --json       # machine-readable report
#   bash scripts/tooling-doctor.sh --quiet      # summary line only
#   bash scripts/tooling-doctor.sh --help
#
# Exit codes:
#   0 = all hard checks passed (warn-only checks may have fired)
#   1 = one or more hard checks failed
# ============================================================================

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

MODE="prose"
for arg in "$@"; do
    case "$arg" in
        -h|--help)
            cat <<'EOF'
tooling-doctor.sh -- repo-local developer tooling health check

Usage:
  bash scripts/tooling-doctor.sh              Prose report (default)
  bash scripts/tooling-doctor.sh --json       Machine-readable JSON report
  bash scripts/tooling-doctor.sh --quiet      One-line summary only
  bash scripts/tooling-doctor.sh --help       Show this help

Exit codes:
  0 = all hard checks passed
  1 = one or more hard checks failed

Checks (grouped):
  toolchain    setup.sh --verify (clang-19, ld.lld-19, nasm, qemu, OVMF, ...)
  wrappers     scripts/{build,test,lint,run-qemu,debug,test-smoke,
                        install-hooks,setup}.sh are executable and --help works
  hooks        core.hooksPath points at .githooks; pre-commit/post-commit
                installed; pre-push sentinel (opt-in) state reported
  workflows    .github/workflows/{build,release,pages,labeler,stale}.yml
                parse as valid YAML and invoke the canonical wrappers
  runtime      /dev/kvm availability (writable = KVM fast path, unwritable
                = TCG fallback), qemu-img, host-profile classification
                (Ubuntu/Debian / WSL2 / Fedora / Arch / other)
  docs         docs/infrastructure/development-tooling.md exists and the
                anchored sections the doctor references still resolve
EOF
            exit 0
            ;;
        --json)   MODE="json" ;;
        --quiet)  MODE="quiet" ;;
        *)
            echo "Unknown option: $arg" >&2
            echo "Try --help." >&2
            exit 1
            ;;
    esac
done

# ---- Colors (only when stdout is a tty and MODE != json) ----
if [ "$MODE" = "prose" ] && [ -t 1 ]; then
    RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'
    CYAN='\033[0;36m'; DIM='\033[0;90m'; NC='\033[0m'
else
    RED=''; GREEN=''; YELLOW=''; CYAN=''; DIM=''; NC=''
fi

# ---- Check result accumulator ----
# Each check contributes 5 parallel array entries (group, status, name,
# detail, fix). We avoid pipe-delimited strings because detail/fix values
# can legitimately contain any printable character (paths with `|`, for
# example) and a single-delimiter scheme would corrupt the JSON emitter.
# status in: pass warn fail skip
CHECK_GROUPS=()
CHECK_STATUSES=()
CHECK_NAMES=()
CHECK_DETAILS=()
CHECK_FIXES=()
HARD_FAILS=0
WARN_COUNT=0

record() {
    local group="$1" status="$2" name="$3" detail="$4" fix="$5"
    CHECK_GROUPS+=("$group")
    CHECK_STATUSES+=("$status")
    CHECK_NAMES+=("$name")
    CHECK_DETAILS+=("$detail")
    CHECK_FIXES+=("$fix")
    case "$status" in
        fail) HARD_FAILS=$((HARD_FAILS + 1)) ;;
        warn) WARN_COUNT=$((WARN_COUNT + 1)) ;;
    esac
}

# ---- Helpers ----

need_cmd() {
    command -v "$1" >/dev/null 2>&1
}

script_ok() {
    local path="$1"
    [ -f "$path" ] && [ -r "$path" ]
}

help_exits_zero() {
    local script="$1"
    # Run --help with a timeout; no output to terminal.
    timeout 5 bash "$script" --help >/dev/null 2>&1
}

yaml_parses() {
    local path="$1"
    need_cmd python3 || return 2  # can't check; caller treats as warn
    python3 -c "import yaml,sys; yaml.safe_load(open(sys.argv[1]))" "$path" 2>/dev/null
}

# ============================================================================
# Group: toolchain (delegated to setup.sh --verify)
# ============================================================================

check_toolchain() {
    if ! script_ok "$SCRIPT_DIR/setup.sh"; then
        record toolchain fail "setup.sh missing" \
            "Cannot locate scripts/setup.sh" \
            "Check out the repo cleanly."
        return
    fi
    if timeout 30 bash "$SCRIPT_DIR/setup.sh" --verify >/dev/null 2>&1; then
        record toolchain pass "setup.sh --verify" \
            "All 14 sentinels present" \
            "-"
    else
        record toolchain fail "setup.sh --verify" \
            "One or more required tools missing" \
            "Run: bash scripts/setup.sh --verify   # to see the missing entries, then bash scripts/setup.sh to install them."
    fi
}

# ============================================================================
# Group: wrappers
# ============================================================================

check_wrappers() {
    local wrappers=(
        "build.sh"
        "test.sh"
        "lint.sh"
        "run-qemu.sh"
        "debug.sh"
        "test-smoke.sh"
        "install-hooks.sh"
        "setup.sh"
    )
    local w path
    for w in "${wrappers[@]}"; do
        path="$SCRIPT_DIR/$w"
        if ! script_ok "$path"; then
            record wrappers fail "$w" \
                "Missing or unreadable" \
                "Check out the repo cleanly or restore scripts/$w."
            continue
        fi
        if help_exits_zero "$path"; then
            record wrappers pass "$w" \
                "--help exits 0" \
                "-"
        else
            record wrappers warn "$w" \
                "--help did not exit 0 within 5s" \
                "Run: bash scripts/$w --help   # to see the failure output."
        fi
    done
}

# ============================================================================
# Group: hooks
# ============================================================================

check_hooks() {
    if ! script_ok "$SCRIPT_DIR/install-hooks.sh"; then
        record hooks fail "install-hooks.sh" \
            "Missing" \
            "Check out the repo cleanly."
        return
    fi
    local status_out
    status_out=$(bash "$SCRIPT_DIR/install-hooks.sh" --status 2>/dev/null || echo "")
    if [ -z "$status_out" ]; then
        record hooks fail "install-hooks.sh --status" \
            "install-hooks.sh --status produced no output" \
            "Run: bash scripts/install-hooks.sh --status"
        return
    fi
    # Parse: "Hooks path     : .githooks (expected: .githooks)"
    local configured
    configured=$(echo "$status_out" | awk -F': ' '/^Hooks path/{gsub(/ \(.*/,"",$2); print $2}')
    if [ "$configured" = ".githooks" ]; then
        record hooks pass "core.hooksPath" \
            "Set to .githooks" \
            "-"
    elif [ "$configured" = "<unset>" ] || [ -z "$configured" ]; then
        # Unset is a workstation concern only: CI / fresh clones run
        # wrappers directly and do not need git hooks. Warn (not fail) so
        # the doctor still exits 0 on CI while telling a developer the
        # exact fix. Hook FILE presence remains a hard fail below -- that
        # actually matters everywhere because the files are in .githooks/.
        record hooks warn "core.hooksPath" \
            "Unset; pre-commit + post-commit hooks will NOT fire (OK on CI and fresh clones; developers should configure)" \
            "Run: bash scripts/install-hooks.sh"
    else
        record hooks warn "core.hooksPath" \
            "Set to '$configured' instead of '.githooks'" \
            "Run: bash scripts/install-hooks.sh   # to restore the canonical path."
    fi

    # Parse pre-push gate
    local prepush_state
    prepush_state=$(echo "$status_out" | awk -F': ' '/^Pre-push gate/{print $2}')
    if [ -n "$prepush_state" ]; then
        record hooks pass "pre-push gate" \
            "$prepush_state" \
            "-"
    fi

    # Per-hook files exist
    local h
    for h in pre-commit post-commit pre-push; do
        if [ -x "$REPO_ROOT/.githooks/$h" ]; then
            record hooks pass ".githooks/$h" \
                "Present and executable" \
                "-"
        else
            record hooks fail ".githooks/$h" \
                "Missing or not executable" \
                "Check out the repo cleanly; do not chmod -x .githooks/*"
        fi
    done
}

# ============================================================================
# Group: workflows
# ============================================================================

check_workflows() {
    local workflows=(
        "build.yml"
        "release.yml"
        "pages.yml"
        "labeler.yml"
        "stale.yml"
    )
    local wf path
    for wf in "${workflows[@]}"; do
        path="$REPO_ROOT/.github/workflows/$wf"
        if [ ! -f "$path" ]; then
            record workflows fail "$wf" \
                "Missing" \
                "Check out the repo cleanly."
            continue
        fi
        if ! yaml_parses "$path"; then
            if ! need_cmd python3; then
                record workflows warn "$wf" \
                    "python3 missing; cannot parse YAML" \
                    "Install python3 or inspect .github/workflows/$wf manually."
            else
                record workflows fail "$wf" \
                    "YAML parse error" \
                    "Run: python3 -c 'import yaml; yaml.safe_load(open(\".github/workflows/$wf\"))'"
            fi
            continue
        fi
        record workflows pass "$wf" \
            "Parses as valid YAML" \
            "-"
    done

    # build.yml and release.yml must invoke the canonical wrappers
    local bld="$REPO_ROOT/.github/workflows/build.yml"
    if [ -f "$bld" ]; then
        if grep -q "bash scripts/build.sh" "$bld" && grep -q "bash scripts/test.sh" "$bld"; then
            record workflows pass "build.yml wrapper alignment" \
                "Invokes bash scripts/build.sh and bash scripts/test.sh" \
                "-"
        else
            record workflows fail "build.yml wrapper alignment" \
                "Missing bash scripts/build.sh and/or bash scripts/test.sh" \
                "See the GitHub Actions section of docs/infrastructure/development-tooling.md for the canonical step names."
        fi
    fi
}

# ============================================================================
# Group: runtime
# ============================================================================

check_runtime() {
    # KVM availability (Linux only)
    if [ "$(uname -s)" = "Linux" ]; then
        if [ -c /dev/kvm ]; then
            if [ -w /dev/kvm ]; then
                record runtime pass "/dev/kvm" \
                    "Present and writable; KVM acceleration available" \
                    "-"
            else
                record runtime warn "/dev/kvm" \
                    "Present but not writable by current user; falling back to TCG" \
                    "Run: sudo usermod -aG kvm \"\$USER\" && newgrp kvm   # to enable KVM for this user."
            fi
        else
            record runtime warn "/dev/kvm" \
                "Not present; QEMU will run under TCG (slower, still functional)" \
                "Install a KVM-capable kernel and enable the kvm module. Optional."
        fi
    else
        record runtime skip "/dev/kvm" \
            "Non-Linux host; KVM not applicable" \
            "-"
    fi

    # qemu-img (needed by release.yml VDI conversion)
    if need_cmd qemu-img; then
        record runtime pass "qemu-img" \
            "Installed" \
            "-"
    else
        record runtime warn "qemu-img" \
            "Not installed; release.yml VDI conversion would fail locally" \
            "Install qemu-utils (Debian/Ubuntu) or qemu-img (Fedora/Arch)."
    fi

    # VBoxManage (optional; only needed for VirtualBox scenarios)
    if need_cmd VBoxManage; then
        record runtime pass "VBoxManage" \
            "Installed (VirtualBox scenarios available)" \
            "-"
    else
        record runtime skip "VBoxManage" \
            "Not installed; VirtualBox scenarios skipped. Optional." \
            "-"
    fi
}

check_host_profile() {
    if [ ! -r /etc/os-release ]; then
        record runtime warn "host profile" \
            "/etc/os-release not readable; cannot classify host" \
            "Check the developer tooling supported-host profile matrix manually."
        return
    fi
    local os_id os_version_id
    # shellcheck disable=SC1091
    os_id=$(. /etc/os-release && echo "${ID:-unknown}")
    os_version_id=$(. /etc/os-release && echo "${VERSION_ID:-}")
    local is_wsl2=0
    if [ -r /proc/sys/kernel/osrelease ] && grep -qi microsoft /proc/sys/kernel/osrelease 2>/dev/null; then
        is_wsl2=1
    fi

    local tier msg
    case "$os_id" in
        ubuntu|debian)
            tier="fully supported"
            if [ "$is_wsl2" = "1" ]; then
                msg="WSL2 + $os_id $os_version_id"
            else
                msg="native $os_id $os_version_id"
            fi
            ;;
        fedora|arch)
            tier="best-effort"
            msg="native $os_id $os_version_id (LLVM-19 + OVMF shim required; see supported-host profile matrix)"
            ;;
        *)
            tier="unsupported-or-unclassified"
            msg="$os_id $os_version_id"
            ;;
    esac

    if [ "$tier" = "fully supported" ]; then
        record runtime pass "host profile" "$msg ($tier)" "-"
    elif [ "$tier" = "best-effort" ]; then
        record runtime warn "host profile" "$msg ($tier)" \
            "See the supported-host profile matrix for the shim procedure."
    else
        record runtime warn "host profile" "$msg ($tier)" \
            "Use WSL2 + Ubuntu/Debian or a supported Linux host for the fully-supported path."
    fi
}

# ============================================================================
# Group: docs
# ============================================================================

check_docs() {
    local doc="$REPO_ROOT/docs/infrastructure/development-tooling.md"
    if [ ! -f "$doc" ]; then
        record docs fail "development-tooling.md" \
            "Missing" \
            "Check out the repo cleanly."
        return
    fi
    local anchor expected_anchors=(
        "host-bootstrap-contract"
        "wrapper-contract"
        "local-ci-hooks"
        "github-actions-workflows"
        "supported-host-profiles-and-reproducible-environments"
    )
    for anchor in "${expected_anchors[@]}"; do
        # Convert anchor to its heading-slug source; headings are normalized by
        # GitHub Pages from `##`/`###` titles, so a grep for the surface title
        # is sufficient as a sanity check.
        if grep -qiE "^#{2,} .*${anchor//-/[- ]}" "$doc" 2>/dev/null; then
            record docs pass "anchor: $anchor" "Resolves in development-tooling.md" "-"
        else
            record docs warn "anchor: $anchor" \
                "Could not find a matching heading in development-tooling.md" \
                "Either the anchor moved or the heading was renamed; update the doctor script or the doc."
        fi
    done

    # machine-matrix.md and boot-info-fields.md are recent additions; check existence
    if [ -f "$REPO_ROOT/docs/infrastructure/machine-matrix.md" ]; then
        record docs pass "machine-matrix.md" "Present" "-"
    else
        record docs warn "machine-matrix.md" \
            "Missing" \
            "docs/infrastructure/machine-matrix.md is referenced by the tooling contract; restore it."
    fi
    if [ -f "$REPO_ROOT/docs/boot/boot-info-fields.md" ]; then
        record docs pass "boot-info-fields.md" "Present" "-"
    else
        record docs warn "boot-info-fields.md" \
            "Missing" \
            "docs/boot/boot-info-fields.md is the canonical boot_info field matrix; restore it."
    fi
}

# ============================================================================
# Run all checks
# ============================================================================

check_toolchain
check_wrappers
check_hooks
check_workflows
check_runtime
check_host_profile
check_docs

# ============================================================================
# Output
# ============================================================================

emit_json() {
    local n="${#CHECK_GROUPS[@]}"
    if ! need_cmd python3; then
        # Minimal fallback: structured summary only; fields need python3.
        echo '{"summary":{"hard_fails":'"$HARD_FAILS"',"warnings":'"$WARN_COUNT"',"checks":'"$n"',"ok":'"$([ "$HARD_FAILS" = "0" ] && echo true || echo false)"'},"checks":[],"note":"python3 not available; install python3 for full JSON output"}'
        return
    fi
    # Write records to a NUL-delimited tempfile so record contents can
    # legitimately contain `|` or newlines without corrupting the JSON.
    # Cannot pipe stdin to python3 directly because the `<<'PYEOF'` heredoc
    # already provides the interpreter's stdin; use a tempfile argv slot
    # instead.
    local tmp i
    tmp="$(mktemp 2>/dev/null)"
    if [ -z "$tmp" ] || [ ! -f "$tmp" ]; then
        echo '{"error":"tempfile creation failed"}'
        return
    fi
    # Ensure the tempfile is cleaned up when the script exits, regardless
    # of how emit_json returns.
    # shellcheck disable=SC2064
    trap "rm -f '$tmp'" EXIT
    for ((i = 0; i < n; i++)); do
        printf '%s\0%s\0%s\0%s\0%s\0' \
            "${CHECK_GROUPS[$i]}" \
            "${CHECK_STATUSES[$i]}" \
            "${CHECK_NAMES[$i]}" \
            "${CHECK_DETAILS[$i]}" \
            "${CHECK_FIXES[$i]}" >> "$tmp"
    done
    python3 - "$HARD_FAILS" "$WARN_COUNT" "$n" "$tmp" <<'PYEOF'
import json, sys
hard_fails = int(sys.argv[1])
warns = int(sys.argv[2])
n = int(sys.argv[3])
tmpfile = sys.argv[4]
with open(tmpfile, "rb") as f:
    raw = f.read().decode("utf-8", errors="replace")
parts = raw.split("\x00")
out = {
    "summary": {
        "hard_fails": hard_fails,
        "warnings": warns,
        "checks": n,
        "ok": hard_fails == 0,
    },
    "checks": [],
}
for i in range(n):
    base = i * 5
    group, status, name, detail, fix = parts[base:base + 5]
    out["checks"].append({
        "group": group,
        "status": status,
        "name": name,
        "detail": detail,
        "fix": fix,
    })
print(json.dumps(out, indent=2))
PYEOF
}

emit_prose() {
    echo ""
    echo -e "${CYAN}══════════════════════════════════════════════════${NC}"
    echo -e "${CYAN}  Impossible OS -- Tooling Doctor${NC}"
    echo -e "${CYAN}══════════════════════════════════════════════════${NC}"
    echo ""
    local last_group=""
    local i n="${#CHECK_GROUPS[@]}"
    local group status name detail fix
    for ((i = 0; i < n; i++)); do
        group="${CHECK_GROUPS[$i]}"
        status="${CHECK_STATUSES[$i]}"
        name="${CHECK_NAMES[$i]}"
        detail="${CHECK_DETAILS[$i]}"
        fix="${CHECK_FIXES[$i]}"
        if [ "$group" != "$last_group" ]; then
            echo ""
            echo -e "${DIM}[$group]${NC}"
            last_group="$group"
        fi
        case "$status" in
            pass) echo -e "  ${GREEN}PASS${NC}  ${name}: ${detail}" ;;
            warn) echo -e "  ${YELLOW}WARN${NC}  ${name}: ${detail}"
                  [ "$fix" != "-" ] && echo -e "        ${DIM}fix:${NC} $fix" ;;
            fail) echo -e "  ${RED}FAIL${NC}  ${name}: ${detail}"
                  [ "$fix" != "-" ] && echo -e "        ${DIM}fix:${NC} $fix" ;;
            skip) echo -e "  ${DIM}skip${NC}  ${name}: ${detail}" ;;
        esac
    done
    echo ""
    echo -e "${CYAN}══════════════════════════════════════════════════${NC}"
    if [ "$HARD_FAILS" = "0" ]; then
        echo -e "  ${GREEN}TOOLING HEALTHY${NC} (${n} checks, ${WARN_COUNT} warning(s))"
    else
        echo -e "  ${RED}TOOLING DOCTOR FOUND ${HARD_FAILS} FAILURE(S)${NC} (${n} checks, ${WARN_COUNT} warning(s))"
    fi
    echo -e "${CYAN}══════════════════════════════════════════════════${NC}"
}

emit_quiet() {
    local n="${#CHECK_GROUPS[@]}"
    if [ "$HARD_FAILS" = "0" ]; then
        echo "tooling-doctor: HEALTHY (${n} checks, ${WARN_COUNT} warning(s))"
    else
        echo "tooling-doctor: ${HARD_FAILS} FAILURE(S) (${n} checks, ${WARN_COUNT} warning(s))"
    fi
}

case "$MODE" in
    json)  emit_json ;;
    quiet) emit_quiet ;;
    *)     emit_prose ;;
esac

[ "$HARD_FAILS" = "0" ] && exit 0 || exit 1
