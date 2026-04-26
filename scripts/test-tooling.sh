#!/usr/bin/env bash
# ============================================================================
# test-tooling.sh -- Host-side regression pack for developer wrappers.
#
# Exercises the stable surface of scripts/{build,test,lint,run-qemu,debug,
# test-smoke,install-hooks,setup,tooling-doctor}.sh and .github/workflows/*
# without actually building the OS or running the kernel test suite.
# Catches drift that tooling-doctor.sh cannot: argument parsing behavior,
# sentinel path strings, hook install/remove idempotence against a throwaway
# git repo, workflow YAML references to the canonical wrappers.
#
# Usage:
#   bash scripts/test-tooling.sh              Run the full regression pack
#   bash scripts/test-tooling.sh --quiet      Summary line only
#   bash scripts/test-tooling.sh --help
#
# Exit codes:
#   0 = all tests passed
#   1 = one or more tests failed
# ============================================================================

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

QUIET=0
for arg in "$@"; do
    case "$arg" in
        -h|--help)
            cat <<'EOF'
test-tooling.sh -- developer tooling regression pack

Usage:
  bash scripts/test-tooling.sh              Run the full regression pack
  bash scripts/test-tooling.sh --quiet      Summary line only
  bash scripts/test-tooling.sh --help

What this tests:
  - Every wrapper script (build/test/lint/run-qemu/debug/test-smoke/
    install-hooks/setup/tooling-doctor) has a --help that exits 0
  - bash scripts/tooling-doctor.sh --quiet exits 0 on this host
  - bash scripts/setup.sh --verify exits 0 (toolchain sentinels)
  - bash scripts/lint.sh --help names 5 checks (matches current lint surface)
  - install-hooks.sh idempotent install / --status / --enable-pre-push /
    --disable-pre-push / --remove in a throwaway git repo
  - .github/workflows/{build,release,pages,labeler,stale}.yml parse as
    valid YAML
  - build.yml and release.yml reference bash scripts/build.sh and
    bash scripts/test.sh (wrapper alignment)
  - .githooks/{pre-commit,post-commit,pre-push} are all executable
  - CLAUDE.md and README.md mention the wrappers they should mention

Exit codes:
  0 = all tests passed
  1 = one or more tests failed
EOF
            exit 0
            ;;
        --quiet) QUIET=1 ;;
        *)
            echo "Unknown option: $arg" >&2
            exit 1
            ;;
    esac
done

# ---- Colors ----
if [ "$QUIET" = "0" ] && [ -t 1 ]; then
    RED='\033[0;31m'; GREEN='\033[0;32m'; CYAN='\033[0;36m'; DIM='\033[0;90m'; NC='\033[0m'
else
    RED=''; GREEN=''; CYAN=''; DIM=''; NC=''
fi

# ---- Test runner ----
PASS=0
FAIL=0
FAILURES=()

t_pass() {
    PASS=$((PASS + 1))
    [ "$QUIET" = "0" ] && echo -e "  ${GREEN}PASS${NC}  $1"
}

t_fail() {
    FAIL=$((FAIL + 1))
    FAILURES+=("$1")
    [ "$QUIET" = "0" ] && echo -e "  ${RED}FAIL${NC}  $1"
    [ -n "${2:-}" ] && [ "$QUIET" = "0" ] && echo -e "        ${DIM}${2}${NC}"
}

assert_exit_zero() {
    local desc="$1"; shift
    local output
    output=$("$@" 2>&1)
    local rc=$?
    if [ "$rc" = "0" ]; then
        t_pass "$desc"
    else
        t_fail "$desc" "exit $rc; last output line: $(echo "$output" | tail -1)"
    fi
}

assert_grep() {
    local desc="$1" file="$2" pattern="$3"
    if [ ! -f "$file" ]; then
        t_fail "$desc" "file missing: $file"
        return
    fi
    if grep -qE "$pattern" "$file" 2>/dev/null; then
        t_pass "$desc"
    else
        t_fail "$desc" "pattern not found in $file: $pattern"
    fi
}

# ============================================================================
# Wrapper --help contract
# ============================================================================

[ "$QUIET" = "0" ] && echo -e "${DIM}[wrapper --help contract]${NC}"
for w in build.sh test.sh lint.sh run-qemu.sh debug.sh test-smoke.sh install-hooks.sh setup.sh tooling-doctor.sh; do
    path="$SCRIPT_DIR/$w"
    if [ ! -f "$path" ]; then
        t_fail "scripts/$w present" "file missing"
        continue
    fi
    if timeout 10 bash "$path" --help >/dev/null 2>&1; then
        t_pass "scripts/$w --help exits 0"
    else
        t_fail "scripts/$w --help exits 0"
    fi
done

# ============================================================================
# Doctor + setup invariants
# ============================================================================

[ "$QUIET" = "0" ] && echo "" && echo -e "${DIM}[doctor + setup invariants]${NC}"
assert_exit_zero "tooling-doctor --quiet exit 0" bash "$SCRIPT_DIR/tooling-doctor.sh" --quiet
assert_exit_zero "setup --verify exit 0" bash "$SCRIPT_DIR/setup.sh" --verify
if bash "$SCRIPT_DIR/lint.sh" --help 2>/dev/null | grep -qE 'Checks \(5\)'; then
    t_pass "lint --help names 5 checks (post section-ref gate)"
else
    t_fail "lint --help names 5 checks (post section-ref gate)" \
        "expected 'Checks (5)' in --help output; if a check was added or removed, update this assertion AND lint.sh's help text together"
fi

# ============================================================================
# Hook lifecycle in a throwaway git repo
# ============================================================================

[ "$QUIET" = "0" ] && echo "" && echo -e "${DIM}[hook lifecycle]${NC}"

# Create a per-run throwaway tempdir. If mktemp fails (read-only FS, quota,
# missing /tmp), FAIL the hook lifecycle group immediately -- do NOT fall
# through silently. Keep results inside the tempdir so parallel test runs
# do not stomp on a shared /tmp file.
TMPDIR_BASE="${TMPDIR:-/tmp}"
TMPDIR_HOOK=""
if ! TMPDIR_HOOK="$(mktemp -d "$TMPDIR_BASE/impossible-tooling-XXXXXX" 2>/dev/null)"; then
    t_fail "hook lifecycle setup" "mktemp -d failed under $TMPDIR_BASE (read-only FS, quota, or /tmp unavailable?)"
    TMPDIR_HOOK=""
fi

# Safety: only install the cleanup trap when we actually have a throwaway
# path under the expected base. Prevents rm -rf against an empty or wrong
# variable if mktemp partially succeeded.
if [ -n "$TMPDIR_HOOK" ] && [ -d "$TMPDIR_HOOK" ] && \
   case "$TMPDIR_HOOK" in "$TMPDIR_BASE"/impossible-tooling-*) true ;; *) false ;; esac
then
    trap 'rm -rf "$TMPDIR_HOOK"' EXIT INT TERM
    HOOK_RESULTS="$TMPDIR_HOOK/results.txt"

    # Run the hook lifecycle subshell under `set -e` so a failed cd / git
    # init / symlink aborts the check block instead of leaking into the
    # next assertion. Capture the subshell exit code explicitly; a non-zero
    # exit means setup itself broke and we must surface that as a test
    # failure, not as "no assertions recorded".
    (
        set -e
        cd "$TMPDIR_HOOK"
        git init -q
        # Symlink the repo's .githooks/ and scripts/ into the tempdir so
        # install-hooks.sh operates on a throwaway git config without
        # touching the real repo's core.hooksPath.
        ln -s "$REPO_ROOT/.githooks" .githooks
        ln -s "$REPO_ROOT/scripts" scripts

        # Default install: sets core.hooksPath, idempotent second run.
        if bash scripts/install-hooks.sh >/dev/null 2>&1 \
           && bash scripts/install-hooks.sh >/dev/null 2>&1
        then
            echo "ok first-install-idempotent"
        else
            echo "fail first-install-idempotent"
        fi

        configured="$(git config --get core.hooksPath 2>/dev/null || echo unset)"
        if [ "$configured" = ".githooks" ]; then
            echo "ok core.hooksPath set"
        else
            echo "fail core.hooksPath got '$configured'"
        fi

        # Enable pre-push -> sentinel exists -> status reports ENABLED.
        bash scripts/install-hooks.sh --enable-pre-push >/dev/null 2>&1 || true
        sentinel="$(git rev-parse --git-path .impossible-os-prepush 2>/dev/null || echo "")"
        if [ -n "$sentinel" ] && [ -f "$sentinel" ]; then
            echo "ok pre-push sentinel present"
        else
            echo "fail pre-push sentinel missing"
        fi
        if bash scripts/install-hooks.sh --status 2>&1 | grep -q "ENABLED"; then
            echo "ok pre-push status ENABLED"
        else
            echo "fail pre-push status not ENABLED"
        fi

        # Disable pre-push -> sentinel gone.
        bash scripts/install-hooks.sh --disable-pre-push >/dev/null 2>&1 || true
        if [ -z "$sentinel" ] || [ ! -f "$sentinel" ]; then
            echo "ok pre-push sentinel removed"
        else
            echo "fail pre-push sentinel still present"
        fi

        # --remove: unset hooksPath.
        bash scripts/install-hooks.sh --remove >/dev/null 2>&1 || true
        configured_after="$(git config --get core.hooksPath 2>/dev/null || echo unset)"
        if [ "$configured_after" = "unset" ]; then
            echo "ok --remove clears core.hooksPath"
        else
            echo "fail --remove left core.hooksPath='$configured_after'"
        fi
    ) > "$HOOK_RESULTS" 2>&1
    hook_subshell_rc=$?

    # If the subshell aborted before any assertion ran, the results file
    # is empty or missing. Surface that as a hard failure rather than a
    # silent pass.
    if [ ! -s "$HOOK_RESULTS" ]; then
        t_fail "hook lifecycle setup" "subshell produced no output (rc=$hook_subshell_rc); setup likely failed before assertions ran"
    else
        while IFS= read -r line; do
            case "$line" in
                ok*)   t_pass "${line#ok }" ;;
                fail*) t_fail "${line#fail }" ;;
            esac
        done < "$HOOK_RESULTS"
        if [ "$hook_subshell_rc" -ne 0 ]; then
            t_fail "hook lifecycle subshell exit" "subshell exited rc=$hook_subshell_rc after assertions; partial failure"
        fi
    fi
fi

# ============================================================================
# Workflow YAML sanity
# ============================================================================

[ "$QUIET" = "0" ] && echo "" && echo -e "${DIM}[workflow YAML]${NC}"
if ! command -v python3 >/dev/null 2>&1; then
    t_fail "python3 available for YAML parse" "install python3"
else
    for wf in build.yml release.yml pages.yml labeler.yml stale.yml; do
        path="$REPO_ROOT/.github/workflows/$wf"
        if [ ! -f "$path" ]; then
            t_fail ".github/workflows/$wf exists"
            continue
        fi
        if python3 -c "import yaml; yaml.safe_load(open('$path'))" 2>/dev/null; then
            t_pass ".github/workflows/$wf parses as YAML"
        else
            t_fail ".github/workflows/$wf parses as YAML"
        fi
    done

    # Wrapper alignment: build.yml + release.yml invoke the canonical
    # wrappers. Absence here means CI and local flow have diverged.
    for wf in build.yml release.yml; do
        path="$REPO_ROOT/.github/workflows/$wf"
        assert_grep "$wf invokes bash scripts/build.sh" "$path" 'bash scripts/build.sh'
        assert_grep "$wf invokes bash scripts/test.sh" "$path" 'bash scripts/test.sh'
        assert_grep "$wf invokes bash scripts/setup.sh --verify" "$path" 'bash scripts/setup.sh --verify'
    done

    # Maintenance / deployment workflows (pages, labeler, stale) are not
    # build-participants so they don't need to invoke build.sh / test.sh,
    # but they MUST parse as YAML (checked above) and MUST NOT reference
    # the legacy ISO / make-test path that the legacy-XREF sweep and
    # the wrapper contract retired.
    for wf in pages.yml labeler.yml stale.yml; do
        path="$REPO_ROOT/.github/workflows/$wf"
        for stale in 'make test' 'run-tests\.sh' 'build/os-build\.iso'; do
            if grep -qE "$stale" "$path" 2>/dev/null; then
                t_fail "$wf rejects stale '$stale'" \
                    "found a reference to $stale; + retired this; use the canonical wrapper path"
            else
                t_pass "$wf rejects stale '$stale'"
            fi
        done
    done
fi

# ============================================================================
# Supported-host profile + reproducible env + unsupported-host fallback
# ============================================================================
# The development-tooling.md is the single source of truth for which hosts
# are supported and which require a shim. A promotion/demotion in that
# table must be reflected in setup-deps.sh (distro install paths), the
# wrapper contract doc section, and this regression. If setup-deps.sh ever
# stops matching the profile matrix, new contributors on a best-effort
# profile hit a silent install failure instead of the documented shim.

[ "$QUIET" = "0" ] && echo "" && echo -e "${DIM}[supported-host profile + repro-env]${NC}"
DEV_TOOLING_MD="$REPO_ROOT/docs/infrastructure/development-tooling.md"
# Profile matrix must name every distro we claim to support or explicitly
# reject. Missing row => silent support promise.
for profile in 'Ubuntu/Debian' 'WSL2' 'devcontainer' 'Fedora' 'Arch' 'Native Windows' 'macOS'; do
    assert_grep "dev-tooling.md names '$profile' in profile matrix" \
        "$DEV_TOOLING_MD" "$profile"
done
# Unsupported-host policy paragraph must be explicit, not a silent omission.
assert_grep "dev-tooling.md explicitly marks macOS Unsupported" \
    "$DEV_TOOLING_MD" 'macOS[[:space:]]+.*(Unsupported|❌)'
assert_grep "dev-tooling.md explicitly marks native Windows Unsupported" \
    "$DEV_TOOLING_MD" 'Native Windows.*(Unsupported|❌)'
assert_grep "dev-tooling.md names WSL2 as the Windows fallback" \
    "$DEV_TOOLING_MD" 'WSL2[[:space:]]+(instead|Ubuntu)'
# Reproducible environment: .devcontainer file must exist and be referenced
# from the doc. Either half missing = broken reproducibility claim.
if [ -f "$REPO_ROOT/.devcontainer/devcontainer.json" ]; then
    t_pass ".devcontainer/devcontainer.json present"
else
    t_fail ".devcontainer/devcontainer.json present" \
        "reproducible-environment claim in dev-tooling.md has no matching file"
fi
assert_grep "dev-tooling.md references .devcontainer" \
    "$DEV_TOOLING_MD" '\.devcontainer'
# Fedora/Arch best-effort path must have the shim procedure documented,
# or best-effort users hit a silent --verify failure.
assert_grep "dev-tooling.md documents Fedora/Arch shim procedure" \
    "$DEV_TOOLING_MD" 'Fedora.*Arch.*Shim|Shim.*Procedure'

# ============================================================================
# Stale-command rejection (wrapper contract + legacy-XREF sweep)
# ============================================================================
# After codified `bash scripts/build.sh` / `bash scripts/test.sh` as the
# canonical surface, and drained the legacy numeric-TODO shorthand, a
# regression could easily re-introduce `make test` / `run-tests.sh` /
# `build/os-build.iso` into a doc or README as if it were a primary
# command. This group ensures those strings never reappear as primary
# wrapper calls in the operator-facing docs. They remain allowed in
# explicit legacy markers (e.g. the Makefile's `iso:` target, or a doc
# paragraph explicitly titled "Legacy" or "Historical").

[ "$QUIET" = "0" ] && echo "" && echo -e "${DIM}[stale-command rejection in operator docs]${NC}"
# Files that speak to operators/contributors. If any of them re-adopts a
# stale primary command, a new contributor will follow a broken path.
OPERATOR_DOCS=(
    "$REPO_ROOT/README.md"
    "$REPO_ROOT/CONTRIBUTING.md"
    "$REPO_ROOT/CLAUDE.md"
    "$REPO_ROOT/docs/infrastructure/development-tooling.md"
)
# Forbidden primary-command patterns. These catch `make test`, `run-tests.sh`,
# `build/os-build.iso` UNLESS the line is explicitly marked legacy (contains
# the word "legacy" or "Legacy" or "retired" or "deprecated" on the same
# line) or is inside a fenced doc block pinned as a historical reference.
# grep -vE filters out legacy-marked lines, so a doc describing the retired
# ISO path with "(legacy, retired)" remains acceptable.
for doc in "${OPERATOR_DOCS[@]}"; do
    if [ ! -f "$doc" ]; then
        t_fail "operator doc present: $(basename "$doc")"
        continue
    fi
    # `make test` as a primary recommendation. Allow:
    #   - `make test-<something>` (test-mm, test-fs, test-tooling, etc.) --
    #     legitimate category-filter shorthand
    #   - `make test*` (with literal star, used in docs to describe the
    #     shorthand family as a whole)
    #   - lines explicitly describing delegation / passthrough / shorthand
    #     relationship to `bash scripts/test.sh` (the doc's job is to
    #     explain that `make test` forms exist and delegate)
    #   - lines flagged as legacy / retired / deprecated / historical
    # The word boundary is "non-identifier char" on both sides, so
    # `make test.` / `make test\`` / `make test ` all match; `make test-mm`
    # (hyphen is in the exclusion set) does not.
    if grep -nE '(^|[^a-zA-Z])make[[:space:]]+test([^a-zA-Z0-9_-])' "$doc" 2>/dev/null \
        | grep -viE 'legacy|retired|deprecated|historical|bash scripts/test\.sh|make test\*|shorthand|passthrough|delegates?' >/dev/null; then
        t_fail "$(basename "$doc") rejects stale 'make test' as primary" \
            "found 'make test' not marked legacy; use 'bash scripts/test.sh' per "
    else
        t_pass "$(basename "$doc") rejects stale 'make test' as primary"
    fi
    # `run-tests.sh` was retired in. Any mention outside a legacy marker
    # means the operator will follow a dead path.
    if grep -nE 'run-tests\.sh' "$doc" 2>/dev/null \
        | grep -viE 'legacy|retired|deprecated|historical' >/dev/null; then
        t_fail "$(basename "$doc") rejects stale 'run-tests.sh'" \
            "found 'run-tests.sh' not marked legacy; script was retired in "
    else
        t_pass "$(basename "$doc") rejects stale 'run-tests.sh'"
    fi
    # `build/os-build.iso` was retired in. The GPT disk image
    # `build/system-disk.img` replaced it.
    if grep -nE 'build/os-build\.iso' "$doc" 2>/dev/null \
        | grep -viE 'legacy|retired|deprecated|historical' >/dev/null; then
        t_fail "$(basename "$doc") rejects stale 'build/os-build.iso'" \
            "found 'build/os-build.iso' not marked legacy; use 'build/system-disk.img' per "
    else
        t_pass "$(basename "$doc") rejects stale 'build/os-build.iso'"
    fi
done

# ============================================================================
# boot_info ABI drift-detection harness surface contract
# ============================================================================
# Surface-only checks; the harness itself compiles fixtures and needs a
# fresh kernel manifest, so it runs post-build (make test-boot-info-abi /
# GitHub Actions "Run boot_info ABI drift tests" step), not here.

[ "$QUIET" = "0" ] && echo "" && echo -e "${DIM}[boot_info ABI drift harness]${NC}"
DRIFT_SH="$REPO_ROOT/tools/boot-info-manifest/test-drift-detection.sh"
if [ -x "$DRIFT_SH" ]; then
    t_pass "test-drift-detection.sh executable"
else
    t_fail "test-drift-detection.sh executable" "missing or not chmod +x: $DRIFT_SH"
fi
assert_exit_zero "test-drift-detection.sh --help exits 0" bash "$DRIFT_SH" --help
# The harness must document the exit-code contract in its --help so
# humans and CI readers can parse it without reading the source.
if bash "$DRIFT_SH" --help 2>/dev/null | grep -qE 'Exit codes:'; then
    t_pass "test-drift-detection.sh --help documents exit codes"
else
    t_fail "test-drift-detection.sh --help documents exit codes" \
        "expected 'Exit codes:' block in --help output"
fi
# Makefile exposes the harness via a .PHONY target that CI and developers
# can call after a build. A missing target means the test is orphaned.
assert_grep "Makefile defines test-boot-info-abi target" \
    "$REPO_ROOT/Makefile" '^test-boot-info-abi:'
assert_grep "Makefile lists test-boot-info-abi as .PHONY" \
    "$REPO_ROOT/Makefile" '\.PHONY:.*test-boot-info-abi'

# ============================================================================
# .githooks/ file presence
# ============================================================================

[ "$QUIET" = "0" ] && echo "" && echo -e "${DIM}[.githooks/ present]${NC}"
for h in pre-commit post-commit pre-push; do
    path="$REPO_ROOT/.githooks/$h"
    if [ -x "$path" ]; then
        t_pass ".githooks/$h executable"
    else
        t_fail ".githooks/$h executable" "missing or not executable"
    fi
done

# ============================================================================
# Doc mention contract
# ============================================================================

[ "$QUIET" = "0" ] && echo "" && echo -e "${DIM}[doc mentions]${NC}"
assert_grep "CLAUDE.md mentions install-hooks.sh" \
    "$REPO_ROOT/CLAUDE.md" 'install-hooks\.sh'
assert_grep "README.md mentions install-hooks.sh" \
    "$REPO_ROOT/README.md" 'install-hooks\.sh'
assert_grep "development-tooling.md mentions tooling-doctor" \
    "$REPO_ROOT/docs/infrastructure/development-tooling.md" 'tooling-doctor'

# ============================================================================
# Build / test sentinel contract (surface-only; full runs in CI)
# ============================================================================
# The TODO's Unit Tests and bullets assert:
#   - bash scripts/build.sh clean writes build/build.log; tail -1 equals
#     '=== BUILD OK ==='
#   - bash scripts/test.sh QUIET=1 keeps summary path, suppresses per-test
#     PASS lines
# Running these from this regression pack would take ~10 seconds (build) +
# ~15 seconds (tests) per invocation, which crosses the "lightweight
# regression" bar this script is sized for. They are validated by:
#   - scripts/build.sh's own tail -1 check (writes === BUILD OK === and
#     documents it in --help; any future commit that breaks the sentinel
#     is caught by CI's build.yml or the smoke test).
#   - scripts/test.sh's QUIET_MODE flag handling is checked via --help
#     text grep below (surface contract); the per-test suppression
#     behavior is covered end-to-end by CI running the full suite.
# This section is a STATIC contract check, not a runtime exercise.

[ "$QUIET" = "0" ] && echo "" && echo -e "${DIM}[build / test sentinel surface]${NC}"
assert_grep "build.sh documents '=== BUILD OK ===' sentinel" \
    "$SCRIPT_DIR/build.sh" '=== BUILD OK ==='
assert_grep "build.sh --help advertises the sentinel to operators" \
    "$SCRIPT_DIR/build.sh" 'tail -1 build/build\.log'
assert_grep "test.sh advertises QUIET=1 in --help" \
    "$SCRIPT_DIR/test.sh" 'QUIET=1'
assert_grep "test.sh parses QUIET_MODE from args" \
    "$SCRIPT_DIR/test.sh" 'QUIET=1\).*QUIET_MODE=1'
# Codex-2026-04-18: it's not enough to PARSE the QUIET flag -- the
# per-test result loop must actually GATE its [ OK ] output on it, or a
# silent boot.conf-patching failure (kernel still emits TEST: lines)
# defeats the --help contract "summary only". Assert the gate on the
# host-side loop, not just the flag parse.
assert_grep "test.sh gates per-test OK output on QUIET_MODE (host layer)" \
    "$SCRIPT_DIR/test.sh" 'QUIET_MODE"? -eq 0 ?\]'
# Kernel-side gate too: test_runner.c's g_quiet flag must guard the PASS
# emissions. If someone rewrites the runner and drops the g_quiet check,
# QUIET=1 becomes a lie at the source.
assert_grep "test_runner.c implements g_quiet gate (kernel layer)" \
    "$REPO_ROOT/src/kernel/test/test_runner.c" 'g_quiet'
# Confirm the wrappers actually wire the sentinel write, not just
# advertise it. A commit that replaces the sentinel with a different
# string would silently pass CI's exit-code check but break the
# operator-visible contract.
assert_grep "build.sh WRITES the sentinel (not just mentions it)" \
    "$SCRIPT_DIR/build.sh" 'echo "=== BUILD OK ===" >> .*LOG'

# ============================================================================
# todo-graph regression suite
# ============================================================================
#
# scripts/todo-graph/build.py is host-side Python tooling owned by the
# TODO-metadata-layer plan in todo/00-infrastructure/. The dedicated
# regression suite (test_build.sh) lives next to the script and exercises
# the generator against the live todo/ tree + a synthetic malformed
# fixture. Shell out to it as a single combined check so failures bubble
# into this aggregate runner without re-implementing each assertion.
[ "$QUIET" = "0" ] && echo "" && echo -e "${DIM}[todo-graph]${NC}"
TODO_GRAPH_TEST="$REPO_ROOT/scripts/todo-graph/tests/test_build.sh"
if [ -x "$TODO_GRAPH_TEST" ]; then
    # Capture the "[test_build] N/N passed, 0 failed" summary so the
    # aggregate runner's message tracks the current sub-test count
    # instead of drifting (prior hard-coded "37 sub-tests" went stale
    # as -+ review passes added more sub-tests through 105+).
    TG_OUT=$("$TODO_GRAPH_TEST" 2>&1)
    TG_RC=$?
    TG_SUMMARY=$(printf '%s\n' "$TG_OUT" | grep -E '^\[test_build\] ' | tail -1)
    if [ "$TG_RC" = "0" ]; then
        t_pass "scripts/todo-graph/tests/test_build.sh PASS (${TG_SUMMARY:-summary unavailable})"
    else
        t_fail "scripts/todo-graph/tests/test_build.sh FAIL (${TG_SUMMARY:-run directly for details})"
    fi
else
    t_fail "scripts/todo-graph/tests/test_build.sh not found or not executable"
fi

# ============================================================================
# LSP-MCP bridge test harness wiring (TODO-07 in 00-infrastructure)
#
# scripts/lsp-mcp/bridge.py is the FastMCP stdio server proxying five
# language servers (clangd / asm-lsp / bash-lsp / pyright / PSES). The
# harness at scripts/lsp-mcp/tests/test_bridge.sh runs N host-side
# sub-tests (see file header for the running list); each per-language
# sub-test gracefully SKIPs when the corresponding LSP binary is not
# installed, so the harness exits 0 in CI even with only clangd-19
# available. Same shell-out-and-aggregate pattern as todo-graph above.
# scripts/lsp-mcp/tests/test_boundary.sh is the standalone read-only
# boundary audit; sub-test 7f wraps it, but we ALSO run it standalone
# so a future test_bridge.sh refactor cannot accidentally bypass the
# audit gate.
#
# CI deduplication: build.yml runs both harnesses again as a standalone
# step for log placement, so it sets TEST_TOOLING_SKIP_LSP_MCP=1 when
# invoking this script to avoid running the harness twice per build (one
# clangd cold-spawn + one stress pass = ~tens of seconds saved). Local
# developers running the script directly still get the full surface.
if [ "${TEST_TOOLING_SKIP_LSP_MCP:-0}" = "1" ]; then
    [ "$QUIET" = "0" ] && echo "" && echo -e "${DIM}[lsp-mcp]${NC}"
    t_pass "lsp-mcp harness skipped (TEST_TOOLING_SKIP_LSP_MCP=1; run directly via test_bridge.sh + test_boundary.sh)"
else
    [ "$QUIET" = "0" ] && echo "" && echo -e "${DIM}[lsp-mcp]${NC}"
    LSP_MCP_TEST="$REPO_ROOT/scripts/lsp-mcp/tests/test_bridge.sh"
    if [ -x "$LSP_MCP_TEST" ]; then
        LM_OUT=$("$LSP_MCP_TEST" 2>&1)
        LM_RC=$?
        LM_SUMMARY=$(printf '%s\n' "$LM_OUT" | grep -E '^\[lsp-mcp-tests\] [0-9]+/[0-9]+ sub-tests PASS$' | tail -1)
        if [ "$LM_RC" = "0" ]; then
            t_pass "scripts/lsp-mcp/tests/test_bridge.sh PASS (${LM_SUMMARY:-summary unavailable})"
        else
            t_fail "scripts/lsp-mcp/tests/test_bridge.sh FAIL (${LM_SUMMARY:-run directly for details})"
        fi
    else
        t_fail "scripts/lsp-mcp/tests/test_bridge.sh not found or not executable"
    fi
    LSP_BOUNDARY_TEST="$REPO_ROOT/scripts/lsp-mcp/tests/test_boundary.sh"
    if [ -x "$LSP_BOUNDARY_TEST" ]; then
        LB_OUT=$("$LSP_BOUNDARY_TEST" 2>&1)
        LB_RC=$?
        LB_SUMMARY=$(printf '%s\n' "$LB_OUT" | grep -E '^\[boundary\] ' | tail -1)
        if [ "$LB_RC" = "0" ]; then
            t_pass "scripts/lsp-mcp/tests/test_boundary.sh PASS (${LB_SUMMARY:-summary unavailable})"
        else
            t_fail "scripts/lsp-mcp/tests/test_boundary.sh FAIL (${LB_SUMMARY:-run directly for details})"
        fi
    else
        t_fail "scripts/lsp-mcp/tests/test_boundary.sh not found or not executable"
    fi
fi

# ============================================================================
# Cross-tool MCP drift detection (TODO-08 in 00-infrastructure section 1)
#
# Asserts the Claude Code side (.mcp.json) and the Codex CLI side
# (~/.codex/config.toml, validated against the repo-owned fixture
# docs/infrastructure/codex-mcp.config.toml) expose the same MCP
# server set with the same wiring (command, args, cwd, timeouts,
# enabled_tools allowlist where applicable).
#
# Two-step drift check:
#   1. Codex side -- delegate to scripts/codex-mcp-install.sh --check
#      so the rule "Codex config matches repo fixture" is enforced
#      from the same source the installer uses (no two-source bug).
#   2. Cross-side parity -- diff the SERVER NAME SET between
#      .mcp.json (mcpServers keys) and the repo fixture
#      (mcp_servers.* tables). A mismatch means somebody added a
#      server to one client without adding it to the other.
#
# Skip-with-WARN behavior: a fresh dev environment with no
# ~/.codex/config.toml gets a clear actionable message pointing at
# scripts/codex-mcp-install.sh (the installer creates the file from
# the fixture). The Cross-side parity check still runs even without
# Codex installed because it compares the repo's .mcp.json against
# the repo's fixture, both repo-tracked.
[ "$QUIET" = "0" ] && echo "" && echo -e "${DIM}[mcp_drift]${NC}"

MCP_FIXTURE="$REPO_ROOT/docs/infrastructure/codex-mcp.config.toml"
CLAUDE_MCP_JSON="$REPO_ROOT/.mcp.json"
CODEX_MCP_INSTALL="$REPO_ROOT/scripts/codex-mcp-install.sh"

if [ ! -f "$MCP_FIXTURE" ]; then
    t_fail "mcp_drift fixture missing: $MCP_FIXTURE"
elif [ ! -f "$CLAUDE_MCP_JSON" ]; then
    t_fail "mcp_drift Claude .mcp.json missing: $CLAUDE_MCP_JSON"
else
    # Cross-side parity check (always runs). The python script is
    # written to a tempfile to avoid a heredoc-inside-$() pattern
    # bash misparses with a "unterminated here-document" warning.
    # Codex post-impl adversarial review High caught the original
    # name-only diff as insufficient: a .mcp.json command/args
    # change for an existing server name silently passed. The
    # parity script now compares command + args + cwd between
    # .mcp.json and the repo fixture for each named server, on
    # top of the server-name set check.
    MCP_PARITY_PY="$(mktemp)"
    cat > "$MCP_PARITY_PY" <<'PY'
import json
import sys

try:
    import tomllib
except ImportError:
    sys.stderr.write("python3 lacks tomllib (need 3.11+)\n")
    sys.exit(2)

claude_path, fixture_path = sys.argv[1], sys.argv[2]
claude = json.loads(open(claude_path).read())
claude_servers = (claude.get("mcpServers") or {})

with open(fixture_path, "rb") as f:
    fixture = tomllib.load(f)
fixture_servers = (fixture.get("mcp_servers") or {})

claude_names = set(claude_servers.keys())
fixture_names = set(fixture_servers.keys())

# Server-name set parity.
errors = []
claude_only = sorted(claude_names - fixture_names)
fixture_only = sorted(fixture_names - claude_names)
if claude_only:
    errors.append(
        f"in .mcp.json but not in fixture: {', '.join(claude_only)}"
    )
if fixture_only:
    errors.append(
        f"in fixture but not in .mcp.json: {', '.join(fixture_only)}"
    )

# Per-server semantic compare for shared names. The two sides use
# different schemas (.mcp.json: {command, args}; Codex fixture:
# {command, args, cwd, ...}).
#
# Default rule: command + args[0] (the script path) must match.
# We do NOT compare cwd (Codex needs an absolute path; .mcp.json
# is implicitly cwd=repo-root).
#
# Per-server overrides for FULL_ARGS_SERVERS: the entire args list
# must match. lsp-bridge belongs here because the cross-tool MCP
# contract explicitly requires the same warm-start flags on both
# sides (the bridge needs --warm-start-mode=background to survive
# any MCP launcher's stdio handshake; dropping it on either side
# would leave one client with a materially different startup
# profile). Codex post-ship consistency review M caught the
# previous args[0]-only check as too loose. (todo-graph stays on
# the args[0]-only rule because it has no flags today and any
# future flag would be a deliberate per-client tuning.)
FULL_ARGS_SERVERS = {"lsp-bridge"}

for name in sorted(claude_names & fixture_names):
    c = claude_servers[name]
    f = fixture_servers[name]
    c_cmd = c.get("command")
    f_cmd = f.get("command")
    if c_cmd != f_cmd:
        errors.append(
            f"[{name}] command mismatch: .mcp.json={c_cmd!r} "
            f"fixture={f_cmd!r}"
        )
    c_args = c.get("args") or []
    f_args = f.get("args") or []
    if not c_args or not f_args:
        errors.append(
            f"[{name}] empty args: .mcp.json={c_args!r} "
            f"fixture={f_args!r}"
        )
    elif name in FULL_ARGS_SERVERS:
        # Full args list must match. Render the diff so an
        # operator can see exactly which flag drifted.
        if list(c_args) != list(f_args):
            errors.append(
                f"[{name}] full args mismatch (cross-tool contract "
                f"requires identical args for this server): "
                f".mcp.json={c_args!r} fixture={f_args!r}"
            )
    elif c_args[0] != f_args[0]:
        errors.append(
            f"[{name}] script path mismatch: "
            f".mcp.json args[0]={c_args[0]!r} "
            f"fixture args[0]={f_args[0]!r}"
        )

if errors:
    for e in errors:
        sys.stderr.write(f"  {e}\n")
    sys.exit(1)
print(f"both sides expose {len(claude_names)} servers with matching "
      f"command + script path: {', '.join(sorted(claude_names))}")
PY
    PARITY_OUT=$(python3 "$MCP_PARITY_PY" "$CLAUDE_MCP_JSON" "$MCP_FIXTURE" 2>&1)
    PARITY_RC=$?
    rm -f "$MCP_PARITY_PY"

    # Codex-side check (delegates to installer's --check mode).
    if [ -f "${HOME}/.codex/config.toml" ] && [ -x "$CODEX_MCP_INSTALL" ]; then
        CODEX_OUT=$("$CODEX_MCP_INSTALL" --check 2>&1)
        CODEX_RC=$?
    elif [ ! -f "${HOME}/.codex/config.toml" ]; then
        CODEX_OUT="~/.codex/config.toml absent (run: bash scripts/codex-mcp-install.sh)"
        CODEX_RC=2
    else
        CODEX_OUT="scripts/codex-mcp-install.sh missing or not executable"
        CODEX_RC=2
    fi

    # Codex post-impl adversarial review High caught the original
    # condition's mask: a previous revision did `t_pass` on
    # CODEX_RC == 2 alone, which silently passed even when the
    # cross-side parity check FAILed. Always check PARITY_RC
    # first; only consider the codex-side skip-with-PASS when
    # PARITY_RC is also 0.
    if [ "$PARITY_RC" != "0" ]; then
        t_fail "mcp_drift cross-side parity FAIL: ${PARITY_OUT}"
    elif [ "$CODEX_RC" = "0" ]; then
        t_pass "mcp_drift Claude .mcp.json and Codex config in sync (${PARITY_OUT})"
    elif [ "$CODEX_RC" = "2" ]; then
        # Skip-with-PASS: fresh dev env / installer missing AND
        # cross-side parity passed. The hint message tells the
        # operator how to install the Codex side.
        t_pass "mcp_drift cross-side parity OK; codex-side SKIP (${CODEX_OUT})"
    else
        t_fail "mcp_drift codex-side: ${CODEX_OUT}"
    fi
fi

# ============================================================================
# Codex --model/--effort flag-block hook (TODO-08 in 00-infrastructure section 2)
#
# Asserts the `.claude/hooks/codex_model_flag_block.py` PreToolUse
# hook blocks Codex invocations that pass --model or --effort flags
# (per the Codex Invocation Policy in CLAUDE.md "Model Roles") AND
# correctly allows clean Codex commands, non-Codex commands, prompt
# text that contains the literal flag string, and opt-out via
# CODEX_FLAG_OVERRIDE=1.
[ "$QUIET" = "0" ] && echo "" && echo -e "${DIM}[codex_flag_block]${NC}"

CODEX_FLAG_HOOK="$REPO_ROOT/.claude/hooks/codex_model_flag_block.py"
if [ ! -f "$CODEX_FLAG_HOOK" ]; then
    t_fail "codex_flag_block hook missing: $CODEX_FLAG_HOOK"
else
    _flag_probe() {
        # Args: <expected_rc> <description> <command-string>
        local want="$1" desc="$2" cmd="$3"
        local got
        local payload
        payload=$(python3 -c "import json; print(json.dumps({'tool_name':'Bash','tool_input':{'command':'''$cmd'''}}))" 2>/dev/null || echo "{}")
        got=$(printf '%s' "$payload" | python3 "$CODEX_FLAG_HOOK" 2>/dev/null; echo " rc=$?")
        local rc=${got##* rc=}
        if [ "$rc" = "$want" ]; then
            t_pass "codex_flag_block: $desc (rc=$rc)"
        else
            t_fail "codex_flag_block: $desc (want $want, got $rc; cmd='$cmd')"
        fi
    }
    _flag_probe_env() {
        # Same as _flag_probe but injects an env var for the hook call.
        local var="$1" want="$2" desc="$3" cmd="$4"
        local payload got rc
        payload=$(python3 -c "import json; print(json.dumps({'tool_name':'Bash','tool_input':{'command':'''$cmd'''}}))" 2>/dev/null || echo "{}")
        got=$(printf '%s' "$payload" | env "$var" python3 "$CODEX_FLAG_HOOK" 2>/dev/null; echo " rc=$?")
        rc=${got##* rc=}
        if [ "$rc" = "$want" ]; then
            t_pass "codex_flag_block: $desc (rc=$rc)"
        else
            t_fail "codex_flag_block: $desc (want $want, got $rc; cmd='$cmd')"
        fi
    }
    # 1. --model on codex-companion.mjs invocation -> BLOCK (rc=2)
    _flag_probe 2 "block --model on codex-companion.mjs" \
        'node /path/to/codex-companion.mjs adversarial-review --model gpt-5.5 prompt'
    # 2. --effort on codex exec invocation -> BLOCK (rc=2)
    _flag_probe 2 "block --effort on codex exec" \
        'codex exec --effort high prompt'
    # 3. --model=value attached form -> BLOCK (rc=2)
    _flag_probe 2 "block --model=value attached form" \
        'codex exec --model=gpt-5.5 prompt'
    # 4. Clean Codex command (no forbidden flags) -> ALLOW (rc=0)
    _flag_probe 0 "allow clean codex-companion.mjs invocation" \
        'node /path/to/codex-companion.mjs adversarial-review some prompt'
    # 5. Non-Codex command that happens to mention --model -> ALLOW (rc=0)
    _flag_probe 0 "allow non-codex rg --model" \
        'rg --model whatever-rg-flag-here'
    # 6. Prompt-text containing the literal --model string in a quoted arg -> ALLOW
    _flag_probe 0 "allow --model literal inside quoted prompt text" \
        'codex exec "review whether --model gpt-5.5 is correct"'
    # 7. CODEX_FLAG_OVERRIDE=1 lets the call through (rc=0)
    _flag_probe_env 'CODEX_FLAG_OVERRIDE=1' 0 \
        "opt-out via CODEX_FLAG_OVERRIDE=1" \
        'codex exec --model gpt-5.5 prompt'
    # 8. Empty command -> ALLOW (rc=0)
    _flag_probe 0 "allow empty command" ''
    # 9. Bare codex (no subcommand) -> ALLOW even with --model (TUI/help)
    _flag_probe 0 "allow bare codex --version" 'codex --version'
    # 10. Regression: python3 heredoc whose body contains --model
    #     literal token -> ALLOW (the FIRST command is python3, not
    #     Codex; an earlier revision walked all tokens and blocked
    #     this case, breaking edits to TODO files / commit messages
    #     that contain `--model` as plain text).
    _flag_probe 0 "allow python3 with --model in argv" \
        'python3 -c "x = \"--model\""'
    # 11. Bypass closed: `codex --json exec --model X` -- global
    #     option BEFORE subcommand. An earlier revision only
    #     checked tokens[1] for the subcommand; the post-impl
    #     adversarial review found this letting --model through
    #     because tokens[1] was --json. Now blocks (rc=2).
    _flag_probe 2 "block --model after global option (--json before exec)" \
        'codex --json exec --model gpt-5.5 prompt'
    # 12. Bypass closed: `-c model="x"` config override. The bare
    #     -c is legitimate (e.g. -c sandbox_mode="..."); only
    #     model / model_reasoning_effort keys are forbidden.
    _flag_probe 2 "block -c model=value config override" \
        'codex exec -c model=gpt-5.5 prompt'
    # 13. Bypass closed: `--config model_reasoning_effort=...`
    _flag_probe 2 "block --config model_reasoning_effort=value override" \
        'codex exec --config model_reasoning_effort=high prompt'
    # 14. Allow: `-c sandbox_mode=...` (legitimate, non-forbidden key)
    _flag_probe 0 "allow -c sandbox_mode=value (non-forbidden key)" \
        'codex exec -c sandbox_mode=workspace-write prompt'
    # 15. Bypass closed: `codex e --model X` (e is alias for exec)
    _flag_probe 2 "block --model on codex e (exec alias)" \
        'codex e --model gpt-5.5 prompt'
    # 16. Bypass closed: short form `-m gpt-5.5`
    _flag_probe 2 "block -m short form" \
        'codex exec -m gpt-5.5 prompt'
    # 17. Bypass closed: env-assignment prefix
    _flag_probe 2 "block env-prefix MODEL=x codex exec --model" \
        'MODEL=foo codex exec --model gpt-5.5 prompt'
    # 18. Bypass closed: timeout wrapper
    _flag_probe 2 "block timeout wrapper codex exec --model" \
        'timeout 60 codex exec --model gpt-5.5 prompt'
    # 19. Bypass closed: && chain
    _flag_probe 2 "block && chain to codex exec --model" \
        'true && codex exec --model gpt-5.5 prompt'
    # 20. Bypass closed: -c model_provider=
    _flag_probe 2 "block -c model_provider=value override" \
        'codex exec -c model_provider=foo prompt'
    # 21. Bypass closed: env wrapper
    _flag_probe 2 "block env-wrapper env MODEL=x codex exec --model" \
        'env MODEL=x codex exec --model gpt-5.5 prompt'
    # 22. Allow: && chain where second segment is non-Codex
    _flag_probe 0 "allow && chain non-codex 2nd segment" \
        'true && rg --model thing'
    # 23. Allow: env-only command (env-prefix to non-codex)
    _flag_probe 0 "allow env-prefix to non-codex" \
        'MODEL=foo true'
    # 24. Heredoc fail-open: complex shell constructs (heredocs,
    #     process substitution) bail-out -- shlex flattens heredoc
    #     bodies into ordinary tokens, so a body containing
    #     `&& codex exec --model X` would otherwise look like a real
    #     bypass after segmentation. File redirects (`> /tmp/out`)
    #     are NOT bailed out -- they don't introduce phantom commands.
    _flag_probe 0 "allow heredoc with --model in body" \
        'python3 << EOF\nfoo --model bar\nEOF'
    # 25. Bypass closed: redirect after a real Codex command must
    #     still BLOCK (re-adversarial review H -- the previous
    #     revision's broad-redirect fail-open let this slip).
    _flag_probe 2 "block codex exec --model X > /tmp/out (redirect after Codex)" \
        'codex exec --model gpt-5.5 prompt > /tmp/out'
    # 26. Bypass closed: sudo -u VALUE codex exec --model X (sudo
    #     value-taking flag was missed by the per-wrapper parser).
    _flag_probe 2 "block sudo -u root codex exec --model X" \
        'sudo -u root codex exec --model gpt-5.5 prompt'
    # 27. Bypass closed: timeout 5s codex exec --model X (duration
    #     with suffix unit was missed by the float-parse heuristic).
    _flag_probe 2 "block timeout 5s codex exec --model X (suffix duration)" \
        'timeout 5s codex exec --model gpt-5.5 prompt'
    # 28. Allow: redirect on a NON-Codex command must NOT bail-out
    #     (re-adversarial review H regression check).
    _flag_probe 0 "allow echo --model > /tmp/out (non-codex with redirect)" \
        'echo --model > /tmp/out'
    # 29. Allow: process substitution with --model in argv (proc-sub
    #     fail-open guard handles `<(...)` / `>(...)` constructs).
    _flag_probe 0 "allow diff <(echo a) <(echo b)" \
        'diff <(echo a) <(echo b)'
    # 30. Allow: command substitution body containing flag literals
    #     (consistency review caught the false-positive when the
    #     review-stamp git commit blocked itself because its message
    #     body inside $(cat <<EOF ... EOF) mentioned `-m` and
    #     `codex` as text). Trim helper now handles `$(`, `${`, and
    #     backticks the same way it handles `<<` heredocs.
    _flag_probe 0 "allow git commit -m with codex+-m in cmd-sub body" \
        'git commit -m "$(cat <<EOF\nblock -m short form for codex exec\nEOF\n)"'
fi

# ============================================================================
# receiving-code-review hard gate (TODO-08 in 00-infrastructure section 3)
#
# Asserts the codex_review_completed.py PostToolUse hook + the
# receiving_review_required.py PreToolUse hook chain works:
#   - Trigger event writes state file with received:false
#   - Receive event flips received:true
#   - PreToolUse blocks Edit/Write while received:false within 1h TTL
#   - PreToolUse allows after received:true
#   - PreToolUse allows when state file missing (no recent review)
#   - PreToolUse allows on TTL expiry (>1h since trigger)
#   - Opt-out via RECEIVING_REVIEW_OVERRIDE=1 allows
#   - Bare-name Skill (per design-review M finding) is recognized
[ "$QUIET" = "0" ] && echo "" && echo -e "${DIM}[receiving_review_gate]${NC}"

POST_HOOK="$REPO_ROOT/.claude/hooks/codex_review_completed.py"
PRE_HOOK="$REPO_ROOT/.claude/hooks/receiving_review_required.py"
STATE_FILE="$REPO_ROOT/.claude/state/last-codex-review.json"

if [ ! -f "$POST_HOOK" ]; then
    t_fail "receiving_review_gate post hook missing: $POST_HOOK"
elif [ ! -f "$PRE_HOOK" ]; then
    t_fail "receiving_review_gate pre hook missing: $PRE_HOOK"
else
    _gate_probe() {
        # Args: <expected_rc> <description> <hook> <payload-json> [env_var]
        local want="$1" desc="$2" hook="$3" payload="$4" envvar="${5:-}"
        local got
        if [ -n "$envvar" ]; then
            got=$(printf '%s' "$payload" | env "$envvar" python3 "$hook" >/dev/null 2>&1; echo $?)
        else
            got=$(printf '%s' "$payload" | python3 "$hook" >/dev/null 2>&1; echo $?)
        fi
        if [ "$got" = "$want" ]; then
            t_pass "receiving_review_gate: $desc (rc=$got)"
        else
            t_fail "receiving_review_gate: $desc (want $want, got $got)"
        fi
    }
    _state_received() {
        # Print received field from state file; "missing" if absent.
        # Use single-quoted python -c body and pass STATE_FILE as argv
        # so quote-escape headaches cannot eat the json.load argument.
        if [ -f "$STATE_FILE" ]; then
            python3 -c 'import json, sys; print(json.load(open(sys.argv[1])).get("received", "missing"))' "$STATE_FILE"
        else
            echo "missing"
        fi
    }

    # Clean any prior state from earlier test runs.
    rm -f "$STATE_FILE"

    # Sub-test 1: Bash trigger writes state with received:false
    printf "%s" '''{"tool_name":"Bash","tool_input":{"command":"node /tmp/codex-companion.mjs adversarial-review test"}}''' \
        | python3 "$POST_HOOK" >/dev/null 2>&1
    GOT=$(_state_received)
    if [ "$GOT" = "False" ]; then
        t_pass "receiving_review_gate: Bash trigger writes state.received=False"
    else
        t_fail "receiving_review_gate: Bash trigger writes state.received=False (got $GOT)"
    fi

    # Sub-test 2: Skill trigger via codex-* skill writes state
    rm -f "$STATE_FILE"
    printf "%s" '''{"tool_name":"Skill","tool_input":{"skill":"codex-adversarial-review-section"}}''' \
        | python3 "$POST_HOOK" >/dev/null 2>&1
    GOT=$(_state_received)
    if [ "$GOT" = "False" ]; then
        t_pass "receiving_review_gate: Skill trigger writes state.received=False"
    else
        t_fail "receiving_review_gate: Skill trigger writes state.received=False (got $GOT)"
    fi

    # Sub-test 3: PreToolUse BLOCKS (rc=2) when received:false
    _gate_probe 2 "PreToolUse blocks Edit when received=false" \
        "$PRE_HOOK" '''{"tool_name":"Edit","tool_input":{"file_path":"src/foo.c"}}'''

    # Sub-test 4: PreToolUse BLOCKS Write too
    _gate_probe 2 "PreToolUse blocks Write when received=false" \
        "$PRE_HOOK" '''{"tool_name":"Write","tool_input":{"file_path":"src/foo.c","content":"x"}}'''

    # Sub-test 5: Receive (namespaced) flips state to received=true
    printf "%s" '''{"tool_name":"Skill","tool_input":{"skill":"superpowers:receiving-code-review"}}''' \
        | python3 "$POST_HOOK" >/dev/null 2>&1
    GOT=$(_state_received)
    if [ "$GOT" = "True" ]; then
        t_pass "receiving_review_gate: namespaced receive flips received=True"
    else
        t_fail "receiving_review_gate: namespaced receive flips received=True (got $GOT)"
    fi

    # Sub-test 6: PreToolUse ALLOWS after receive
    _gate_probe 0 "PreToolUse allows Edit after received=true" \
        "$PRE_HOOK" '''{"tool_name":"Edit","tool_input":{"file_path":"src/foo.c"}}'''

    # Sub-test 7: Bare-name receive (skill key) -- design review M
    rm -f "$STATE_FILE"
    printf "%s" '''{"tool_name":"Skill","tool_input":{"skill":"codex-adversarial-review-section"}}''' \
        | python3 "$POST_HOOK" >/dev/null 2>&1
    printf "%s" '''{"tool_name":"Skill","tool_input":{"skill":"receiving-code-review"}}''' \
        | python3 "$POST_HOOK" >/dev/null 2>&1
    GOT=$(_state_received)
    if [ "$GOT" = "True" ]; then
        t_pass "receiving_review_gate: bare-name receive (skill key) flips True"
    else
        t_fail "receiving_review_gate: bare-name receive (skill key) flips True (got $GOT)"
    fi

    # Sub-test 8: Bare-name receive (name key) -- design review M
    rm -f "$STATE_FILE"
    printf "%s" '''{"tool_name":"Skill","tool_input":{"skill":"codex-adversarial-review-section"}}''' \
        | python3 "$POST_HOOK" >/dev/null 2>&1
    printf "%s" '''{"tool_name":"Skill","tool_input":{"name":"receiving-code-review"}}''' \
        | python3 "$POST_HOOK" >/dev/null 2>&1
    GOT=$(_state_received)
    if [ "$GOT" = "True" ]; then
        t_pass "receiving_review_gate: bare-name receive (name key) flips True"
    else
        t_fail "receiving_review_gate: bare-name receive (name key) flips True (got $GOT)"
    fi

    # Sub-test 9: PreToolUse ALLOWS when state file missing
    rm -f "$STATE_FILE"
    _gate_probe 0 "PreToolUse allows Edit when no state file" \
        "$PRE_HOOK" '''{"tool_name":"Edit","tool_input":{"file_path":"src/foo.c"}}'''

    # Sub-test 10: Opt-out via RECEIVING_REVIEW_OVERRIDE=1
    printf "%s" '''{"tool_name":"Skill","tool_input":{"skill":"codex-adversarial-review-section"}}''' \
        | python3 "$POST_HOOK" >/dev/null 2>&1
    _gate_probe 0 "PreToolUse opt-out via RECEIVING_REVIEW_OVERRIDE=1" \
        "$PRE_HOOK" '''{"tool_name":"Edit","tool_input":{"file_path":"src/foo.c"}}''' \
        "RECEIVING_REVIEW_OVERRIDE=1"

    # Sub-test 11: TTL stale-pass (>1h) -> allow
    printf "%s" '''{"tool_name":"Skill","tool_input":{"skill":"codex-adversarial-review-section"}}''' \
        | python3 "$POST_HOOK" >/dev/null 2>&1
    python3 -c '
import json, sys
p = sys.argv[1]
s = json.load(open(p))
s["timestamp_ns"] = s["timestamp_ns"] - 2 * 3600 * 10**9
open(p, "w").write(json.dumps(s))
' "$STATE_FILE"
    _gate_probe 0 "PreToolUse allows on TTL stale-pass (>1h old)" \
        "$PRE_HOOK" '''{"tool_name":"Edit","tool_input":{"file_path":"src/foo.c"}}'''

    # Sub-test 12: Non-Edit/Write/MultiEdit tools are not blocked
    rm -f "$STATE_FILE"
    printf "%s" '''{"tool_name":"Skill","tool_input":{"skill":"codex-adversarial-review-section"}}''' \
        | python3 "$POST_HOOK" >/dev/null 2>&1
    _gate_probe 0 "PreToolUse passes through Bash/Read/Grep tools" \
        "$PRE_HOOK" '''{"tool_name":"Bash","tool_input":{"command":"ls"}}'''

    # Sub-test 13 (M1 regression): substring-only mention of
    # codex-companion.mjs in a Bash command (e.g. heredoc body, rg
    # search, git grep) MUST NOT write a trigger. Codex adversarial
    # review caught this on first pass; live-reproduced when the
    # heredoc that built this very review's prompt left a
    # `Bash(cat > /tmp/...)` trigger label in the state file.
    rm -f "$STATE_FILE"
    printf "%s" '''{"tool_name":"Bash","tool_input":{"command":"rg codex-companion.mjs .claude/skills/"}}''' \
        | python3 "$POST_HOOK" >/dev/null 2>&1
    GOT=$(_state_received)
    if [ "$GOT" = "missing" ]; then
        t_pass "receiving_review_gate: rg codex-companion.mjs does NOT write trigger (M1 regression)"
    else
        t_fail "receiving_review_gate: rg codex-companion.mjs wrote trigger (M1 regression, got $GOT)"
    fi

    # Sub-test 14 (M1 regression): heredoc body containing the literal
    # is also not a trigger.
    rm -f "$STATE_FILE"
    printf "%s" '''{"tool_name":"Bash","tool_input":{"command":"cat > /tmp/x <<EOF\nrun codex-companion.mjs from here\nEOF"}}''' \
        | python3 "$POST_HOOK" >/dev/null 2>&1
    GOT=$(_state_received)
    if [ "$GOT" = "missing" ]; then
        t_pass "receiving_review_gate: heredoc body literal does NOT write trigger (M1 regression)"
    else
        t_fail "receiving_review_gate: heredoc body literal wrote trigger (M1 regression, got $GOT)"
    fi

    # Sub-test 15 (M1 regression): git grep mentioning the literal is
    # also not a trigger.
    rm -f "$STATE_FILE"
    printf "%s" '''{"tool_name":"Bash","tool_input":{"command":"git grep \"codex review\""}}''' \
        | python3 "$POST_HOOK" >/dev/null 2>&1
    GOT=$(_state_received)
    if [ "$GOT" = "missing" ]; then
        t_pass "receiving_review_gate: git grep \"codex review\" does NOT write trigger (M1 regression)"
    else
        t_fail "receiving_review_gate: git grep \"codex review\" wrote trigger (M1 regression, got $GOT)"
    fi

    # Sub-test 16 (M1 positive): wrapped Codex via env-prefix +
    # wrapper command IS still a trigger (the shlex tokenizer walks
    # past `RECEIVING_REVIEW_OVERRIDE=1 timeout 600 node ...`).
    rm -f "$STATE_FILE"
    printf "%s" '''{"tool_name":"Bash","tool_input":{"command":"RECEIVING_REVIEW_OVERRIDE=1 timeout 600 node /abs/codex-companion.mjs adversarial-review prompt"}}''' \
        | python3 "$POST_HOOK" >/dev/null 2>&1
    GOT=$(_state_received)
    if [ "$GOT" = "False" ]; then
        t_pass "receiving_review_gate: env-prefix + timeout-wrapper Codex IS a trigger (M1 positive)"
    else
        t_fail "receiving_review_gate: env-prefix + timeout-wrapper Codex missed (M1 positive, got $GOT)"
    fi

    # Sub-test 17 (H1 mitigation): rapid-fire trigger that overwrites
    # an unreceived previous trigger emits a stderr WARN naming both
    # the new and the previous trigger label. Captures stderr to
    # verify the WARN text shape.
    rm -f "$STATE_FILE"
    printf "%s" '''{"tool_name":"Bash","tool_input":{"command":"node /abs/codex-companion.mjs adversarial-review first"}}''' \
        | python3 "$POST_HOOK" >/dev/null 2>&1
    WARN_OUT=$(printf "%s" '''{"tool_name":"Bash","tool_input":{"command":"node /abs/codex-companion.mjs adversarial-review second"}}''' \
        | python3 "$POST_HOOK" 2>&1 >/dev/null || true)
    if echo "$WARN_OUT" | grep -q "overwriting previous unreceived trigger"; then
        t_pass "receiving_review_gate: rapid-fire trigger emits WARN (H1 mitigation)"
    else
        t_fail "receiving_review_gate: no WARN on rapid-fire trigger (H1 mitigation, got: $WARN_OUT)"
    fi

    # Cleanup
    rm -f "$STATE_FILE"
fi


# ============================================================================
# Summary
# ============================================================================

TOTAL=$((PASS + FAIL))
if [ "$QUIET" = "0" ]; then
    echo ""
    echo -e "${CYAN}══════════════════════════════════════════════════${NC}"
fi
if [ "$FAIL" = "0" ]; then
    echo -e "  ${GREEN}PASS${NC}  ${TOTAL}/${TOTAL} tooling tests passed"
else
    echo -e "  ${RED}FAIL${NC}  ${FAIL}/${TOTAL} tooling tests failed"
    for f in "${FAILURES[@]}"; do
        echo -e "        ${DIM}- $f${NC}"
    done
fi
if [ "$QUIET" = "0" ]; then
    echo -e "${CYAN}══════════════════════════════════════════════════${NC}"
fi

[ "$FAIL" = "0" ] && exit 0 || exit 1
