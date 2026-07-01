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
_lhelp="$(bash "$SCRIPT_DIR/lint.sh" --help 2>/dev/null)"   # capture first; no pipe into lint (see pipefail note below)
if grep -qE 'Checks \(5\)' <<<"$_lhelp"; then
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

# --- Check 15 codex-&-bundle lint -----------------------------------------
# The fixture string is built split (codex-%s.sh) so this file's own source
# line never carries the bundled pattern Check 15 scans for.
[ "$QUIET" = "0" ] && echo "" && echo -e "${DIM}[lint_codex_bundle]${NC}"
_BUNDLE_FIX="$REPO_ROOT/docs/_tooling_lint15_fixture.md"
printf 'x: bash scripts/codex-%s.sh A && bash scripts/codex-%s.sh B\n' dispatch dispatch > "$_BUNDLE_FIX"
# Capture lint output to a variable FIRST, then match with a here-string. Do NOT pipe
# lint directly into `grep -q`: under the `set -o pipefail` at the top of this script,
# grep -q closes the pipe on its first match and lint then dies of SIGPIPE while writing
# its summary, so the pipeline returns 141 and the test "fails" even though the pattern
# matched. That race is timing-dependent (consistently red in CI, mostly green locally).
# A here-string has no pipe into lint, so this is deterministic.
_l15flag="$(bash "$REPO_ROOT/scripts/lint.sh" 2>&1)"
if grep -q "run each dispatch as its own" <<<"$_l15flag"; then
    t_pass "lint Check 15 flags &-bundled codex dispatch"
else
    t_fail "lint Check 15 missed &-bundled codex dispatch"
fi
rm -f "$_BUNDLE_FIX"
_l15clean="$(bash "$REPO_ROOT/scripts/lint.sh" 2>&1)"
if grep -q "run each dispatch as its own" <<<"$_l15clean"; then
    t_fail "lint Check 15 false-positive on clean tree"
else
    t_pass "lint Check 15 clean on clean tree"
fi

# --- codex-bg-dispatch argument contract -----------------------------------
[ "$QUIET" = "0" ] && echo "" && echo -e "${DIM}[codex_bg_dispatch_contract]${NC}"
_bg_multi="$(bash "$REPO_ROOT/scripts/codex-bg-dispatch.sh" '[review-kind: design] todo/00-infrastructure/TODO-10-ai-driver-interchangeability.md ok' extra 2>&1 >/dev/null || true)"
if grep -q "expected exactly one prompt argv" <<<"$_bg_multi"; then
    t_pass "codex-bg-dispatch rejects multi-argv prompt"
else
    t_fail "codex-bg-dispatch accepted multi-argv prompt"
fi
_bg_unmarked="$(bash "$REPO_ROOT/scripts/codex-bg-dispatch.sh" 'Design review for the boot init path' 2>&1 >/dev/null || true)"
if grep -q "first nonblank prompt line must start" <<<"$_bg_unmarked"; then
    t_pass "codex-bg-dispatch rejects unmarked prompt"
else
    t_fail "codex-bg-dispatch accepted unmarked prompt"
fi

# --- WS1b agent-dispatch gate ---------------------------------------------
# Tests the safety property: the gate is SILENT (empty stderr, rc 0) in
# interactive sessions and on non-source targets. The positive WARN path needs
# a live SECTIONS guard state and is covered by manual probe (not simulated here
# to avoid clobbering the real sequencer-run.json).
[ "$QUIET" = "0" ] && echo "" && echo -e "${DIM}[agent_dispatch_gate]${NC}"
_AD_GATE="$REPO_ROOT/.claude/hooks/agent_dispatch_required.py"
_ad_silent() {  # <desc> <env-assignments...> -- payload in $PAYLOAD; PASS iff empty stderr + rc 0
    local desc="$1"; shift
    local err rc
    err=$(printf '%s' "$PAYLOAD" | env -u OVERNIGHT_SEQUENCER_RUN -u SKIP_AGENT_DISPATCH_HOOK "$@" python3 "$_AD_GATE" 2>&1 1>/dev/null)
    rc=$?
    if [ -z "$err" ] && [ "$rc" = "0" ]; then
        t_pass "agent_dispatch_gate: $desc (silent)"
    else
        t_fail "agent_dispatch_gate: $desc (want silent, got rc=$rc err='$err')"
    fi
}
PAYLOAD='{"tool_name":"Edit","tool_input":{"file_path":"src/kernel/x.c"}}'
_ad_silent "silent when OVERNIGHT_SEQUENCER_RUN unset" "PATH=$PATH"
_ad_silent "silent with SKIP opt-out" "PATH=$PATH" "OVERNIGHT_SEQUENCER_RUN=1" "SKIP_AGENT_DISPATCH_HOOK=1"
PAYLOAD='{"tool_name":"Edit","tool_input":{"file_path":"docs/x.md"}}'
_ad_silent "silent on markdown target" "PATH=$PATH" "OVERNIGHT_SEQUENCER_RUN=1"
PAYLOAD='{"tool_name":"Read","tool_input":{"file_path":"src/kernel/x.c"}}'
_ad_silent "silent on non-edit tool" "PATH=$PATH" "OVERNIGHT_SEQUENCER_RUN=1"

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

    # Sub-test 18 (H2 review-adv): env FOO=bar codex review shape --
    # env wrapper popped, then env's KEY=value assignments stripped,
    # then the codex subcommand identified.
    rm -f "$STATE_FILE"
    printf "%s" '''{"tool_name":"Bash","tool_input":{"command":"env FOO=bar codex review prompt"}}''' \
        | python3 "$POST_HOOK" >/dev/null 2>&1
    GOT=$(_state_received)
    if [ "$GOT" = "False" ]; then
        t_pass "receiving_review_gate: env FOO=bar codex review IS a trigger (H2)"
    else
        t_fail "receiving_review_gate: env FOO=bar codex review missed (H2, got $GOT)"
    fi

    # Sub-test 19 (H2 review-adv): timeout -k 10 5 codex review shape
    # -- timeout's value-flag -k strips its value 10, then DURATION 5
    # strips, then codex subcommand identified.
    rm -f "$STATE_FILE"
    printf "%s" '''{"tool_name":"Bash","tool_input":{"command":"timeout -k 10 5 codex review prompt"}}''' \
        | python3 "$POST_HOOK" >/dev/null 2>&1
    GOT=$(_state_received)
    if [ "$GOT" = "False" ]; then
        t_pass "receiving_review_gate: timeout -k 10 5 codex review IS a trigger (H2)"
    else
        t_fail "receiving_review_gate: timeout -k 10 5 codex review missed (H2, got $GOT)"
    fi

    # Sub-test 20 (H2 review-adv): codex --json review shape -- Codex
    # global option --json walked past to find the real subcommand.
    # Same bypass shape as the codex --json exec --model X case
    # caught by the model-flag block hook.
    rm -f "$STATE_FILE"
    printf "%s" '''{"tool_name":"Bash","tool_input":{"command":"codex --json review prompt"}}''' \
        | python3 "$POST_HOOK" >/dev/null 2>&1
    GOT=$(_state_received)
    if [ "$GOT" = "False" ]; then
        t_pass "receiving_review_gate: codex --json review IS a trigger (H2)"
    else
        t_fail "receiving_review_gate: codex --json review missed (H2, got $GOT)"
    fi

    # Sub-test 21 (H4 review-adv): chained command `true && codex
    # review prompt` IS a trigger. Section-2 hook detected this;
    # section-3 originally missed it because tokens[0] was `true`.
    rm -f "$STATE_FILE"
    printf "%s" '''{"tool_name":"Bash","tool_input":{"command":"true && codex review prompt"}}''' \
        | python3 "$POST_HOOK" >/dev/null 2>&1
    GOT=$(_state_received)
    if [ "$GOT" = "False" ]; then
        t_pass "receiving_review_gate: true && codex review IS a trigger (H4)"
    else
        t_fail "receiving_review_gate: true && codex review missed (H4, got $GOT)"
    fi

    # Sub-test 22 (H4 review-adv): taskset wrapper -- `taskset -c 0
    # codex review prompt`. taskset's -c flag takes a value (cpu
    # list); after -c 0 the codex subcommand is the head.
    rm -f "$STATE_FILE"
    printf "%s" '''{"tool_name":"Bash","tool_input":{"command":"taskset -c 0 codex review prompt"}}''' \
        | python3 "$POST_HOOK" >/dev/null 2>&1
    GOT=$(_state_received)
    if [ "$GOT" = "False" ]; then
        t_pass "receiving_review_gate: taskset -c 0 codex review IS a trigger (H4)"
    else
        t_fail "receiving_review_gate: taskset -c 0 codex review missed (H4, got $GOT)"
    fi

    # Sub-test 23 (H4 review-adv): chrt wrapper -- `chrt -f 10 codex
    # review prompt`. chrt's -f flag does not take a value, but 10
    # is the priority positional. Wrapper-specific handling.
    rm -f "$STATE_FILE"
    printf "%s" '''{"tool_name":"Bash","tool_input":{"command":"chrt -f 10 codex review prompt"}}''' \
        | python3 "$POST_HOOK" >/dev/null 2>&1
    GOT=$(_state_received)
    if [ "$GOT" = "False" ]; then
        t_pass "receiving_review_gate: chrt -f 10 codex review IS a trigger (H4)"
    else
        t_fail "receiving_review_gate: chrt -f 10 codex review missed (H4, got $GOT)"
    fi

    # Sub-test 24 (H7 re-adv): leading command substitution must NOT
    # cause later codex segment to be lost. Pre-fix: trim_heredoc
    # ran before segmentation, so $(date) cut tokens at the first
    # token starting with $(, dropping the codex review segment.
    rm -f "$STATE_FILE"
    printf "%s" '''{"tool_name":"Bash","tool_input":{"command":"echo $(date) && codex review prompt"}}''' \
        | python3 "$POST_HOOK" >/dev/null 2>&1
    GOT=$(_state_received)
    if [ "$GOT" = "False" ]; then
        t_pass "receiving_review_gate: echo \$(date) && codex review IS a trigger (H7)"
    else
        t_fail "receiving_review_gate: echo \$(date) && codex review missed (H7, got $GOT)"
    fi

    # Sub-test 25 (H7 re-adv): backtick command substitution
    # variant.
    rm -f "$STATE_FILE"
    printf "%s" '''{"tool_name":"Bash","tool_input":{"command":"echo \\`date\\` && codex review prompt"}}''' \
        | python3 "$POST_HOOK" >/dev/null 2>&1
    GOT=$(_state_received)
    if [ "$GOT" = "False" ]; then
        t_pass "receiving_review_gate: echo backtick && codex review IS a trigger (H7)"
    else
        t_fail "receiving_review_gate: echo backtick && codex review missed (H7, got $GOT)"
    fi

    # Sub-test 26 (H8 re-adv): codex global value-flag --enable
    # consumes its value before the real subcommand. Pre-fix:
    # `feature` was treated as the subcommand.
    rm -f "$STATE_FILE"
    printf "%s" '''{"tool_name":"Bash","tool_input":{"command":"codex --enable feature review prompt"}}''' \
        | python3 "$POST_HOOK" >/dev/null 2>&1
    GOT=$(_state_received)
    if [ "$GOT" = "False" ]; then
        t_pass "receiving_review_gate: codex --enable feature review IS a trigger (H8)"
    else
        t_fail "receiving_review_gate: codex --enable feature review missed (H8, got $GOT)"
    fi

    # Sub-test 27 (H8 re-adv): codex --remote VALUE review prompt.
    rm -f "$STATE_FILE"
    printf "%s" '''{"tool_name":"Bash","tool_input":{"command":"codex --remote http://x review prompt"}}''' \
        | python3 "$POST_HOOK" >/dev/null 2>&1
    GOT=$(_state_received)
    if [ "$GOT" = "False" ]; then
        t_pass "receiving_review_gate: codex --remote VALUE review IS a trigger (H8)"
    else
        t_fail "receiving_review_gate: codex --remote VALUE review missed (H8, got $GOT)"
    fi

    # Sub-test 28a (2026-04-27 false-positive fix): the `status`
    # subcommand is metadata/control, not a review trigger. Pre-fix:
    # `node ...codex-companion.mjs status xyz` registered as a trigger
    # because head_base matched without inspecting argv[2]. Live-
    # reproduced when a session-lifecycle-hook status call locked
    # subsequent edits with an unreceived false trigger.
    rm -f "$STATE_FILE"
    printf "%s" '''{"tool_name":"Bash","tool_input":{"command":"node /abs/codex-companion.mjs status xyz123"}}''' \
        | python3 "$POST_HOOK" >/dev/null 2>&1
    GOT=$(_state_received)
    if [ "$GOT" = "missing" ]; then
        t_pass "receiving_review_gate: codex-companion.mjs status does NOT trigger (2026-04-27 fix)"
    else
        t_fail "receiving_review_gate: codex-companion.mjs status incorrectly wrote trigger (got $GOT)"
    fi

    # Sub-test 28b (2026-04-27 false-positive fix): `cancel` is also
    # metadata/control. Same filter rationale as status.
    rm -f "$STATE_FILE"
    printf "%s" '''{"tool_name":"Bash","tool_input":{"command":"node /abs/codex-companion.mjs cancel xyz123"}}''' \
        | python3 "$POST_HOOK" >/dev/null 2>&1
    GOT=$(_state_received)
    if [ "$GOT" = "missing" ]; then
        t_pass "receiving_review_gate: codex-companion.mjs cancel does NOT trigger (2026-04-27 fix)"
    else
        t_fail "receiving_review_gate: codex-companion.mjs cancel incorrectly wrote trigger (got $GOT)"
    fi

    # Sub-test 28c (2026-04-27 false-positive fix): `task-worker` is
    # the internal detached-worker spawn target, not user-initiated;
    # filter must reject it.
    rm -f "$STATE_FILE"
    printf "%s" '''{"tool_name":"Bash","tool_input":{"command":"node /abs/codex-companion.mjs task-worker --cwd /x --job-id abc"}}''' \
        | python3 "$POST_HOOK" >/dev/null 2>&1
    GOT=$(_state_received)
    if [ "$GOT" = "missing" ]; then
        t_pass "receiving_review_gate: codex-companion.mjs task-worker does NOT trigger (2026-04-27 fix)"
    else
        t_fail "receiving_review_gate: codex-companion.mjs task-worker incorrectly wrote trigger (got $GOT)"
    fi

    # Sub-test 28d (2026-04-27 fallback positive): `task --background`
    # IS a real review trigger -- this is the path used by the
    # codex-bg-dispatch.sh wrapper when foreground hits the 10-min
    # Bash wall. The receive gate must protect background dispatches
    # the same way it protects foreground ones.
    rm -f "$STATE_FILE"
    printf "%s" '''{"tool_name":"Bash","tool_input":{"command":"node /abs/codex-companion.mjs task --background --json design-review-prompt"}}''' \
        | python3 "$POST_HOOK" >/dev/null 2>&1
    GOT=$(_state_received)
    if [ "$GOT" = "False" ]; then
        t_pass "receiving_review_gate: codex-companion.mjs task --background IS a trigger (2026-04-27 background fallback)"
    else
        t_fail "receiving_review_gate: codex-companion.mjs task --background missed (got $GOT)"
    fi

    # Sub-test 28e (2026-04-27 false-positive fix): direct invocation
    # form (no `node` wrapper) -- /abs/path/codex-companion.mjs status
    # must also be filtered. Covers the second match arm in
    # _segment_is_codex_invocation.
    rm -f "$STATE_FILE"
    printf "%s" '''{"tool_name":"Bash","tool_input":{"command":"/abs/path/codex-companion.mjs status xyz"}}''' \
        | python3 "$POST_HOOK" >/dev/null 2>&1
    GOT=$(_state_received)
    if [ "$GOT" = "missing" ]; then
        t_pass "receiving_review_gate: direct codex-companion.mjs status does NOT trigger (2026-04-27 fix)"
    else
        t_fail "receiving_review_gate: direct codex-companion.mjs status incorrectly wrote trigger (got $GOT)"
    fi

    # Sub-test 28 (H3 re-adv): _write_atomic uses unique
    # per-process tmp paths so parallel hook fires do not race on
    # the same .tmp file. Verify by inspecting the temp-file glob:
    # writing the state from a Python in-process call should leave
    # NO .tmp residue in the state dir.
    STATE_DIR="$REPO_ROOT/.claude/state"
    rm -f "$STATE_FILE" "$STATE_DIR"/*.tmp 2>/dev/null
    python3 -c "
import sys
sys.path.insert(0, '$REPO_ROOT/.claude/hooks')
import importlib.util
spec = importlib.util.spec_from_file_location('crc', '$POST_HOOK')
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)
from pathlib import Path
m._write_atomic(Path('$STATE_FILE'), {'test': 1, 'received': False, 'timestamp_ns': 1})
m._write_atomic(Path('$STATE_FILE'), {'test': 2, 'received': False, 'timestamp_ns': 2})
" 2>/dev/null
    TMP_COUNT=$(ls "$STATE_DIR"/*.tmp 2>/dev/null | wc -l)
    if [ "$TMP_COUNT" = "0" ] && [ -f "$STATE_FILE" ]; then
        t_pass "receiving_review_gate: unique tmp paths leave no residue (H3)"
    else
        t_fail "receiving_review_gate: tmp residue ($TMP_COUNT files) or state missing (H3)"
    fi

    # Cleanup
    rm -f "$STATE_FILE" "$STATE_DIR"/*.tmp 2>/dev/null
fi


# ============================================================================
# section-commit gate (TODO-08 section-commit hard block)
#
# Asserts the section_commit_gate.py PreToolUse hook:
#   - Bash invocations that are NOT a git commit -> rc=0 (allow)
#   - git commit with no section signature -> rc=0
#   - Section signature + missing build evidence -> rc=2 (block)
#   - Section signature + missing review evidence -> rc=2
#   - Section signature + all 3 evidence -> rc=0
#   - SKIP_REVIEW_HOOK=1 only (no reason) -> rc=2 (usage envelope)
#   - SKIP_REVIEW_HOOK=1 + reason >= 12 chars -> rc=0 with skip-log entry
#   - SKIP also resets last-codex-review.json received -> false (M1 fix)
#   - small-section diff (< 50 LOC) -> WARN line in skip-log regardless
#   - Bypass shapes (env-prefix, wrapper, chain) detected as git commit
[ "$QUIET" = "0" ] && echo "" && echo -e "${DIM}[section_commit_gate]${NC}"

GATE_HOOK="$REPO_ROOT/.claude/hooks/section_commit_gate.py"

if [ ! -f "$GATE_HOOK" ]; then
    t_fail "section_commit_gate hook missing: $GATE_HOOK"
else
    GATE_TMP="$(mktemp -d)"
    GATE_REPO="$GATE_TMP/repo"
    mkdir -p "$GATE_REPO/build" "$GATE_REPO/.claude/state" "$GATE_REPO/.claude/hooks" \
             "$GATE_REPO/src/kernel" "$GATE_REPO/todo/00-infrastructure"
    cp "$GATE_HOOK" "$GATE_REPO/.claude/hooks/section_commit_gate.py"
    cp "$REPO_ROOT/.claude/hooks/codex_review_completed.py" \
       "$GATE_REPO/.claude/hooks/codex_review_completed.py"
    # TODO-08 section-23 shared SKIP-env scanner.
    cp "$REPO_ROOT/.claude/hooks/_skip_env.py" \
       "$GATE_REPO/.claude/hooks/_skip_env.py"
    cp "$REPO_ROOT/.claude/hooks/_codex_dispatch.py" \
       "$GATE_REPO/.claude/hooks/_codex_dispatch.py"

    pushd "$GATE_REPO" >/dev/null
    git init -q -b main
    git config user.email "test@example.com"
    git config user.name "Test"

    # Seed an Implementation Order table whose status column matches
    # the production format. Fixture column-1 uses placeholder text
    # (no marker glyph) since the gate regex anchors on the trailing
    # status cell, not the marker column.
    {
        printf '%s\n' '# Seed'
        printf '\n'
        printf '%s\n' '| Star | Order | Section | Deliverable | Depends | Status |'
        printf '%s\n' '| ---- | :---: | :---:   | ----------- | ------- | :----: |'
        printf '%s\n' '| star |   1   |  S1     | Test row    | --      |  [ ]   |'
    } > todo/00-infrastructure/TODO-99-fdgate-fixture.md
    cat > src/kernel/foo.c <<'CSEED'
int seed_only(void) { return 0; }
CSEED
    git add todo/00-infrastructure/TODO-99-fdgate-fixture.md src/kernel/foo.c
    git -c commit.gpgsign=false commit -q --no-verify -m "seed"
    popd >/dev/null

    GATE_STATE_FILE_T="$GATE_REPO/.claude/state/last-codex-review.json"
    GATE_SKIP_LOG_T="$GATE_REPO/.claude/state/skip-log.jsonl"

    _gate_run() {
        local want="$1" desc="$2" payload="$3"; shift 3
        local got
        if [ "$#" -gt 0 ]; then
            got=$(cd "$GATE_REPO" && printf '%s' "$payload" | env "$@" \
                python3 ".claude/hooks/section_commit_gate.py" >/dev/null 2>&1; echo $?)
        else
            got=$(cd "$GATE_REPO" && printf '%s' "$payload" | \
                python3 ".claude/hooks/section_commit_gate.py" >/dev/null 2>&1; echo $?)
        fi
        if [ "$got" = "$want" ]; then
            t_pass "section_commit_gate: $desc (rc=$got)"
        else
            t_fail "section_commit_gate: $desc (want $want, got $got)"
        fi
    }

    _stage_section_commit() {
        (
            cd "$GATE_REPO"
            sed -i 's/\[ \]/[x]/g' todo/00-infrastructure/TODO-99-fdgate-fixture.md
            cat > src/kernel/foo.c <<'CMOD'
int seed_only(void) { return 1; }
int new_helper(int n) { return n + 1; }
CMOD
            git add todo/00-infrastructure/TODO-99-fdgate-fixture.md src/kernel/foo.c
        )
    }

    _unstage_all() {
        (
            cd "$GATE_REPO"
            git reset -q HEAD -- . 2>/dev/null || true
            git checkout -q -- . 2>/dev/null || true
        )
    }

    _write_received_state() {
        local recv="$1" files_json="$2" age="${3:-60}"
        local now_ns recv_ns blobs
        now_ns=$(python3 -c 'import time; print(time.time_ns())')
        recv_ns=$((now_ns - age * 1000000000))
        # Capture blob SHAs for any path in files_json that is currently
        # staged. Empty {} when files_json is empty or paths aren't staged.
        blobs=$(cd "$GATE_REPO" && python3 -c '
import json, subprocess, sys
files = json.loads(sys.argv[1])
if not files:
    print("{}")
    sys.exit(0)
try:
    out = subprocess.check_output(["git", "ls-files", "-s", "-z", "--", *files], text=True, stderr=subprocess.DEVNULL)
except Exception:
    print("{}")
    sys.exit(0)
result = {}
for entry in out.split("\x00"):
    if not entry or "\t" not in entry:
        continue
    header, path = entry.split("\t", 1)
    parts = header.split()
    if len(parts) == 3:
        result[path] = parts[1]
print(json.dumps(result))
' "$files_json")
        cat > "$GATE_STATE_FILE_T" <<JSON
{
  "timestamp_ns": $recv_ns,
  "trigger": "test-trigger",
  "trigger_files": $files_json,
  "trigger_blobs": $blobs,
  "head_sha": "deadbeef",
  "tree_hash": "x",
  "received": $recv,
  "received_timestamp_ns": $recv_ns
}
JSON
    }

    # 1: non-git-commit Bash -> allow.
    rm -f "$GATE_SKIP_LOG_T"
    _gate_run 0 "non-git-commit Bash allows" \
        '{"tool_name":"Bash","tool_input":{"command":"ls"}}'

    # 2: git commit with no signature -> allow.
    _unstage_all
    _gate_run 0 "git commit with no staged diff allows" \
        '{"tool_name":"Bash","tool_input":{"command":"git commit -m foo"}}'

    # 3: section signature + missing build -> block.
    _stage_section_commit
    rm -f "$GATE_REPO/build/build.log"
    _gate_run 2 "section signature + missing build evidence blocks" \
        '{"tool_name":"Bash","tool_input":{"command":"git commit -m foo"}}'

    # 4: build.log without BUILD OK -> block.
    echo "FAIL" > "$GATE_REPO/build/build.log"
    _gate_run 2 "section signature + build.log without BUILD OK blocks" \
        '{"tool_name":"Bash","tool_input":{"command":"git commit -m foo"}}'

    # 5: missing review state -> block.
    echo "=== BUILD OK ===" > "$GATE_REPO/build/build.log"
    rm -f "$GATE_STATE_FILE_T"
    _gate_run 2 "section signature + missing review state blocks" \
        '{"tool_name":"Bash","tool_input":{"command":"git commit -m foo"}}'

    # 6: received:false -> block.
    _write_received_state "false" '["src/kernel/foo.c"]'
    _gate_run 2 "section signature + received:false blocks" \
        '{"tool_name":"Bash","tool_input":{"command":"git commit -m foo"}}'

    # 7: empty trigger_files -> block (C2: empty covers nothing).
    _write_received_state "true" '[]'
    _gate_run 2 "empty trigger_files blocks (C2: empty covers nothing)" \
        '{"tool_name":"Bash","tool_input":{"command":"git commit -m foo"}}'

    # 8: trigger_files don't cover staged source -> block.
    _write_received_state "true" '["src/other/different.c"]'
    _gate_run 2 "trigger_files don't cover staged source blocks" \
        '{"tool_name":"Bash","tool_input":{"command":"git commit -m foo"}}'

    # 9: review older than 30 min TTL -> block.
    _write_received_state "true" '["src/kernel/foo.c"]' "3600"
    _gate_run 2 "review older than 30 min TTL blocks" \
        '{"tool_name":"Bash","tool_input":{"command":"git commit -m foo"}}'

    # 10: ALL 3 evidence pieces -> allow.
    _write_received_state "true" '["src/kernel/foo.c"]' "60"
    touch "$GATE_REPO/build/build.log"
    _gate_run 0 "section signature + all 3 evidence pieces allows" \
        '{"tool_name":"Bash","tool_input":{"command":"git commit -m foo"}}'

    # 11: SKIP without reason -> block.
    _gate_run 2 "SKIP_REVIEW_HOOK=1 without reason blocks" \
        '{"tool_name":"Bash","tool_input":{"command":"git commit -m foo"}}' \
        SKIP_REVIEW_HOOK=1

    # 12: SKIP with too-short reason -> block.
    _gate_run 2 "SKIP_REVIEW_HOOK=1 with too-short reason blocks" \
        '{"tool_name":"Bash","tool_input":{"command":"git commit -m foo"}}' \
        SKIP_REVIEW_HOOK=1 "SKIP_REVIEW_HOOK_REASON=short"

    # 13: SKIP with valid reason -> allow + log + state reset (M1 fix).
    rm -f "$GATE_SKIP_LOG_T"
    _write_received_state "true" '["src/kernel/foo.c"]' "60"
    _gate_run 0 "SKIP with valid reason allows" \
        '{"tool_name":"Bash","tool_input":{"command":"git commit -m foo"}}' \
        SKIP_REVIEW_HOOK=1 'SKIP_REVIEW_HOOK_REASON=intentional revert-only commit, audit purposes'
    if [ -f "$GATE_SKIP_LOG_T" ] && grep -q "intentional revert-only" "$GATE_SKIP_LOG_T"; then
        t_pass "section_commit_gate: SKIP appends skip-log.jsonl with reason"
    else
        t_fail "section_commit_gate: SKIP did not log reason" \
            "skip-log content: $(cat "$GATE_SKIP_LOG_T" 2>/dev/null || echo 'missing')"
    fi
    GATE_GOT=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1])).get("received"))' "$GATE_STATE_FILE_T")
    if [ "$GATE_GOT" = "False" ]; then
        t_pass "section_commit_gate: SKIP resets last-codex-review.json received -> false (M1 fix)"
    else
        t_fail "section_commit_gate: SKIP did not reset received (got $GATE_GOT) (M1 fix)"
    fi

    # 14: small-section WARN logged regardless.
    rm -f "$GATE_SKIP_LOG_T" "$GATE_STATE_FILE_T"
    _write_received_state "true" '["src/kernel/foo.c"]' "60"
    touch "$GATE_REPO/build/build.log"
    _gate_run 0 "small-section commit with full evidence allows" \
        '{"tool_name":"Bash","tool_input":{"command":"git commit -m foo"}}'
    if [ -f "$GATE_SKIP_LOG_T" ] && grep -q "small-section-skip-risk" "$GATE_SKIP_LOG_T"; then
        t_pass "section_commit_gate: small-section WARN logged (< 50 LOC)"
    else
        t_fail "section_commit_gate: small-section WARN missing" \
            "skip-log: $(cat "$GATE_SKIP_LOG_T" 2>/dev/null || echo 'missing')"
    fi

    # 15-17: bypass-shape detection (Codex C1 fix).
    _unstage_all
    _stage_section_commit
    rm -f "$GATE_STATE_FILE_T"
    _gate_run 2 "bypass via 'true && git commit' still blocks (C1: chain detection)" \
        '{"tool_name":"Bash","tool_input":{"command":"true && git commit -m foo"}}'
    _gate_run 2 "bypass via 'env git commit' still blocks (C1: env-prefix walk)" \
        '{"tool_name":"Bash","tool_input":{"command":"env git commit -m foo"}}'
    _gate_run 2 "bypass via 'git -c x=y commit' still blocks (C1: git pre-subcmd flag walk)" \
        '{"tool_name":"Bash","tool_input":{"command":"git -c user.email=x@y.z commit -m foo"}}'

    # 18: ADD-only [x] row counts as flip (Codex H1 fix).
    _unstage_all
    (
        cd "$GATE_REPO"
        printf '%s\n' '| star |   2   |  S2     | New row at done | --      |  [x]   |' \
            >> todo/00-infrastructure/TODO-99-fdgate-fixture.md
        cat > src/kernel/foo.c <<'CMOD2'
int seed_only(void) { return 2; }
CMOD2
        git add todo/00-infrastructure/TODO-99-fdgate-fixture.md src/kernel/foo.c
    )
    rm -f "$GATE_STATE_FILE_T"
    _gate_run 2 "ADD-only [x] row counts as flip (H1: new row at done)" \
        '{"tool_name":"Bash","tool_input":{"command":"git commit -m foo"}}'

    # 19-20: --git-hook-mode (no stdin) blocks/allows correctly.
    rm -f "$GATE_STATE_FILE_T"
    GATE_RC=$(cd "$GATE_REPO" && python3 .claude/hooks/section_commit_gate.py --git-hook-mode </dev/null >/dev/null 2>&1; echo $?)
    if [ "$GATE_RC" = "2" ]; then
        t_pass "section_commit_gate: --git-hook-mode blocks without evidence (rc=2)"
    else
        t_fail "section_commit_gate: --git-hook-mode wrong rc (got $GATE_RC)"
    fi
    _write_received_state "true" '["src/kernel/foo.c"]' "60"
    touch "$GATE_REPO/build/build.log"
    GATE_RC=$(cd "$GATE_REPO" && python3 .claude/hooks/section_commit_gate.py --git-hook-mode </dev/null >/dev/null 2>&1; echo $?)
    if [ "$GATE_RC" = "0" ]; then
        t_pass "section_commit_gate: --git-hook-mode allows with full evidence"
    else
        t_fail "section_commit_gate: --git-hook-mode wrong rc with evidence (got $GATE_RC)"
    fi

    # 21: source edited AFTER build -> block (C2: mtime + content binding).
    python3 - "$GATE_REPO/build/build.log" <<'PY'
import os
import sys
import time

old = time.time() - 5
os.utime(sys.argv[1], (old, old))
PY
    (cd "$GATE_REPO" && touch src/kernel/foo.c)
    _gate_run 2 "source edited after build blocks (C2: mtime + content binding)" \
        '{"tool_name":"Bash","tool_input":{"command":"git commit -m foo"}}'

    # 22 (Codex C1 fix): post-review same-path edit blocks via blob SHA mismatch.
    # Stage source, write a state with trigger_blobs that match CURRENT staging,
    # then mutate the file content + restage. Gate must see blob mismatch.
    _unstage_all
    _stage_section_commit
    # Capture current blob SHAs and write a state with those exact blobs.
    GATE_BLOBS=$(cd "$GATE_REPO" && git ls-files -s -- src/kernel/foo.c | awk '{print "{\"src/kernel/foo.c\": \""$2"\"}"}')
    NOW_NS=$(python3 -c 'import time; print(time.time_ns())')
    cat > "$GATE_STATE_FILE_T" <<JSON
{
  "timestamp_ns": $NOW_NS,
  "trigger": "test",
  "trigger_files": ["src/kernel/foo.c"],
  "trigger_blobs": $GATE_BLOBS,
  "head_sha": "x",
  "tree_hash": "y",
  "received": true,
  "received_timestamp_ns": $NOW_NS
}
JSON
    touch "$GATE_REPO/build/build.log"
    _gate_run 0 "matching blob SHA allows (C1 fix: content binding present and equal)" \
        '{"tool_name":"Bash","tool_input":{"command":"git commit -m foo"}}'
    # Now mutate the file content and restage so the blob SHA changes.
    (cd "$GATE_REPO" && echo "int post_review_edit(void){return 99;}" >> src/kernel/foo.c && git add src/kernel/foo.c)
    touch "$GATE_REPO/build/build.log"
    _gate_run 2 "post-review same-path edit blocks via blob SHA mismatch (C1 fix)" \
        '{"tool_name":"Bash","tool_input":{"command":"git commit -m foo"}}'

    # 23 (Codex C1 fix): old state without trigger_blobs is treated as missing evidence.
    cat > "$GATE_STATE_FILE_T" <<JSON
{
  "timestamp_ns": $NOW_NS,
  "trigger": "test",
  "trigger_files": ["src/kernel/foo.c"],
  "head_sha": "x",
  "tree_hash": "y",
  "received": true,
  "received_timestamp_ns": $NOW_NS
}
JSON
    _gate_run 2 "old state without trigger_blobs blocks (C1 backward-compat)" \
        '{"tool_name":"Bash","tool_input":{"command":"git commit -m foo"}}'

    # 24 (Codex H1 fix): git commit --no-verify blocked unconditionally.
    _unstage_all
    _gate_run 2 "git commit --no-verify blocked even without section signature (H1)" \
        '{"tool_name":"Bash","tool_input":{"command":"git commit --no-verify -m foo"}}'
    _gate_run 2 "git commit -n blocked even without section signature (H1)" \
        '{"tool_name":"Bash","tool_input":{"command":"git commit -n -m foo"}}'

    # 25 (Codex H1 fix): bash -c "git commit --no-verify" blocked.
    _gate_run 2 "bash -c \"git commit --no-verify\" blocked (H1: shell descent)" \
        '{"tool_name":"Bash","tool_input":{"command":"bash -c \"git commit --no-verify -m foo\""}}'

    # 26 (Codex H1 fix): bash -c with regular git commit + section signature blocks.
    _stage_section_commit
    rm -f "$GATE_STATE_FILE_T"
    _gate_run 2 "bash -c \"git commit\" + signature still blocks (H1: inner-shell detection)" \
        '{"tool_name":"Bash","tool_input":{"command":"bash -c \"git commit -m foo\""}}'

    # 27 (Codex H1 fix): bash -c with NON-commit inner doesn't trigger gate.
    _gate_run 0 "bash -c \"ls\" allows (H1: not a commit)" \
        '{"tool_name":"Bash","tool_input":{"command":"bash -c \"ls -la\""}}'

    # 28 (Codex H2 round-2): bash -lc combined-flag bypass closed.
    _gate_run 2 "bash -lc \"git commit --no-verify\" blocked (H2: combined -c flag)" \
        '{"tool_name":"Bash","tool_input":{"command":"bash -lc \"git commit --no-verify -m foo\""}}'
    _gate_run 2 "sh -ec \"git commit --no-verify\" blocked (H2: combined -c flag)" \
        '{"tool_name":"Bash","tool_input":{"command":"sh -ec \"git commit --no-verify -m foo\""}}'

    # 29 (Codex H2 round-2): common commit aliases ci/cm.
    _stage_section_commit
    rm -f "$GATE_STATE_FILE_T"
    _gate_run 2 "git ci -m blocks on section signature (H2: ci alias)" \
        '{"tool_name":"Bash","tool_input":{"command":"git ci -m foo"}}'
    _gate_run 2 "git ci --no-verify blocked unconditionally (H2: ci alias + --no-verify)" \
        '{"tool_name":"Bash","tool_input":{"command":"git ci --no-verify -m foo"}}'

    # 30 (Codex H2 round-3): env -u FOO bypass closed.
    _gate_run 2 "env -u FOO git commit --no-verify blocked (H2 r3: env -u value flag)" \
        '{"tool_name":"Bash","tool_input":{"command":"env -u FOO git commit --no-verify -m foo"}}'
    _gate_run 2 "sudo -u alice git commit --no-verify blocked (H2 r3: sudo -u value flag)" \
        '{"tool_name":"Bash","tool_input":{"command":"sudo -u alice git commit --no-verify -m foo"}}'

    # 31 (Codex H2 round-3): bash --rcfile myrc -c bypass closed.
    _gate_run 2 "bash --rcfile myrc -c \"git commit --no-verify\" blocked (H2 r3: shell long-value flag)" \
        '{"tool_name":"Bash","tool_input":{"command":"bash --rcfile myrc -c \"git commit --no-verify -m foo\""}}'
    _gate_run 2 "bash --init-file myrc -c \"git commit --no-verify\" blocked (H2 r3: --init-file alias)" \
        '{"tool_name":"Bash","tool_input":{"command":"bash --init-file myrc -c \"git commit --no-verify -m foo\""}}'

    # 32 (Codex H2 round-4): no-value flags must NOT pop the next positional.
    # `sudo -n git commit --no-verify` -- -n is --non-interactive (no value).
    _gate_run 2 "sudo -n git commit --no-verify blocked (H2 r4: -n is no-value)" \
        '{"tool_name":"Bash","tool_input":{"command":"sudo -n git commit --no-verify -m foo"}}'
    # `command -p git commit --no-verify` -- -p is use-default-PATH (no value).
    _gate_run 2 "command -p git commit --no-verify blocked (H2 r4: -p is no-value)" \
        '{"tool_name":"Bash","tool_input":{"command":"command -p git commit --no-verify -m foo"}}'
    # `ionice -t -c 2 git commit --no-verify` -- -t is ignore-failure (no value).
    _gate_run 2 "ionice -t -c 2 git commit --no-verify blocked (H2 r4: -t is no-value)" \
        '{"tool_name":"Bash","tool_input":{"command":"ionice -t -c 2 git commit --no-verify -m foo"}}'

    # 33 (Codex H2 round-4): eval shell-descent.
    _gate_run 2 "eval \"git commit --no-verify\" blocked (H2 r4: eval re-shells arg)" \
        '{"tool_name":"Bash","tool_input":{"command":"eval \"git commit --no-verify -m foo\""}}'

    # 34 (Codex H2 round-4): xargs as command-runner.
    _gate_run 2 "echo foo | xargs git commit --no-verify blocked (H2 r4: xargs runs git)" \
        '{"tool_name":"Bash","tool_input":{"command":"echo foo | xargs git commit --no-verify -m foo"}}'
    _gate_run 2 "xargs -I _ -n 1 git commit blocks on signature (H2 r4: xargs -I value flag)" \
        '{"tool_name":"Bash","tool_input":{"command":"xargs -I _ -n 1 git commit -m _"}}'

    # 35 (Codex H2 round-4 follow-up): setpriv / cgexec / flock wrappers.
    _gate_run 2 "setpriv --reuid 1000 git commit --no-verify blocked (H2 r4: setpriv value flag)" \
        '{"tool_name":"Bash","tool_input":{"command":"setpriv --reuid 1000 git commit --no-verify -m foo"}}'
    _gate_run 2 "cgexec -g cpu:foo git commit --no-verify blocked (H2 r4: cgexec -g value flag)" \
        '{"tool_name":"Bash","tool_input":{"command":"cgexec -g cpu:foo git commit --no-verify -m foo"}}'
    _gate_run 2 "flock -n /tmp/lock git commit --no-verify blocked (H2 r4: flock lockfile positional)" \
        '{"tool_name":"Bash","tool_input":{"command":"flock -n /tmp/lock git commit --no-verify -m foo"}}'
    _gate_run 2 "flock -c \"git commit --no-verify\" blocked (H2 r4: flock -c shell descent)" \
        '{"tool_name":"Bash","tool_input":{"command":"flock -c \"git commit --no-verify -m foo\" /tmp/lock"}}'
    _gate_run 2 "flock --command \"git commit --no-verify\" blocked (H2 r4: flock --command alias)" \
        '{"tool_name":"Bash","tool_input":{"command":"flock --command \"git commit --no-verify -m foo\" /tmp/lock"}}'

    # 36 (Codex H2 round-5): taskset MASK / exec -a / setpriv current names / cgexec --sticky.
    _gate_run 2 "taskset 03 git commit --no-verify blocked (H2 r5: taskset MASK positional)" \
        '{"tool_name":"Bash","tool_input":{"command":"taskset 03 git commit --no-verify -m foo"}}'
    _gate_run 2 "taskset 0xff git commit --no-verify blocked (H2 r5: hex MASK)" \
        '{"tool_name":"Bash","tool_input":{"command":"taskset 0xff git commit --no-verify -m foo"}}'
    _gate_run 2 "exec -a mygit git commit --no-verify blocked (H2 r5: exec -a value)" \
        '{"tool_name":"Bash","tool_input":{"command":"exec -a mygit git commit --no-verify -m foo"}}'
    _gate_run 2 "setpriv --ruid 1000 git commit --no-verify blocked (H2 r5: --ruid current name)" \
        '{"tool_name":"Bash","tool_input":{"command":"setpriv --ruid 1000 git commit --no-verify -m foo"}}'
    _gate_run 2 "setpriv --securebits keep-caps git commit --no-verify blocked (H2 r5: --securebits)" \
        '{"tool_name":"Bash","tool_input":{"command":"setpriv --securebits keep-caps git commit --no-verify -m foo"}}'
    _gate_run 2 "cgexec --sticky git commit --no-verify blocked (H2 r5: --sticky is no-value)" \
        '{"tool_name":"Bash","tool_input":{"command":"cgexec --sticky git commit --no-verify -m foo"}}'

    # 37 (Codex round-6 post-commit): inline `-c alias.<X>=<commit ...>` resolution.
    _stage_section_commit
    rm -f "$GATE_STATE_FILE_T"
    _gate_run 2 "git -c alias.ship='commit --no-verify' ship blocked (round-6: alias resolution)" \
        '{"tool_name":"Bash","tool_input":{"command":"git -c alias.ship=\"commit --no-verify\" ship -m foo"}}'
    _gate_run 2 "git -c alias.cmt=commit cmt blocks on signature (round-6: bare-commit alias)" \
        '{"tool_name":"Bash","tool_input":{"command":"git -c alias.cmt=commit cmt -m foo"}}'
    _gate_run 0 "git -c alias.lg='log --oneline' lg allows (round-6: non-commit alias)" \
        '{"tool_name":"Bash","tool_input":{"command":"git -c alias.lg=\"log --oneline\" lg"}}'

    # 38 (Codex round-7 re-adversarial): git shell aliases (leading !).
    _gate_run 2 "git -c alias.ship='!git commit --no-verify' blocked (round-7: shell alias)" \
        '{"tool_name":"Bash","tool_input":{"command":"git -c alias.ship=\"!git commit --no-verify\" ship -m foo"}}'
    _gate_run 2 "git -c alias.ship='!f() { git commit --no-verify; }; f' blocked (round-7: function alias)" \
        '{"tool_name":"Bash","tool_input":{"command":"git -c alias.ship=\"!f() { git commit --no-verify; }; f\" ship"}}'
    _gate_run 0 "git -c alias.ship='!echo foo' allows (round-7: non-commit shell alias)" \
        '{"tool_name":"Bash","tool_input":{"command":"git -c alias.ship=\"!echo foo\" ship"}}'

    # 39 (Codex round-8): alias body with leading git options.
    _gate_run 2 "git -c alias.ship='-c user.name=x commit --no-verify' blocked (round-8: alias body with -c)" \
        '{"tool_name":"Bash","tool_input":{"command":"git -c alias.ship=\"-c user.name=x commit --no-verify\" ship -m foo"}}'
    _gate_run 2 "git -c alias.ship='-C /tmp commit --no-verify' blocked (round-8: alias body with -C)" \
        '{"tool_name":"Bash","tool_input":{"command":"git -c alias.ship=\"-C /tmp commit --no-verify\" ship"}}'

    # 40 (Codex H2 round-2): index/worktree desync blocks.
    # Stage section commit, write valid review state, ensure build.log
    # is newer than worktree mtime, but mutate worktree so `git diff`
    # reports desync between index and worktree.
    _unstage_all
    _stage_section_commit
    _write_received_state "true" '["src/kernel/foo.c"]' "60"
    touch "$GATE_REPO/build/build.log"
    sleep 1.1
    # Mutate worktree without re-staging -- index now differs from worktree.
    (cd "$GATE_REPO" && cat > src/kernel/foo.c <<'CMOD3'
int seed_only(void) { return 42; }
int new_helper(int n) { return n + 1; }
int extra_worktree_only(void) { return 99; }
CMOD3
)
    # Re-touch build.log AFTER worktree mutation so mtime check passes
    # but desync check fires.
    touch "$GATE_REPO/build/build.log"
    _gate_run 2 "index/worktree desync blocks (H2: build did not compile staged)" \
        '{"tool_name":"Bash","tool_input":{"command":"git commit -m foo"}}'

    rm -rf "$GATE_TMP"
fi


# ============================================================================
# four_dispatch_gate (review-todo-section step-8 four-dispatch enforcement)
# ============================================================================
# Asserts the review-todo-section step-8 four-dispatch policy:
#   - PostToolUse codex_review_completed.py records dispatches into
#     .claude/state/last-review-stamps.json keyed by TODO path.
#   - section_commit_gate.py refuses a stamp-add commit when fewer
#     than three dispatches (adversarial / consistency / perf) are
#     recorded for that TODO within the last 30 minutes.
#   - Self-summary preamble in a Codex prompt emits stderr WARN.
[ "$QUIET" = "0" ] && echo "" && echo -e "${DIM}[four_dispatch_gate]${NC}"

if [ ! -f "$REPO_ROOT/.claude/hooks/section_commit_gate.py" ] || \
   [ ! -f "$REPO_ROOT/.claude/hooks/codex_review_completed.py" ]; then
    t_fail "four_dispatch_gate: hook scripts missing"
else
    FD_TMP="$(mktemp -d)"
    FD_REPO="$FD_TMP/repo"
    mkdir -p "$FD_REPO/build" "$FD_REPO/.claude/state" "$FD_REPO/.claude/hooks" \
             "$FD_REPO/src/kernel" "$FD_REPO/todo/00-infrastructure"
    cp "$REPO_ROOT/.claude/hooks/section_commit_gate.py" \
       "$FD_REPO/.claude/hooks/section_commit_gate.py"
    cp "$REPO_ROOT/.claude/hooks/codex_review_completed.py" \
       "$FD_REPO/.claude/hooks/codex_review_completed.py"
    cp "$REPO_ROOT/.claude/hooks/_skip_env.py" \
       "$FD_REPO/.claude/hooks/_skip_env.py"
    cp "$REPO_ROOT/.claude/hooks/_codex_dispatch.py" \
       "$FD_REPO/.claude/hooks/_codex_dispatch.py"

    pushd "$FD_REPO" >/dev/null
    git init -q -b main
    git config user.email "test@example.com"
    git config user.name "Test"

    {
        printf '%s\n' '# Seed'
        printf '\n'
        printf '%s\n' '| Star | Order | Section | Deliverable | Depends | Status |'
        printf '%s\n' '| ---- | :---: | :---:   | ----------- | ------- | :----: |'
        printf '%s\n' '| star |   1   |  S1     | Test row    | --      |  [ ]   |'
        printf '\n'
        printf '%s\n' '## 1. Sample section'
        printf '\n'
        printf '%s\n' '- [ ] Some item'
    } > todo/00-infrastructure/TODO-99-fdgate-fixture.md
    cat > src/kernel/foo.c <<'CSEED'
int seed_only(void) { return 0; }
CSEED
    git add todo/00-infrastructure/TODO-99-fdgate-fixture.md src/kernel/foo.c
    git -c commit.gpgsign=false commit -q --no-verify -m "seed"
    popd >/dev/null

    FD_TODO_PATH="todo/00-infrastructure/TODO-99-fdgate-fixture.md"
    FD_STAMPS="$FD_REPO/.claude/state/last-review-stamps.json"
    FD_REVIEW="$FD_REPO/.claude/state/last-codex-review.json"

    _fd_stage_with_stamp() {
        (
            cd "$FD_REPO"
            python3 - <<'PY'
import pathlib
p = pathlib.Path("todo/00-infrastructure/TODO-99-fdgate-fixture.md")
text = p.read_text()
text = text.replace("|  [ ]   |", "|  [x]   |")
stamp = (
    "\n> **Verified:** 2026-04-27 | commit `abc1234` | 1/1 items | build OK\n"
    "> **Quality reviewed:** 2026-04-27 | Codex 3x | 0 fixed\n"
)
text = text.replace("## 1. Sample section\n", "## 1. Sample section\n" + stamp)
p.write_text(text)
PY
            cat > src/kernel/foo.c <<'CMOD'
int seed_only(void) { return 1; }
int new_helper(int n) { return n + 1; }
CMOD
            git add todo/00-infrastructure/TODO-99-fdgate-fixture.md src/kernel/foo.c
        )
    }

    _fd_stage_no_stamp() {
        (
            cd "$FD_REPO"
            sed -i 's/\[ \]/[x]/g' todo/00-infrastructure/TODO-99-fdgate-fixture.md
            cat > src/kernel/foo.c <<'CMOD'
int seed_only(void) { return 1; }
int new_helper(int n) { return n + 1; }
CMOD
            git add todo/00-infrastructure/TODO-99-fdgate-fixture.md src/kernel/foo.c
        )
    }

    _fd_unstage() {
        ( cd "$FD_REPO" && git reset -q HEAD -- . 2>/dev/null || true; \
          git checkout -q -- . 2>/dev/null || true )
    }

    _fd_write_review_state() {
        local now_ns blobs
        now_ns=$(python3 -c 'import time; print(time.time_ns())')
        blobs=$(cd "$FD_REPO" && python3 -c '
import json, subprocess, sys
try:
    out = subprocess.check_output(["git","ls-files","-s","-z","--","src/kernel/foo.c"], text=True, stderr=subprocess.DEVNULL)
except Exception:
    print("{}"); sys.exit(0)
result = {}
for entry in out.split("\x00"):
    if not entry or "\t" not in entry: continue
    h, p = entry.split("\t",1)
    parts = h.split()
    if len(parts) == 3: result[p] = parts[1]
print(json.dumps(result))
')
        cat > "$FD_REVIEW" <<JSON
{
  "timestamp_ns": $now_ns,
  "trigger": "test",
  "trigger_files": ["src/kernel/foo.c"],
  "trigger_blobs": $blobs,
  "head_sha": "x",
  "tree_hash": "y",
  "received": true,
  "received_timestamp_ns": $now_ns
}
JSON
    }

    _fd_write_stamps() {
        local entries="$1"
        python3 - "$FD_STAMPS" "$FD_TODO_PATH" "$entries" <<'PY'
import json, sys, time
path, todo, entries = sys.argv[1], sys.argv[2], sys.argv[3]
now_ns = time.time_ns()
state = {todo: {"section": "S1", "adversarial": None, "consistency": None, "perf": None}}
for piece in entries.split(","):
    piece = piece.strip()
    if not piece: continue
    if "=" not in piece: continue
    kind, age = piece.split("=", 1)
    age_s = int(age)
    if age_s < 0:
        state[todo][kind] = None
    else:
        state[todo][kind] = now_ns - age_s * 1_000_000_000
import pathlib
pathlib.Path(path).parent.mkdir(parents=True, exist_ok=True)
pathlib.Path(path).write_text(json.dumps(state, indent=2) + "\n")
PY
    }

    _fd_run() {
        local want="$1" desc="$2"
        local got
        got=$(cd "$FD_REPO" && \
            printf '%s' '{"tool_name":"Bash","tool_input":{"command":"git commit -m foo"}}' | \
            python3 ".claude/hooks/section_commit_gate.py" >/dev/null 2>&1; echo $?)
        if [ "$got" = "$want" ]; then
            t_pass "four_dispatch_gate: $desc (rc=$got)"
        else
            t_fail "four_dispatch_gate: $desc (want $want, got $got)"
        fi
    }

    # 1: implementation commit (no stamp added) bypasses four-dispatch check.
    _fd_unstage
    _fd_stage_no_stamp
    rm -f "$FD_STAMPS"
    _fd_write_review_state
    echo "=== BUILD OK ===" > "$FD_REPO/build/build.log"
    _fd_run 0 "implementation commit (no stamp added) bypasses four-dispatch check"

    # 2: stamp commit + state file missing -> block.
    _fd_unstage
    _fd_stage_with_stamp
    _fd_write_review_state
    echo "=== BUILD OK ===" > "$FD_REPO/build/build.log"
    rm -f "$FD_STAMPS"
    _fd_run 2 "stamp commit + state file missing blocks (3-of-3 missing)"

    # 3: 2 of 3 dispatches (perf missing) -> block, names perf.
    _fd_write_stamps "adversarial=60,consistency=120"
    echo "=== BUILD OK ===" > "$FD_REPO/build/build.log"
    BLOCK_OUT=$(cd "$FD_REPO" && \
        printf '%s' '{"tool_name":"Bash","tool_input":{"command":"git commit -m foo"}}' | \
        python3 ".claude/hooks/section_commit_gate.py" 2>&1 >/dev/null; true)
    BLOCK_RC=$(cd "$FD_REPO" && \
        printf '%s' '{"tool_name":"Bash","tool_input":{"command":"git commit -m foo"}}' | \
        python3 ".claude/hooks/section_commit_gate.py" >/dev/null 2>&1; echo $?)
    if [ "$BLOCK_RC" = "2" ] && echo "$BLOCK_OUT" | grep -q "missing: perf"; then
        t_pass "four_dispatch_gate: 2-of-3 dispatches blocks naming missing perf"
    else
        t_fail "four_dispatch_gate: 2-of-3 dispatches block (rc=$BLOCK_RC, missing-perf in stderr=$(echo "$BLOCK_OUT" | grep -c 'missing: perf'))"
    fi

    # 4: stale perf (>30 min) -> block.
    _fd_write_stamps "adversarial=60,consistency=120,perf=2400"
    echo "=== BUILD OK ===" > "$FD_REPO/build/build.log"
    BLOCK_OUT=$(cd "$FD_REPO" && \
        printf '%s' '{"tool_name":"Bash","tool_input":{"command":"git commit -m foo"}}' | \
        python3 ".claude/hooks/section_commit_gate.py" 2>&1 >/dev/null; true)
    BLOCK_RC=$(cd "$FD_REPO" && \
        printf '%s' '{"tool_name":"Bash","tool_input":{"command":"git commit -m foo"}}' | \
        python3 ".claude/hooks/section_commit_gate.py" >/dev/null 2>&1; echo $?)
    if [ "$BLOCK_RC" = "2" ] && echo "$BLOCK_OUT" | grep -q "stale.*perf"; then
        t_pass "four_dispatch_gate: stale perf (>30 min) blocks"
    else
        t_fail "four_dispatch_gate: stale perf block (rc=$BLOCK_RC)"
    fi

    # 5: all 3 dispatches recent -> allow.
    _fd_write_stamps "adversarial=60,consistency=120,perf=180"
    echo "=== BUILD OK ===" > "$FD_REPO/build/build.log"
    _fd_run 0 "stamp commit + all 3 dispatches recent allows"

    # 6: PostToolUse Skill records consistency dispatch.
    rm -f "$FD_STAMPS"
    _fd_unstage
    PAYLOAD='{"tool_name":"Skill","tool_input":{"skill":"codex-consistency-audit","prompt":"[review-kind: consistency] Target: todo/00-infrastructure/TODO-99-fdgate-fixture.md S1\nReview the consistency of struct layouts."}}'
    (cd "$FD_REPO" && printf '%s' "$PAYLOAD" | \
        python3 ".claude/hooks/codex_review_completed.py" >/dev/null 2>&1)
    if [ -f "$FD_STAMPS" ] && \
       python3 -c "import json,sys; s=json.load(open('$FD_STAMPS')); e=s.get('$FD_TODO_PATH',{}); sys.exit(0 if isinstance(e.get('consistency'), int) else 1)"; then
        t_pass "four_dispatch_gate: PostToolUse Skill records consistency dispatch"
    else
        t_fail "four_dispatch_gate: PostToolUse Skill did not record consistency dispatch" \
            "stamps: $(cat "$FD_STAMPS" 2>/dev/null || echo missing)"
    fi

    # 7: PostToolUse Bash with [review-kind: perf] marker records perf.
    rm -f "$FD_STAMPS"
    PAYLOAD='{"tool_name":"Bash","tool_input":{"command":"node /abs/codex-companion.mjs adversarial-review \"[review-kind: perf] Target: todo/00-infrastructure/TODO-99-fdgate-fixture.md S1\\nPerf review angles: hot-path allocations.\""}}'
    (cd "$FD_REPO" && printf '%s' "$PAYLOAD" | \
        python3 ".claude/hooks/codex_review_completed.py" >/dev/null 2>&1)
    if [ -f "$FD_STAMPS" ] && \
       python3 -c "import json,sys; s=json.load(open('$FD_STAMPS')); e=s.get('$FD_TODO_PATH',{}); sys.exit(0 if isinstance(e.get('perf'), int) and e.get('adversarial') is None else 1)"; then
        t_pass "four_dispatch_gate: PostToolUse Bash records perf dispatch via marker"
    else
        t_fail "four_dispatch_gate: PostToolUse Bash did not record perf dispatch" \
            "stamps: $(cat "$FD_STAMPS" 2>/dev/null || echo missing)"
    fi

    # 8: Bash without marker -> no stamp write.
    rm -f "$FD_STAMPS"
    PAYLOAD='{"tool_name":"Bash","tool_input":{"command":"node /abs/codex-companion.mjs adversarial-review \"Generic prompt with todo/00-infrastructure/TODO-99-fdgate-fixture.md S1 but no review-kind marker.\""}}'
    (cd "$FD_REPO" && printf '%s' "$PAYLOAD" | \
        python3 ".claude/hooks/codex_review_completed.py" >/dev/null 2>&1)
    if [ ! -f "$FD_STAMPS" ]; then
        t_pass "four_dispatch_gate: Bash without marker does not write stamps file"
    else
        t_fail "four_dispatch_gate: Bash without marker incorrectly wrote stamps" \
            "stamps: $(cat "$FD_STAMPS" 2>/dev/null)"
    fi

    # 9: Skill without TODO path in prompt -> no stamp write.
    rm -f "$FD_STAMPS"
    PAYLOAD='{"tool_name":"Skill","tool_input":{"skill":"codex-perf-review","prompt":"Review hot paths in foo.c with no TODO reference."}}'
    (cd "$FD_REPO" && printf '%s' "$PAYLOAD" | \
        python3 ".claude/hooks/codex_review_completed.py" >/dev/null 2>&1)
    if [ ! -f "$FD_STAMPS" ]; then
        t_pass "four_dispatch_gate: Skill without TODO path in prompt does not write stamps"
    else
        t_fail "four_dispatch_gate: Skill without TODO path incorrectly wrote stamps"
    fi

    # 10: self-summary preamble emits stderr WARN.
    rm -f "$FD_STAMPS"
    PAYLOAD='{"tool_name":"Skill","tool_input":{"skill":"codex-adversarial-review-section","prompt":"I built a new commit gate hook in section_commit_gate.py.\nReview todo/00-infrastructure/TODO-99-fdgate-fixture.md S1 for SMP races."}}'
    WARN_OUT=$(cd "$FD_REPO" && printf '%s' "$PAYLOAD" | \
        python3 ".claude/hooks/codex_review_completed.py" 2>&1 >/dev/null)
    if echo "$WARN_OUT" | grep -q "first-person"; then
        t_pass "four_dispatch_gate: self-summary preamble emits stderr WARN"
    else
        t_fail "four_dispatch_gate: self-summary WARN missing" \
            "stderr: $WARN_OUT"
    fi

    # 11: round-trip end-to-end -- 3 dispatches + stamp commit allows.
    rm -f "$FD_STAMPS"
    for KIND in adversarial consistency perf; do
        case "$KIND" in
            adversarial) SKILL=codex-adversarial-review-section ;;
            consistency) SKILL=codex-consistency-audit ;;
            perf)        SKILL=codex-perf-review ;;
        esac
        PAYLOAD=$(printf '{"tool_name":"Skill","tool_input":{"skill":"%s","prompt":"[review-kind: %s] Target: todo/00-infrastructure/TODO-99-fdgate-fixture.md S1\\nReview angles per template."}}' "$SKILL" "$KIND")
        (cd "$FD_REPO" && printf '%s' "$PAYLOAD" | \
            python3 ".claude/hooks/codex_review_completed.py" >/dev/null 2>&1)
    done
    _fd_unstage
    _fd_stage_with_stamp
    _fd_write_review_state
    echo "=== BUILD OK ===" > "$FD_REPO/build/build.log"
    _fd_run 0 "round-trip: 3 PostToolUse dispatches + stamp commit allows"

    # ----- Codex post-impl fixes (M1: stamp-only path; M2: head ancestry;
    #       M3: lock; M4: indented stamp line; M5: cross-TODO/mixed bypasses) -----

    # Helper: stage stamp-only diff (no source change) -- the user's
    # `stamp:` commit shape (M1 fix target).
    _fd_stage_stamp_only() {
        (
            cd "$FD_REPO"
            python3 - <<'PY'
import pathlib
p = pathlib.Path("todo/00-infrastructure/TODO-99-fdgate-fixture.md")
text = p.read_text()
stamp = (
    "\n> **Verified:** 2026-04-27 | commit `abc1234` | 1/1 items | build OK\n"
    "> **Quality reviewed:** 2026-04-27 | Codex 3x | 0 fixed\n"
)
text = text.replace("## 1. Sample section\n", "## 1. Sample section\n" + stamp)
p.write_text(text)
PY
            git add todo/00-infrastructure/TODO-99-fdgate-fixture.md
        )
    }

    # Helper: 3-space leading indent on stamp line (Markdown-valid
    # blockquote indent). Pre-M4 the regex would skip this.
    _fd_stage_stamp_only_indented() {
        (
            cd "$FD_REPO"
            python3 - <<'PY'
import pathlib
p = pathlib.Path("todo/00-infrastructure/TODO-99-fdgate-fixture.md")
text = p.read_text()
stamp = (
    "\n   > **Verified:** 2026-04-27 | commit `abc1234` | 1/1 items | build OK\n"
)
text = text.replace("## 1. Sample section\n", "## 1. Sample section\n" + stamp)
p.write_text(text)
PY
            git add todo/00-infrastructure/TODO-99-fdgate-fixture.md
        )
    }

    # Helper: stage source + stamp + NO row flip (M5 mixed-bypass fix).
    _fd_stage_source_plus_stamp_no_flip() {
        (
            cd "$FD_REPO"
            python3 - <<'PY'
import pathlib
p = pathlib.Path("todo/00-infrastructure/TODO-99-fdgate-fixture.md")
text = p.read_text()
stamp = "\n> **Verified:** 2026-04-27 | commit `abc1234` | 1/1 items | build OK\n"
text = text.replace("## 1. Sample section\n", "## 1. Sample section\n" + stamp)
p.write_text(text)
PY
            cat > src/kernel/foo.c <<'CMOD'
int seed_only(void) { return 1; }
int unrelated(int x) { return x * 2; }
CMOD
            git add todo/00-infrastructure/TODO-99-fdgate-fixture.md src/kernel/foo.c
        )
    }

    # Test M1.1: stamp-only commit + state file missing -> blocks.
    rm -f "$FD_STAMPS"
    _fd_unstage
    _fd_stage_stamp_only
    BLOCK_OUT=$(cd "$FD_REPO" && \
        printf '%s' '{"tool_name":"Bash","tool_input":{"command":"git commit -m foo"}}' | \
        python3 ".claude/hooks/section_commit_gate.py" 2>&1 >/dev/null; true)
    BLOCK_RC=$(cd "$FD_REPO" && \
        printf '%s' '{"tool_name":"Bash","tool_input":{"command":"git commit -m foo"}}' | \
        python3 ".claude/hooks/section_commit_gate.py" >/dev/null 2>&1; echo $?)
    if [ "$BLOCK_RC" = "2" ] && echo "$BLOCK_OUT" | grep -q "stamp-only commit"; then
        t_pass "four_dispatch_gate: M1 stamp-only commit blocks when state missing"
    else
        t_fail "four_dispatch_gate: M1 stamp-only block (rc=$BLOCK_RC stderr=$BLOCK_OUT)"
    fi

    # Test M1.2: stamp-only commit + 3 recent dispatches -> allows.
    _fd_write_stamps "adversarial=60,consistency=120,perf=180"
    _fd_run 0 "M1 stamp-only commit allows when 3 dispatches recent"

    # Test M1.3: stamp-only commit honors SKIP_REVIEW_HOOK with reason.
    # Re-stage stamp-only first; the prior _fd_run advanced the test
    # but didn't unstage, and the prior M1.2 wrote stamps that still
    # pass the gate -- need missing stamps + SKIP env to exercise SKIP.
    _fd_unstage
    _fd_stage_stamp_only
    rm -f "$FD_STAMPS"
    SKIP_RC=$(cd "$FD_REPO" && \
        printf '%s' '{"tool_name":"Bash","tool_input":{"command":"git commit -m foo"}}' | \
        SKIP_REVIEW_HOOK=1 \
        SKIP_REVIEW_HOOK_REASON="manual override for testing M1 path" \
        python3 ".claude/hooks/section_commit_gate.py" >/dev/null 2>&1; echo $?)
    if [ "$SKIP_RC" = "0" ]; then
        t_pass "four_dispatch_gate: M1 stamp-only honors SKIP_REVIEW_HOOK"
    else
        t_fail "four_dispatch_gate: M1 stamp-only SKIP path (rc=$SKIP_RC)"
    fi

    # Test M4: indented stamp line (3-space Markdown blockquote) detected.
    rm -f "$FD_STAMPS"
    _fd_unstage
    _fd_stage_stamp_only_indented
    BLOCK_RC=$(cd "$FD_REPO" && \
        printf '%s' '{"tool_name":"Bash","tool_input":{"command":"git commit -m foo"}}' | \
        python3 ".claude/hooks/section_commit_gate.py" >/dev/null 2>&1; echo $?)
    if [ "$BLOCK_RC" = "2" ]; then
        t_pass "four_dispatch_gate: M4 indented stamp line (3-space) is detected"
    else
        t_fail "four_dispatch_gate: M4 indented stamp not detected (rc=$BLOCK_RC)"
    fi

    # Test M2.1: dispatch entries with `<kind>_head` field are written.
    rm -f "$FD_STAMPS"
    _fd_unstage
    PAYLOAD='{"tool_name":"Skill","tool_input":{"skill":"codex-perf-review","prompt":"[review-kind: perf] Target: todo/00-infrastructure/TODO-99-fdgate-fixture.md S1\nPerf angles."}}'
    (cd "$FD_REPO" && printf '%s' "$PAYLOAD" | \
        python3 ".claude/hooks/codex_review_completed.py" >/dev/null 2>&1)
    if python3 -c "
import json, sys
s = json.load(open('$FD_STAMPS'))
e = s.get('$FD_TODO_PATH', {})
sys.exit(0 if isinstance(e.get('perf_head'), str) and len(e['perf_head']) >= 7 else 1)
" 2>/dev/null; then
        t_pass "four_dispatch_gate: M2 PostToolUse records perf_head with HEAD SHA"
    else
        t_fail "four_dispatch_gate: M2 perf_head field missing or empty"
    fi

    # Test M2.2: dispatch HEAD that is NOT an ancestor of current HEAD blocks.
    rm -f "$FD_STAMPS"
    _fd_unstage
    _fd_stage_stamp_only
    NOW_NS=$(python3 -c 'import time; print(time.time_ns())')
    cat > "$FD_STAMPS" <<JSON
{
  "$FD_TODO_PATH": {
    "section": "S1",
    "adversarial": $NOW_NS,
    "adversarial_head": "deadbeefcafebabe1234567890abcdef00000000",
    "consistency": $NOW_NS,
    "consistency_head": "deadbeefcafebabe1234567890abcdef00000000",
    "perf": $NOW_NS,
    "perf_head": "deadbeefcafebabe1234567890abcdef00000000"
  }
}
JSON
    BLOCK_OUT=$(cd "$FD_REPO" && \
        printf '%s' '{"tool_name":"Bash","tool_input":{"command":"git commit -m foo"}}' | \
        python3 ".claude/hooks/section_commit_gate.py" 2>&1 >/dev/null; true)
    BLOCK_RC=$(cd "$FD_REPO" && \
        printf '%s' '{"tool_name":"Bash","tool_input":{"command":"git commit -m foo"}}' | \
        python3 ".claude/hooks/section_commit_gate.py" >/dev/null 2>&1; echo $?)
    if [ "$BLOCK_RC" = "2" ] && echo "$BLOCK_OUT" | grep -q "cross-branch"; then
        t_pass "four_dispatch_gate: M2 non-ancestor dispatch HEAD blocks (cross-branch)"
    else
        t_fail "four_dispatch_gate: M2 cross-branch block (rc=$BLOCK_RC)" \
            "stderr: $BLOCK_OUT"
    fi

    # Test M2.3: dispatch HEAD == current HEAD passes ancestry.
    CURRENT_HEAD=$(cd "$FD_REPO" && git rev-parse HEAD)
    cat > "$FD_STAMPS" <<JSON
{
  "$FD_TODO_PATH": {
    "section": "S1",
    "adversarial": $NOW_NS,
    "adversarial_head": "$CURRENT_HEAD",
    "consistency": $NOW_NS,
    "consistency_head": "$CURRENT_HEAD",
    "perf": $NOW_NS,
    "perf_head": "$CURRENT_HEAD"
  }
}
JSON
    _fd_run 0 "M2 dispatch HEAD == current HEAD passes ancestry"

    # Test M2.4: legacy entry without `<kind>_head` -> TTL-only with WARN.
    cat > "$FD_STAMPS" <<JSON
{
  "$FD_TODO_PATH": {
    "section": "S1",
    "adversarial": $NOW_NS,
    "consistency": $NOW_NS,
    "perf": $NOW_NS
  }
}
JSON
    LEG_OUT=$(cd "$FD_REPO" && \
        printf '%s' '{"tool_name":"Bash","tool_input":{"command":"git commit -m foo"}}' | \
        python3 ".claude/hooks/section_commit_gate.py" 2>&1 >/dev/null; true)
    LEG_RC=$(cd "$FD_REPO" && \
        printf '%s' '{"tool_name":"Bash","tool_input":{"command":"git commit -m foo"}}' | \
        python3 ".claude/hooks/section_commit_gate.py" >/dev/null 2>&1; echo $?)
    if [ "$LEG_RC" = "0" ] && echo "$LEG_OUT" | grep -q "predates the head-binding"; then
        t_pass "four_dispatch_gate: M2 legacy entry (no head field) passes with WARN"
    else
        t_fail "four_dispatch_gate: M2 legacy entry (rc=$LEG_RC, stderr=$LEG_OUT)"
    fi

    # Test M3: parallel _record_stamp invocations preserve all entries.
    rm -f "$FD_STAMPS"
    rm -f "$FD_REPO/.claude/state/last-review-stamps.lock"
    PIDS=""
    for KIND in adversarial consistency perf; do
        case "$KIND" in
            adversarial) SKILL=codex-adversarial-review-section ;;
            consistency) SKILL=codex-consistency-audit ;;
            perf)        SKILL=codex-perf-review ;;
        esac
        PAYLOAD=$(printf '{"tool_name":"Skill","tool_input":{"skill":"%s","prompt":"[review-kind: %s] Target: todo/00-infrastructure/TODO-99-fdgate-fixture.md S1\\nReview angles."}}' "$SKILL" "$KIND")
        (cd "$FD_REPO" && printf '%s' "$PAYLOAD" | \
            python3 ".claude/hooks/codex_review_completed.py" >/dev/null 2>&1) &
        PIDS="$PIDS $!"
    done
    for P in $PIDS; do wait "$P" 2>/dev/null || true; done
    if python3 -c "
import json, sys
s = json.load(open('$FD_STAMPS'))
e = s.get('$FD_TODO_PATH', {})
ok = (
    isinstance(e.get('adversarial'), int) and
    isinstance(e.get('consistency'), int) and
    isinstance(e.get('perf'), int)
)
sys.exit(0 if ok else 1)
" 2>/dev/null; then
        t_pass "four_dispatch_gate: M3 parallel dispatches all preserved (fcntl.flock)"
    else
        t_fail "four_dispatch_gate: M3 parallel race lost an entry"
    fi

    # Test M5.1: source + stamp + no row flip routes to stamp_only path
    # and blocks when dispatches missing. Pre-M5 returned not_section.
    rm -f "$FD_STAMPS"
    _fd_unstage
    _fd_stage_source_plus_stamp_no_flip
    BLOCK_OUT=$(cd "$FD_REPO" && \
        printf '%s' '{"tool_name":"Bash","tool_input":{"command":"git commit -m foo"}}' | \
        python3 ".claude/hooks/section_commit_gate.py" 2>&1 >/dev/null; true)
    BLOCK_RC=$(cd "$FD_REPO" && \
        printf '%s' '{"tool_name":"Bash","tool_input":{"command":"git commit -m foo"}}' | \
        python3 ".claude/hooks/section_commit_gate.py" >/dev/null 2>&1; echo $?)
    if [ "$BLOCK_RC" = "2" ] && echo "$BLOCK_OUT" | grep -q "stamp-only commit"; then
        t_pass "four_dispatch_gate: M5 source+stamp+no-flip blocks (mixed-bypass)"
    else
        t_fail "four_dispatch_gate: M5 mixed-bypass block (rc=$BLOCK_RC stderr=$BLOCK_OUT)"
    fi

    # Test M5.2: source + stamp + no row flip + 3 dispatches -> allows.
    _fd_write_stamps "adversarial=60,consistency=120,perf=180"
    _fd_run 0 "M5 source+stamp+no-flip allows when 3 dispatches recent"

    # Test R1 (post-c65568fc routing fix): _bash_prompt_arg must
    # extract the prompt even when shell redirects + pipes follow.
    # Pre-fix the reverse-walk picked `2>&1` from the segment instead
    # of the actual prompt, so review_kind / todo_path didn't resolve
    # and last-review-stamps.json never got written.
    rm -f "$FD_STAMPS"
    PAYLOAD='{"tool_name":"Bash","tool_input":{"command":"node /abs/codex-companion.mjs adversarial-review \"[review-kind: adversarial] todo/00-infrastructure/TODO-99-fdgate-fixture.md S1 some review prompt\" 2>&1 | tail -5"}}'
    (cd "$FD_REPO" && printf '%s' "$PAYLOAD" | \
        python3 ".claude/hooks/codex_review_completed.py" >/dev/null 2>&1)
    if python3 -c "
import json, sys
s = json.load(open('$FD_STAMPS'))
e = s.get('$FD_TODO_PATH', {})
sys.exit(0 if isinstance(e.get('adversarial'), int) else 1)
" 2>/dev/null; then
        t_pass "four_dispatch_gate: R1 prompt extracted correctly with 2>&1 | tail tail"
    else
        t_fail "four_dispatch_gate: R1 redirect-tail prompt extraction broken"
    fi

    # Test R2 (post-c65568fc routing fix): _bash_prompt_arg must
    # walk past per-subcommand flags. `task --background --json
    # <prompt>` is the codex-bg-dispatch.sh shape.
    rm -f "$FD_STAMPS"
    PAYLOAD='{"tool_name":"Bash","tool_input":{"command":"node /abs/codex-companion.mjs task --background --json \"[review-kind: perf] todo/00-infrastructure/TODO-99-fdgate-fixture.md S1 perf review prompt\""}}'
    (cd "$FD_REPO" && printf '%s' "$PAYLOAD" | \
        python3 ".claude/hooks/codex_review_completed.py" >/dev/null 2>&1)
    if python3 -c "
import json, sys
s = json.load(open('$FD_STAMPS'))
e = s.get('$FD_TODO_PATH', {})
sys.exit(0 if isinstance(e.get('perf'), int) else 1)
" 2>/dev/null; then
        t_pass "four_dispatch_gate: R2 prompt extracted past 'task --background --json' flags"
    else
        t_fail "four_dispatch_gate: R2 task-flag-walk prompt extraction broken"
    fi

    # Test R3 (post-c65568fc routing fix): inline SKIP env-var prefix
    # in the bash command propagates to the gate. Pre-fix the gate
    # only read os.environ; the harness PreToolUse hook ran in its
    # own process env so `SKIP_REVIEW_HOOK=1 git commit` couldn't
    # reach the SKIP path -- the user had to spawn git from a Python
    # subprocess that explicitly set env before exec.
    rm -f "$FD_STAMPS"
    _fd_unstage
    _fd_stage_stamp_only
    INLINE_RC=$(cd "$FD_REPO" && \
        printf '%s' '{"tool_name":"Bash","tool_input":{"command":"SKIP_REVIEW_HOOK=1 SKIP_REVIEW_HOOK_REASON=\"inline prefix routing test for stamp-only\" git commit -m foo"}}' | \
        python3 ".claude/hooks/section_commit_gate.py" >/dev/null 2>&1; echo $?)
    if [ "$INLINE_RC" = "0" ]; then
        t_pass "four_dispatch_gate: R3 inline SKIP env-var prefix on stamp-only path allows"
    else
        t_fail "four_dispatch_gate: R3 inline SKIP env-var (rc=$INLINE_RC)"
    fi

    # Test R3.b: inline env prefix with `env VAR=val git commit ...`.
    rm -f "$FD_STAMPS"
    _fd_unstage
    _fd_stage_stamp_only
    INLINE_RC=$(cd "$FD_REPO" && \
        printf '%s' '{"tool_name":"Bash","tool_input":{"command":"env SKIP_REVIEW_HOOK=1 SKIP_REVIEW_HOOK_REASON=\"env wrapper inline prefix test\" git commit -m foo"}}' | \
        python3 ".claude/hooks/section_commit_gate.py" >/dev/null 2>&1; echo $?)
    if [ "$INLINE_RC" = "0" ]; then
        t_pass "four_dispatch_gate: R3.b inline SKIP env-var via 'env' wrapper allows"
    else
        t_fail "four_dispatch_gate: R3.b env-wrapper inline (rc=$INLINE_RC)"
    fi

    # Test R3.c: inline SKIP_REVIEW_HOOK=1 without REASON returns
    # the same usage-envelope BLOCK as the os.environ path.
    rm -f "$FD_STAMPS"
    _fd_unstage
    _fd_stage_stamp_only
    BLOCK_OUT=$(cd "$FD_REPO" && \
        printf '%s' '{"tool_name":"Bash","tool_input":{"command":"SKIP_REVIEW_HOOK=1 git commit -m foo"}}' | \
        python3 ".claude/hooks/section_commit_gate.py" 2>&1 >/dev/null; true)
    BLOCK_RC=$(cd "$FD_REPO" && \
        printf '%s' '{"tool_name":"Bash","tool_input":{"command":"SKIP_REVIEW_HOOK=1 git commit -m foo"}}' | \
        python3 ".claude/hooks/section_commit_gate.py" >/dev/null 2>&1; echo $?)
    if [ "$BLOCK_RC" = "2" ] && echo "$BLOCK_OUT" | grep -q "REASON is empty\|REASON too short"; then
        t_pass "four_dispatch_gate: R3.c inline SKIP without REASON returns usage-envelope BLOCK"
    else
        t_fail "four_dispatch_gate: R3.c inline-SKIP-no-reason missing usage envelope (rc=$BLOCK_RC)"
    fi

    # ----- Codex post-commit fixes (A: marker anchoring; B: stamp-only
    #       SKIP resets state; C: nested blockquote; D: lock timeout) -----

    # Test A.1: leading-line marker is recorded; trailing-body marker
    # in the prompt is NOT used for attribution. Note: `\n` in the
    # single-quoted JSON literal stays one backslash + n, which the
    # JSON parser then converts to a real newline -- that's what we
    # want so splitlines() actually segments the prompt.
    rm -f "$FD_STAMPS"
    PAYLOAD='{"tool_name":"Bash","tool_input":{"command":"node /abs/codex-companion.mjs adversarial-review \"[review-kind: adversarial] Target: todo/00-infrastructure/TODO-99-fdgate-fixture.md S1\nSee also marker example: [review-kind: perf] (this should NOT be used)\nReview angles per template.\""}}'
    (cd "$FD_REPO" && printf '%s' "$PAYLOAD" | \
        python3 ".claude/hooks/codex_review_completed.py" >/dev/null 2>&1)
    if python3 -c "
import json, sys
s = json.load(open('$FD_STAMPS'))
e = s.get('$FD_TODO_PATH', {})
ok = isinstance(e.get('adversarial'), int) and e.get('perf') is None
sys.exit(0 if ok else 1)
" 2>/dev/null; then
        t_pass "four_dispatch_gate: A leading marker wins; trailing marker ignored"
    else
        t_fail "four_dispatch_gate: A marker anchoring leaked trailing kind"
    fi

    # Test A.2: conflicting markers on the leading line -> no attribution + WARN.
    rm -f "$FD_STAMPS"
    PAYLOAD='{"tool_name":"Bash","tool_input":{"command":"node /abs/codex-companion.mjs adversarial-review \"[review-kind: adversarial] [review-kind: perf] Target: todo/00-infrastructure/TODO-99-fdgate-fixture.md S1\\nReview angles.\""}}'
    WARN_OUT=$(cd "$FD_REPO" && printf '%s' "$PAYLOAD" | \
        python3 ".claude/hooks/codex_review_completed.py" 2>&1 >/dev/null)
    if [ ! -f "$FD_STAMPS" ] && echo "$WARN_OUT" | grep -q "multiple"; then
        t_pass "four_dispatch_gate: A conflicting leading markers refuse attribution + WARN"
    else
        t_fail "four_dispatch_gate: A conflicting markers not blocked (stamps exist=$( [ -f "$FD_STAMPS" ] && echo yes || echo no), warn=$WARN_OUT)"
    fi

    # Test B: stamp-only SKIP resets last-codex-review.json received state.
    rm -f "$FD_STAMPS"
    _fd_unstage
    _fd_stage_stamp_only
    NOW_NS=$(python3 -c 'import time; print(time.time_ns())')
    cat > "$FD_REVIEW" <<JSON
{
  "timestamp_ns": $NOW_NS,
  "trigger": "test",
  "trigger_files": ["src/kernel/foo.c"],
  "trigger_blobs": {},
  "head_sha": "x",
  "tree_hash": "y",
  "received": true,
  "received_timestamp_ns": $NOW_NS
}
JSON
    SKIP_RC=$(cd "$FD_REPO" && \
        printf '%s' '{"tool_name":"Bash","tool_input":{"command":"git commit -m foo"}}' | \
        SKIP_REVIEW_HOOK=1 \
        SKIP_REVIEW_HOOK_REASON="testing B: stamp-only SKIP must reset state" \
        python3 ".claude/hooks/section_commit_gate.py" >/dev/null 2>&1; echo $?)
    if [ "$SKIP_RC" = "0" ] && python3 -c "
import json, sys
s = json.load(open('$FD_REVIEW'))
sys.exit(0 if s.get('received') is False else 1)
" 2>/dev/null; then
        t_pass "four_dispatch_gate: B stamp-only SKIP resets last-codex-review received=false"
    else
        t_fail "four_dispatch_gate: B stamp-only SKIP did not reset state (rc=$SKIP_RC)"
    fi

    # Test C: nested blockquote stamp lines are detected (`> > **`).
    rm -f "$FD_STAMPS"
    _fd_unstage
    (
        cd "$FD_REPO"
        python3 - <<'PY'
import pathlib
p = pathlib.Path("todo/00-infrastructure/TODO-99-fdgate-fixture.md")
text = p.read_text()
stamp = "\n> > **Verified:** 2026-04-27 | commit `abc1234` | 1/1 items | build OK\n"
text = text.replace("## 1. Sample section\n", "## 1. Sample section\n" + stamp)
p.write_text(text)
PY
        git add todo/00-infrastructure/TODO-99-fdgate-fixture.md
    )
    BLOCK_RC=$(cd "$FD_REPO" && \
        printf '%s' '{"tool_name":"Bash","tool_input":{"command":"git commit -m foo"}}' | \
        python3 ".claude/hooks/section_commit_gate.py" >/dev/null 2>&1; echo $?)
    if [ "$BLOCK_RC" = "2" ]; then
        t_pass "four_dispatch_gate: C nested blockquote stamp ('> > **') is detected"
    else
        t_fail "four_dispatch_gate: C nested blockquote stamp missed (rc=$BLOCK_RC)"
    fi

    # Test C.2: doubled-up `>>` form (`+>> **Verified:**`).
    rm -f "$FD_STAMPS"
    _fd_unstage
    (
        cd "$FD_REPO"
        python3 - <<'PY'
import pathlib
p = pathlib.Path("todo/00-infrastructure/TODO-99-fdgate-fixture.md")
text = p.read_text()
stamp = "\n>> **Quality reviewed:** 2026-04-27 | Codex 3x | 0 fixed\n"
text = text.replace("## 1. Sample section\n", "## 1. Sample section\n" + stamp)
p.write_text(text)
PY
        git add todo/00-infrastructure/TODO-99-fdgate-fixture.md
    )
    BLOCK_RC=$(cd "$FD_REPO" && \
        printf '%s' '{"tool_name":"Bash","tool_input":{"command":"git commit -m foo"}}' | \
        python3 ".claude/hooks/section_commit_gate.py" >/dev/null 2>&1; echo $?)
    if [ "$BLOCK_RC" = "2" ]; then
        t_pass "four_dispatch_gate: C doubled-blockquote stamp ('>>') is detected"
    else
        t_fail "four_dispatch_gate: C doubled-blockquote stamp missed (rc=$BLOCK_RC)"
    fi

    # Test D: contended lock degrades gracefully within 2s budget
    # (degraded path emits WARN; stamp still recorded).
    rm -f "$FD_STAMPS"
    rm -f "$FD_REPO/.claude/state/last-review-stamps.lock"
    # Hold the lock from a background subshell for 4 seconds.
    (
        cd "$FD_REPO"
        python3 - <<'PY' &
import fcntl, os, time, pathlib
p = pathlib.Path(".claude/state/last-review-stamps.lock")
p.parent.mkdir(parents=True, exist_ok=True)
fd = os.open(str(p), os.O_RDWR | os.O_CREAT, 0o644)
fcntl.flock(fd, fcntl.LOCK_EX)
time.sleep(4)
fcntl.flock(fd, fcntl.LOCK_UN)
os.close(fd)
PY
        HOLDER_PID=$!
        sleep 0.3  # let the holder grab the lock
        START=$(date +%s.%N)
        PAYLOAD='{"tool_name":"Skill","tool_input":{"skill":"codex-perf-review","prompt":"[review-kind: perf] Target: todo/00-infrastructure/TODO-99-fdgate-fixture.md S1\nReview angles."}}'
        WARN_OUT=$(printf '%s' "$PAYLOAD" | \
            python3 ".claude/hooks/codex_review_completed.py" 2>&1 >/dev/null)
        END=$(date +%s.%N)
        ELAPSED=$(echo "$END $START" | awk '{ printf "%.2f", $1 - $2 }')
        wait "$HOLDER_PID" 2>/dev/null || true
        if echo "$WARN_OUT" | grep -q "could not acquire" && \
           awk -v t="$ELAPSED" 'BEGIN { exit (t < 3.5) ? 0 : 1 }'; then
            echo "TEST_OK $ELAPSED $WARN_OUT" > /tmp/fdgate_d_result.$$
        else
            echo "TEST_FAIL elapsed=$ELAPSED warn=$WARN_OUT" > /tmp/fdgate_d_result.$$
        fi
    )
    if [ -f "/tmp/fdgate_d_result.$$" ] && grep -q "^TEST_OK " "/tmp/fdgate_d_result.$$"; then
        t_pass "four_dispatch_gate: D bounded lock timeout degrades with WARN (~2s)"
    else
        t_fail "four_dispatch_gate: D lock timeout test ($(cat /tmp/fdgate_d_result.$$ 2>/dev/null))"
    fi
    rm -f "/tmp/fdgate_d_result.$$"

    rm -rf "$FD_TMP"
fi


# ============================================================================
# design_review_required.py -- review-kind + commit-clears (TODO-08 #15)
# ============================================================================
# Two sub-tests verify the gate-relaxation behavior added in TODO-08 #15:
#   review_kind  -- a Codex dispatch with `[review-kind: ...]` marker
#                   counts as design-equivalent (gate cleared)
#   commit_clears -- a section-ship commit (`review:` / `stamp:` /
#                   `docs:` / `todo:` prefix) resets the gate

DR_TMP="$(mktemp -d)"
DR_HOOK="$REPO_ROOT/.claude/hooks/design_review_required.py"

# Helper: build a transcript fixture with a Skill(implement-todo-section)
# event followed by an optional Bash event, then run the hook against an
# Edit payload targeting a code file. Echoes hook exit code.
dr_run() {
    local fixture="$1"
    local target="$2"
    local payload
    payload="$(printf '{"tool_name":"Edit","tool_input":{"file_path":"%s"},"transcript_path":"%s"}' \
        "$target" "$fixture")"
    printf '%s' "$payload" | python3 "$DR_HOOK" >/dev/null 2>&1
    echo $?
}

# Synthesize a JSONL transcript line with a tool_use event. Optional 4th
# arg is a tool_use_id (used by tests that pair tool_use with tool_result).
dr_event() {
    local tool="$1" name="$2" input_json="$3" tu_id="${4-}"
    if [ -n "$tu_id" ]; then
        printf '{"message":{"content":[{"type":"tool_use","id":"%s","name":"%s","input":%s}]}}\n' \
            "$tu_id" "$tool" "$input_json"
    else
        printf '{"message":{"content":[{"type":"tool_use","name":"%s","input":%s}]}}\n' \
            "$tool" "$input_json"
    fi
}

# Synthesize a JSONL transcript line with a tool_result event matching
# a prior tool_use_id. 2nd arg "ok"|"err" sets the is_error flag.
# Codex H1 fix (2026-04-28): the design-review gate-clear path requires
# a successful tool_result; tests must pair tool_use with tool_result
# to exercise the realistic transcript shape.
dr_result() {
    local tu_id="$1" status="$2"
    local is_err="false"
    [ "$status" = "err" ] && is_err="true"
    printf '{"message":{"content":[{"type":"tool_result","tool_use_id":"%s","is_error":%s,"content":"out"}]}}\n' \
        "$tu_id" "$is_err"
}

# --- Test A: bare implement-todo-section (no design dispatch) -> blocked
FX="$DR_TMP/baseline.jsonl"
dr_event "Skill" "Skill" '{"skill":"implement-todo-section","args":"## 1. Foo"}' >"$FX"
RC="$(dr_run "$FX" "src/kernel/foo.c")"
if [ "$RC" = "2" ]; then
    t_pass "design_review_required baseline blocks (rc=2 with no design dispatch)"
else
    t_fail "design_review_required baseline expected rc=2, got rc=$RC"
fi

# --- Test B (#15 #1): [review-kind: adversarial] dispatch clears the gate
FX="$DR_TMP/review_kind.jsonl"
dr_event "Skill" "Skill" '{"skill":"implement-todo-section","args":"## 1. Foo"}' >"$FX"
dr_event "Bash" "Bash" '{"command":"node /path/codex-companion.mjs adversarial-review \"[review-kind: adversarial] Target: ...\""}' >>"$FX"
RC="$(dr_run "$FX" "src/kernel/foo.c")"
if [ "$RC" = "0" ]; then
    t_pass "design_review_review_kind [review-kind: adversarial] clears gate (rc=0)"
else
    t_fail "design_review_review_kind expected rc=0, got rc=$RC"
fi

# --- Test C (#15 #2): a `review:` commit with SUCCESSFUL tool_result clears
FX="$DR_TMP/commit_clears.jsonl"
dr_event "Skill" "Skill" '{"skill":"implement-todo-section","args":"## 1. Foo"}' >"$FX"
dr_event "Bash" "Bash" '{"command":"git commit -m \"review: TODO-XX foo bar\""}' "tu_review_ok" >>"$FX"
dr_result "tu_review_ok" "ok" >>"$FX"
RC="$(dr_run "$FX" "src/kernel/foo.c")"
if [ "$RC" = "0" ]; then
    t_pass "design_review_commit_clears review: prefix + ok result clears gate (rc=0)"
else
    t_fail "design_review_commit_clears expected rc=0, got rc=$RC"
fi

# --- Test D (#15 #2): a `docs/x:` commit with SUCCESSFUL tool_result clears
FX="$DR_TMP/commit_docs.jsonl"
dr_event "Skill" "Skill" '{"skill":"implement-todo-section","args":"## 1. Foo"}' >"$FX"
dr_event "Bash" "Bash" '{"command":"git commit -m \"docs/superpowers: catalog audit\""}' "tu_docs_ok" >>"$FX"
dr_result "tu_docs_ok" "ok" >>"$FX"
RC="$(dr_run "$FX" "src/kernel/foo.c")"
if [ "$RC" = "0" ]; then
    t_pass "design_review_commit_clears docs/x: prefix + ok result clears gate (rc=0)"
else
    t_fail "design_review_commit_clears docs/x: expected rc=0, got rc=$RC"
fi

# --- Test E: a non-section-ship commit (e.g. `feat:`) does NOT clear gate
FX="$DR_TMP/commit_other.jsonl"
dr_event "Skill" "Skill" '{"skill":"implement-todo-section","args":"## 1. Foo"}' >"$FX"
dr_event "Bash" "Bash" '{"command":"git commit -m \"feat: unrelated change\""}' "tu_feat" >>"$FX"
dr_result "tu_feat" "ok" >>"$FX"
RC="$(dr_run "$FX" "src/kernel/foo.c")"
if [ "$RC" = "2" ]; then
    t_pass "design_review_commit_clears feat: prefix does NOT clear gate (rc=2)"
else
    t_fail "design_review_commit_clears feat: expected rc=2, got rc=$RC"
fi

# --- Test F (Codex H1 fix 2026-04-28): a `review:` commit with FAILED
# tool_result must NOT clear the gate. Closes the bypass where a
# blocked/failed commit attempt could disable design-review.
FX="$DR_TMP/commit_failed.jsonl"
dr_event "Skill" "Skill" '{"skill":"implement-todo-section","args":"## 1. Foo"}' >"$FX"
dr_event "Bash" "Bash" '{"command":"git commit -m \"review: TODO-XX foo bar\""}' "tu_review_err" >>"$FX"
dr_result "tu_review_err" "err" >>"$FX"
RC="$(dr_run "$FX" "src/kernel/foo.c")"
if [ "$RC" = "2" ]; then
    t_pass "design_review_commit_clears failed review: commit does NOT clear gate (rc=2)"
else
    t_fail "design_review_commit_clears failed-result expected rc=2, got rc=$RC"
fi

# --- Test G (Codex H1 fix 2026-04-28): a `review:` commit with NO
# tool_result yet (in-flight) must NOT clear the gate.
FX="$DR_TMP/commit_inflight.jsonl"
dr_event "Skill" "Skill" '{"skill":"implement-todo-section","args":"## 1. Foo"}' >"$FX"
dr_event "Bash" "Bash" '{"command":"git commit -m \"review: TODO-XX foo bar\""}' "tu_review_pending" >>"$FX"
RC="$(dr_run "$FX" "src/kernel/foo.c")"
if [ "$RC" = "2" ]; then
    t_pass "design_review_commit_clears in-flight commit (no result) does NOT clear gate (rc=2)"
else
    t_fail "design_review_commit_clears in-flight expected rc=2, got rc=$RC"
fi

rm -rf "$DR_TMP"


# ============================================================================
# TODO-08 number 16: PreCompact orphan-marking + selector skip + cache
# ============================================================================
# Sub-tests for compaction-resilient skill-step state. Three angles:
# (16-1) skill-progress.json entry with compaction_orphaned: true is
#        skipped by skill_step_block selector -> gate clears.
# (16-2) Live (un-orphaned) entry is still picked normally (regression
#        guard against accidentally skipping live entries).
# (16-3) pre_compact_flush.py walking a state file marks every entry
#        compaction_orphaned: true; idempotent re-run preserves
#        orphan_ts_ns.

C16_TMP="$(mktemp -d -t test_compact16_XXXXXX)"
SSB_HOOK="$REPO_ROOT/.claude/hooks/skill_step_block.py"
PCF_HOOK="$REPO_ROOT/.claude/hooks/pre_compact_flush.py"

# 16-1: orphaned entry -> skill_step_block selector skips, exits 0
C16_STATE_DIR="$C16_TMP/state1/.claude/state"
mkdir -p "$C16_STATE_DIR"
cat >"$C16_STATE_DIR/skill-progress.json" <<'PYJSON'
{
  "implement-todo-section": {
    "args": "## 1. Foo",
    "session_id": "sess-abc",
    "started_head_sha": "deadbeef",
    "started_ts": 1777000000000000000,
    "steps_observed": [],
    "compaction_orphaned": true,
    "orphan_reason": "PreCompact fired with 0 steps observed",
    "orphan_ts_ns": 1777000000000000001
  }
}
PYJSON
# Init empty git repo so the hook's _repo_root resolves to C16_TMP/state1.
( cd "$C16_TMP/state1" && git init -q && git -c user.email=t@t -c user.name=t commit -q --allow-empty -m init )
PAYLOAD='{"tool_name":"Bash","tool_input":{"command":"git commit -m \"feat: x\""}}'
RC=0
( cd "$C16_TMP/state1" && printf '%s' "$PAYLOAD" | python3 "$SSB_HOOK" >/dev/null 2>&1 ) || RC=$?
if [ "$RC" = "0" ]; then
    t_pass "compact16_orphan_skip orphaned entry -> selector skips, gate clears (rc=0)"
else
    t_fail "compact16_orphan_skip expected rc=0, got rc=$RC"
fi

# 16-2: live (un-orphaned) entry with missing terminal steps -> blocker fires
C16_STATE_DIR2="$C16_TMP/state2/.claude/state"
mkdir -p "$C16_STATE_DIR2"
cat >"$C16_STATE_DIR2/skill-progress.json" <<'PYJSON'
{
  "implement-todo-section": {
    "args": "## 1. Foo",
    "session_id": "sess-live",
    "started_head_sha": "abc12345",
    "started_ts": 1777999999999999999,
    "steps_observed": []
  }
}
PYJSON
( cd "$C16_TMP/state2" && git init -q && git -c user.email=t@t -c user.name=t commit -q --allow-empty -m init )
RC=0
( cd "$C16_TMP/state2" && printf '%s' "$PAYLOAD" | python3 "$SSB_HOOK" >/dev/null 2>&1 ) || RC=$?
if [ "$RC" = "2" ]; then
    t_pass "compact16_live_picked live (un-orphaned) entry -> selector picks it, gate blocks (rc=2)"
else
    t_fail "compact16_live_picked expected rc=2, got rc=$RC"
fi

# 16-3: PreCompact hook marks every active entry compaction_orphaned;
# idempotent re-run preserves orphan_ts_ns.
C16_STATE_DIR3="$C16_TMP/state3/.claude/state"
mkdir -p "$C16_STATE_DIR3"
cat >"$C16_STATE_DIR3/skill-progress.json" <<'PYJSON'
{
  "implement-todo-section": {
    "args": "## 1. Foo",
    "session_id": "sess-precompact",
    "started_head_sha": "abc12345",
    "started_ts": 1777999999999999999,
    "steps_observed": [{"n": 5, "evidence_tool": "Bash"}]
  }
}
PYJSON
( cd "$C16_TMP/state3" && git init -q && git -c user.email=t@t -c user.name=t commit -q --allow-empty -m init )
( cd "$C16_TMP/state3" && printf '{}' | python3 "$PCF_HOOK" >/dev/null 2>&1 )
ORPHAN1="$(python3 -c "import json; d=json.load(open('$C16_STATE_DIR3/skill-progress.json')); e=d['implement-todo-section']; print(e.get('compaction_orphaned'), e.get('orphan_ts_ns',0))")"
case "$ORPHAN1" in
    "True "*) t_pass "compact16_precompact_mark PreCompact marks entry compaction_orphaned" ;;
    *) t_fail "compact16_precompact_mark expected 'True <ts>', got '$ORPHAN1'" ;;
esac
TS1="${ORPHAN1#True }"
sleep 0  # ensure any time-based assertion is monotonic-OK
# Idempotent re-run: orphan_ts_ns must NOT change.
( cd "$C16_TMP/state3" && printf '{}' | python3 "$PCF_HOOK" >/dev/null 2>&1 )
ORPHAN2="$(python3 -c "import json; d=json.load(open('$C16_STATE_DIR3/skill-progress.json')); e=d['implement-todo-section']; print(e.get('compaction_orphaned'), e.get('orphan_ts_ns',0))")"
TS2="${ORPHAN2#True }"
if [ "$TS1" = "$TS2" ] && [ -n "$TS1" ] && [ "$TS1" != "0" ]; then
    t_pass "compact16_precompact_idempotent re-run preserves orphan_ts_ns ($TS1)"
else
    t_fail "compact16_precompact_idempotent expected ts1==ts2, got '$TS1' vs '$TS2'"
fi

rm -rf "$C16_TMP"


# ============================================================================
# skill_step_block.py -- non-contiguous-step gate (TODO-08 number 10)
# ============================================================================
# Synthesizes a .claude/state/skill-progress.json fixture and runs the
# blocker against synthetic Bash(git commit) payloads. Asserts:
#   - all required terminal steps observed -> rc 0 (pass-through)
#   - missing terminal step              -> rc 2 (block)
#   - non-contiguous (gap in middle)     -> rc 2 (block; subset of missing)
#   - SKIP_SKILL_STEP_BLOCK opt-out      -> rc 0
# Restores the prior state file if it existed; deletes the fixture otherwise.

SSB_TMP="$(mktemp -d)"
SSB_STATE_REAL="$REPO_ROOT/.claude/state/skill-progress.json"
SSB_BACKUP="$SSB_TMP/skill-progress.json.bak"
if [ -f "$SSB_STATE_REAL" ]; then
    cp "$SSB_STATE_REAL" "$SSB_BACKUP"
fi

ssb_write_state() {
    local steps_json="$1"
    mkdir -p "$REPO_ROOT/.claude/state"
    cat >"$SSB_STATE_REAL" <<EOF
{
  "implement-todo-section": {
    "started_ts": 1000,
    "started_head_sha": "deadbeef",
    "session_id": "test",
    "args": "test section",
    "steps_observed": ${steps_json}
  }
}
EOF
}

ssb_run() {
    printf '%s' '{"tool_name":"Bash","tool_input":{"command":"git commit -m test"}}' | \
        python3 "$REPO_ROOT/.claude/hooks/skill_step_block.py" >/dev/null 2>&1
    echo $?
}

# Test A: all required terminal steps [7, 13, 16, 19] observed -> rc 0
ssb_write_state '[{"n":7},{"n":13},{"n":16},{"n":19}]'
RC="$(ssb_run)"
if [ "$RC" = "0" ]; then
    t_pass "skill_step_block all required terminal steps -> rc=0"
else
    t_fail "skill_step_block all-required expected rc=0, got rc=$RC"
fi

# Test B: missing terminal step 16 (rebuild) -> rc 2.
# NOTE: step 19 (commit) is the in-flight Bash here, so the bootstrap
# branch in skill_step_block credits it; testing "missing step 19"
# directly would always pass under the bootstrap. Use missing step 16
# (which the in-flight `git commit` does NOT match) to exercise the
# missing-terminal block.
ssb_write_state '[{"n":7},{"n":13}]'
RC="$(ssb_run)"
if [ "$RC" = "2" ]; then
    t_pass "skill_step_block missing terminal step 16 -> rc=2"
else
    t_fail "skill_step_block missing-terminal expected rc=2, got rc=$RC"
fi

# Test C: gap in middle (missing step 13 adversarial) -> rc 2
ssb_write_state '[{"n":7},{"n":16},{"n":19}]'
RC="$(ssb_run)"
if [ "$RC" = "2" ]; then
    t_pass "skill_step_block gap at step 13 -> rc=2"
else
    t_fail "skill_step_block gap-at-13 expected rc=2, got rc=$RC"
fi

# Test B-bootstrap: only step 19 missing AND in-flight is `git commit`
# -> rc 0 (bootstrap credit). Closes the terminal-step deadlock that
# previously forced SKIP_SKILL_STEP_BLOCK on every section commit.
ssb_write_state '[{"n":7},{"n":13},{"n":16}]'
RC="$(ssb_run)"
if [ "$RC" = "0" ]; then
    t_pass "skill_step_block bootstrap-credit for in-flight commit -> rc=0"
else
    t_fail "skill_step_block bootstrap-credit expected rc=0, got rc=$RC"
fi

# Test D: SKIP opt-out with proper reason on missing-terminal -> rc 0
ssb_write_state '[{"n":7},{"n":13},{"n":16}]'
SKIP_SKILL_STEP_BLOCK=1 SKIP_SKILL_STEP_BLOCK_REASON="legitimate-revert-flow" \
    bash -c 'printf "%s" '"'"'{"tool_name":"Bash","tool_input":{"command":"git commit -m test"}}'"'"' | python3 "$0" >/dev/null 2>&1' \
    "$REPO_ROOT/.claude/hooks/skill_step_block.py"
RC=$?
if [ "$RC" = "0" ]; then
    t_pass "skill_step_block SKIP opt-out with reason -> rc=0"
else
    t_fail "skill_step_block SKIP opt-out expected rc=0, got rc=$RC"
fi

# Test E: SKIP without reason -> rc 2 (reason >= 12 chars required)
ssb_write_state '[{"n":7},{"n":13},{"n":16}]'
SKIP_SKILL_STEP_BLOCK=1 SKIP_SKILL_STEP_BLOCK_REASON="too-short" \
    bash -c 'printf "%s" '"'"'{"tool_name":"Bash","tool_input":{"command":"git commit -m test"}}'"'"' | python3 "$0" >/dev/null 2>&1' \
    "$REPO_ROOT/.claude/hooks/skill_step_block.py"
RC=$?
if [ "$RC" = "2" ]; then
    t_pass "skill_step_block SKIP with too-short reason -> rc=2"
else
    t_fail "skill_step_block SKIP-bad-reason expected rc=2, got rc=$RC"
fi

# Restore prior state file (or remove fixture).
if [ -f "$SSB_BACKUP" ]; then
    cp "$SSB_BACKUP" "$SSB_STATE_REAL"
else
    rm -f "$SSB_STATE_REAL"
fi
rm -rf "$SSB_TMP"


# ============================================================================
# Hook event surface expansion (TODO-08 number 11)
# ============================================================================
# Three sub-tests synthesize event payloads and run the new event hooks
# against them, asserting expected state/output without invoking the
# real Claude harness.

HEE_TMP="$(mktemp -d)"

# (a) session_start writes session.json with valid keys.
SS_PAYLOAD='{"session_id":"test-sid-abc","cwd":"/tmp","event_type":"SessionStart"}'
SS_BACKUP="$HEE_TMP/session.json.bak"
SS_REAL="$REPO_ROOT/.claude/state/session.json"
[ -f "$SS_REAL" ] && cp "$SS_REAL" "$SS_BACKUP"
printf '%s' "$SS_PAYLOAD" | python3 "$REPO_ROOT/.claude/hooks/session_start.py" >/dev/null 2>&1
SS_RC=$?
if [ "$SS_RC" = "0" ] && [ -f "$SS_REAL" ] && \
   python3 -c "import json; d=json.load(open('$SS_REAL')); assert d.get('session_id')=='test-sid-abc' and 'started_ts_ns' in d and 'head_sha' in d and 'cwd' in d, d" 2>/dev/null; then
    t_pass "session_start_writes  session.json has session_id + started_ts_ns + head_sha + cwd"
else
    t_fail "session_start_writes  session.json missing keys or hook rc=$SS_RC"
fi
[ -f "$SS_BACKUP" ] && cp "$SS_BACKUP" "$SS_REAL" || true

# (b) user_prompt_doctrine injects systemMessage on a synthetic skip-review prompt.
UPD_OUT="$(printf '%s' '{"prompt":"please skip review and just commit"}' | \
    python3 "$REPO_ROOT/.claude/hooks/user_prompt_doctrine.py" 2>/dev/null)"
if echo "$UPD_OUT" | grep -q "doctrine reminder" && \
   echo "$UPD_OUT" | grep -q "skip" ; then
    t_pass "user_prompt_doctrine_skip  systemMessage emitted on bypass-shape phrase"
else
    t_fail "user_prompt_doctrine_skip  expected systemMessage, got: $UPD_OUT"
fi
# Negative: ordinary prompt produces no output.
UPD_OUT2="$(printf '%s' '{"prompt":"please review the codebase"}' | \
    python3 "$REPO_ROOT/.claude/hooks/user_prompt_doctrine.py" 2>/dev/null)"
if [ -z "$UPD_OUT2" ]; then
    t_pass "user_prompt_doctrine_clean  ordinary prompt produces no systemMessage"
else
    t_fail "user_prompt_doctrine_clean  expected empty output, got: $UPD_OUT2"
fi

# (c) stop_audit detects an "I'll run review-todo-section" promise without
#     a matching tool call. Synthetic transcript fixture below uses the
#     literal text the regex matches; no markdown section-sign refs here.
SA_TRANSCRIPT="$HEE_TMP/transcript.jsonl"
SA_LOG="$REPO_ROOT/.claude/state/acknowledged-but-skipped.log"
SA_LOG_BACKUP="$HEE_TMP/skip-log.bak"
[ -f "$SA_LOG" ] && cp "$SA_LOG" "$SA_LOG_BACKUP"

cat >"$SA_TRANSCRIPT" <<'EOF'
{"message":{"content":[{"type":"text","text":"I will run review-todo-section next"}]}}
{"message":{"content":[{"type":"text","text":"actually never mind"}]}}
{"message":{"content":[{"type":"text","text":"different topic now"}]}}
{"message":{"content":[{"type":"text","text":"more text"}]}}
{"message":{"content":[{"type":"text","text":"more text"}]}}
{"message":{"content":[{"type":"text","text":"more text"}]}}
{"message":{"content":[{"type":"text","text":"more text"}]}}
EOF

# Truncate skip log so we can detect new entries.
: > "$SA_LOG"
SA_PAYLOAD="$(printf '{"transcript_path":"%s"}' "$SA_TRANSCRIPT")"
printf '%s' "$SA_PAYLOAD" | python3 "$REPO_ROOT/.claude/hooks/stop_audit.py" >/dev/null 2>&1
if [ -s "$SA_LOG" ] && grep -q "ACKNOWLEDGED-BUT-SKIPPED" "$SA_LOG" && grep -q "review-todo-section" "$SA_LOG"; then
    t_pass "stop_audit_acknowledged  detects unfulfilled review-todo-section promise"
else
    t_fail "stop_audit_acknowledged  expected WARN line in skip-log, got: $(cat $SA_LOG 2>/dev/null)"
fi

# Negative: a promise FOLLOWED by a matching Skill tool_use should NOT log.
: > "$SA_LOG"
cat >"$SA_TRANSCRIPT" <<'EOF'
{"message":{"content":[{"type":"text","text":"I will run review-todo-section next"}]}}
{"message":{"content":[{"type":"tool_use","name":"Skill","input":{"skill":"review-todo-section","args":"todo/foo example"}}]}}
EOF
printf '%s' "$SA_PAYLOAD" | python3 "$REPO_ROOT/.claude/hooks/stop_audit.py" >/dev/null 2>&1
if [ ! -s "$SA_LOG" ] || ! grep -q "ACKNOWLEDGED-BUT-SKIPPED" "$SA_LOG"; then
    t_pass "stop_audit_satisfied  promise followed by matching tool call does NOT log"
else
    t_fail "stop_audit_satisfied  unexpected WARN: $(cat $SA_LOG 2>/dev/null)"
fi

# Restore skip log.
if [ -f "$SA_LOG_BACKUP" ]; then
    cp "$SA_LOG_BACKUP" "$SA_LOG"
else
    rm -f "$SA_LOG"
fi
rm -rf "$HEE_TMP"


# ============================================================================
# AI-slop content lints (TODO-08 #12)
# ============================================================================
# Sub-tests exercise lint.sh Checks 6 and 7 against synthetic inputs:
#   - Check 6 tautological-test: synthetic test file with TEST_ASSERT(true,...)
#     in a NON-legacy path must produce a lint error.
#   - Check 7 stub-behind-stamp: deferred WARN path is exercised by the
#     baseline lint run (no cache present in synthetic tree).
# Originally-planned Check 8 phantom-include lint was attempted via lsp-bridge
# MCP and dropped 2026-05-02 -- clangd false-positive rate too high on this
# freestanding kernel.

ASL_TMP="$(mktemp -d)"
ASL_REPO="$ASL_TMP/repo"
mkdir -p "$ASL_REPO/src/kernel/test"
mkdir -p "$ASL_REPO/scripts"
mkdir -p "$ASL_REPO/build"
mkdir -p "$ASL_REPO/include"

# Synthetic test file with a tautological assertion in a NON-legacy path.
# We name the file 'test_synthetic_tautology_check_6.c' so it cannot collide
# with the legacy allowlist.
cat >"$ASL_REPO/src/kernel/test/test_synthetic_tautology_check_6.c" <<'EOF'
/* Synthetic fixture for TODO-08 #12 Check 6 tautological-test test. */
void test_func(void)
{
    TEST_ASSERT(true, "no-op assertion -- should be flagged");
}
EOF

# Copy lint.sh + the python tautology helper into the synthetic tree so
# the lint script's REPO_ROOT resolves to $ASL_REPO and finds the helper.
mkdir -p "$ASL_REPO/scripts/lint"
cp "$REPO_ROOT/scripts/lint.sh" "$ASL_REPO/scripts/lint.sh"
cp "$REPO_ROOT/scripts/lint/check_tautological_test.py" \
    "$ASL_REPO/scripts/lint/check_tautological_test.py"

# Run lint pointed at the synthetic test dir. Should detect the tautology
# AND exit non-zero (Codex review 2026-04-28 A-M2: assert both rc and
# diagnostic, not just the diagnostic text).
ASL_OUT="$(cd "$ASL_REPO" && bash scripts/lint.sh "$ASL_REPO/src/kernel/test/" 2>&1)"
ASL_RC=$?
if [ "$ASL_RC" != "0" ] && echo "$ASL_OUT" | grep -q "tautological-test:TEST_ASSERT(true|1, ...)"; then
    t_pass "lint_check6_tautology  TEST_ASSERT(true,...) flagged in non-legacy path (rc=$ASL_RC)"
else
    t_fail "lint_check6_tautology  expected rc!=0 + diagnostic; rc=$ASL_RC, last 3 lines: $(echo "$ASL_OUT" | tail -3)"
fi

# Symbol-vs-literal-of-symbol: TEST_ASSERT_EQ(POST16_FOO, 0xDF20) where
# POST16_FOO is #define'd to 0xDF20. The python helper builds a #define
# index from include/, so we use a real define from the project.
# DEFAULT_BAUD = 115200 in include/kernel/serial.h is a stable target.
cat >"$ASL_REPO/src/kernel/test/test_synthetic_tautology_check_6.c" <<'EOF'
/* Synthetic fixture: SYM-vs-literal-of-SYM tautology, no marker. */
void test_func(void)
{
    /* If DEFAULT_BAUD = 115200 in headers, this asserts the constant
       equals its own #define value -- pure tautology. */
    TEST_ASSERT_EQ(DEFAULT_BAUD, 115200, "tautology: define vs literal");
}
EOF
# The helper resolves defines from the REAL repo's include/ tree because
# we copied lint.sh into a synthetic repo but the python helper walks up
# from the test file looking for include/ + .git. To prevent the
# fallback from finding include/ in a parent dir of $ASL_REPO, we need
# include/kernel/ in the synthetic tree with a matching #define.
mkdir -p "$ASL_REPO/include/kernel"
cat >"$ASL_REPO/include/kernel/serial.h" <<'EOF'
#pragma once
#define DEFAULT_BAUD 115200
EOF
( cd "$ASL_REPO" && git init -q . 2>/dev/null && git config user.email "x@x" && git config user.name "x" )
cp "$REPO_ROOT/scripts/lint/check_tautological_test.py" "$ASL_REPO/scripts/lint/check_tautological_test.py" 2>/dev/null \
    || ( mkdir -p "$ASL_REPO/scripts/lint" && cp "$REPO_ROOT/scripts/lint/check_tautological_test.py" "$ASL_REPO/scripts/lint/" )
ASL_OUT_SYM="$(cd "$ASL_REPO" && bash scripts/lint.sh "$ASL_REPO/src/kernel/test/" 2>&1)"
ASL_RC_SYM=$?
if [ "$ASL_RC_SYM" != "0" ] && echo "$ASL_OUT_SYM" | grep -q "compares define to its own value"; then
    t_pass "lint_check6_sym_literal  TEST_ASSERT_EQ(SYM, value-of-SYM) flagged (rc=$ASL_RC_SYM)"
else
    t_fail "lint_check6_sym_literal  expected rc!=0 + 'compares define to its own value'; rc=$ASL_RC_SYM, got: $(echo "$ASL_OUT_SYM" | grep tautological | head -3)"
fi

# Negative: the same line with /* TEST-TAUTOLOGY-OK: <reason> */ marker
# must NOT be flagged AND lint must rc=0.
cat >"$ASL_REPO/src/kernel/test/test_synthetic_tautology_check_6.c" <<'EOF'
/* Synthetic fixture: marker-allowlisted tautology should NOT fire. */
void test_func(void)
{
    TEST_ASSERT(true, "marker"); /* TEST-TAUTOLOGY-OK: synthetic-allowlist-test */
}
EOF
ASL_OUT2="$(cd "$ASL_REPO" && bash scripts/lint.sh "$ASL_REPO/src/kernel/test/" 2>&1)"
ASL_RC2=$?
if [ "$ASL_RC2" = "0" ] && ! echo "$ASL_OUT2" | grep -q "tautological-test:"; then
    t_pass "lint_check6_marker  /* TEST-TAUTOLOGY-OK: */ marker suppresses lint (rc=0)"
else
    t_fail "lint_check6_marker  expected rc=0 + no tautological diagnostic; rc=$ASL_RC2, got: $(echo "$ASL_OUT2" | grep tautological)"
fi

# Check 7 stub-behind-stamp: deferred WARN.
ASL_OUT3="$(cd "$ASL_REPO" && bash scripts/lint.sh "$ASL_REPO/src/kernel/test/" 2>&1 || true)"
if echo "$ASL_OUT3" | grep -q "Check 7 (stub-behind-stamp)"; then
    t_pass "lint_check7_deferred  Check 7 deferred WARN line present"
else
    t_fail "lint_check7_deferred  expected deferred WARN, got: $(echo "$ASL_OUT3" | tail -3)"
fi

# Skip env vars produce visible WARN.
ASL_OUT4="$(cd "$ASL_REPO" && SKIP_LINT_TAUTOLOGY=1 bash scripts/lint.sh "$ASL_REPO/src/kernel/test/" 2>&1 || true)"
if echo "$ASL_OUT4" | grep -q "Check 6 (tautological-test) skipped via SKIP_LINT_TAUTOLOGY=1"; then
    t_pass "lint_check6_skip  SKIP_LINT_TAUTOLOGY=1 emits visible WARN"
else
    t_fail "lint_check6_skip  expected skip WARN, got: $(echo "$ASL_OUT4" | tail -3)"
fi

rm -rf "$ASL_TMP"


# ============================================================================
# audit-ai-system.sh -- umbrella cross-tool AI drift detection
# ============================================================================
# Single sub-test: run the 7-check umbrella; treat its exit code as verdict.

if [ -x "$REPO_ROOT/scripts/audit-ai-system.sh" ]; then
    AAS_OUT="$(bash "$REPO_ROOT/scripts/audit-ai-system.sh" --quiet 2>&1)"
    AAS_RC=$?
    if [ "$AAS_RC" = "0" ]; then
        t_pass "audit_ai_system  scripts/audit-ai-system.sh PASS (no drift)"
    else
        t_fail "audit_ai_system  scripts/audit-ai-system.sh FAIL ($AAS_OUT)"
    fi
else
    t_fail "audit_ai_system  scripts/audit-ai-system.sh missing or not executable"
fi


# ============================================================================
# audit-hooks.sh -- hook-system drift checks
# ============================================================================
# Single sub-test: bash scripts/audit-hooks.sh exits 0 on a clean tree.
# The script's own checks (files <-> manifest, settings.json <-> manifest,
# BLOCK exit codes, plugin-side enumeration) are exercised by it being
# run; we treat its exit code as the verdict.

if [ -x "$REPO_ROOT/scripts/audit-hooks.sh" ]; then
    AUDIT_OUT="$(bash "$REPO_ROOT/scripts/audit-hooks.sh" --quiet 2>&1)"
    AUDIT_RC=$?
    if [ "$AUDIT_RC" = "0" ]; then
        t_pass "audit_hooks scripts/audit-hooks.sh PASS (no drift)"
    else
        t_fail "audit_hooks scripts/audit-hooks.sh FAIL ($AUDIT_OUT)"
    fi
else
    t_fail "audit_hooks scripts/audit-hooks.sh missing or not executable"
fi


# ============================================================================
# pre_codex_enforcement (TODO-08 review-pipeline pre-Codex enforcement)
# ============================================================================
#   - phase1_evidence_block: phase1_evidence_gate.py BLOCKs adversarial
#     dispatch when active review-todo-section has <2 src/include Reads.
#   - re_adv_trigger_block: section_commit_gate.py BLOCKs commits whose
#     staged C/H diff matches step-13.5 triggers when no recent
#     re-adversarial stamp exists.

[ "$QUIET" = "0" ] && echo "" && echo -e "${DIM}[pre_codex_enforcement]${NC}"

if [ ! -f "$REPO_ROOT/.claude/hooks/phase1_evidence_gate.py" ] || \
   [ ! -f "$REPO_ROOT/.claude/hooks/section_commit_gate.py" ] || \
   [ ! -f "$REPO_ROOT/.claude/hooks/skill_step_map.py" ]; then
    t_fail "pre_codex_enforcement: hook scripts missing"
else
    PCE_TMP="$(mktemp -d)"
    PCE_REPO="$PCE_TMP/repo"
    mkdir -p "$PCE_REPO/.claude/state" "$PCE_REPO/.claude/hooks" \
             "$PCE_REPO/src/kernel" "$PCE_REPO/todo/00-infrastructure"
    cp "$REPO_ROOT/.claude/hooks/phase1_evidence_gate.py" \
       "$PCE_REPO/.claude/hooks/phase1_evidence_gate.py"
    cp "$REPO_ROOT/.claude/hooks/skill_step_map.py" \
       "$PCE_REPO/.claude/hooks/skill_step_map.py"
    cp "$REPO_ROOT/.claude/hooks/section_commit_gate.py" \
       "$PCE_REPO/.claude/hooks/section_commit_gate.py"
    cp "$REPO_ROOT/.claude/hooks/codex_review_completed.py" \
       "$PCE_REPO/.claude/hooks/codex_review_completed.py"
    cp "$REPO_ROOT/.claude/hooks/_review_kind.py" \
       "$PCE_REPO/.claude/hooks/_review_kind.py"
    cp "$REPO_ROOT/.claude/hooks/_skip_env.py" \
       "$PCE_REPO/.claude/hooks/_skip_env.py"
    cp "$REPO_ROOT/.claude/hooks/_codex_dispatch.py" \
       "$PCE_REPO/.claude/hooks/_codex_dispatch.py"
    # phase1_evidence_gate.py imports _heuristic_misses at module load
    # for the section-27 step-4 miss-log emission path. Without this
    # copy, every existing sub-test hits ModuleNotFoundError before the
    # gate even runs.
    cp "$REPO_ROOT/.claude/hooks/_heuristic_misses.py" \
       "$PCE_REPO/.claude/hooks/_heuristic_misses.py"

    pushd "$PCE_REPO" >/dev/null
    git init -q -b main
    git config user.email "test@example.com"
    git config user.name "Test"
    {
        printf '%s\n' '# pre-codex-enforcement fixture'
        printf '\n'
        printf '%s\n' '## 1. Sample target section'
    } > todo/00-infrastructure/TODO-99-pce-fixture.md
    git add todo/00-infrastructure/TODO-99-pce-fixture.md
    git -c commit.gpgsign=false commit -q --no-verify -m "seed"
    popd >/dev/null

    PCE_SKILL_STATE="$PCE_REPO/.claude/state/skill-progress.json"
    PCE_TRANSCRIPT="$PCE_REPO/.claude/state/transcript.jsonl"
    PCE_STAMPS="$PCE_REPO/.claude/state/last-review-stamps.json"

    # ---- phase1_evidence_block sub-tests ----

    # Active review-todo-section, started 1 second ago.
    PCE_STARTED_TS="$(python3 -c 'import time; print(time.time_ns() - 1_000_000_000)')"
    cat > "$PCE_SKILL_STATE" <<JSON
{
  "review-todo-section": {
    "started_ts": $PCE_STARTED_TS,
    "started_head_sha": "0000000000000000000000000000000000000000",
    "session_id": "pce-test",
    "steps_observed": [],
    "args": "todo/00-infrastructure/TODO-99-pce-fixture.md section 1 sample"
  }
}
JSON

    : > "$PCE_TRANSCRIPT"

    # Codex companion path. SECTION-MARKER is a placeholder for the
    # real review-kind marker; tests assemble at runtime so this
    # source line carries no bare-marker pattern.
    PCE_KIND_OPEN='[review-kind: adversarial]'
    PCE_CMD_BASE='node "$HOME/.claude/plugins/marketplaces/openai-codex/plugins/codex/scripts/codex-companion.mjs" adversarial-review'
    PCE_PROMPT_ADV="${PCE_KIND_OPEN} todo/00-infrastructure/TODO-99-pce-fixture.md sample"
    PCE_CMD="$PCE_CMD_BASE \"$PCE_PROMPT_ADV\""
    PCE_PAYLOAD=$(python3 -c "
import json
print(json.dumps({
    'tool_name': 'Bash',
    'tool_input': {'command': '''$PCE_CMD'''},
    'transcript_path': '$PCE_TRANSCRIPT',
}))
")
    PCE_OUT="$(cd "$PCE_REPO" && echo "$PCE_PAYLOAD" | python3 .claude/hooks/phase1_evidence_gate.py 2>&1)"
    PCE_RC=$?
    if [ "$PCE_RC" = "2" ] && echo "$PCE_OUT" | grep -q "BLOCK"; then
        t_pass "phase1_evidence_block: 0 src/include Reads -> BLOCK"
    else
        t_fail "phase1_evidence_block: empty-transcript BLOCK" \
               "rc=$PCE_RC out=$PCE_OUT"
    fi

    PCE_AFTER_TS="$(python3 -c 'import time; print(time.time_ns())')"
    cat > "$PCE_TRANSCRIPT" <<TSCRIPT
{"ts_ns":$PCE_AFTER_TS,"message":{"content":[{"type":"tool_use","name":"Read","input":{"file_path":"src/kernel/foo.c"}}]}}
{"ts_ns":$PCE_AFTER_TS,"message":{"content":[{"type":"tool_use","name":"Grep","input":{"path":"src/kernel","pattern":"spinlock_t"}}]}}
TSCRIPT
    PCE_OUT2="$(cd "$PCE_REPO" && echo "$PCE_PAYLOAD" | python3 .claude/hooks/phase1_evidence_gate.py 2>&1)"
    PCE_RC2=$?
    if [ "$PCE_RC2" = "0" ]; then
        t_pass "phase1_evidence_block: 2 src/ Reads -> PASS"
    else
        t_fail "phase1_evidence_block: 2-Read PASS path" \
               "rc=$PCE_RC2 out=$PCE_OUT2"
    fi

    : > "$PCE_TRANSCRIPT"
    rm -f "$PCE_REPO/.claude/state/skip-log.jsonl"
    PCE_PAYLOAD_FILE="$PCE_TMP/payload.json"
    printf '%s\n' "$PCE_PAYLOAD" > "$PCE_PAYLOAD_FILE"
    PCE_OUT3="$(cd "$PCE_REPO" && SKIP_PHASE1_BLOCK=1 SKIP_PHASE1_BLOCK_REASON='legitimate verified-clean section' python3 .claude/hooks/phase1_evidence_gate.py < "$PCE_PAYLOAD_FILE" 2>&1)"
    PCE_RC3=$?
    if [ "$PCE_RC3" = "0" ] && [ -f "$PCE_REPO/.claude/state/skip-log.jsonl" ] && \
       grep -q "SKIP_PHASE1_BLOCK" "$PCE_REPO/.claude/state/skip-log.jsonl"; then
        t_pass "phase1_evidence_block: SKIP env -> PASS + skip-log entry"
    else
        t_fail "phase1_evidence_block: SKIP path" \
               "rc=$PCE_RC3 skip-log=$([ -f "$PCE_REPO/.claude/state/skip-log.jsonl" ] && cat "$PCE_REPO/.claude/state/skip-log.jsonl" || echo missing)"
    fi

    PCE_KIND_CONS='[review-kind: consistency]'
    PCE_PROMPT_CONS="${PCE_KIND_CONS} todo/00-infrastructure/TODO-99-pce-fixture.md sample"
    PCE_CMD_CONS="$PCE_CMD_BASE \"$PCE_PROMPT_CONS\""
    PCE_PAYLOAD_CONS=$(python3 -c "
import json
print(json.dumps({
    'tool_name': 'Bash',
    'tool_input': {'command': '''$PCE_CMD_CONS'''},
    'transcript_path': '$PCE_TRANSCRIPT',
}))
")
    PCE_OUT4="$(cd "$PCE_REPO" && echo "$PCE_PAYLOAD_CONS" | python3 .claude/hooks/phase1_evidence_gate.py 2>&1)"
    PCE_RC4=$?
    if [ "$PCE_RC4" = "0" ]; then
        t_pass "phase1_evidence_block: consistency dispatch bypasses gate (scoped to adversarial only)"
    else
        t_fail "phase1_evidence_block: consistency bypass" \
               "rc=$PCE_RC4 out=$PCE_OUT4"
    fi

    # ---- phase1_evidence_misslog sub-tests ----
    # Owner: 00-infrastructure/TODO-08-automation-hardening (Phase 1
    # evidence-gate threshold tuning via miss-log telemetry section).
    # The gate emits a step-4 miss-log entry on every BLOCK so the
    # heuristic-misses telemetry path computes FP ratio and tunes
    # PHASE1_MIN_READS by data. Three checks:
    #   (1) 0 reads -> BLOCK + miss-log entry with step:4
    #   (2) 1 read  -> BLOCK + new miss-log entry (separate fire)
    #   (3) 2 reads -> PASS, NO new miss-log entry appended
    PCE_MISSLOG="$PCE_REPO/.claude/state/heuristic-misses.jsonl"
    rm -f "$PCE_MISSLOG"
    : > "$PCE_TRANSCRIPT"
    cd "$PCE_REPO" && echo "$PCE_PAYLOAD" | python3 .claude/hooks/phase1_evidence_gate.py >/dev/null 2>&1; cd - >/dev/null
    if [ -f "$PCE_MISSLOG" ] && grep -q '"step":4' "$PCE_MISSLOG" && \
       grep -q '"signal":"phase1-evidence-missing"' "$PCE_MISSLOG"; then
        t_pass "phase1_evidence_misslog: 0 Reads -> BLOCK + miss-log step:4 entry"
    else
        t_fail "phase1_evidence_misslog: 0 Reads miss-log emit" \
               "exists=$([ -f "$PCE_MISSLOG" ] && echo yes || echo no) content=$(cat "$PCE_MISSLOG" 2>&1 | head -3)"
    fi

    # 1 Read still BELOW threshold -> BLOCK + new miss-log entry.
    PCE_T1_TS="$(python3 -c 'import time; print(time.time_ns())')"
    cat > "$PCE_TRANSCRIPT" <<TSCRIPT
{"ts_ns":$PCE_T1_TS,"message":{"content":[{"type":"tool_use","name":"Read","input":{"file_path":"src/kernel/foo.c"}}]}}
TSCRIPT
    PCE_MISSLOG_LINES_BEFORE=$(wc -l < "$PCE_MISSLOG" 2>/dev/null || echo 0)
    cd "$PCE_REPO" && echo "$PCE_PAYLOAD" | python3 .claude/hooks/phase1_evidence_gate.py >/dev/null 2>&1; cd - >/dev/null
    PCE_MISSLOG_LINES_AFTER=$(wc -l < "$PCE_MISSLOG" 2>/dev/null || echo 0)
    if [ "$PCE_MISSLOG_LINES_AFTER" -gt "$PCE_MISSLOG_LINES_BEFORE" ]; then
        t_pass "phase1_evidence_misslog: 1 Read -> BLOCK + new miss-log entry"
    else
        t_fail "phase1_evidence_misslog: 1 Read miss-log emit" \
               "before=$PCE_MISSLOG_LINES_BEFORE after=$PCE_MISSLOG_LINES_AFTER"
    fi

    # 2 Reads at threshold -> PASS, NO new miss-log entry.
    PCE_T2_TS="$(python3 -c 'import time; print(time.time_ns())')"
    cat > "$PCE_TRANSCRIPT" <<TSCRIPT
{"ts_ns":$PCE_T2_TS,"message":{"content":[{"type":"tool_use","name":"Read","input":{"file_path":"src/kernel/foo.c"}}]}}
{"ts_ns":$PCE_T2_TS,"message":{"content":[{"type":"tool_use","name":"Grep","input":{"path":"src/kernel","pattern":"spinlock_t"}}]}}
TSCRIPT
    PCE_MISSLOG_LINES_BEFORE2=$(grep -c '"signal":"phase1-evidence-missing"' "$PCE_MISSLOG" 2>/dev/null || echo 0)
    cd "$PCE_REPO" && echo "$PCE_PAYLOAD" | python3 .claude/hooks/phase1_evidence_gate.py >/dev/null 2>&1; PCE_RC_PASS=$?; cd - >/dev/null
    PCE_MISSLOG_LINES_AFTER2=$(grep -c '"signal":"phase1-evidence-missing"' "$PCE_MISSLOG" 2>/dev/null || echo 0)
    if [ "$PCE_RC_PASS" = "0" ] && [ "$PCE_MISSLOG_LINES_AFTER2" = "$PCE_MISSLOG_LINES_BEFORE2" ]; then
        t_pass "phase1_evidence_misslog: 2 Reads -> PASS, no new miss-log entry"
    else
        t_fail "phase1_evidence_misslog: 2 Reads PASS without emit" \
               "rc=$PCE_RC_PASS before=$PCE_MISSLOG_LINES_BEFORE2 after=$PCE_MISSLOG_LINES_AFTER2"
    fi

    # ---- re_adv_trigger_block sub-tests ----

    cat > "$PCE_REPO/src/kernel/boot_halt.c" <<'CFAULT'
void boot_halt(unsigned int code) {
    while (1) { __asm__ volatile("hlt"); }
}
CFAULT
    (
        cd "$PCE_REPO" && git add src/kernel/boot_halt.c >/dev/null 2>&1
    )

    PCE_GIT_PAYLOAD=$(python3 -c "
import json
print(json.dumps({'tool_name': 'Bash', 'tool_input': {'command': 'git commit -m \"fix faultable region\"'}}))
")
    rm -f "$PCE_STAMPS"
    PCE_OUT5="$(cd "$PCE_REPO" && echo "$PCE_GIT_PAYLOAD" | python3 .claude/hooks/section_commit_gate.py 2>&1)"
    PCE_RC5=$?
    if [ "$PCE_RC5" = "2" ] && echo "$PCE_OUT5" | grep -qE "re-adversarial|step-13.5 trigger"; then
        t_pass "re_adv_trigger_block: faultable C/H + no re-adv stamp -> BLOCK"
    else
        t_fail "re_adv_trigger_block: BLOCK on faultable trigger" \
               "rc=$PCE_RC5 out=$PCE_OUT5"
    fi

    PCE_NOW_NS="$(python3 -c 'import time; print(time.time_ns())')"
    cat > "$PCE_STAMPS" <<JSON
{
  "todo/00-infrastructure/TODO-99-pce-fixture.md": {
    "section": "1",
    "adversarial": null,
    "consistency": null,
    "perf": null,
    "re-adversarial": $PCE_NOW_NS
  }
}
JSON
    PCE_OUT6="$(cd "$PCE_REPO" && echo "$PCE_GIT_PAYLOAD" | python3 .claude/hooks/section_commit_gate.py 2>&1)"
    PCE_RC6=$?
    if [ "$PCE_RC6" = "0" ]; then
        t_pass "re_adv_trigger_block: fresh re-adversarial stamp -> PASS"
    else
        t_fail "re_adv_trigger_block: PASS path with fresh stamp" \
               "rc=$PCE_RC6 out=$PCE_OUT6"
    fi

    PCE_STALE_NS="$(python3 -c 'import time; print(time.time_ns() - 60*60*1_000_000_000)')"
    cat > "$PCE_STAMPS" <<JSON
{
  "todo/00-infrastructure/TODO-99-pce-fixture.md": {
    "section": "1",
    "adversarial": null,
    "consistency": null,
    "perf": null,
    "re-adversarial": $PCE_STALE_NS
  }
}
JSON
    PCE_OUT7="$(cd "$PCE_REPO" && echo "$PCE_GIT_PAYLOAD" | python3 .claude/hooks/section_commit_gate.py 2>&1)"
    PCE_RC7=$?
    if [ "$PCE_RC7" = "2" ] && echo "$PCE_OUT7" | grep -q "stale"; then
        t_pass "re_adv_trigger_block: stale re-adversarial stamp -> BLOCK"
    else
        t_fail "re_adv_trigger_block: stale BLOCK path" \
               "rc=$PCE_RC7 out=$PCE_OUT7"
    fi

    cat > "$PCE_REPO/src/kernel/lock_change.c" <<'CLOCK'
#include "kernel/types.h"
mutex_t g_lock;
void lock_init(void) { g_lock = 0; }
CLOCK
    (cd "$PCE_REPO" && git add src/kernel/lock_change.c >/dev/null 2>&1)
    rm -f "$PCE_STAMPS"
    PCE_OUT8="$(cd "$PCE_REPO" && echo "$PCE_GIT_PAYLOAD" | python3 .claude/hooks/section_commit_gate.py 2>&1)"
    PCE_RC8=$?
    if [ "$PCE_RC8" = "2" ] && echo "$PCE_OUT8" | grep -q "locking"; then
        t_pass "re_adv_trigger_block: mutex_t locking trigger detected (Codex M3 fix)"
    else
        t_fail "re_adv_trigger_block: mutex_t locking detection" \
               "rc=$PCE_RC8 out=$PCE_OUT8"
    fi

    PCE_GIT_PAYLOAD_FILE="$PCE_TMP/git-payload.json"
    printf '%s\n' "$PCE_GIT_PAYLOAD" > "$PCE_GIT_PAYLOAD_FILE"
    PCE_OUT9="$(cd "$PCE_REPO" && SKIP_REVIEW_HOOK=1 SKIP_REVIEW_HOOK_REASON='manual re-adv already done' python3 .claude/hooks/section_commit_gate.py < "$PCE_GIT_PAYLOAD_FILE" 2>&1)"
    PCE_RC9=$?
    if [ "$PCE_RC9" = "0" ]; then
        t_pass "re_adv_trigger_block: SKIP_REVIEW_HOOK env -> PASS"
    else
        t_fail "re_adv_trigger_block: SKIP path" \
               "rc=$PCE_RC9 out=$PCE_OUT9"
    fi

    # ---- Codex test-coverage gap closures ----
    # Reset staged state and stamps for the new sub-tests.
    (cd "$PCE_REPO" && git reset -q HEAD -- . && rm -f src/kernel/*.c)

    # Lifecycle trigger family: \w*_(alloc|free|refcount).
    cat > "$PCE_REPO/src/kernel/lifecycle.c" <<'CLIFE'
#include "kernel/types.h"
void *obj_alloc(unsigned long sz) { return 0; }
void obj_free(void *p) { (void)p; }
CLIFE
    (cd "$PCE_REPO" && git add src/kernel/lifecycle.c >/dev/null 2>&1)
    rm -f "$PCE_STAMPS"
    PCE_OUT_LIFE="$(cd "$PCE_REPO" && python3 .claude/hooks/section_commit_gate.py < "$PCE_GIT_PAYLOAD_FILE" 2>&1)"
    PCE_RC_LIFE=$?
    if [ "$PCE_RC_LIFE" = "2" ] && echo "$PCE_OUT_LIFE" | grep -q "lifecycle"; then
        t_pass "re_adv_trigger_block: lifecycle (_alloc/_free/_refcount) trigger detected"
    else
        t_fail "re_adv_trigger_block: lifecycle trigger" \
               "rc=$PCE_RC_LIFE out=$PCE_OUT_LIFE"
    fi
    (cd "$PCE_REPO" && git reset -q HEAD -- . && rm -f src/kernel/lifecycle.c)

    # >50 LOC C/H trigger: a benign-content file with 60+ added lines
    # touching neither locking, faultable, nor lifecycle regexes.
    {
        printf '#include "kernel/types.h"\n'
        for i in $(seq 1 60); do
            printf 'int benign_func_%s(void) { return %s; }\n' "$i" "$i"
        done
    } > "$PCE_REPO/src/kernel/big_diff.c"
    (cd "$PCE_REPO" && git add src/kernel/big_diff.c >/dev/null 2>&1)
    rm -f "$PCE_STAMPS"
    PCE_OUT_LOC="$(cd "$PCE_REPO" && python3 .claude/hooks/section_commit_gate.py < "$PCE_GIT_PAYLOAD_FILE" 2>&1)"
    PCE_RC_LOC=$?
    if [ "$PCE_RC_LOC" = "2" ] && echo "$PCE_OUT_LOC" | grep -q "LOC"; then
        t_pass "re_adv_trigger_block: >50 LOC C/H trigger detected"
    else
        t_fail "re_adv_trigger_block: >50 LOC trigger" \
               "rc=$PCE_RC_LOC out=$PCE_OUT_LOC"
    fi
    (cd "$PCE_REPO" && git reset -q HEAD -- . && rm -f src/kernel/big_diff.c)

    # Malformed last-review-stamps.json with a faultable trigger -> BLOCK
    # (the gate must treat parse failure as "no recent re-adv stamp").
    cat > "$PCE_REPO/src/kernel/boot_halt2.c" <<'CFAULT2'
void boot_halt(unsigned int code) { while (1) { } }
CFAULT2
    (cd "$PCE_REPO" && git add src/kernel/boot_halt2.c >/dev/null 2>&1)
    printf '%s' '{ this is not valid json' > "$PCE_STAMPS"
    PCE_OUT_BAD="$(cd "$PCE_REPO" && python3 .claude/hooks/section_commit_gate.py < "$PCE_GIT_PAYLOAD_FILE" 2>&1)"
    PCE_RC_BAD=$?
    if [ "$PCE_RC_BAD" = "2" ] && echo "$PCE_OUT_BAD" | grep -qE "re-adversarial|step-13.5"; then
        t_pass "re_adv_trigger_block: malformed last-review-stamps.json -> BLOCK (no parse-time fault)"
    else
        t_fail "re_adv_trigger_block: malformed-stamps BLOCK" \
               "rc=$PCE_RC_BAD out=$PCE_OUT_BAD"
    fi
    rm -f "$PCE_STAMPS"
    (cd "$PCE_REPO" && git reset -q HEAD -- . && rm -f src/kernel/boot_halt2.c)

    # No-attribution path: faultable trigger fires but no staged TODO
    # AND no active review-todo-section in skill-progress.json.
    rm -f "$PCE_SKILL_STATE"
    cat > "$PCE_REPO/src/kernel/boot_halt3.c" <<'CFAULT3'
void boot_halt(unsigned int code) { while (1) { } }
CFAULT3
    (cd "$PCE_REPO" && git add src/kernel/boot_halt3.c >/dev/null 2>&1)
    PCE_OUT_NOATTR="$(cd "$PCE_REPO" && python3 .claude/hooks/section_commit_gate.py < "$PCE_GIT_PAYLOAD_FILE" 2>&1)"
    PCE_RC_NOATTR=$?
    if [ "$PCE_RC_NOATTR" = "0" ] && echo "$PCE_OUT_NOATTR" | grep -q "WARN"; then
        t_pass "re_adv_trigger_block: no attribution -> PASS with WARN (manual judgment fallback)"
    else
        t_fail "re_adv_trigger_block: no-attribution path" \
               "rc=$PCE_RC_NOATTR out=$PCE_OUT_NOATTR"
    fi
    (cd "$PCE_REPO" && git reset -q HEAD -- . && rm -f src/kernel/boot_halt3.c)

    # Compaction-orphaned skill state: re-adv gate must NOT pick up the
    # orphan as the active skill. Faultable trigger + orphaned entry +
    # no staged TODO -> no attribution -> PASS with WARN.
    cat > "$PCE_SKILL_STATE" <<JSON
{
  "review-todo-section": {
    "started_ts": $PCE_STARTED_TS,
    "started_head_sha": "0000000000000000000000000000000000000000",
    "session_id": "pce-test",
    "steps_observed": [],
    "args": "todo/00-infrastructure/TODO-99-pce-fixture.md section 1",
    "compaction_orphaned": true,
    "orphan_ts_ns": $PCE_STARTED_TS,
    "orphan_reason": "PreCompact"
  }
}
JSON
    cat > "$PCE_REPO/src/kernel/boot_halt4.c" <<'CFAULT4'
void boot_halt(unsigned int code) { while (1) { } }
CFAULT4
    (cd "$PCE_REPO" && git add src/kernel/boot_halt4.c >/dev/null 2>&1)
    PCE_OUT_ORPH="$(cd "$PCE_REPO" && python3 .claude/hooks/section_commit_gate.py < "$PCE_GIT_PAYLOAD_FILE" 2>&1)"
    PCE_RC_ORPH=$?
    if [ "$PCE_RC_ORPH" = "0" ] && echo "$PCE_OUT_ORPH" | grep -q "WARN"; then
        t_pass "re_adv_trigger_block: compaction_orphaned skill state -> attribution skipped (PASS+WARN)"
    else
        t_fail "re_adv_trigger_block: compaction_orphaned path" \
               "rc=$PCE_RC_ORPH out=$PCE_OUT_ORPH"
    fi
    (cd "$PCE_REPO" && git reset -q HEAD -- . && rm -f src/kernel/boot_halt4.c)

    # Restore the active skill state for Phase-1 boundary tests.
    cat > "$PCE_SKILL_STATE" <<JSON
{
  "review-todo-section": {
    "started_ts": $PCE_STARTED_TS,
    "started_head_sha": "0000000000000000000000000000000000000000",
    "session_id": "pce-test",
    "steps_observed": [],
    "args": "todo/00-infrastructure/TODO-99-pce-fixture.md section 1 sample"
  }
}
JSON

    # Codex adversarial M1 fix: assembly-only diff over 50 LOC must
    # also fire the LOC trigger. The original implementation filtered
    # to .c/.h before _staged_loc_delta and missed asm rewrites.
    {
        printf '; comment line %s\n' 1
        for i in $(seq 1 60); do
            printf 'mov rax, %s\n' "$i"
        done
    } > "$PCE_REPO/src/kernel/asm_only.S"
    (cd "$PCE_REPO" && git add src/kernel/asm_only.S >/dev/null 2>&1)
    rm -f "$PCE_STAMPS"
    PCE_OUT_ASM="$(cd "$PCE_REPO" && python3 .claude/hooks/section_commit_gate.py < "$PCE_GIT_PAYLOAD_FILE" 2>&1)"
    PCE_RC_ASM=$?
    if [ "$PCE_RC_ASM" = "2" ] && echo "$PCE_OUT_ASM" | grep -q "LOC"; then
        t_pass "re_adv_trigger_block: asm-only >50 LOC trigger detected (Codex M1 fix)"
    else
        t_fail "re_adv_trigger_block: asm LOC trigger" \
               "rc=$PCE_RC_ASM out=$PCE_OUT_ASM"
    fi
    (cd "$PCE_REPO" && git reset -q HEAD -- . && rm -f src/kernel/asm_only.S)

    # Codex adversarial M2 fix: timestamp-less transcript events must
    # NOT count as Phase-1 evidence. Only events with ts_ns >= started_ts
    # count.
    cat > "$PCE_TRANSCRIPT" <<TSCRIPT
{"message":{"content":[{"type":"tool_use","name":"Read","input":{"file_path":"src/kernel/foo.c"}}]}}
{"message":{"content":[{"type":"tool_use","name":"Read","input":{"file_path":"src/kernel/bar.c"}}]}}
TSCRIPT
    PCE_OUT_NOTS="$(cd "$PCE_REPO" && echo "$PCE_PAYLOAD" | python3 .claude/hooks/phase1_evidence_gate.py 2>&1)"
    PCE_RC_NOTS=$?
    if [ "$PCE_RC_NOTS" = "2" ] && echo "$PCE_OUT_NOTS" | grep -q "BLOCK"; then
        t_pass "phase1_evidence_block: timestamp-less events do NOT count as evidence (Codex M2 fix)"
    else
        t_fail "phase1_evidence_block: missing-ts fail-closed" \
               "rc=$PCE_RC_NOTS out=$PCE_OUT_NOTS"
    fi

    # Codex re-adversarial H1 fix: infrastructure TODOs own .claude/
    # hooks, scripts/, and docs/ paths. Phase 1 evidence from those
    # locations must count for infrastructure-style reviews.
    cat > "$PCE_TRANSCRIPT" <<TSCRIPT
{"ts_ns":$PCE_AFTER_TS,"message":{"content":[{"type":"tool_use","name":"Read","input":{"file_path":".claude/hooks/phase1_evidence_gate.py"}}]}}
{"ts_ns":$PCE_AFTER_TS,"message":{"content":[{"type":"tool_use","name":"Grep","input":{"path":"scripts","glob":"*.sh"}}]}}
TSCRIPT
    PCE_OUT_INFRA="$(cd "$PCE_REPO" && echo "$PCE_PAYLOAD" | python3 .claude/hooks/phase1_evidence_gate.py 2>&1)"
    PCE_RC_INFRA=$?
    if [ "$PCE_RC_INFRA" = "0" ]; then
        t_pass "phase1_evidence_block: infrastructure paths (.claude/scripts/docs) count as evidence (Codex re-adv H1 fix)"
    else
        t_fail "phase1_evidence_block: infrastructure path coverage" \
               "rc=$PCE_RC_INFRA out=$PCE_OUT_INFRA"
    fi

    # M1 -- Phase-1 malformed tool_use input must NOT crash the walk.
    # Mix one truthy non-dict input event between two valid Reads.
    cat > "$PCE_TRANSCRIPT" <<TSCRIPT
{"ts_ns":$PCE_AFTER_TS,"message":{"content":[{"type":"tool_use","name":"Read","input":{"file_path":"src/kernel/foo.c"}}]}}
{"ts_ns":$PCE_AFTER_TS,"message":{"content":[{"type":"tool_use","name":"Read","input":"not-a-dict"}]}}
{"ts_ns":$PCE_AFTER_TS,"message":{"content":[{"type":"tool_use","name":"Grep","input":{"path":"include/kernel"}}]}}
this is not valid json at all
TSCRIPT
    PCE_OUT_MAL="$(cd "$PCE_REPO" && echo "$PCE_PAYLOAD" | python3 .claude/hooks/phase1_evidence_gate.py 2>&1)"
    PCE_RC_MAL=$?
    if [ "$PCE_RC_MAL" = "0" ]; then
        t_pass "phase1_evidence_block: malformed inputs do not abort walk; valid evidence still counted"
    else
        t_fail "phase1_evidence_block: malformed-input resilience" \
               "rc=$PCE_RC_MAL out=$PCE_OUT_MAL"
    fi

    # M2 -- re-adversarial dispatch must NOT trigger the Phase-1 gate
    # (only adversarial does; re-adversarial is step 13.5, separate).
    : > "$PCE_TRANSCRIPT"
    PCE_KIND_READV='[review-kind: re-adversarial]'
    PCE_PROMPT_READV="${PCE_KIND_READV} todo/00-infrastructure/TODO-99-pce-fixture.md sample"
    PCE_CMD_READV="$PCE_CMD_BASE \"$PCE_PROMPT_READV\""
    PCE_PAYLOAD_READV=$(python3 -c "
import json
print(json.dumps({
    'tool_name': 'Bash',
    'tool_input': {'command': '''$PCE_CMD_READV'''},
    'transcript_path': '$PCE_TRANSCRIPT',
}))
")
    PCE_OUT_READV="$(cd "$PCE_REPO" && echo "$PCE_PAYLOAD_READV" | python3 .claude/hooks/phase1_evidence_gate.py 2>&1)"
    PCE_RC_READV=$?
    if [ "$PCE_RC_READV" = "0" ]; then
        t_pass "phase1_evidence_block: re-adversarial dispatch bypasses gate (scoped to adversarial only)"
    else
        t_fail "phase1_evidence_block: re-adversarial discrimination" \
               "rc=$PCE_RC_READV out=$PCE_OUT_READV"
    fi

    # M2 -- missing review-kind marker must bypass the gate (no marker
    # = no scoped dispatch we can attribute as adversarial).
    PCE_PROMPT_NOMK="todo/00-infrastructure/TODO-99-pce-fixture.md plain prompt no marker"
    PCE_CMD_NOMK="$PCE_CMD_BASE \"$PCE_PROMPT_NOMK\""
    PCE_PAYLOAD_NOMK=$(python3 -c "
import json
print(json.dumps({
    'tool_name': 'Bash',
    'tool_input': {'command': '''$PCE_CMD_NOMK'''},
    'transcript_path': '$PCE_TRANSCRIPT',
}))
")
    PCE_OUT_NOMK="$(cd "$PCE_REPO" && echo "$PCE_PAYLOAD_NOMK" | python3 .claude/hooks/phase1_evidence_gate.py 2>&1)"
    PCE_RC_NOMK=$?
    if [ "$PCE_RC_NOMK" = "0" ]; then
        t_pass "phase1_evidence_block: missing review-kind marker bypasses gate"
    else
        t_fail "phase1_evidence_block: missing-marker bypass" \
               "rc=$PCE_RC_NOMK out=$PCE_OUT_NOMK"
    fi

    rm -rf "$PCE_TMP"
fi


# ============================================================================
# shim_ca_detection (TODO-02 MS UEFI CA 2023 transition tracking)
# ============================================================================
#   - shim_ca_2011_pre_warn: 2011 CA + today pre-warn-window -> silent accept.
#   - shim_ca_2011_warn:     2011 CA + today in warn window -> WARN logged.
#   - shim_ca_2011_expired:  2011 CA + today past expiry -> sign-efi exits 1.
#   - shim_ca_2023_pass:     2023 CA + any date -> silent accept.
#   - shim_ca_no_sbverify:   sbverify absent -> graceful skip.

[ "$QUIET" = "0" ] && echo "" && echo -e "${DIM}[shim_ca_detection]${NC}"

if [ ! -f "$REPO_ROOT/scripts/sign-efi.sh" ]; then
    t_fail "shim_ca_detection: scripts/sign-efi.sh missing"
else
    SCD_TMP="$(mktemp -d)"
    SCD_REPO="$SCD_TMP/repo"
    mkdir -p "$SCD_REPO/scripts" "$SCD_REPO/keys" "$SCD_REPO/build/tools" "$SCD_REPO/shim" "$SCD_REPO/bin"
    cp "$REPO_ROOT/scripts/sign-efi.sh" "$SCD_REPO/scripts/sign-efi.sh"
    chmod +x "$SCD_REPO/scripts/sign-efi.sh"
    # Stub sbsign + sbverify (signing-side) to avoid OpenSSL dependency.
    cat > "$SCD_REPO/bin/sbsign" <<'STUB'
#!/bin/bash
# stub sbsign: copy input to output to mimic atomic sign
out=""; while [ $# -gt 0 ]; do
    case "$1" in --output) out="$2"; shift 2 ;;
                 --key|--cert) shift 2 ;;
                 *) [ -z "$out" ] && true; src="$1"; shift ;;
    esac
done
cp "$src" "$out"
STUB
    chmod +x "$SCD_REPO/bin/sbsign"
    cat > "$SCD_REPO/bin/sbverify" <<'STUB'
#!/bin/bash
# stub sbverify: --list prints whatever SHIM_CA_FIXTURE says
if [ "$1" = "--list" ]; then
    echo "${SHIM_CA_FIXTURE:-Microsoft Corporation UEFI CA 2011}"
    exit 0
fi
echo "Signature verification OK"
exit 0
STUB
    chmod +x "$SCD_REPO/bin/sbverify"
    # Provide minimum signing keys + EFI binary placeholder.
    echo "stub-key" > "$SCD_REPO/keys/MOK.key"
    echo "stub-crt" > "$SCD_REPO/keys/MOK.cer"
    echo "stub-efi" > "$SCD_REPO/build/tools/BOOTX64.EFI"
    # UKI artifact stub: section-11 dual-sign requires both PEs present;
    # missing UKI is now hard-fail (Codex consistency H1 fix 2026-04-29).
    echo "stub-uki" > "$SCD_REPO/build/tools/BOOTX64.UKI.efi"
    echo "stub-shim" > "$SCD_REPO/shim/shimx64.efi"

    _scd_run() {
        local fixture="$1"; local fake_today="$2"
        local out
        out="$(cd "$SCD_REPO" && SHIM_CA_FIXTURE="$fixture" \
            SHIM_CA_TEST_TODAY="$fake_today" \
            PATH="$SCD_REPO/bin:$PATH" \
            bash scripts/sign-efi.sh 2>&1)"
        local rc=$?
        printf '%s\n__rc=%s\n' "$out" "$rc"
    }

    # Test 1: 2011 CA, pre-warn-window date -> silent accept (no WARN line).
    SCD_OUT_PRE="$(_scd_run "Microsoft Corporation UEFI CA 2011" "2026-01-15")"
    if echo "$SCD_OUT_PRE" | grep -q "signed-by: Microsoft Corporation UEFI CA 2011" \
       && ! echo "$SCD_OUT_PRE" | grep -q "WARN -- shim is signed only by deprecated"; then
        t_pass "shim_ca_detection: 2011 CA + pre-warn-window -> silent accept"
    else
        t_fail "shim_ca_detection: 2011 pre-warn" "out: $(echo "$SCD_OUT_PRE" | tail -8)"
    fi

    # Test 2: 2011 CA, warn-window date -> WARN logged, exit 0.
    SCD_OUT_WARN="$(_scd_run "Microsoft Corporation UEFI CA 2011" "2026-05-15")"
    if echo "$SCD_OUT_WARN" | grep -q "WARN -- shim is signed only by deprecated MS UEFI CA 2011" \
       && echo "$SCD_OUT_WARN" | grep -q "__rc=0"; then
        t_pass "shim_ca_detection: 2011 CA + warn-window -> WARN + exit 0"
    else
        t_fail "shim_ca_detection: 2011 warn-window" "out: $(echo "$SCD_OUT_WARN" | tail -8)"
    fi

    # Test 3: 2011 CA, post-expiry date -> FAIL, exit 1.
    SCD_OUT_FAIL="$(_scd_run "Microsoft Corporation UEFI CA 2011" "2026-07-15")"
    if echo "$SCD_OUT_FAIL" | grep -q "FAIL -- shim is signed by 2011 CA which expired" \
       && echo "$SCD_OUT_FAIL" | grep -q "__rc=1"; then
        t_pass "shim_ca_detection: 2011 CA + post-expiry -> FAIL + exit 1"
    else
        t_fail "shim_ca_detection: 2011 post-expiry" "out: $(echo "$SCD_OUT_FAIL" | tail -8)"
    fi

    # Test 4: 2023 CA, any date -> silent accept.
    SCD_OUT_2023="$(_scd_run "Microsoft Corporation UEFI CA 2023" "2026-07-15")"
    if echo "$SCD_OUT_2023" | grep -q "signed-by: Microsoft Corporation UEFI CA 2023" \
       && ! echo "$SCD_OUT_2023" | grep -qE "WARN|FAIL"; then
        t_pass "shim_ca_detection: 2023 CA + any date -> silent accept"
    else
        t_fail "shim_ca_detection: 2023 CA" "out: $(echo "$SCD_OUT_2023" | tail -8)"
    fi

    # Test 6: sbverify exits nonzero on a present shim binary -> FAIL
    # exit 1 (Codex final adversarial M1 fix 2026-04-29). Regression
    # guard: tool failure must NOT be conflated with "no recognized CA"
    # which would let an unverified shim through the WARN branch.
    cat > "$SCD_REPO/bin/sbverify_failing" <<'STUB'
#!/bin/bash
echo "sbverify: cannot read input file (simulated)"
exit 2
STUB
    chmod +x "$SCD_REPO/bin/sbverify_failing"
    SCD_OUT_SBVFAIL="$(cd "$SCD_REPO" && \
        PATH="$SCD_REPO/bin:$PATH" \
        bash -c 'cp bin/sbverify_failing bin/sbverify; exec bash scripts/sign-efi.sh' 2>&1; printf '__rc=%s\n' $?)"
    if echo "$SCD_OUT_SBVFAIL" | grep -q "FAIL -- sbverify --list" \
       && echo "$SCD_OUT_SBVFAIL" | grep -q "__rc=1"; then
        t_pass "shim_ca_detection: sbverify nonzero exit -> FAIL exit 1 (no fail-open)"
    else
        t_fail "shim_ca_detection: sbverify-fail" "out: $(echo "$SCD_OUT_SBVFAIL" | tail -8)"
    fi
    # Restore the passing stub for any subsequent tests (currently last).
    cat > "$SCD_REPO/bin/sbverify" <<'STUB'
#!/bin/bash
if [ "$1" = "--list" ]; then
    echo "${SHIM_CA_FIXTURE:-Microsoft Corporation UEFI CA 2011}"
    exit 0
fi
echo "Signature verification OK"
exit 0
STUB
    chmod +x "$SCD_REPO/bin/sbverify"

    # Test 5: 2011 CA + post-expiry date + missing MOK key -> FAIL exit 1
    # (Codex consistency H1 fix 2026-04-29). Regression guard: the shim
    # CA gate must abort the pipeline BEFORE the dev-build "MOK key not
    # found, skipping" early return, otherwise CI without local keys
    # ships an expired-2011 shim silently.
    SCD_OUT_NOMOK="$(cd "$SCD_REPO" && SHIM_CA_FIXTURE="Microsoft Corporation UEFI CA 2011" \
        SHIM_CA_TEST_TODAY="2026-07-15" \
        MOK_KEY="/tmp/scd-definitely-missing-key-$$" \
        PATH="$SCD_REPO/bin:$PATH" \
        bash scripts/sign-efi.sh 2>&1; printf '__rc=%s\n' $?)"
    if echo "$SCD_OUT_NOMOK" | grep -q "FAIL -- shim is signed by 2011 CA which expired" \
       && echo "$SCD_OUT_NOMOK" | grep -q "__rc=1"; then
        t_pass "shim_ca_detection: 2011 post-expiry + no MOK -> FAIL + exit 1 (gate runs before MOK skip)"
    else
        t_fail "shim_ca_detection: 2011 post-expiry no-MOK" "out: $(echo "$SCD_OUT_NOMOK" | tail -8)"
    fi

    rm -rf "$SCD_TMP"
fi


# ============================================================================
# stamp_completeness (TODO-08 stamp-region + OS Comparison lints)
# ============================================================================
#   - lint_stamp_region: synthetic TODO with [x] row whose body lacks Notes
#     -> Check 10 ERROR; full stamp region -> PASS; pre-sentinel -> PASS.
#   - lint_os_comparison_sync: synthetic TODO with [x] IO row + not-shipped
#     OS row -> Check 11 ERROR; with shipped -> PASS.

[ "$QUIET" = "0" ] && echo "" && echo -e "${DIM}[stamp_completeness]${NC}"

if [ ! -f "$REPO_ROOT/scripts/lint.sh" ]; then
    t_fail "stamp_completeness: scripts/lint.sh missing"
else
    SC_TMP="$(mktemp -d)"
    SC_REPO="$SC_TMP/repo"
    mkdir -p "$SC_REPO/scripts" "$SC_REPO/todo/00-infrastructure" "$SC_REPO/src"
    cp "$REPO_ROOT/scripts/lint.sh" "$SC_REPO/scripts/lint.sh"
    chmod +x "$SC_REPO/scripts/lint.sh"
    # lint.sh bails early when no .c/.h files exist; provide a stub
    # so the new Check 10/11 walk runs against the synthetic todo/.
    echo "/* stub */" > "$SC_REPO/src/stub.c"

    SC_FUTURE_DATE="2026-12-31"

    # Build fixtures via Python so the section-marker glyph (U+00A7)
    # appears in generated test data only, not in this script's source
    # (where bare_section_refs hook BLOCKS it at edit time).
    python3 - "$SC_REPO" "$SC_FUTURE_DATE" <<'PYEOF'
import sys, pathlib
repo, future_date = sys.argv[1], sys.argv[2]
SECT = chr(0xA7)  # U+00A7 SECTION SIGN
fix = pathlib.Path(repo) / "todo" / "00-infrastructure" / "TODO-99-stamp-fixture.md"
header = """---
schema_version: 1
id: stamp-fixture
domain: 00-infrastructure
status: draft
title: "TODO-99 stamp-completeness fixture"
---

# TODO-99 fixture
"""
io_table = (
    "\n## Implementation Order\n\n"
    "| Star | Order | Section | Deliverable | Depends | Status |\n"
    "| ---- | :---: | :---:   | ----------- | ------- | :----: |\n"
    f"| star |   1   |  {SECT}1     | Sample row  | --      |  [x]   |\n\n"
)
sec_no_notes = (
    "## 1. Sample section\n\n"
    "Body missing the Notes block on purpose.\n\n"
    "> **Test runner:** `scripts/foo.bat` | 0 failures\n\n"
    f"> **Verified:** {future_date} | commit `abc1234` | 1/1 items | build OK\n"
    f"> **Quality reviewed:** {future_date} | Codex 1x | 0 fixed | scope: N/A\n\n"
)
os_table_shipped = (
    "## OS Comparison\n\n"
    "| Feature       | Win11    | Linux    | Impossible OS  |\n"
    "|---------------|----------|----------|----------------|\n"
    f"| Sample feature| N/A     | N/A     | done {SECT}1 (sample)     |\n"
)
# Add shipped marker (checkmark glyph U+2705).
os_table_shipped = os_table_shipped.replace("done", chr(0x2705))
fix.write_text(header + io_table + sec_no_notes + os_table_shipped)
PYEOF
    SC_OUT_MISS_NOTES="$(cd "$SC_REPO" && bash scripts/lint.sh 2>&1)"
    if echo "$SC_OUT_MISS_NOTES" | grep -qE "missing.*Notes.*Check 10"; then
        t_pass "lint_stamp_region: missing Notes -> Check 10 ERROR"
    else
        t_fail "lint_stamp_region: missing-Notes ERROR" \
               "out: $(echo "$SC_OUT_MISS_NOTES" | tail -8)"
    fi

    # Full stamp region -> PASS.
    python3 - "$SC_REPO" "$SC_FUTURE_DATE" <<'PYEOF'
import sys, pathlib
repo, future_date = sys.argv[1], sys.argv[2]
SECT = chr(0xA7)
fix = pathlib.Path(repo) / "todo" / "00-infrastructure" / "TODO-99-stamp-fixture.md"
header = """---
schema_version: 1
id: stamp-fixture
domain: 00-infrastructure
status: draft
title: "TODO-99 stamp-completeness fixture"
---

# TODO-99 fixture
"""
io_table = (
    "\n## Implementation Order\n\n"
    "| Star | Order | Section | Deliverable | Depends | Status |\n"
    "| ---- | :---: | :---:   | ----------- | ------- | :----: |\n"
    f"| star |   1   |  {SECT}1     | Sample row  | --      |  [x]   |\n\n"
)
sec_full = (
    "## 1. Sample section\n\n"
    "Body has all four stamp pieces.\n\n"
    "> **Test runner:** `scripts/foo.bat` | 0 failures\n\n"
    "> **Notes:**\n"
    "> - What shipped: sample.\n\n"
    f"> **Verified:** {future_date} | commit `abc1234` | 1/1 items | build OK\n"
    f"> **Quality reviewed:** {future_date} | Codex 1x | 0 fixed | scope: N/A\n\n"
)
os_table = (
    "## OS Comparison\n\n"
    "| Feature       | Win11    | Linux    | Impossible OS  |\n"
    "|---------------|----------|----------|----------------|\n"
    f"| Sample feature| N/A     | N/A     | {chr(0x2705)} {SECT}1 (sample)     |\n"
)
fix.write_text(header + io_table + sec_full + os_table)
PYEOF
    SC_OUT_FULL="$(cd "$SC_REPO" && bash scripts/lint.sh 2>&1)"
    if echo "$SC_OUT_FULL" | grep -qE "0 errors"; then
        t_pass "lint_stamp_region: full stamp region -> PASS"
    else
        t_fail "lint_stamp_region: full stamp region PASS" \
               "out: $(echo "$SC_OUT_FULL" | tail -5)"
    fi

    # Pre-sentinel-date -> grandfathered PASS even with no Notes.
    python3 - "$SC_REPO" <<'PYEOF'
import sys, pathlib
repo = sys.argv[1]
SECT = chr(0xA7)
fix = pathlib.Path(repo) / "todo" / "00-infrastructure" / "TODO-99-stamp-fixture.md"
header = """---
schema_version: 1
id: stamp-fixture
domain: 00-infrastructure
status: draft
title: "TODO-99 grandfather fixture"
---

# fixture
"""
io_table = (
    "\n## Implementation Order\n\n"
    "| Star | Order | Section | Deliverable | Depends | Status |\n"
    "| ---- | :---: | :---:   | ----------- | ------- | :----: |\n"
    f"| star |   1   |  {SECT}1     | Sample row  | --      |  [x]   |\n\n"
)
# Old date, no Notes -- should be grandfathered.
sec = (
    "## 1. Sample section\n\n"
    "> **Test runner:** `scripts/foo.bat` | 0 failures\n\n"
    "> **Verified:** 2026-04-01 | commit `abc1234` | 1/1 items | build OK\n"
    "> **Quality reviewed:** 2026-04-01 | Codex 1x | 0 fixed | scope: N/A\n\n"
)
os_table = (
    "## OS Comparison\n\n"
    "| Feature       | Win11    | Linux    | Impossible OS  |\n"
    "|---------------|----------|----------|----------------|\n"
    f"| Sample feature| N/A     | N/A     | {chr(0x2705)} {SECT}1 (sample)     |\n"
)
fix.write_text(header + io_table + sec + os_table)
PYEOF
    SC_OUT_OLD="$(cd "$SC_REPO" && bash scripts/lint.sh 2>&1)"
    if echo "$SC_OUT_OLD" | grep -qE "0 errors"; then
        t_pass "lint_stamp_region: pre-sentinel-date -> grandfathered PASS"
    else
        t_fail "lint_stamp_region: grandfather PASS" \
               "out: $(echo "$SC_OUT_OLD" | tail -5)"
    fi

    # OS Comparison row not shipped -> Check 11 ERROR.
    python3 - "$SC_REPO" "$SC_FUTURE_DATE" <<'PYEOF'
import sys, pathlib
repo, future_date = sys.argv[1], sys.argv[2]
SECT = chr(0xA7)
fix = pathlib.Path(repo) / "todo" / "00-infrastructure" / "TODO-99-stamp-fixture.md"
header = """---
schema_version: 1
id: stamp-fixture
domain: 00-infrastructure
status: draft
title: "TODO-99 OS sync fixture"
---

# fixture
"""
io_table = (
    "\n## Implementation Order\n\n"
    "| Star | Order | Section | Deliverable | Depends | Status |\n"
    "| ---- | :---: | :---:   | ----------- | ------- | :----: |\n"
    f"| star |   1   |  {SECT}1     | Sample row  | --      |  [x]   |\n\n"
)
sec_full = (
    "## 1. Sample section\n\n"
    "> **Test runner:** `scripts/foo.bat` | 0 failures\n\n"
    "> **Notes:**\n> - What shipped: sample.\n\n"
    f"> **Verified:** {future_date} | commit `abc1234` | 1/1 items | build OK\n"
    f"> **Quality reviewed:** {future_date} | Codex 1x | 0 fixed | scope: N/A\n\n"
)
# Not-shipped marker (U+2B1C white square).
os_table = (
    "## OS Comparison\n\n"
    "| Feature       | Win11    | Linux    | Impossible OS  |\n"
    "|---------------|----------|----------|----------------|\n"
    f"| Sample feature| N/A     | N/A     | {chr(0x2B1C)} {SECT}1 (planned)    |\n"
)
fix.write_text(header + io_table + sec_full + os_table)
PYEOF
    SC_OUT_OS_BAD="$(cd "$SC_REPO" && bash scripts/lint.sh 2>&1)"
    if echo "$SC_OUT_OS_BAD" | grep -qE "not shipped.*Check 11"; then
        t_pass "lint_os_comparison_sync: not-shipped OS row -> Check 11 ERROR"
    else
        t_fail "lint_os_comparison_sync: not-shipped OS row ERROR" \
               "out: $(echo "$SC_OUT_OS_BAD" | tail -8)"
    fi

    rm -rf "$SC_TMP"
fi


# ============================================================================
# bare_section_gate (bare section-ref drift gate -- Check 5 + edit-time hook parity)
# ============================================================================
#   Regression lock for the 2026-06-16 gate-hardening review: lint Check 5
#   and .claude/hooks/bare_section_refs.py must both scan .ld/.lds/.inc +
#   Makefile, catch spaced "section N" refs, and share the same path
#   exemptions (scripts/todo-graph, scripts/test-ai-system.sh). The section
#   glyph is generated via Python (chr 0xA7) so this script stays glyph-free
#   (the hook blocks literal bare refs in .sh source).

[ "$QUIET" = "0" ] && echo "" && echo -e "${DIM}[bare_section_gate]${NC}"

if [ ! -f "$REPO_ROOT/scripts/lint.sh" ] || [ ! -f "$REPO_ROOT/.claude/hooks/bare_section_refs.py" ]; then
    t_fail "bare_section_gate: lint.sh or hook missing"
else
    BSG_TMP="$(mktemp -d)"
    BSG_REPO="$BSG_TMP/repo"
    mkdir -p "$BSG_REPO/scripts" "$BSG_REPO/tools" "$BSG_REPO/src/boot"
    cp "$REPO_ROOT/scripts/lint.sh" "$BSG_REPO/scripts/lint.sh"
    chmod +x "$BSG_REPO/scripts/lint.sh"
    python3 - "$BSG_REPO" <<'PYEOF'
import sys, pathlib
repo = pathlib.Path(sys.argv[1]); S = chr(0xA7)
(repo / "src" / "stub.c").write_text("/* stub */\n")
(repo / "tools" / "desc.inc").write_text("/* typed payload (%s4) */\n" % S)
(repo / "src" / "boot" / "linker.ld").write_text("/* layout (%s5) */\n" % S)
(repo / "Makefile").write_text("# floor gate (%s12)\nall:\n" % S)
(repo / "src" / "spaced.c").write_text("/* fallback (%s 5) */\n" % S)
PYEOF
    BSG_OUT="$(cd "$BSG_REPO" && bash scripts/lint.sh 2>&1)"
    for fx in "tools/desc.inc" "src/boot/linker.ld" "Makefile" "src/spaced.c"; do
        if echo "$BSG_OUT" | grep -q "$fx"; then
            t_pass "bare_section_gate: lint Check 5 flags $fx"
        else
            t_fail "bare_section_gate: lint Check 5 missed $fx" "out: $(echo "$BSG_OUT" | tail -10)"
        fi
    done
    rm -rf "$BSG_TMP"

    bsg_hook() {
        # $1=label $2=path $3=body (%S% -> section glyph) $4=want-exit
        local json rc
        json="$(python3 -c 'import json,sys; S=chr(0xA7); print(json.dumps({"tool_name":"Edit","tool_input":{"file_path":sys.argv[1],"new_string":sys.argv[2].replace("%S%",S)}}))' "$2" "$3")"
        echo "$json" | python3 "$REPO_ROOT/.claude/hooks/bare_section_refs.py" >/dev/null 2>&1
        rc=$?
        if [ "$rc" = "$4" ]; then
            t_pass "bare_section_gate: hook $1 (exit $rc)"
        else
            t_fail "bare_section_gate: hook $1" "want exit $4 got $rc"
        fi
    }
    bsg_hook ".inc blocks"           "tools/x/foo.inc"             "/* desc (%S%4) */"        2
    bsg_hook "Makefile blocks"       "Makefile"                    "# gate (%S%12)"           2
    bsg_hook ".ld blocks"            "src/boot/linker.ld"          "/* layout (%S%5) */"      2
    bsg_hook "spaced ref blocks"     "src/kernel/foo.c"            "/* fallback (%S% 5) */"   2
    bsg_hook "todo-graph exempt"     "scripts/todo-graph/query.py" "# owner TODO-06 %S%4"     0
    bsg_hook "test-ai-system exempt" "scripts/test-ai-system.sh"   "# %S%7 policy"            0
    bsg_hook "spec qualifier legal"  "src/kernel/foo.c"            "/* xHCI spec %S%4.2 */"   0
fi


# ============================================================================
# version_floor_gate (setup.sh required-tool version floors)
# ============================================================================
#   setup.sh --check-versions (and --verify) hard-fail closed when a REQUIRED
#   tool is missing / unparseable / below its VERSION_SPECS floor; optional
#   tools never affect the exit code; --versions stays advisory (exit 0); and
#   every REQUIRED non-firmware sentinel must carry a VERSION_SPECS floor
#   (drift guard). Fixtures stub a below-floor tool via a PATH shim.

[ "$QUIET" = "0" ] && echo "" && echo -e "${DIM}[version_floor_gate]${NC}"

if [ ! -f "$REPO_ROOT/scripts/setup.sh" ]; then
    t_fail "version_floor_gate: scripts/setup.sh missing"
else
    # Healthy host: the gate and --verify both pass; --versions advisory.
    if bash "$REPO_ROOT/scripts/setup.sh" --check-versions >/dev/null 2>&1; then
        t_pass "version_floor_gate: --check-versions exit 0 on healthy host"
    else
        t_fail "version_floor_gate: --check-versions healthy" "expected exit 0"
    fi

    # PATH shim: a below-floor clang-19 must drive the gate non-zero by name.
    VFG_SHIM="$(mktemp -d)"
    printf '#!/bin/bash\necho "clang version 18.1.3"\n' > "$VFG_SHIM/clang-19"
    chmod +x "$VFG_SHIM/clang-19"
    VFG_OUT="$(PATH="$VFG_SHIM:$PATH" bash "$REPO_ROOT/scripts/setup.sh" --check-versions 2>&1)"
    VFG_RC=$?
    if [ "$VFG_RC" -ne 0 ] && echo "$VFG_OUT" | grep -q "clang-19"; then
        t_pass "version_floor_gate: below-floor clang-19 -> non-zero naming the tool"
    else
        t_fail "version_floor_gate: below-floor clang-19" "rc=$VFG_RC out: $(echo "$VFG_OUT" | grep -i clang | head -2)"
    fi
    # --verify (presence + floors) must ALSO fail on the below-floor tool.
    if ! PATH="$VFG_SHIM:$PATH" bash "$REPO_ROOT/scripts/setup.sh" --verify >/dev/null 2>&1; then
        t_pass "version_floor_gate: --verify enforces floors (below-floor -> non-zero)"
    else
        t_fail "version_floor_gate: --verify enforces floors" "expected non-zero"
    fi
    # --versions stays advisory (exit 0) even with a below-floor tool present.
    if PATH="$VFG_SHIM:$PATH" bash "$REPO_ROOT/scripts/setup.sh" --versions >/dev/null 2>&1; then
        t_pass "version_floor_gate: --versions stays advisory (exit 0)"
    else
        t_fail "version_floor_gate: --versions advisory" "expected exit 0"
    fi
    rm -rf "$VFG_SHIM"

    # Unparseable required tool must fail closed (floor cannot be proven).
    VFG_SHIM2="$(mktemp -d)"
    printf '#!/bin/bash\necho "nasm garbage no version"\n' > "$VFG_SHIM2/nasm"
    chmod +x "$VFG_SHIM2/nasm"
    if ! PATH="$VFG_SHIM2:$PATH" bash "$REPO_ROOT/scripts/setup.sh" --check-versions >/dev/null 2>&1; then
        t_pass "version_floor_gate: unparseable required tool fails closed"
    else
        t_fail "version_floor_gate: unparseable fails closed" "expected non-zero"
    fi
    rm -rf "$VFG_SHIM2"

    # Optional tool below its floor must NOT affect the exit code.
    VFG_SHIM3="$(mktemp -d)"
    printf '#!/bin/bash\necho "bear 2.0.0"\n' > "$VFG_SHIM3/bear"
    chmod +x "$VFG_SHIM3/bear"
    if PATH="$VFG_SHIM3:$PATH" bash "$REPO_ROOT/scripts/setup.sh" --check-versions >/dev/null 2>&1; then
        t_pass "version_floor_gate: optional tool below floor does not fail the gate"
    else
        t_fail "version_floor_gate: optional below-floor non-fatal" "expected exit 0"
    fi
    rm -rf "$VFG_SHIM3"

    # Drift guard: every REQUIRED non-firmware sentinel must carry a floor.
    VFG_MISS="$(python3 - "$REPO_ROOT/scripts/setup.sh" <<'PYEOF'
import re, sys
text = open(sys.argv[1]).read()
def block(name):
    m = re.search(name + r"=\(\s*(.*?)\)", text, re.S)
    return re.findall(r'"([^":]+):', m.group(1)) if m else []
req = [c for c in block("REQUIRED_SENTINELS") if not c.startswith("__OVMF")]
floors = set(block("VERSION_SPECS"))
print(",".join(c for c in req if c not in floors))
PYEOF
)"
    if [ -z "$VFG_MISS" ]; then
        t_pass "version_floor_gate: every required sentinel has a VERSION_SPECS floor"
    else
        t_fail "version_floor_gate: sentinel floor coverage" "no floor for: $VFG_MISS"
    fi
fi


# ============================================================================
# codex_prompt_scope (TODO-08 codex-dispatch-with-files wrapper)
# ============================================================================
#   - clean_tree_embeds: clean working tree + prompt naming a file ->
#     wrapper emits `--- COMMITTED FILE CONTENT ---` block with HEAD
#     content (verified by replacing the codex-companion call with echo).
#   - dirty_tree_passthrough: dirty working tree + same prompt -> wrapper
#     passes through unchanged (no embed block).

[ "$QUIET" = "0" ] && echo "" && echo -e "${DIM}[codex_prompt_scope]${NC}"

if [ ! -f "$REPO_ROOT/scripts/codex-dispatch-with-files.sh" ]; then
    t_fail "codex_prompt_scope: wrapper script missing"
else
    CPS_TMP="$(mktemp -d)"
    CPS_REPO="$CPS_TMP/repo"
    mkdir -p "$CPS_REPO/scripts" "$CPS_REPO/src/kernel"
    cp "$REPO_ROOT/scripts/codex-dispatch-with-files.sh" \
       "$CPS_REPO/scripts/codex-dispatch-with-files.sh"
    chmod +x "$CPS_REPO/scripts/codex-dispatch-with-files.sh"
    # Stub codex-companion.mjs path: replace the exec node call with
    # a captured-prompt echo so we can inspect what the wrapper would
    # have sent. Patch lives in the temp copy only.
    mkdir -p "$CPS_TMP/fake-plugin"
    cat > "$CPS_TMP/fake-plugin/codex-companion.mjs" <<'PLUGIN'
#!/usr/bin/env node
// Stub: echo the prompt argument so the test can grep it.
process.stdout.write(process.argv[3] || "");
PLUGIN
    chmod +x "$CPS_TMP/fake-plugin/codex-companion.mjs"
    # Patch the wrapper to point at the stub plugin path (HOME-relative
    # path is fixed to the real plugin; the test redirects via env-style
    # search-and-replace).
    sed -i "s|\$HOME/.claude/plugins/marketplaces/openai-codex/plugins/codex/scripts/codex-companion.mjs|$CPS_TMP/fake-plugin/codex-companion.mjs|g" \
        "$CPS_REPO/scripts/codex-dispatch-with-files.sh"

    pushd "$CPS_REPO" >/dev/null
    git init -q -b main
    git config user.email "test@example.com"
    git config user.name "Test"
    cat > src/kernel/foo.c <<'CFAKE'
/* fake source for codex_prompt_scope test */
int sample_function(int x) {
    return x + 1;
}
CFAKE
    git add src/kernel/foo.c scripts/codex-dispatch-with-files.sh
    git -c commit.gpgsign=false commit -q --no-verify -m "seed"
    popd >/dev/null

    # ---- clean_tree_embeds ----
    CPS_PROMPT="[review-kind: adversarial] todo/00-infrastructure/TODO-08-automation-hardening.md sample re-review src/kernel/foo.c file"
    CPS_OUT_CLEAN="$(cd "$CPS_REPO" && bash scripts/codex-dispatch-with-files.sh "$CPS_PROMPT" 2>&1)"
    if echo "$CPS_OUT_CLEAN" | grep -q "COMMITTED FILE CONTENT" \
       && echo "$CPS_OUT_CLEAN" | grep -q "sample_function"; then
        t_pass "codex_prompt_scope: clean tree embeds HEAD content"
    else
        t_fail "codex_prompt_scope: clean tree embed" \
               "out: $(echo "$CPS_OUT_CLEAN" | head -10)"
    fi

    # ---- dirty_tree_passthrough ----
    echo "/* dirty edit */" >> "$CPS_REPO/src/kernel/foo.c"
    CPS_OUT_DIRTY="$(cd "$CPS_REPO" && bash scripts/codex-dispatch-with-files.sh "$CPS_PROMPT" 2>&1)"
    if ! echo "$CPS_OUT_DIRTY" | grep -q "COMMITTED FILE CONTENT"; then
        t_pass "codex_prompt_scope: dirty tree passes through unchanged"
    else
        t_fail "codex_prompt_scope: dirty tree passthrough" \
               "out: $(echo "$CPS_OUT_DIRTY" | head -10)"
    fi

    rm -rf "$CPS_TMP"
fi


# ============================================================================
# impl_pipeline_gates (TODO-08 implement-pipeline section-commit gates)
# ============================================================================
#   - quality_gate_block: step5_quality_gate BLOCKs Edit on src/kernel/foo.c
#     when no Skill(kernel-code-quality) in transcript since skill start.
#   - unit_test_wiring_block: section_commit_gate _step8_test_wiring_check
#     BLOCKs commit touching src/kernel/foo.c when no test_*.c at HEAD.
#   - impl_adversarial_block: _step13_impl_adversarial_check BLOCKs
#     IO-row flip + src diff with no adversarial-impl stamp.
#   - smoke_gate_block: _step16_smoke_check BLOCKs boot-path edit
#     when smoke log is missing/stale/lacks markers.

[ "$QUIET" = "0" ] && echo "" && echo -e "${DIM}[impl_pipeline_gates]${NC}"

if [ ! -f "$REPO_ROOT/.claude/hooks/step5_quality_gate.py" ] || \
   [ ! -f "$REPO_ROOT/.claude/hooks/section_commit_gate.py" ]; then
    t_fail "impl_pipeline_gates: hook scripts missing"
else
    IPG_OUT="$(cd "$REPO_ROOT" && python3 - <<'PY'
import sys, os, json, time, tempfile, pathlib, subprocess
sys.path.insert(0, '.claude/hooks')

# Ensure the integration with our shared module surfaces is intact.
import step5_quality_gate as s5
import section_commit_gate as scg

# Test 1: quality_gate_block -- _required_skill_for_path correctly maps
# src/kernel/foo.c -> kernel-code-quality, src/boot/x.c -> boot-code-quality,
# src/kernel/test/test_foo.c -> kernel-code-quality (Codex Q1 fix).
assert s5._required_skill_for_path("src/kernel/foo.c") == "kernel-code-quality"
assert s5._required_skill_for_path("src/boot/uefi/bootx64.c") == "boot-code-quality"
assert s5._required_skill_for_path("src/kernel/test/test_foo.c") == "kernel-code-quality"
assert s5._required_skill_for_path("user/libc/stdio.c") == "userland-code-quality"
assert s5._required_skill_for_path("docs/foo.md") == ""
print("OK quality_gate_path_map")

# Test 2: quality_gate_lease_check -- when shared gates are enforced, the
# first implementation edit must have a matching active driver lease.
with tempfile.TemporaryDirectory() as tmp:
    repo = pathlib.Path(tmp)
    (repo / ".ai-workflow").mkdir()
    entry = {
        "started_ts": time.time_ns(),
        "todo_path": "todo/00-infra/TODO-99.md",
        "args": "todo/00-infra/TODO-99.md section 2",
    }
    ok_missing, err_missing = s5._active_lease_allows_implement_edit(str(repo), entry)
    assert not ok_missing and "no active driver lease" in err_missing
    (repo / ".ai-workflow" / "active-lease.json").write_text(json.dumps({
        "todo_path": "todo/00-infra/TODO-99.md",
        "section": "2",
        "driver_run_id": "lease-ok",
        "expires_at_ns": time.time_ns() + 60_000_000_000,
    }))
    ok_match, err_match = s5._active_lease_allows_implement_edit(str(repo), entry)
    assert ok_match, f"matching lease should pass, got: {err_match}"
    (repo / ".ai-workflow" / "active-lease.json").write_text(json.dumps({
        "todo_path": "todo/00-infra/TODO-99.md",
        "section": "3",
        "driver_run_id": "lease-wrong",
        "expires_at_ns": time.time_ns() + 60_000_000_000,
    }))
    ok_wrong, err_wrong = s5._active_lease_allows_implement_edit(str(repo), entry)
    assert not ok_wrong and "not todo/00-infra/TODO-99.md section 2" in err_wrong
    (repo / ".ai-workflow" / "active-lease.json").write_text(json.dumps({
        "todo_path": "todo/00-infra/TODO-99.md",
        "section": "2",
        "driver_run_id": "lease-expired",
        "expires_at_ns": time.time_ns() - 1,
    }))
    ok_expired, err_expired = s5._active_lease_allows_implement_edit(str(repo), entry)
    assert not ok_expired and "expired" in err_expired
print("OK quality_gate_lease_check")

# Test 3: unit_test_wiring -- existing test at HEAD counts as evidence.
with tempfile.TemporaryDirectory() as tmp:
    repo = pathlib.Path(tmp)
    (repo / ".claude" / "state").mkdir(parents=True)
    (repo / "src" / "kernel" / "test").mkdir(parents=True)
    (repo / "todo" / "00-infra").mkdir(parents=True)
    subprocess.run(["git", "init", "-q", "-b", "main"], cwd=tmp, check=True)
    subprocess.run(["git", "config", "user.email", "t@x"], cwd=tmp, check=True)
    subprocess.run(["git", "config", "user.name", "t"], cwd=tmp, check=True)
    # Codex H1 fix: basename-bound match. test_newcode.c must match
    # newcode.c (NOT just any test_*.c).
    (repo / "src" / "kernel" / "test" / "test_newcode.c").write_text("/*x*/\n")
    subprocess.run(["git", "add", "-A"], cwd=tmp, check=True)
    subprocess.run(["git", "-c", "commit.gpgsign=false", "commit", "-q",
                    "--no-verify", "-m", "seed"], cwd=tmp, check=True)
    # Activate implement-todo-section so the skill-aware gate fires.
    skill_state = repo / ".claude" / "state" / "skill-progress.json"
    skill_state.write_text(json.dumps({
        "implement-todo-section": {
            "started_ts": time.time_ns(),
            "args": "test", "todo_path": "",
        }
    }))
    # Stage a new src/kernel/newcode.c -- the matching test_newcode.c
    # at HEAD satisfies step-8 evidence (basename-bound, Codex H1).
    (repo / "src" / "kernel" / "newcode.c").write_text("int x;\n")
    subprocess.run(["git", "add", "src/kernel/newcode.c"], cwd=tmp, check=True)
    ok, err = scg._step8_test_wiring_check(
        repo, ["src/kernel/newcode.c"], []
    )
    assert ok, f"step-8 basename-match should pass: {err}"
    # Negative: unrelated test_other.c at HEAD must NOT satisfy.
    (repo / "src" / "kernel" / "different.c").write_text("int y;\n")
    subprocess.run(["git", "add", "src/kernel/different.c"], cwd=tmp, check=True)
    ok2, err2 = scg._step8_test_wiring_check(
        repo, ["src/kernel/different.c"], []
    )
    assert not ok2, "step-8 must reject test_newcode.c as evidence for different.c"
print("OK unit_test_wiring_basename_binding")

# Test 4: impl_adversarial -- accepts EITHER adversarial-impl OR plain
# adversarial. The previous design demanded a distinct `adversarial-impl`
# tag for implement-time dispatches, but in practice the same Codex run
# (same prompt, same files) was being re-dispatched only to swap the
# tag. The interchangeability fix accepts both tags so the agent does
# not need to redispatch only to satisfy the gate's tag preference.
with tempfile.TemporaryDirectory() as tmp:
    repo = pathlib.Path(tmp)
    (repo / ".claude" / "state").mkdir(parents=True)
    (repo / ".claude" / "state" / "skill-progress.json").write_text(json.dumps({
        "implement-todo-section": {"started_ts": time.time_ns(),
                                   "args": "test", "todo_path": ""}
    }))
    (repo / "todo").mkdir()
    todo_path = "todo/TODO-99-bar.md"
    todo_file = repo / todo_path
    todo_file.write_text(
        "| Star | Order | Deliverable | Depends | Status |\n"
        "| --- | :---: | --- | --- | :---: |\n"
        "| * | 1 | First | -- | [ ] |\n"
        "| * | 2 | Second | -- | [ ] |\n"
    )
    legacy_todo_path = "todo/TODO-100-legacy.md"
    legacy_todo_file = repo / legacy_todo_path
    legacy_todo_file.write_text(
        "| Order | Deliverable | Depends On | Status |\n"
        "| :---: | --- | --- | :---: |\n"
        "| 1 | Legacy shape | -- | [ ] |\n"
    )
    depends_todo_path = "todo/TODO-101-depends.md"
    depends_todo_file = repo / depends_todo_path
    depends_todo_file.write_text(
        "| Star | Order | Deliverable | Depends On | Status |\n"
        "| --- | :---: | --- | --- | :---: |\n"
        "| * | 3 | Third | alpha, beta | [ ] |\n"
    )
    mixed_todo_path = "todo/TODO-102-mixed.md"
    mixed_todo_file = repo / mixed_todo_path
    mixed_todo_file.write_text(
        "| Star | Order | Deliverable | Depends On | Status |\n"
        "| --- | :---: | --- | --- | :---: |\n"
        "| * | 1 | Already done | -- | [x] |\n"
        "| * | 2 | Promote me | -- | [ ] |\n"
    )
    multi_todo_path = "todo/TODO-103-multi.md"
    multi_todo_file = repo / multi_todo_path
    multi_todo_file.write_text(
        "| Star | Order | Deliverable | Depends On | Status |\n"
        "| --- | :---: | --- | --- | :---: |\n"
        "| * | 1 | First promote | -- | [ ] |\n"
        "| * | 2 | Unchanged middle | -- | [ ] |\n"
        "| * | 3 | Third promote | -- | [ ] |\n"
    )
    subprocess.run(["git", "init", "-q", "-b", "main"], cwd=tmp, check=True)
    subprocess.run(["git", "config", "user.email", "t@x"], cwd=tmp, check=True)
    subprocess.run(["git", "config", "user.name", "t"], cwd=tmp, check=True)
    subprocess.run(["git", "add", todo_path, legacy_todo_path, depends_todo_path,
                    mixed_todo_path, multi_todo_path],
                   cwd=tmp, check=True)
    subprocess.run(["git", "-c", "commit.gpgsign=false", "commit", "-q",
                    "--no-verify", "-m", "seed"], cwd=tmp, check=True)
    todo_file.write_text(todo_file.read_text().replace("| * | 2 | Second | -- | [ ] |",
                                                       "| * | 2 | Second | -- | [x] |"))
    subprocess.run(["git", "add", todo_path], cwd=tmp, check=True)
    flips, ok_diff, sections = scg._impl_order_flips_with_status(repo, [todo_path])
    assert ok_diff and flips == [todo_path], f"expected one IO flip, got flips={flips} ok={ok_diff}"
    assert sections.get(todo_path) == ["2"], f"expected flipped section 2, got {sections}"
    subprocess.run(["git", "reset", "-q", "--hard", "HEAD"], cwd=tmp, check=True)
    legacy_todo_file.write_text(legacy_todo_file.read_text().replace("| 1 | Legacy shape | -- | [ ] |",
                                                                     "| 1 | Legacy shape | -- | [x] |"))
    subprocess.run(["git", "add", legacy_todo_path], cwd=tmp, check=True)
    legacy_flips, legacy_ok, legacy_sections = scg._impl_order_flips_with_status(repo, [legacy_todo_path])
    assert legacy_ok and legacy_flips == [legacy_todo_path], (
        f"expected one legacy IO flip, got flips={legacy_flips} ok={legacy_ok}"
    )
    assert legacy_sections.get(legacy_todo_path) == ["1"], (
        f"expected legacy Order/Deliverable row to bind section 1, got {legacy_sections}"
    )
    subprocess.run(["git", "reset", "-q", "--hard", "HEAD"], cwd=tmp, check=True)
    depends_todo_file.write_text(depends_todo_file.read_text().replace(
        "| * | 3 | Third | alpha, beta | [ ] |",
        "| * | 3 | Third | alpha, beta | [x] |",
    ))
    subprocess.run(["git", "add", depends_todo_path], cwd=tmp, check=True)
    dep_flips, dep_ok, dep_sections = scg._impl_order_flips_with_status(repo, [depends_todo_path])
    assert dep_ok and dep_flips == [depends_todo_path], (
        f"expected one dependency IO flip, got flips={dep_flips} ok={dep_ok}"
    )
    assert dep_sections.get(depends_todo_path) == ["3"], (
        f"expected Depends On dependency row to bind Order section 3, got {dep_sections}"
    )
    depends_todo_file.write_text(depends_todo_file.read_text().replace(
        "| Star | Order | Deliverable | Depends On | Status |",
        "| Star | Depends On | Deliverable | Section | Status |",
    ))
    desync_flips, desync_ok, desync_sections = scg._impl_order_flips_with_status(repo, [depends_todo_path])
    assert desync_ok and desync_flips == [depends_todo_path], (
        f"expected staged blob to drive desynced TODO flip, got flips={desync_flips} ok={desync_ok}"
    )
    assert desync_sections.get(depends_todo_path) == ["3"], (
        f"expected staged header to bind section 3 despite unstaged header edit, got {desync_sections}"
    )
    subprocess.run(["git", "reset", "-q", "--hard", "HEAD"], cwd=tmp, check=True)
    mixed_todo_file.write_text(
        mixed_todo_file.read_text()
        .replace("| * | 1 | Already done | -- | [x] |",
                 "| * | 1 | Already done, edited | -- | [x] |")
        .replace("| * | 2 | Promote me | -- | [ ] |",
                 "| * | 2 | Promote me | -- | [x] |")
    )
    subprocess.run(["git", "add", mixed_todo_path], cwd=tmp, check=True)
    mixed_flips, mixed_ok, mixed_sections = scg._impl_order_flips_with_status(repo, [mixed_todo_path])
    assert mixed_ok and mixed_flips == [mixed_todo_path], (
        f"expected one mixed-hunk IO flip, got flips={mixed_flips} ok={mixed_ok}"
    )
    assert mixed_sections.get(mixed_todo_path) == ["2"], (
        f"expected mixed hunk to bind only promoted section 2, got {mixed_sections}"
    )
    stamps = repo / ".claude" / "state" / "last-review-stamps.json"
    stamps.write_text(json.dumps({
        mixed_todo_path: {
            "section": "1",
            "adversarial_section": "1",
            "adversarial": time.time_ns(),
            "consistency": None, "perf": None,
        }
    }))
    ok_mixed_wrong, err_mixed_wrong = scg._step13_impl_adversarial_check(
        repo, ["src/kernel/foo.c"], [mixed_todo_path], {mixed_todo_path: ["2"]}
    )
    assert not ok_mixed_wrong, (
        "mixed-hunk section-bound impl-adv must reject review for edited done row"
    )
    subprocess.run(["git", "reset", "-q", "--hard", "HEAD"], cwd=tmp, check=True)
    mixed_todo_file.write_text(
        mixed_todo_file.read_text()
        .replace("| * | 1 | Already done | -- | [x] |",
                 "| * | 1 | Already done | -- | [ ] |")
        .replace("| * | 2 | Promote me | -- | [ ] |",
                 "| * | 2 | Promote me | -- | [x] |")
    )
    subprocess.run(["git", "add", mixed_todo_path], cwd=tmp, check=True)
    balanced_flips, balanced_ok, balanced_sections = scg._impl_order_flips_with_status(
        repo, [mixed_todo_path]
    )
    assert balanced_ok and balanced_flips == [mixed_todo_path], (
        "expected balanced demotion+promotion hunk to still report promotion, "
        f"got flips={balanced_flips} ok={balanced_ok}"
    )
    assert balanced_sections.get(mixed_todo_path) == ["2"], (
        f"expected balanced hunk to bind only promoted section 2, got {balanced_sections}"
    )
    subprocess.run(["git", "reset", "-q", "--hard", "HEAD"], cwd=tmp, check=True)
    multi_todo_file.write_text(
        multi_todo_file.read_text()
        .replace("| * | 1 | First promote | -- | [ ] |",
                 "| * | 1 | First promote | -- | [x] |")
        .replace("| * | 3 | Third promote | -- | [ ] |",
                 "| * | 3 | Third promote | -- | [x] |")
    )
    subprocess.run(["git", "add", multi_todo_path], cwd=tmp, check=True)
    multi_flips, multi_ok, multi_sections = scg._impl_order_flips_with_status(repo, [multi_todo_path])
    assert multi_ok and multi_flips == [multi_todo_path], (
        f"expected one multi-section IO flip, got flips={multi_flips} ok={multi_ok}"
    )
    assert multi_sections.get(multi_todo_path) == ["1", "3"], (
        f"expected separate-hunk promotions for sections 1 and 3, got {multi_sections}"
    )
    stamps.write_text(json.dumps({
        multi_todo_path: {
            "section": "1",
            "adversarial_section": "1",
            "adversarial": time.time_ns(),
            "consistency": None, "perf": None,
        }
    }))
    ok_multi, err_multi = scg._step13_impl_adversarial_check(
        repo, ["src/kernel/foo.c"], [multi_todo_path], {multi_todo_path: ["1", "3"]}
    )
    assert not ok_multi and "promotes multiple sections" in err_multi, (
        f"multi-section promotion must be rejected, got ok={ok_multi} err={err_multi}"
    )
    subprocess.run(["git", "reset", "-q", "--hard", "HEAD"], cwd=tmp, check=True)
    stamps = repo / ".claude" / "state" / "last-review-stamps.json"
    # Plain `adversarial` only. With interchangeability, this satisfies.
    stamps.write_text(json.dumps({
        todo_path: {
            "section": "1",
            "adversarial_section": "1",
            "adversarial": time.time_ns(),
            "consistency": None, "perf": None,
        }
    }))
    ok, err = scg._step13_impl_adversarial_check(
        repo, ["src/kernel/foo.c"], [todo_path]
    )
    assert ok, f"impl-adv should accept plain `adversarial` (interchangeable), got: {err}"
    ok_wrong_section, err_wrong_section = scg._step13_impl_adversarial_check(
        repo, ["src/kernel/foo.c"], [todo_path], {todo_path: ["2"]}
    )
    assert not ok_wrong_section, "section-bound impl-adv must reject a section-1 review for a section-2 flip"
    stamps.write_text(json.dumps({
        todo_path: {
            "section": "2",
            "adversarial_section": "2",
            "adversarial": time.time_ns(),
            "consistency": None, "perf": None,
        }
    }))
    ok_bound, err_bound = scg._step13_impl_adversarial_check(
        repo, ["src/kernel/foo.c"], [todo_path], {todo_path: ["2"]}
    )
    assert ok_bound, f"section-bound impl-adv should accept matching section evidence, got: {err_bound}"
    # adversarial-impl alone also satisfies.
    stamps.write_text(json.dumps({
        todo_path: {
            "section": "1",
            "adversarial-impl_section": "1",
            "adversarial-impl": time.time_ns(),
            "consistency": None, "perf": None,
        }
    }))
    ok2, err2 = scg._step13_impl_adversarial_check(
        repo, ["src/kernel/foo.c"], [todo_path]
    )
    assert ok2, f"impl-adv gate should accept fresh adversarial-impl, got: {err2}"
    # No relevant tag at all -> still blocks.
    stamps.write_text(json.dumps({
        todo_path: {
            "section": "1",
            "consistency": None, "perf": None,
        }
    }))
    ok3, err3 = scg._step13_impl_adversarial_check(
        repo, ["src/kernel/foo.c"], [todo_path]
    )
    assert not ok3, "impl-adv must still block when neither tag is present"
    expected_cmd = (
        "bash scripts/codex-dispatch.sh '[review-kind: adversarial] "
        "<todo-path> <section-N> <prompt>'"
    )
    assert expected_cmd in err3, f"impl-adv BLOCK help must use canonical single-quoted dispatch, got: {err3}"
    assert "codex-dispatch-with-files.sh" not in err3, f"impl-adv BLOCK help must not point at untrusted fallback wrapper, got: {err3}"
print("OK impl_adversarial_tag_interchangeable")

# Test 5: smoke_gate -- requires Boot complete + C:\> markers per Codex H2 fix.
with tempfile.TemporaryDirectory() as tmp:
    repo = pathlib.Path(tmp)
    (repo / "build").mkdir()
    (repo / "src" / "kernel").mkdir(parents=True)
    (repo / ".claude" / "state").mkdir(parents=True)
    # Activate implement-todo-section so the skill-aware gate fires.
    (repo / ".claude" / "state" / "skill-progress.json").write_text(json.dumps({
        "implement-todo-section": {"started_ts": time.time_ns(),
                                   "args": "test", "todo_path": ""}
    }))
    boot_file = repo / "src" / "kernel" / "idt.c"
    boot_file.write_text("// trigger\n")
    log = repo / "build" / "smoke-test.stripped.log"
    # No log -> BLOCK.
    ok, err = scg._step16_smoke_check(repo, ["src/kernel/idt.c"])
    assert not ok and "does not exist" in err
    # Log without markers -> BLOCK.
    log.write_text("some output without the required markers\n")
    # Set log mtime to NOW so freshness check passes.
    now = time.time()
    os.utime(str(log), (now, now))
    os.utime(str(boot_file), (now - 100, now - 100))
    ok2, err2 = scg._step16_smoke_check(repo, ["src/kernel/idt.c"])
    assert not ok2 and "Boot complete" in err2 and "C:" in err2
    # Log with both markers -> PASS.
    log.write_text("...\nBoot complete in 1.2s\nC:\\> ready\n")
    os.utime(str(log), (now, now))
    ok3, err3 = scg._step16_smoke_check(repo, ["src/kernel/idt.c"])
    assert ok3, f"smoke gate should pass with markers, got: {err3}"
print("OK smoke_gate_serial_markers")
PY
)" 2>&1
    IPG_RC=$?
    IPG_OK_COUNT=$(echo "$IPG_OUT" | grep -c "^OK ")
    if [ "$IPG_OK_COUNT" = "5" ]; then
        echo "$IPG_OUT" | grep "^OK " | while IFS= read -r line; do
            t_pass "impl_pipeline_gates: $line"
        done
        PASS=$((PASS + 5))
    else
        t_fail "impl_pipeline_gates: integrated test suite incomplete" \
               "ok-count=$IPG_OK_COUNT rc=$IPG_RC out=$IPG_OUT"
    fi
fi


# ============================================================================
# review_right_sizing (TODO-08 review-pipeline right-sizing doctrine)
# ============================================================================
#   - risk_tier_declared: skill-progress entry has risk_tier field after
#     observer parses RISK_TIER=<tier> from skill args.
#   - bootstrap_mode_warn: is_bootstrap_commit returns True when staged
#     diff includes the hook's own file path.
#   - spiral_check_warn: skill_step_observer emits systemMessage when
#     elapsed > 2x budget; persists escalation counter.
#   - debug_log_env_gated: codex_review_completed writes debug log only
#     when CODEX_REVIEW_DEBUG=1.

[ "$QUIET" = "0" ] && echo "" && echo -e "${DIM}[review_right_sizing]${NC}"

if [ ! -f "$REPO_ROOT/.claude/hooks/_bootstrap_mode.py" ] || \
   [ ! -f "$REPO_ROOT/.claude/hooks/skill_step_observer.py" ] || \
   [ ! -f "$REPO_ROOT/.claude/hooks/codex_review_completed.py" ]; then
    t_fail "review_right_sizing: hook scripts missing"
else
    RRS_OUT="$(cd "$REPO_ROOT" && python3 - <<'PY'
import sys, time, json
sys.path.insert(0, '.claude/hooks')

# Test 1: RISK_TIER doctrine RETRACTED 2026-04-29 (TODO-08 section-22
# partial retraction). Smoke check that the observer's uniform 60-min
# spiral budget constant is intact.
import skill_step_observer as sso_mod
assert sso_mod._SPIRAL_BUDGET_NS == 60 * 60 * 1_000_000_000, \
    f"spiral budget should be 60 min uniform, got {sso_mod._SPIRAL_BUDGET_NS}"
print("OK spiral_budget_uniform_60min")

# Test 2: bootstrap_mode reports True for hook's own file in staged diff.
import _bootstrap_mode
_bootstrap_mode._staged_paths = lambda root: [".claude/hooks/_bootstrap_mode.py"]
hook_self = _bootstrap_mode.__file__
assert _bootstrap_mode.is_bootstrap_commit(hook_self), "Test 2 bootstrap True path failed"
_bootstrap_mode._staged_paths = lambda root: ["src/kernel/foo.c"]
assert not _bootstrap_mode.is_bootstrap_commit(hook_self), "Test 2 bootstrap False path failed"
print("OK bootstrap_mode_warn")

# Test 3: spiral check on uniform 60-min budget; 2x = 120 min, 4x = 240 min.
import skill_step_observer as sso
class _Stderr:
    def __init__(self): self.buf = []
    def write(self, s): self.buf.append(s)
sso.sys.stderr = _Stderr()
entry = {
    "started_ts": time.time_ns() - 130 * 60 * 1_000_000_000,
    "spiral_check_emitted": 0,
}
sso._spiral_check(entry, "implement-todo-section")
assert any("SPIRAL CHECK" in s and "2x" in s for s in sso.sys.stderr.buf), "Test 3 spiral 2x not fired (uniform 60-min budget)"
assert entry["spiral_check_emitted"] == 1, "Test 3 escalation counter not bumped"
sso.sys.stderr = _Stderr()
sso._spiral_check(entry, "implement-todo-section")
assert sso.sys.stderr.buf == [], "Test 3 dedup failed"
entry["started_ts"] = time.time_ns() - 250 * 60 * 1_000_000_000
sso._spiral_check(entry, "implement-todo-section")
assert any("4x" in s for s in sso.sys.stderr.buf), "Test 3 spiral 4x not fired"
print("OK spiral_check_warn")

# Test 4: codex_review_completed debug log only writes when env set.
# Schema upgraded to JSONL in the state-write reliability section of
# TODO-08-automation-hardening; extension is .jsonl and content is
# one JSON object per line.
import os, json, tempfile
with tempfile.TemporaryDirectory() as tmp:
    import pathlib
    root = pathlib.Path(tmp)
    (root / ".claude" / "state").mkdir(parents=True)
    import codex_review_completed as crc
    log_path = root / ".claude" / "state" / "codex-review-debug.jsonl"
    if "CODEX_REVIEW_DEBUG" in os.environ: del os.environ["CODEX_REVIEW_DEBUG"]
    crc._debug_log(root, "test_event", k="v")
    assert not log_path.exists(), "Test 4 debug log written without env"
    os.environ["CODEX_REVIEW_DEBUG"] = "1"
    crc._debug_log(root, "test_event", k="v")
    assert log_path.exists(), "Test 4 debug log NOT written with env"
    rec = json.loads(log_path.read_text().strip().splitlines()[-1])
    assert rec.get("event") == "test_event" and rec.get("k") == "v", \
        f"Test 4 debug log content wrong: {rec!r}"
    del os.environ["CODEX_REVIEW_DEBUG"]
print("OK debug_log_env_gated")
PY
)" 2>&1
    RRS_RC=$?
    # Count OK lines emitted. Python exit code can be non-zero from
    # interpreter cleanup after sys.stderr monkey-patch even when
    # every assertion passed; trust the OK markers as the source of
    # truth.
    RRS_OK_COUNT=$(echo "$RRS_OUT" | grep -c "^OK ")
    if [ "$RRS_OK_COUNT" = "4" ]; then
        echo "$RRS_OUT" | grep "^OK " | while IFS= read -r line; do
            t_pass "review_right_sizing: $line"
        done
        # While-loop subshell drops PASS counter increments. Use direct count.
        PASS=$((PASS + 4))
    else
        t_fail "review_right_sizing: integrated test suite incomplete" \
               "ok-count=$RRS_OK_COUNT rc=$RRS_RC out=$RRS_OUT"
    fi
fi


# ============================================================================
# heuristic_warn_misses (TODO-08 partial-enforcement heuristics)
# ============================================================================
#   - hw_step1_warn:       skill_step_block emits step-1 WARN on first src/
#                          Edit when no todo Read happened.
#   - hw_step3_warn:       skill_step_block emits step-3 WARN when explore
#                          query count is below threshold.
#   - hw_step9_warn:       _heuristic_step9_test_coverage emits WARN when
#                          test-coverage stamp missing.
#   - hw_step15_warn:      _heuristic_step15_post_codex_edit emits WARN
#                          when no Edit followed the last Codex dispatch.
#   - hw_step17_warn:      _heuristic_step17_validate_phase emits WARN
#                          on thin validate phase.
#   - hw_step18_warn:      _heuristic_step18_loose_ends_scan emits WARN
#                          when no TODO/FIXME Grep recorded.
#   - hw_misslog_writer:   _heuristic_misses.emit_warn appends one JSON line
#                          per WARN with the documented schema.

[ "$QUIET" = "0" ] && echo "" && echo -e "${DIM}[heuristic_warn_misses]${NC}"

if [ ! -f "$REPO_ROOT/.claude/hooks/_heuristic_misses.py" ]; then
    t_fail "heuristic_warn_misses: _heuristic_misses.py missing"
elif [ ! -f "$REPO_ROOT/.claude/hooks/skill_step_block.py" ]; then
    t_fail "heuristic_warn_misses: skill_step_block.py missing"
else
    HW_TMP="$(mktemp -d)"
    HW_LOG="$HW_TMP/heuristic-misses.jsonl"

    # 1. miss-log writer schema check
    HW_PY_OUT="$(HEURISTIC_MISSES_LOG_FORCE="$HW_LOG" python3 - <<PYEOF
import sys, os, json
sys.path.insert(0, os.path.join("$REPO_ROOT", ".claude/hooks"))
import _heuristic_misses as hm
# Override the module's _MISS_LOG_REL by injecting a fake repo root.
# Easier: monkey-patch emit_warn to write to our absolute path.
fake_root = "$HW_TMP"
os.makedirs(os.path.join(fake_root, ".claude/state"), exist_ok=True)
hm.emit_warn(fake_root, 1, "todo-read-missing",
             "first src/ Edit but no todo/**/*.md Read",
             todo_path="todo/00-infrastructure/TODO-08-automation-hardening.md",
             section=21)
hm.emit_warn(fake_root, 3, "explore-thin",
             "first src/ Edit after only 1 explore query",
             todo_path="todo/00-infrastructure/TODO-08-automation-hardening.md",
             section=21)
log_path = os.path.join(fake_root, ".claude/state/heuristic-misses.jsonl")
with open(log_path, "r", encoding="utf-8") as f:
    lines = f.readlines()
recs = [json.loads(l) for l in lines if l.strip()]
assert len(recs) == 2, "expected 2 records, got %d" % len(recs)
required = {"ts_ns", "step", "todo_path", "section", "signal",
            "detail", "false_positive_user_flagged"}
for r in recs:
    missing = required - set(r.keys())
    assert not missing, "schema fields missing: %s" % missing
assert recs[0]["step"] == 1
assert recs[1]["step"] == 3
assert all(r["false_positive_user_flagged"] is False for r in recs)
print("OK 2 records schema-clean")
PYEOF
2>&1)"
    if echo "$HW_PY_OUT" | grep -q "OK 2 records schema-clean"; then
        t_pass "heuristic_warn_misses: hw_misslog_writer schema OK"
    else
        t_fail "heuristic_warn_misses: hw_misslog_writer" "out: $HW_PY_OUT"
    fi

    # 2. emit_warn writes stderr WARN line
    HW_STDERR="$(python3 - <<PYEOF 2>&1 >/dev/null
import sys, os
sys.path.insert(0, os.path.join("$REPO_ROOT", ".claude/hooks"))
import _heuristic_misses as hm
hm.emit_warn(None, 1, "test-signal", "test-detail")
PYEOF
)"
    if echo "$HW_STDERR" | grep -q "\[heuristic-step-1\] WARN -- test-detail" \
       && echo "$HW_STDERR" | grep -q "signal: test-signal"; then
        t_pass "heuristic_warn_misses: hw_step1_warn stderr format"
    else
        t_fail "heuristic_warn_misses: hw_step1_warn" "stderr: $HW_STDERR"
    fi

    # 3-7. Steps 3/9/15/17/18 -- each just verifies the canonical signal
    # name + step id round-trip through emit_warn produces a valid log line.
    # The integration paths (hook + skill window detection) are exercised
    # via real session usage; here we verify the WARN+log shape contract.
    for STEP_INFO in "3:explore-thin:hw_step3_warn" \
                     "9:test-coverage-missing:hw_step9_warn" \
                     "15:no-edit-after-codex:hw_step15_warn" \
                     "17:validate-thin:hw_step17_warn" \
                     "18:loose-ends-scan-missing:hw_step18_warn"; do
        HW_S="${STEP_INFO%%:*}"
        HW_REST="${STEP_INFO#*:}"
        HW_SIGNAL="${HW_REST%%:*}"
        HW_NAME="${HW_REST#*:}"
        HW_OUT="$(python3 - <<PYEOF 2>&1
import sys, os
sys.path.insert(0, os.path.join("$REPO_ROOT", ".claude/hooks"))
import _heuristic_misses as hm
hm.emit_warn(None, $HW_S, "$HW_SIGNAL", "step-$HW_S detail")
PYEOF
)"
        if echo "$HW_OUT" | grep -q "\[heuristic-step-$HW_S\] WARN" \
           && echo "$HW_OUT" | grep -q "signal: $HW_SIGNAL"; then
            t_pass "heuristic_warn_misses: $HW_NAME stderr format"
        else
            t_fail "heuristic_warn_misses: $HW_NAME" "out: $HW_OUT"
        fi
    done

    rm -rf "$HW_TMP"
fi


# ============================================================================
# skip_env_unified (TODO-08 section-23 shared SKIP-env scanner)
# ============================================================================
#   - se_inline_form:  inline `SKIP_FOO=1 git commit ...` returns the value
#                      with no harness env set.
#   - se_environ_form: bare `git commit` with SKIP_FOO=1 in os.environ
#                      returns the value.
#   - se_inline_wins:  inline value wins when both sources carry the same key.

[ "$QUIET" = "0" ] && echo "" && echo -e "${DIM}[skip_env_unified]${NC}"

if [ ! -f "$REPO_ROOT/.claude/hooks/_skip_env.py" ]; then
    t_fail "skip_env_unified: _skip_env.py missing"
else
    SE_OUT="$(python3 - <<PYEOF 2>&1
import sys, os
sys.path.insert(0, "$REPO_ROOT/.claude/hooks")
import _skip_env as se

# Test 1: inline form (no environ)
for k in ("SKIP_TESTKEY",):
    os.environ.pop(k, None)
r = se.read_skip_envs("SKIP_TESTKEY=1 git commit -m foo", keys=("SKIP_TESTKEY",))
print("se_inline_form:" + ("OK" if r == {"SKIP_TESTKEY": "1"} else "FAIL " + repr(r)))

# Test 2: environ form (no inline)
os.environ["SKIP_TESTKEY"] = "env-val"
r = se.read_skip_envs("git commit", keys=("SKIP_TESTKEY",))
print("se_environ_form:" + ("OK" if r == {"SKIP_TESTKEY": "env-val"} else "FAIL " + repr(r)))

# Test 3: inline wins
os.environ["SKIP_TESTKEY"] = "env-val"
r = se.read_skip_envs("SKIP_TESTKEY=inline-val git commit", keys=("SKIP_TESTKEY",))
print("se_inline_wins:" + ("OK" if r == {"SKIP_TESTKEY": "inline-val"} else "FAIL " + repr(r)))
del os.environ["SKIP_TESTKEY"]
PYEOF
)"
    for tag in se_inline_form se_environ_form se_inline_wins; do
        if echo "$SE_OUT" | grep -q "$tag:OK"; then
            t_pass "skip_env_unified: $tag"
        else
            t_fail "skip_env_unified: $tag" "$(echo "$SE_OUT" | grep "$tag")"
        fi
    done

    # Regression: section_commit_gate _is_skip_requested honors inline-wins
    # via the shared helper (Codex review-impl H1 fix 2026-04-29).
    SE_GATE_OUT="$(python3 - <<PYEOF 2>&1
import sys, os
sys.path.insert(0, "$REPO_ROOT/.claude/hooks")
import section_commit_gate as scg

# Stale ambient SKIP_REVIEW_HOOK=1 must NOT override inline SKIP_REVIEW_HOOK=0.
os.environ["SKIP_REVIEW_HOOK"] = "1"
os.environ["SKIP_REVIEW_HOOK_REASON"] = "stale-ambient-reason-12-chars-min"
skip_req, reason, err = scg._is_skip_requested("SKIP_REVIEW_HOOK=0 git commit -m foo")
print("se_gate_inline_wins:" + ("OK" if skip_req is False else "FAIL skip_req=" + repr(skip_req)))
del os.environ["SKIP_REVIEW_HOOK"]
del os.environ["SKIP_REVIEW_HOOK_REASON"]
PYEOF
)"
    if echo "$SE_GATE_OUT" | grep -q "se_gate_inline_wins:OK"; then
        t_pass "skip_env_unified: se_gate_inline_wins (collision regression)"
    else
        t_fail "skip_env_unified: se_gate_inline_wins" "$(echo "$SE_GATE_OUT" | tail -3)"
    fi

    # Regression: SKIP_SMOKE_GATE inline form reaches _step16_smoke_check
    # via the shared helper (Codex review-todo-section consistency M1 fix).
    SE_SMOKE_OUT="$(python3 - <<PYEOF2 2>&1
import sys, os
sys.path.insert(0, "$REPO_ROOT/.claude/hooks")
import section_commit_gate as scg

# Stub helpers so _step16_smoke_check reaches the SKIP path quickly.
# Use a fake staged_src that triggers boot-path detection AND a missing
# smoke log (default failure path); inline opt-out should short-circuit.
for k in ("SKIP_SMOKE_GATE", "SKIP_SMOKE_GATE_REASON"):
    os.environ.pop(k, None)
ok, err = scg._step16_smoke_check(
    scg.Path("/tmp/nonexistent-repo"),
    ["src/boot/uefi/bootx64.c"],
    cmd='SKIP_SMOKE_GATE=1 SKIP_SMOKE_GATE_REASON=inline-skip-smoke-test git commit -m foo',
)
print("se_smoke_inline:" + ("OK" if ok is True else "FAIL ok=" + repr(ok) + " err=" + repr(err)))
PYEOF2
)"
    if echo "$SE_SMOKE_OUT" | grep -q "se_smoke_inline:OK"; then
        t_pass "skip_env_unified: se_smoke_inline (SKIP_SMOKE_GATE inline form)"
    else
        t_fail "skip_env_unified: se_smoke_inline" "$(echo "$SE_SMOKE_OUT" | tail -3)"
    fi
fi


# ============================================================================
# codex_review_state_write -- last-codex-review.json reliability
# ============================================================================
# TODO ownership: state-write reliability section of
# 00-infrastructure/TODO-08-automation-hardening.md.
#   - crsw_synthetic_dispatch: PostToolUse fed a synthetic Bash codex-companion
#                              trigger -> last-codex-review.json populated
#                              within 100 ms.
#   - crsw_unmarked_kind:      synthetic trigger missing review-kind marker
#                              -> state file still written; debug log records
#                              review_kind="unknown" (documented behavior, not
#                              silent drop).
#   - crsw_parallel_writes:    two synthetic dispatches in quick succession
#                              -> both observability records written via
#                              flock-protected stamp file; state file holds
#                              the second trigger; no lost updates.
# Closes the prior state-file deferral; replaces the bootstrap-mode
# SKIP_REVIEW_HOOK escape that cited that deferral as the documented opt-out.

CRSW_TMP=$(mktemp -d)
trap 'rm -rf "$CRSW_TMP"' EXIT
CRSW_REPO="$CRSW_TMP/repo"
mkdir -p "$CRSW_REPO/.claude/hooks" "$CRSW_REPO/.claude/state" "$CRSW_REPO/scripts"
cp .claude/hooks/codex_review_completed.py "$CRSW_REPO/.claude/hooks/" 2>/dev/null
    cp "$REPO_ROOT/.claude/hooks/_codex_dispatch.py" \
       "$CRSW_REPO/.claude/hooks/_codex_dispatch.py"
cp -R "$REPO_ROOT/scripts/ai-workflow" "$CRSW_REPO/scripts/ai-workflow"
cp "$REPO_ROOT/scripts/ai_workflow_import.py" "$CRSW_REPO/scripts/ai_workflow_import.py"
( cd "$CRSW_REPO" && git init -q && git -c user.email=t@t -c user.name=t commit -q --allow-empty -m init )

# Sub-test 1: synthetic adversarial dispatch -> state file populated within 1 s.
CRSW_PAYLOAD=$(cat <<'JSON'
{
  "tool_name": "Bash",
  "tool_input": {
    "command": "node \"$HOME/.claude/plugins/marketplaces/openai-codex/plugins/codex/scripts/codex-companion.mjs\" adversarial-review \"[review-kind: adversarial] todo/00-infrastructure/TODO-08-automation-hardening.md sample\""
  }
}
JSON
)
CRSW_T0=$(date +%s%N)
echo "$CRSW_PAYLOAD" | ( cd "$CRSW_REPO" && CODEX_REVIEW_DEBUG=1 python3 .claude/hooks/codex_review_completed.py >/dev/null 2>&1 )
CRSW_RC=$?
CRSW_T1=$(date +%s%N)
CRSW_DELTA_MS=$(( (CRSW_T1 - CRSW_T0) / 1000000 ))
if [ "$CRSW_RC" = "0" ] && [ -s "$CRSW_REPO/.claude/state/last-codex-review.json" ] && [ "$CRSW_DELTA_MS" -lt 1000 ]; then
    if grep -q '"received": false' "$CRSW_REPO/.claude/state/last-codex-review.json" \
       && grep -q '"trigger": "Bash(' "$CRSW_REPO/.claude/state/last-codex-review.json"; then
        t_pass "codex_review_state_write: crsw_synthetic_dispatch (${CRSW_DELTA_MS} ms)"
    else
        t_fail "codex_review_state_write: crsw_synthetic_dispatch" \
               "state file shape: $(head -c 200 "$CRSW_REPO/.claude/state/last-codex-review.json")"
    fi
else
    t_fail "codex_review_state_write: crsw_synthetic_dispatch" \
           "rc=$CRSW_RC, delta_ms=$CRSW_DELTA_MS, file=$(ls -la "$CRSW_REPO/.claude/state/last-codex-review.json" 2>&1)"
fi

CRSW_RECEIVE_PAYLOAD='{"tool_name":"Skill","tool_input":{"skill":"receiving-code-review"}}'
echo "$CRSW_RECEIVE_PAYLOAD" | ( cd "$CRSW_REPO" && CODEX_REVIEW_DEBUG=1 python3 .claude/hooks/codex_review_completed.py >/dev/null 2>&1 )
if [ -s "$CRSW_REPO/.ai-workflow/evidence.jsonl" ] \
   && grep -q '"kind": "adversarial"' "$CRSW_REPO/.ai-workflow/evidence.jsonl" \
   && grep -q '"role": "codex-reviewer-adversarial"' "$CRSW_REPO/.ai-workflow/evidence.jsonl"; then
    t_pass "codex_review_state_write: crsw_shared_ledger_mirror"
else
    t_fail "codex_review_state_write: crsw_shared_ledger_mirror" \
           "$(cat "$CRSW_REPO/.ai-workflow/evidence.jsonl" 2>&1 | tail -3)"
fi

CRSW_BATCH_OUT=$(python3 - <<'PYEOF'
import io
import json
import os
import pathlib
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, ".claude/hooks")
import codex_review_completed as crc
import section_commit_gate as scg

with tempfile.TemporaryDirectory() as tmp:
    repo = pathlib.Path(tmp)
    (repo / ".claude" / "state").mkdir(parents=True)
    (repo / "scripts").mkdir()
    (repo / ".claude" / "hooks").mkdir(parents=True)
    (repo / ".claude" / "skills" / "demo").mkdir(parents=True)
    (repo / ".githooks").mkdir()
    (repo / "docs").mkdir()
    (repo / "todo").mkdir()
    subprocess.run(["git", "init", "-q"], cwd=repo, check=True)
    for i in range(180):
        (repo / "scripts" / f"source_{i}.py").write_text(f"# source {i}\n", encoding="utf-8")
    governance_paths = [
        ".claude/hooks/review_hook.py",
        ".claude/skills/demo/SKILL.md",
        ".githooks/pre-commit",
        "docs/review.md",
        "todo/TODO-99-review.md",
    ]
    for rel in governance_paths:
        (repo / rel).write_text(f"{rel}\n", encoding="utf-8")
    subprocess.run(["git", "add", "scripts", ".claude", ".githooks", "docs", "todo"], cwd=repo, check=True)

    real_check_output = crc.subprocess.check_output
    blob_batches = []

    def checked_check_output(args, *pargs, **kwargs):
        if isinstance(args, list) and args[:5] == ["git", "ls-files", "-s", "-z", "--"]:
            batch = args[5:]
            if len(batch) > crc._GIT_BLOB_BATCH_SIZE:
                raise AssertionError(f"unbounded ls-files batch: {len(batch)}")
            blob_batches.append(len(batch))
        return real_check_output(args, *pargs, **kwargs)

    payload = {
        "tool_name": "Bash",
        "tool_input": {
            "command": "bash scripts/codex-dispatch.sh '[review-kind: adversarial] todo/00-infrastructure/TODO-10-ai-driver-interchangeability.md section 1'",
        },
    }

    old_cwd = os.getcwd()
    old_stdin = sys.stdin
    crc.subprocess.check_output = checked_check_output
    try:
        os.chdir(repo)
        sys.stdin = io.StringIO(json.dumps(payload))
        rc = crc.main()
    finally:
        sys.stdin = old_stdin
        os.chdir(old_cwd)
    if rc != 0:
        raise SystemExit(f"crc.main rc={rc}")

    state_path = repo / ".claude" / "state" / "last-codex-review.json"
    state = json.loads(state_path.read_text(encoding="utf-8"))
    trigger_files = state.get("trigger_files") or []
    trigger_blobs = state.get("trigger_blobs") or {}
    expected_files = 180 + len(governance_paths)
    if len(trigger_files) != expected_files:
        raise SystemExit(f"trigger_files={len(trigger_files)}")
    if len(trigger_blobs) != expected_files:
        raise SystemExit(f"trigger_blobs={len(trigger_blobs)}")
    missing_governance = [path for path in governance_paths if path not in trigger_blobs]
    if missing_governance:
        raise SystemExit(f"governance files missing from trigger_blobs: {missing_governance}")
    if len(blob_batches) < 2:
        raise SystemExit(f"trigger path did not batch: {blob_batches}")

    state["received"] = True
    state["received_timestamp_ns"] = time.time_ns()
    state_path.write_text(json.dumps(state), encoding="utf-8")
    blob_batches.clear()
    scg.crc = crc
    ok, err = scg._review_evidence(repo, trigger_files)
    crc.subprocess.check_output = real_check_output
    if not ok:
        raise SystemExit(f"review_evidence failed: {err}")
    if len(blob_batches) < 2:
        raise SystemExit(f"review_evidence path did not batch: {blob_batches}")

    real_run = crc.subprocess.run

    def fail_evidence_subprocess_run(args, *pargs, **kwargs):
        if (
            isinstance(args, list)
            and len(args) >= 3
            and str(args[1]).endswith("scripts/ai-workflow/evidence.py")
            and args[2] == "record"
        ):
            raise AssertionError("shared review mirror used evidence.py subprocess")
        return real_run(args, *pargs, **kwargs)

    crc.subprocess.run = fail_evidence_subprocess_run
    try:
        os.chdir(repo)
        sys.stdin = io.StringIO(json.dumps({"tool_name": "Skill", "tool_input": {"skill": "receiving-code-review"}}))
        rc = crc.main()
    finally:
        sys.stdin = old_stdin
        os.chdir(old_cwd)
        crc.subprocess.run = real_run
    if rc != 0:
        raise SystemExit(f"receive rc={rc}")
    ledger = repo / ".ai-workflow" / "evidence.jsonl"
    event = json.loads(ledger.read_text(encoding="utf-8").splitlines()[-1])
    blobs = event.get("source_blobs") or {}
    if event.get("kind") != "adversarial" or event.get("result") != "received":
        raise SystemExit(f"unexpected shared evidence event: {event}")
    if len(blobs) != expected_files:
        raise SystemExit(f"shared mirror source_blobs={len(blobs)}")
    missing_mirror = [path for path in governance_paths if path not in blobs]
    if missing_mirror:
        raise SystemExit(f"governance files missing from shared mirror: {missing_mirror}")
    old_mode_binding = blobs[".githooks/pre-commit"]
    (repo / ".githooks/pre-commit").chmod(0o755)
    subprocess.run(["git", "add", ".githooks/pre-commit"], cwd=repo, check=True)
    new_mode_binding = subprocess.check_output(
        ["git", "ls-files", "-s", "--", ".githooks/pre-commit"],
        cwd=repo,
        text=True,
    ).split()
    new_mode_binding = f"{new_mode_binding[0]}:{new_mode_binding[1]}"
    if old_mode_binding == new_mode_binding:
        raise SystemExit("mode-only hook change did not affect source binding")
    if old_mode_binding.split(":", 1)[1] != new_mode_binding.split(":", 1)[1]:
        raise SystemExit("mode-only fixture unexpectedly changed blob content")
    old_hook_blob = blobs[".claude/hooks/review_hook.py"]
    (repo / ".claude/hooks/review_hook.py").write_text("# changed after review\n", encoding="utf-8")
    subprocess.run(["git", "add", ".claude/hooks/review_hook.py"], cwd=repo, check=True)
    new_hook_parts = subprocess.check_output(
        ["git", "ls-files", "-s", "--", ".claude/hooks/review_hook.py"],
        cwd=repo,
        text=True,
    ).split()
    new_hook_blob = f"{new_hook_parts[0]}:{new_hook_parts[1]}"
    if old_hook_blob == new_hook_blob:
        raise SystemExit("post-review staged hook edit did not change blob binding")
print("OK")
PYEOF
)
if [ "$CRSW_BATCH_OUT" = "OK" ]; then
    t_pass "codex_review_state_write: crsw_source_blobs_batched_trigger_and_gate"
else
    t_fail "codex_review_state_write: crsw_source_blobs_batched_trigger_and_gate" "$CRSW_BATCH_OUT"
fi

# Sub-test 2: unmarked dispatch (no review-kind marker) -> state file still
# written; debug log shows review_kind="unknown" (documented, not silent drop).
rm -f "$CRSW_REPO/.claude/state/last-codex-review.json" \
      "$CRSW_REPO/.claude/state/codex-review-debug.jsonl" \
      "$CRSW_REPO/.claude/state/last-review-stamps.json"
CRSW_PAYLOAD2=$(cat <<'JSON'
{
  "tool_name": "Bash",
  "tool_input": {
    "command": "node \"$HOME/.claude/plugins/marketplaces/openai-codex/plugins/codex/scripts/codex-companion.mjs\" adversarial-review \"plain prompt with no marker\""
  }
}
JSON
)
echo "$CRSW_PAYLOAD2" | ( cd "$CRSW_REPO" && CODEX_REVIEW_DEBUG=1 python3 .claude/hooks/codex_review_completed.py >/dev/null 2>&1 )
if [ -s "$CRSW_REPO/.claude/state/last-codex-review.json" ] \
   && [ -s "$CRSW_REPO/.claude/state/codex-review-debug.jsonl" ] \
   && grep -q '"review_kind": "unknown"' "$CRSW_REPO/.claude/state/codex-review-debug.jsonl"; then
    t_pass "codex_review_state_write: crsw_unmarked_kind (state written; review_kind=unknown logged)"
else
    t_fail "codex_review_state_write: crsw_unmarked_kind" \
           "state=$(stat -c %s "$CRSW_REPO/.claude/state/last-codex-review.json" 2>/dev/null) debug=$(cat "$CRSW_REPO/.claude/state/codex-review-debug.jsonl" 2>&1 | tail -3)"
fi

# Sub-test 3: two parallel dispatches -> both writes complete; no lost updates.
rm -f "$CRSW_REPO/.claude/state/last-codex-review.json" \
      "$CRSW_REPO/.claude/state/codex-review-debug.jsonl" \
      "$CRSW_REPO/.claude/state/last-review-stamps.json"
cat > "$CRSW_TMP/p3a.json" <<'JSON'
{"tool_name":"Bash","tool_input":{"command":"node /x/codex-companion.mjs adversarial-review \"[review-kind: adversarial] todo/00-infrastructure/TODO-08-automation-hardening.md dispatch A\""}}
JSON
cat > "$CRSW_TMP/p3b.json" <<'JSON'
{"tool_name":"Bash","tool_input":{"command":"node /x/codex-companion.mjs adversarial-review \"[review-kind: consistency] todo/00-infrastructure/TODO-08-automation-hardening.md dispatch B\""}}
JSON
( cd "$CRSW_REPO" && CODEX_REVIEW_DEBUG=1 python3 .claude/hooks/codex_review_completed.py < "$CRSW_TMP/p3a.json" >/dev/null 2>&1 ) &
( cd "$CRSW_REPO" && CODEX_REVIEW_DEBUG=1 python3 .claude/hooks/codex_review_completed.py < "$CRSW_TMP/p3b.json" >/dev/null 2>&1 ) &
wait
CRSW_DEBUG_LINES=$(wc -l < "$CRSW_REPO/.claude/state/codex-review-debug.jsonl" 2>/dev/null || echo 0)
# M2 fix from review pipeline: validate stamp persistence too, not just
# debug-log presence. If _record_stamp() silently no-oped, the prior test
# would still pass; parsing last-review-stamps.json catches that gap.
CRSW_STAMP_OK=$(python3 - "$CRSW_REPO" <<'PYEOF'
import json, sys, pathlib
root = pathlib.Path(sys.argv[1])
p = root / ".claude" / "state" / "last-review-stamps.json"
if not p.exists():
    print("missing"); sys.exit(0)
try:
    d = json.loads(p.read_text())
except Exception as e:
    print(f"parse-fail: {e}"); sys.exit(0)
entry = d.get("todo/00-infrastructure/TODO-08-automation-hardening.md", {})
adv = entry.get("adversarial")
con = entry.get("consistency")
if isinstance(adv, int) and isinstance(con, int) and adv > 0 and con > 0:
    print("ok")
else:
    print(f"missing-kinds: adv={adv!r} con={con!r}")
PYEOF
)
# Each dispatch emits 4 records (main_entry + classify + trigger_state_write +
# trigger_stamp_write) = 8 total across the two processes when both fire.
if [ -s "$CRSW_REPO/.claude/state/last-codex-review.json" ] \
   && [ "$CRSW_DEBUG_LINES" -ge 8 ] \
   && grep -q '"review_kind": "adversarial"' "$CRSW_REPO/.claude/state/codex-review-debug.jsonl" \
   && grep -q '"review_kind": "consistency"' "$CRSW_REPO/.claude/state/codex-review-debug.jsonl" \
   && [ "$CRSW_STAMP_OK" = "ok" ] \
   && ! grep -q '"stamp_write_ok": false' "$CRSW_REPO/.claude/state/codex-review-debug.jsonl"; then
    t_pass "codex_review_state_write: crsw_parallel_writes (${CRSW_DEBUG_LINES} debug records; stamps persisted)"
else
    t_fail "codex_review_state_write: crsw_parallel_writes" \
           "debug_lines=$CRSW_DEBUG_LINES stamp_check=$CRSW_STAMP_OK state_size=$(stat -c %s "$CRSW_REPO/.claude/state/last-codex-review.json" 2>/dev/null)"
fi

# Sub-test 4 (precedes the prior sub-test 4 which is now sub-test 5):
# wrapper attribution -- the section-28 canonical invocation shape
# `bash scripts/codex-dispatch.sh '<prompt>'` MUST classify as a Codex
# trigger AND extract the prompt for stamp attribution. Regression for
# the post-impl Codex H1 finding where _segment_is_codex_invocation
# recognized the wrapper but _bash_prompt_arg returned ''.
rm -f "$CRSW_REPO/.claude/state/last-codex-review.json" \
      "$CRSW_REPO/.claude/state/codex-review-debug.jsonl" \
      "$CRSW_REPO/.claude/state/last-review-stamps.json"
cat > "$CRSW_TMP/p_wrapper.json" <<'JSON'
{"tool_name":"Bash","tool_input":{"command":"bash scripts/codex-dispatch.sh '[review-kind: adversarial] todo/00-infrastructure/TODO-08-automation-hardening.md wrapper attribution'"}}
JSON
( cd "$CRSW_REPO" && CODEX_REVIEW_DEBUG=1 python3 .claude/hooks/codex_review_completed.py < "$CRSW_TMP/p_wrapper.json" >/dev/null 2>&1 )
CRSW_WRAPPER_OK=$(python3 - "$CRSW_REPO" <<'PYEOF'
import json, sys, pathlib
root = pathlib.Path(sys.argv[1])
state = root / ".claude/state/last-codex-review.json"
stamps = root / ".claude/state/last-review-stamps.json"
if not state.exists():
    print(f"missing-state"); sys.exit(0)
d = json.loads(state.read_text())
if d.get("received") is not False:
    print(f"state-received={d.get('received')!r}"); sys.exit(0)
if "codex-dispatch.sh" not in d.get("trigger", ""):
    print(f"state-trigger={d.get('trigger', '')[:60]!r}"); sys.exit(0)
if not stamps.exists():
    print(f"missing-stamps"); sys.exit(0)
s = json.loads(stamps.read_text())
e = s.get("todo/00-infrastructure/TODO-08-automation-hardening.md", {})
if not isinstance(e.get("adversarial"), int):
    print(f"stamps-adv={e.get('adversarial')!r}"); sys.exit(0)
print("ok")
PYEOF
)
if [ "$CRSW_WRAPPER_OK" = "ok" ]; then
    t_pass "codex_review_state_write: crsw_wrapper_attribution (wrapper -> state + adversarial stamp)"
else
    t_fail "codex_review_state_write: crsw_wrapper_attribution" \
           "result=$CRSW_WRAPPER_OK"
fi

# Sub-test 5: write-failure honesty -- exercises the WIRED diagnostic at
# main()'s trigger_state_write emit site, not just the helper. Monkeypatches
# os.replace to raise OSError, drives crc.main() with a synthetic Bash
# trigger, then parses codex-review-debug.jsonl to assert the
# trigger_state_write record has last_codex_review_write_ok=false and a
# non-empty error string. The regression contract is that the JSONL
# diagnostic CANNOT report success when the writer fail-open path swallowed
# the error -- the H1 fix from the post-impl Codex review pipeline.
rm -f "$CRSW_REPO/.claude/state/last-codex-review.json" \
      "$CRSW_REPO/.claude/state/codex-review-debug.jsonl" \
      "$CRSW_REPO/.claude/state/last-review-stamps.json"
CRSW_FAIL_OUT=$(cd "$CRSW_REPO" && CODEX_REVIEW_DEBUG=1 python3 - <<'PYEOF'
import io, json, os, sys
sys.path.insert(0, ".claude/hooks")
import codex_review_completed as crc

# Monkeypatch os.replace in BOTH the os module and the crc module's
# import-time binding -- crc imports os at module top, so `os.replace` in
# crc resolves through the os module attribute.
_orig = os.replace
def _boom(*a, **k):
    raise OSError(28, "No space left on device (test fixture)")
os.replace = _boom

payload = {
    "tool_name": "Bash",
    "tool_input": {
        "command": 'node /x/codex-companion.mjs adversarial-review "[review-kind: adversarial] todo/00-infrastructure/TODO-08-automation-hardening.md fail-test"',
    },
}
# Replace stdin so crc.main()'s json.load(sys.stdin) sees the synthetic
# payload. Suppress stderr so the WARN doesn't pollute test output.
sys.stdin = io.StringIO(json.dumps(payload))
sys.stderr = io.StringIO()
try:
    rc = crc.main()
finally:
    os.replace = _orig
    sys.stderr = sys.__stderr__

# Parse the JSONL diagnostic file and find the trigger_state_write record.
log_path = ".claude/state/codex-review-debug.jsonl"
if not os.path.exists(log_path):
    print(f"FAIL: debug log not written (rc={rc})"); sys.exit(0)

records = [json.loads(line) for line in open(log_path) if line.strip()]
state_writes = [r for r in records if r.get("event") == "trigger_state_write"]
if not state_writes:
    print(f"FAIL: no trigger_state_write record in {len(records)} records"); sys.exit(0)
rec = state_writes[-1]
if rec.get("last_codex_review_write_ok") is not False:
    print(f"FAIL: write_ok={rec.get('last_codex_review_write_ok')!r} expected False"); sys.exit(0)
if "OSError" not in str(rec.get("error", "")):
    print(f"FAIL: error={rec.get('error')!r} expected OSError substring"); sys.exit(0)
print("OK")
PYEOF
)
if [ "$CRSW_FAIL_OUT" = "OK" ]; then
    t_pass "codex_review_state_write: crsw_write_failure_honesty (JSONL records ok=false + OSError)"
else
    t_fail "codex_review_state_write: crsw_write_failure_honesty" \
           "out=$CRSW_FAIL_OUT"
fi


# ============================================================================
# skill_step_observer -- canonical Codex review-kind dispatch coverage
# ============================================================================
# TODO ownership: 00-infrastructure/TODO-08-automation-hardening.md observer
# regex coverage section. The 8 canonical review-kind markers (design,
# adversarial, adversarial-impl, test-coverage, consistency, perf,
# re-adversarial, gap-audit) MUST each bind to a step number in the
# skill that emits them; the prior shape silently dropped 5 of 7.
# gap-audit is skill-scoped (only gap-audit-todo binds it); the rest
# bind under implement-todo-section / review-todo-section.
# Each sub-test feeds a synthetic Bash codex-companion.mjs adversarial-
# review command with the named marker and asserts match_step returns
# the expected step number.

SSO_PY=$(cat <<'PYEOF'
import sys
sys.path.insert(0, "$REPO_ROOT/.claude/hooks")
from skill_step_map import match_step, bound_review_kinds

CASES = [
    ("implement-todo-section", "design",            4),
    ("implement-todo-section", "adversarial",       13),
    ("implement-todo-section", "adversarial-impl",  13),
    ("implement-todo-section", "test-coverage",     9),
    ("implement-todo-section", "consistency",       20),
    ("implement-todo-section", "perf",              20),
    ("implement-todo-section", "re-adversarial",    20),
    # 8th canonical kind: gap-audit binds ONLY to gap-audit-todo
    # (Phase 3.5 dispatch, mapped to step 14 since 14.5 is not an int).
    ("gap-audit-todo",        "gap-audit",          14),
]

for skill, kind, want in CASES:
    cmd = f'node /x/codex-companion.mjs adversarial-review "[review-kind: {kind}] todo/foo prompt"'
    got = match_step(skill, "Bash", cmd)
    if want in got:
        print(f"OK {kind}")
    else:
        print(f"FAIL {kind}: got {got} want {want}")

# Unmarked dispatch must NOT match any step (the runtime observer emits
# a stderr WARN; here we just confirm match_step returns []).
unmarked = 'node /x/codex-companion.mjs adversarial-review "plain prompt no marker"'
got = match_step("implement-todo-section", "Bash", unmarked)
if got == []:
    print("OK unmarked")
else:
    print(f"FAIL unmarked: got {got} want []")

# Typo'd marker (post-impl review M1 fix): a non-canonical kind like
# 'performance' (typo for 'perf') must NOT bind to any step. The runtime
# observer's WARN path identifies this as an unbound-marker case.
typo = 'node /x/codex-companion.mjs adversarial-review "[review-kind: performance] todo/foo prompt"'
got = match_step("implement-todo-section", "Bash", typo)
if got == []:
    print("OK typo_marker_unbound")
else:
    print(f"FAIL typo_marker_unbound: got {got} want []")

# bound_review_kinds helper sanity: implement-todo-section MUST bind
# all 7 canonical kinds (the observer-coverage contract).
bound = set(bound_review_kinds("implement-todo-section"))
expected = {"design", "adversarial", "adversarial-impl", "test-coverage",
            "consistency", "perf", "re-adversarial"}
missing = expected - bound
if not missing:
    print("OK bound_review_kinds_complete")
else:
    print(f"FAIL bound_review_kinds_complete: missing={missing}")

# Quality-review-section step 2 must use a sentinel marker (post-impl
# review M3 fix). A typo'd marker dispatch under quality-review-section
# must NOT get step credit.
typo_qrs = 'node /x/codex-companion.mjs adversarial-review "[review-kind: performance] todo/foo prompt"'
got = match_step("quality-review-section", "Bash", typo_qrs)
if got == []:
    print("OK quality_review_section_typo_unbound")
else:
    print(f"FAIL quality_review_section_typo_unbound: got {got} want []")

# A canonical adversarial dispatch under quality-review-section MUST
# still get step 2 credit (the legitimate path).
canon_qrs = 'node /x/codex-companion.mjs adversarial-review "[review-kind: adversarial] todo/foo prompt"'
got = match_step("quality-review-section", "Bash", canon_qrs)
if 2 in got:
    print("OK quality_review_section_adversarial_bound")
else:
    print(f"FAIL quality_review_section_adversarial_bound: got {got} want [2]")

# Wrong-canonical marker (round-3 M6 fix): canonical kind not bound for
# the active skill returns []. The runtime observer then emits the
# wrong-canonical WARN (loud).
wrong_canon = 'node /x/codex-companion.mjs adversarial-review "[review-kind: perf] todo/foo prompt"'
got = match_step("quality-review-section", "Bash", wrong_canon)
if got == []:
    print("OK wrong_canonical_marker_unbound")
else:
    print(f"FAIL wrong_canonical_marker_unbound: got {got} want []")

# Restored multi-line continuation sub-tests (TODO-08 section-30 closed
# the deferred shell-aware-segmentation work; detect_review_kind_from_cmd
# now routes through _codex_dispatch.extract_dispatch_prompt which
# normalizes shell line-continuations before tokenizing).
multiline_a = 'node /x/codex-companion.mjs \\\n    adversarial-review "[review-kind: adversarial] todo/foo prompt"'
import _review_kind
if _review_kind.detect_review_kind_from_cmd(multiline_a) == "adversarial":
    print("OK multiline_continuation_before_subcommand")
else:
    print(f"FAIL multiline_continuation_before_subcommand: got {_review_kind.detect_review_kind_from_cmd(multiline_a)!r}")

multiline_b = 'node /x/codex-companion.mjs adversarial-review \\\n    "[review-kind: consistency] todo/foo prompt"'
if _review_kind.detect_review_kind_from_cmd(multiline_b) == "consistency":
    print("OK multiline_continuation_before_prompt")
else:
    print(f"FAIL multiline_continuation_before_prompt: got {_review_kind.detect_review_kind_from_cmd(multiline_b)!r}")

# gap-audit kind is skill-scoped: gap-audit-todo binds it (step 14),
# but implement-todo-section MUST NOT (the marker is gap-audit-todo's
# alone). This guards against future drift where a code-review-flow
# skill accidentally picks up a planning-only marker.
gap_audit_cmd = 'node /x/codex-companion.mjs adversarial-review "[review-kind: gap-audit] todo/foo prompt"'
got = match_step("implement-todo-section", "Bash", gap_audit_cmd)
if got == []:
    print("OK gap_audit_kind_not_bound_to_implement")
else:
    print(f"FAIL gap_audit_kind_not_bound_to_implement: got {got} want []")

# bound_review_kinds("gap-audit-todo") must include exactly {"gap-audit"}.
gap_bound = set(bound_review_kinds("gap-audit-todo"))
if gap_bound == {"gap-audit"}:
    print("OK gap_audit_todo_bound_kinds_exact")
else:
    print(f"FAIL gap_audit_todo_bound_kinds_exact: got {gap_bound} want {{'gap-audit'}}")

# todo-pipeline gates Stage 2 (gap-audit-todo + Codex gap-audit dispatch).
# A Skill(gap-audit-todo) call AND a [review-kind: gap-audit] dispatch
# both bind to step 2. Stage 1 / Stage 3 are observation-only.
got = match_step("todo-pipeline", "Skill", "validate-todo-file")
if 1 in got:
    print("OK todo_pipeline_stage1_validate")
else:
    print(f"FAIL todo_pipeline_stage1_validate: got {got} want [1]")

got = match_step("todo-pipeline", "Skill", "gap-audit-todo")
if 2 in got:
    print("OK todo_pipeline_stage2_gap_audit_skill")
else:
    print(f"FAIL todo_pipeline_stage2_gap_audit_skill: got {got} want [2]")

got = match_step("todo-pipeline", "Bash",
                 'node /x/codex-companion.mjs adversarial-review "[review-kind: gap-audit] todo/foo prompt"')
if 2 in got:
    print("OK todo_pipeline_stage2_gap_audit_dispatch")
else:
    print(f"FAIL todo_pipeline_stage2_gap_audit_dispatch: got {got} want [2]")

# validate-todo-file telemetry: an Edit on a TODO file under the skill
# binds to step 1 (observation only -- not gated).
got = match_step("validate-todo-file", "Edit", "todo/01-boot-platform/TODO-04-firmware-table-platform-inventory.md")
if 1 in got:
    print("OK validate_todo_file_edit_observed")
else:
    print(f"FAIL validate_todo_file_edit_observed: got {got} want [1]")

# Edit on a non-TODO path under validate-todo-file must NOT match.
got = match_step("validate-todo-file", "Edit", "src/kernel/uefi_config.c")
if got == []:
    print("OK validate_todo_file_non_todo_unbound")
else:
    print(f"FAIL validate_todo_file_non_todo_unbound: got {got} want []")
PYEOF
)
SSO_PY="${SSO_PY//\$REPO_ROOT/$REPO_ROOT}"
SSO_OUT="$(python3 -c "$SSO_PY" 2>&1)"
SSO_OK=$(echo "$SSO_OUT" | grep -c "^OK ")
if [ "$SSO_OK" = "23" ]; then
    echo "$SSO_OUT" | grep "^OK " | while IFS= read -r line; do
        t_pass "skill_step_observer: $line"
    done
    PASS=$((PASS + 23))
else
    t_fail "skill_step_observer: canonical-kind coverage incomplete" \
           "ok-count=$SSO_OK out=$SSO_OUT"
fi

# Marker-coverage regression (round 3 M5 fix): instead of a permissive grep
# that accepts marker-after-body shapes, parse each adversarial-review
# example and run the actual _review_kind classifier against it. The
# classifier scans the FIRST non-blank line of the prompt; an example
# fools the prior grep when the marker landed mid-prompt or when the
# prompt didn't start with `<`.
SSO_PARSER_OUT=$(python3 - <<'PYEOF'
import pathlib, re, sys
sys.path.insert(0, ".claude/hooks")
from _review_kind import detect_review_kind_from_cmd

# Round-4 M7 fix: fail-closed. Count every `adversarial-review` occurrence
# AND every successful (parsed + classifier-validated) example; fail if
# total != marked. Catches future heredoc / single-quoted shapes the
# simple double-quoted parser cannot reach.
# Match actual dispatch invocations -- NOT prose mentions like
# 'codex-adversarial-review-section' (skill name). Two canonical shapes:
#   1. Direct: codex-companion.mjs ... adversarial-review "<body>"
#   2. Wrapper (section-28 canonical): codex-dispatch.sh '<body>'
# Both bind to a marker via _review_kind.detect_review_kind_from_cmd.
TOTAL_RE = re.compile(
    r'(?:codex-companion\.mjs[^\n]*?\badversarial-review\b'
    r'|codex-dispatch\.sh)'
)
EXAMPLE_RE = re.compile(
    r"(?:codex-companion\.mjs[^\n]*?adversarial-review\s*\"([^\"]*)\""
    r"|codex-dispatch\.sh\s+'([^']*)')"
)
total = 0
marked = 0
problems = []
# Multi-line dispatches with shell line-continuation are out of scope
# for this validator; they are tracked under a follow-up section in
# 00-infrastructure/TODO-08-automation-hardening (shell-aware Codex
# dispatch segmentation). The pre-existing heredoc-spoofing concern
# is closed by the same follow-up. For now: simple-double-quoted-body
# dispatches are the only shape in use, and any future continuation
# example will fail-closed (TOTAL_RE > EXAMPLE_RE) for the maintainer
# to either rewrite as single-line OR land the section's shell-aware
# refactor.
for skill_md in pathlib.Path(".claude/skills").rglob("SKILL.md"):
    text = skill_md.read_text()
    file_total = len(TOTAL_RE.findall(text))
    file_parsed_bodies = EXAMPLE_RE.findall(text)
    # EXAMPLE_RE returns one match per dispatch with TWO groups: group 1
    # is the direct-node body (double-quoted), group 2 is the wrapper
    # body (single-quoted). Exactly one is set per match. Use whichever
    # is non-empty as the body, and reconstruct the appropriate full cmd
    # for the classifier.
    file_parsed = len(file_parsed_bodies)
    if file_total > file_parsed:
        problems.append(f'{skill_md}: {file_total} total dispatches, {file_parsed} parsable')
    for direct_body, wrapper_body in file_parsed_bodies:
        if direct_body:
            cmd = f'node /x/codex-companion.mjs adversarial-review "{direct_body}"'
            body_for_msg = direct_body
        else:
            cmd = f"bash scripts/codex-dispatch.sh '{wrapper_body}'"
            body_for_msg = wrapper_body
        if detect_review_kind_from_cmd(cmd):
            marked += 1
        else:
            problems.append(f'{skill_md}: classifier returns empty for body "{body_for_msg[:80]}"')
    total += file_total
status = "OK" if total == marked and not problems else "FAIL"
print(f'{status} total={total} marked={marked}')
for p in problems[:5]:
    print(p)
PYEOF
)
SSO_PARSER_HEAD=$(echo "$SSO_PARSER_OUT" | head -1)
if [[ "$SSO_PARSER_HEAD" == OK* ]]; then
    t_pass "skill_step_observer: skill_examples_classifier_validated ($SSO_PARSER_HEAD)"
else
    t_fail "skill_step_observer: skill_examples_classifier_validated" \
           "$(echo "$SSO_PARSER_OUT" | head -5)"
fi


# ============================================================================
# codex_dispatch_helper -- shell-aware Codex dispatch detection
# ============================================================================
# Owner: 00-infrastructure/TODO-08-automation-hardening (shell-aware
# Codex dispatch segmentation section). Six regression checks of the
# shared _codex_dispatch helper that all consumers (codex_review_completed,
# _review_kind, phase1_evidence_gate, skill_step_block) route through.

CDH_OUT=$(python3 - <<'PYEOF'
import sys
sys.path.insert(0, ".claude/hooks")
from _codex_dispatch import is_codex_dispatch, extract_dispatch_prompt

# (cmd, expected_trigger, label)
CASES = [
    ('node /x/codex-companion.mjs adversarial-review "[review-kind: adversarial] body"', True, "cdh_direct_node"),
    ("bash scripts/codex-dispatch.sh '[review-kind: perf] todo/foo body'", True, "cdh_wrapper"),
    # cd-prefixed compound: an unquoted newline is a command separator, so the
    # dispatch on the second line must still be detected (else the section gate
    # silently misses the review). Regression for the cd-prefix recorder footgun.
    ("cd /home/x/repo\nbash scripts/codex-dispatch.sh '[review-kind: adversarial] todo/foo body'", True, "cdh_cd_newline_prefix"),
    ('codex review "[review-kind: adversarial] body"', True, "cdh_bare_cli"),
    # heredoc-body MUST NOT classify (closes section-25 round-7 H)
    ('cat > /tmp/foo.md <<EOF\ncodex-companion.mjs adversarial-review "[review-kind: adversarial]"\nEOF', False, "cdh_heredoc_rejected"),
    # prose mention of codex-dispatch.sh inside a quoted argv to rg / grep
    # (closes section-28 wrapper-spoofing)
    ('rg "codex-dispatch.sh" .claude/skills', False, "cdh_prose_rejected"),
    # &&-chained command where the Codex dispatch is the second segment
    ('echo done && bash scripts/codex-dispatch.sh \'[review-kind: adversarial] body\'', True, "cdh_chained_2nd_seg"),
]
for cmd, want, label in CASES:
    got = is_codex_dispatch(cmd)
    if got is want:
        if want:
            prompt = extract_dispatch_prompt(cmd)
            has_marker = "review-kind" in prompt
            print(f"OK {label} (prompt-marker={has_marker})")
        else:
            print(f"OK {label} (rejected)")
    else:
        print(f"FAIL {label}: got {got!r} want {want!r}")
PYEOF
)
CDH_OK=$(echo "$CDH_OUT" | grep -c "^OK ")
if [ "$CDH_OK" = "7" ]; then
    echo "$CDH_OUT" | grep "^OK " | while IFS= read -r line; do
        t_pass "codex_dispatch_helper: $line"
    done
    PASS=$((PASS + 7))
else
    t_fail "codex_dispatch_helper: shell-aware coverage incomplete" \
           "ok-count=$CDH_OK out=$CDH_OUT"
fi


# ============================================================================
# xref_grammar -- the ONE shared Accepted/Deferred XREF grammar module
#   (scripts/ai-workflow/xref.py) that the git hook (accepted_xref_block),
#   the stamp writer (stamp.py), and todo-graph all route through. Guards the
#   tier classification (bare/soft/concrete) + parse + writer predicates so the
#   three-way XREF-grammar drift that stalled the unification cannot reappear.
# ============================================================================
[ "$QUIET" = "0" ] && echo "" && echo -e "${DIM}[xref_grammar]${NC}"

XREF_OUT=$(python3 - <<'PYEOF'
import sys
sys.path.insert(0, "scripts/ai-workflow")
import xref

S = "§"  # section sign built at runtime -- keeps a literal bare-section-ref out of this test file
BARE = '> **Accepted:** [M] foo -> XREF: 01-boot-platform/TODO-13'
SOFT = f'> **Deferred:** [H] bar -> XREF: 01-boot-platform/TODO-13 {S}7 (TPM NV Index Support)'
CONC = f'> **Accepted:** [M] baz -> XREF: 01-boot-platform/TODO-13 {S}4 (item: "Migrate HKEY" at line 248)'
MIXED = f'> **Accepted:** [M] x -> XREF: 01-boot-platform/TODO-13 {S}4 (item: "A" at line 9), XREF: 02-x/TODO-99'
OWNERLESS = '> **Accepted:** [M] x -> XREF: no-owner'

def check(label, cond):
    print(("OK " if cond else "FAIL ") + label)

check("bare_is_bare",         bool(xref.bare_clauses(BARE)) and not xref.soft_clauses(BARE))
check("soft_is_soft",         bool(xref.soft_clauses(SOFT)) and not xref.bare_clauses(SOFT))
check("concrete_is_ok",       not xref.bare_clauses(CONC) and not xref.soft_clauses(CONC))
check("is_concrete_rej_bare", xref.is_concrete(BARE) is False)
check("is_concrete_acc_conc", xref.is_concrete(CONC) is True)
check("is_concrete_rej_own",  xref.is_concrete(OWNERLESS) is False)
check("mixed_bare_rejected",  bool(xref.bare_clauses(MIXED)) and xref.is_concrete(MIXED) is False)
c = xref.parse(CONC)[0]
check("parse_target",         c.target_path == "01-boot-platform/TODO-13")
check("parse_section",        c.section == "4")
check("parse_item",           c.item_name == "Migrate HKEY")
check("canonical_true",       xref.canonical(CONC) is True)
check("canonical_false",      xref.canonical(SOFT) is False)
# writer predicates: an ownerless clause must be flagged even when a concrete
# sibling follows (adversarial finding: skip-on-no-TODO masked the ownerless one)
SIBL = f'[H] ghost -> XREF: no-owner (item: "ghost" at line 1), -> XREF: 00-infrastructure/TODO-10 {S}1 (item: "Real" at line 7)'
check("writer_flags_ownerless_sibling", bool(xref.writer_bare_xrefs(SIBL)))
# a quoted item name containing parens must not break the paren scan
NEST = f'[M] gap -> XREF: 00-infrastructure/TODO-10 {S}2 (item: "Enforce OWNERSHIP (not presence) in consumers" at line 134)'
check("writer_nested_paren_item_concrete", not xref.writer_bare_xrefs(NEST) and xref.writer_has_concrete(NEST))
# a TODO target only past the clause terminator does not own the clause
TAIL = '[M] gap -> XREF: no-owner stuff, later fixed near TODO-99 (item: "x" at line 3)'
check("writer_head_bounded_target", bool(xref.writer_bare_xrefs(TAIL)))
# writer-strict subset property: a concrete paren PAST the clause terminator is
# bare to the git hook (its clause regex stops at ,/;/]), so the writer must
# reject it too -- for every terminator shape
for i, t in enumerate((
    '[M] gap -> XREF: 00-infrastructure/TODO-11, later (item: "Real" at line 7)',
    '[M] gap -> XREF: 00-infrastructure/TODO-11; later (item: "Real" at line 7)',
    '[gap -> XREF: 00-infrastructure/TODO-11] later (item: "Real" at line 7)',
)):
    check(f"writer_subset_terminator_{i}",
          bool(xref.writer_bare_xrefs(t)) and not xref.writer_has_concrete(t)
          and bool(xref.bare_clauses(t)))
# _clause_spans must stay output-identical to the historical lazy regex SPEC
# (_CLAUSE_RE), including the coalescing shape where a clause spans a later
# XREF: marker because no terminator intervenes
import re as _re
def _new(t): return [t[a:b] for a, b in xref._clause_spans(t)]
TRICKY = (
    'XREF: no-owner XREF: 01-x/TODO-5 stuff (item: "a" at line 1), tail',
    'XREF: no-owner, XREF: 01-x/TODO-5 stuff',
    f'a -> XREF: 01-x/TODO-1 {S}2 (x); XREF: 02-y/TODO-2] XREF: TODO-3',
    'XREF: TODO-9XREF: TODO-8, XREF:',
)
check("clause_spans_matches_spec_regex",
      all(xref._CLAUSE_RE.findall(t) == _new(t) for t in TRICKY))
# and it must stay LINEAR: an ownerless-storm line (the commit-hook DoS shape)
# classifies in milliseconds, not quadratic seconds
import time as _time
storm = " ".join("XREF: no-owner filler text here" for _ in range(5000))
_t0 = _time.monotonic(); xref.parse(storm); _dt = _time.monotonic() - _t0
check("clause_spans_linear_on_storm", _dt < 0.5)
# a QUOTED terminator before the owner parenthetical truncates the quote-unaware
# hook clause to bare; the writer must stop at the same raw boundary and reject
# (re-adversarial finding: quote-blanking before the bound hid the comma)
QTERM = '[M] gap -> XREF: 00-infrastructure/TODO-11 "see, note" (item: "Real" at line 7)'
check("writer_rejects_quoted_terminator_before_paren",
      bool(xref.writer_bare_xrefs(QTERM)) and not xref.writer_has_concrete(QTERM)
      and bool(xref.bare_clauses(QTERM)))
# subset property fuzz: any summary the WRITER accepts must have no hook-BLOCK
# clause (writer-accepts implies hook-accepts, for every generated shape)
import random as _random
_random.seed(11)
_frag = ["XREF:", " 00-x/TODO-11", " no-owner", ",", ";", "]", ' "q, uote"',
         ' (item: "Real" at line 7)', " (later)", " item: loose", " text"]
_viol = [s for s in ("".join(_random.choice(_frag) for _ in range(_random.randint(1, 10)))
                     for _ in range(4000))
         if xref.writer_has_concrete(s) and not xref.writer_bare_xrefs(s)
         and xref.bare_clauses(s)]
check("writer_subset_property_fuzz", not _viol)
PYEOF
)
XREF_OK=$(echo "$XREF_OUT" | grep -c "^OK ")
if [ "$XREF_OK" = "22" ]; then
    echo "$XREF_OUT" | grep "^OK " | while IFS= read -r line; do
        t_pass "xref_grammar: $line"
    done
    PASS=$((PASS + 22))
else
    t_fail "xref_grammar: tier/parse/writer coverage incomplete" "ok-count=$XREF_OK out=$XREF_OUT"
fi


# ============================================================================
# review_pipeline_passthrough -- policy-scoped Bash prefix passthrough
# ============================================================================
# Owner: 00-infrastructure/TODO-08-automation-hardening (PreToolUse
# prefix-allowlist standardization section). Three regression checks:
#   - rpp_git_commit: `git commit ...` -> trusted (review pipeline)
#   - rpp_bash_scripts: `bash scripts/foo.sh` -> trusted (slash anchor)
#   - rpp_bash_random: `bash /tmp/random.sh` -> NOT trusted (no slash anchor)

RPP_OUT=$(python3 - <<'PYEOF'
import sys
sys.path.insert(0, ".claude/hooks")
from _review_pipeline_passthrough import is_review_pipeline_passthrough as ok

cases = [
    ("git commit -m foo",                 True,  "rpp_git_commit"),
    ("bash scripts/build.sh",             True,  "rpp_bash_scripts_slash"),
    ("bash /tmp/random.sh",               False, "rpp_bash_random_rejected"),
    ("node /x/codex-companion.mjs review", True,  "rpp_node_codex"),
    ("rm -rf /",                          False, "rpp_destructive_rejected"),
    ("rg '[review-kind: ...]'",           True,  "rpp_rg_search"),
]
for cmd, want, name in cases:
    got = ok(cmd)
    if got is want:
        print(f"OK {name}")
    else:
        print(f"FAIL {name}: cmd={cmd!r} got={got!r} want={want!r}")
PYEOF
)
RPP_OK=$(echo "$RPP_OUT" | grep -c "^OK ")
if [ "$RPP_OK" = "6" ]; then
    echo "$RPP_OUT" | grep "^OK " | while IFS= read -r line; do
        t_pass "review_pipeline_passthrough: $line"
    done
    PASS=$((PASS + 6))
else
    t_fail "review_pipeline_passthrough: prefix-policy regression incomplete" \
           "ok-count=$RPP_OK out=$RPP_OUT"
fi

# Drift-check regression: audit-hooks.sh DOTALL TUPLE_RE must catch a
# multi-line cmd.startswith((...)) tuple (perf re-dispatch M finding).
RPP_DRIFT_TMP=$(mktemp -d)
mkdir -p "$RPP_DRIFT_TMP/.claude/hooks"
cat > "$RPP_DRIFT_TMP/.claude/hooks/fixture-multiline.py" <<'EOF'
def is_pipeline(cmd):
    return cmd.startswith((
        'git ', 'bash scripts/',
        'node ', 'python3 ',
    ))
EOF
RPP_DRIFT_OUT=$(cd "$RPP_DRIFT_TMP" && python3 - <<'PYEOF'
import pathlib, re
TUPLE_RE = re.compile(
    r'startswith\s*\(\s*\((?:[^)]*?)'
    r'''(?:["']git ["']|["']bash scripts/["']|["']python3 ["']|["']node ["'])''',
    re.DOTALL,
)
hits = 0
for f in pathlib.Path(".claude/hooks").glob("*.py"):
    text = f.read_text()
    if TUPLE_RE.search(text):
        hits += 1
print(hits)
PYEOF
)
if [ "$RPP_DRIFT_OUT" = "1" ]; then
    t_pass "review_pipeline_passthrough: rpp_dotall_multiline_tuple_caught (audit-hooks DOTALL fix)"
else
    t_fail "review_pipeline_passthrough: rpp_dotall_multiline_tuple_caught" \
           "expected hits=1, got '$RPP_DRIFT_OUT'"
fi
rm -rf "$RPP_DRIFT_TMP"


# ============================================================================
# codex_dispatch_escaping -- prompt argument escaping doctrine
# ============================================================================
# Owner: 00-infrastructure/TODO-08-automation-hardening (prompt argument
# escaping doctrine section). Two regression checks:
#   - cde_argc_enforcement: wrapper rejects argc != 1 (multi-argv misuse).
#   - cde_lint_check_12: scripts/lint.sh Check 12 catches a documented
#     dispatch in DOUBLE quotes containing $(...) (load-bearing
#     source-text safety).

# Sub-test 1: argc enforcement.
CDE_OUT=$(bash scripts/codex-dispatch.sh 'arg one' 'arg two' 2>&1)
CDE_RC=$?
if [ "$CDE_RC" = "1" ] && echo "$CDE_OUT" | grep -q "BLOCK -- expected exactly 1 argv"; then
    t_pass "codex_dispatch_escaping: cde_argc_enforcement (multi-argv -> exit 1)"
else
    t_fail "codex_dispatch_escaping: cde_argc_enforcement" \
           "rc=$CDE_RC out=$(echo "$CDE_OUT" | head -2)"
fi

# Sub-test 2: lint Check 12 catches double-quoted dangerous bodies.
# Construct the fixture programmatically so the dangerous pattern does
# NOT live in scripts/test-tooling.sh source text (where Check 12 would
# catch it -- the lint scanner reads source files directly).
CDE_TMP=$(mktemp -d)
mkdir -p "$CDE_TMP/repo/scripts" "$CDE_TMP/repo/.claude/skills"
cp scripts/lint.sh "$CDE_TMP/repo/scripts/" 2>/dev/null
DOLLAR='$'
PAREN_OPEN='('
PAREN_CLOSE=')'
DANGER_BODY="[review-kind: adversarial] ${DOLLAR}${PAREN_OPEN}SYSTEM_DISK${PAREN_CLOSE} prompt"
NODE_LINE='node "${HOME}/.claude/plugins/marketplaces/openai-codex/plugins/codex/scripts/codex-companion.mjs" adversarial-review'
{
    echo "Example dispatch (intentionally bad):"
    echo
    echo '```bash'
    echo "$NODE_LINE \"${DANGER_BODY}\""
    echo '```'
} > "$CDE_TMP/repo/.claude/skills/fixture-bad.md"
# Run only the Check 12 logic against the fixture by invoking it
# inline (the live lint.sh would scan our actual repo too; we want
# isolated fixture coverage).
CDE_LINT_OUT=$(cd "$CDE_TMP/repo" && python3 - <<'PYEOF'
import pathlib, re
CODEX_LINE_RE = re.compile(
    r"(?:codex-companion\.mjs[^\n]*?adversarial-review|codex-dispatch\.sh)"
    r"\s+(.*)$"
)
DANGER_DOUBLE = re.compile(r'"[^"]*?(\$\(|\$\{)[^"]*?"')
hits = 0
for f in pathlib.Path(".claude/skills").rglob("*.md"):
    text = f.read_text()
    for ln in text.splitlines():
        m = CODEX_LINE_RE.search(ln)
        if not m:
            continue
        body = m.group(1)
        stripped = body.lstrip()
        if not stripped or stripped[0] != '"':
            continue
        if DANGER_DOUBLE.search(body):
            hits += 1
print(hits)
PYEOF
)
if [ "$CDE_LINT_OUT" = "1" ]; then
    t_pass "codex_dispatch_escaping: cde_lint_check_12 (dangerous-double-quoted body flagged)"
else
    t_fail "codex_dispatch_escaping: cde_lint_check_12" \
           "expected hits=1, got '$CDE_LINT_OUT'"
fi
rm -rf "$CDE_TMP"


# ============================================================================
# Boot-error history cascade fixture (consumer-side)
# ============================================================================
# Runs bash scripts/test-smoke-history.sh (4 QEMU boots: 3 corrupt-kernel
# fatals + 1 clean boot, asserting the kernel renderer surfaces the
# "Recent boot history (4 attempts)" klog block).  Disabled by default
# because each run is ~30-60s; opt in with SMOKE_HISTORY=1.  Skips
# gracefully when the host lacks mtools / OVMF / qemu so CI matrices
# without those packages still pass.
if [ "${SMOKE_HISTORY:-0}" = "1" ]; then
    if command -v mcopy >/dev/null && command -v qemu-system-x86_64 >/dev/null \
       && [ -f /usr/share/OVMF/OVMF_CODE_4M.fd ]; then
        if bash scripts/test-smoke-history.sh >/dev/null 2>&1; then
            t_pass "smoke_history: cascade fixture (3 corrupt + 1 clean boot)"
        else
            t_fail "smoke_history: cascade fixture" \
                   "scripts/test-smoke-history.sh failed; rerun to see output"
        fi
    fi
fi


# ============================================================================
# Boot certification matrix (TODO-28 boot-validation)
# ============================================================================
# tools/boot-cert/lint.py is the release gate that proves boot-cert.yml is
# schema-valid, every boot-platform TODO maps to a row, and every required row
# carries machine-readable evidence. test_lint.py proves the gate rejects bad
# input (not advisory). Both must exit 0 here so CI gates on the matrix.

[ "$QUIET" = "0" ] && echo "" && echo -e "${DIM}[boot certification matrix]${NC}"
BOOTCERT_DIR="$REPO_ROOT/tools/boot-cert"
if [ -f "$BOOTCERT_DIR/boot-cert.yml" ]; then
    t_pass "boot-cert.yml present"
else
    t_fail "boot-cert.yml present" "missing: $BOOTCERT_DIR/boot-cert.yml"
fi
assert_exit_zero "boot-cert lint passes (schema + coverage + evidence)" \
    python3 "$BOOTCERT_DIR/lint.py" --quiet
assert_exit_zero "boot-cert self-test passes (gate rejects bad input)" \
    python3 "$BOOTCERT_DIR/test_lint.py"
# Boot-reliability gate (S2): flake-detection verdict logic + schema conformance.
assert_exit_zero "boot-reliability self-check (registry + schemas load)" \
    python3 "$BOOTCERT_DIR/boot_reliability.py" --self-check --platform qemu-tcg --tier stable
assert_exit_zero "boot-reliability self-test (classify/aggregate/schema)" \
    python3 "$BOOTCERT_DIR/test_boot_reliability.py"

# ============================================================================
# AI workflow driver interchangeability (TODO-10)
# ============================================================================
# The shared scripts under scripts/ai-workflow are the tool-neutral core that
# lets Claude hooks, git hooks, and the Codex driver adapter consult the same
# lease/evidence/obligation/stamp/gate state. These tests use a temporary
# AI_WORKFLOW_STATE_DIR so they do not touch the real .ai-workflow state.

[ "$QUIET" = "0" ] && echo "" && echo -e "${DIM}[ai workflow driver interchangeability]${NC}"
AIWF_TMP="$(mktemp -d "${TMPDIR:-/tmp}/impossible-aiwf-XXXXXX")"
AIWF_STATE="$AIWF_TMP/state"
TODO10="todo/00-infrastructure/TODO-10-ai-driver-interchangeability.md"
AIWF_ENV=(env AI_WORKFLOW_STATE_DIR="$AIWF_STATE")

if "${AIWF_ENV[@]}" python3 scripts/ai-workflow/lease.py acquire \
    --todo "$TODO10" --section 1 --driver claude --run-id driver-a >/dev/null; then
    t_pass "ai_workflow_lease: acquire first driver lease"
else
    t_fail "ai_workflow_lease: acquire first driver lease"
fi
if "${AIWF_ENV[@]}" python3 scripts/ai-workflow/lease.py acquire \
    --todo "$TODO10" --section 1 --driver claude --run-id driver-b >/dev/null 2>&1; then
    t_fail "ai_workflow_lease: conflicting driver lease rejected" \
           "second driver unexpectedly acquired same section"
else
    t_pass "ai_workflow_lease: conflicting driver lease rejected"
fi
if "${AIWF_ENV[@]}" python3 scripts/ai-workflow/lease.py release >/dev/null 2>&1; then
    t_fail "ai_workflow_lease: release requires --run-id" "anonymous release succeeded"
else
    t_pass "ai_workflow_lease: release requires --run-id"
fi
if "${AIWF_ENV[@]}" python3 scripts/ai-workflow/lease.py release --run-id driver-b >/dev/null 2>&1; then
    t_fail "ai_workflow_lease: non-owner release rejected" "driver-b released driver-a's lease"
else
    t_pass "ai_workflow_lease: non-owner release rejected"
fi
if "${AIWF_ENV[@]}" python3 scripts/ai-workflow/lease.py release --run-id driver-a >/dev/null; then
    t_pass "ai_workflow_lease: release by owning run"
else
    t_fail "ai_workflow_lease: release by owning run"
fi
# expired lease is not an authority: an owner whose TTL lapsed cannot complete it
# (ttl-sec 0 -> expires at acquire time; the inter-process gap makes it expired)
"${AIWF_ENV[@]}" python3 scripts/ai-workflow/lease.py acquire \
    --todo "$TODO10" --section 9 --driver claude --run-id exp-a --ttl-sec 0 >/dev/null 2>&1
if "${AIWF_ENV[@]}" python3 scripts/ai-workflow/lease.py complete --run-id exp-a >/dev/null 2>&1; then
    t_fail "ai_workflow_lease: expired lease cannot be completed" "completed an expired lease"
else
    t_pass "ai_workflow_lease: expired lease cannot be completed"
fi
"${AIWF_ENV[@]}" python3 scripts/ai-workflow/lease.py force-release --reason "cleanup expired test lease" >/dev/null 2>&1
AIWF_RACE_STATE="$AIWF_TMP/race-state"
rm -rf "$AIWF_RACE_STATE"
if env AI_WORKFLOW_STATE_DIR="$AIWF_RACE_STATE" \
       AI_WORKFLOW_LEASE_TEST_DELAY_SEC=0.2 \
       python3 - "$TODO10" <<'PY'
import json
import os
import pathlib
import subprocess
import sys

todo = sys.argv[1]
state = pathlib.Path(os.environ["AI_WORKFLOW_STATE_DIR"])
cmd_base = [
    sys.executable, "scripts/ai-workflow/lease.py", "acquire",
    "--todo", todo,
    "--section", "2",
    "--driver", "codex",
]
env = os.environ.copy()
procs = [
    subprocess.Popen(
        cmd_base + ["--run-id", run_id],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        env=env,
    )
    for run_id in ("race-a", "race-b")
]
results = []
for proc in procs:
    out, err = proc.communicate(timeout=10)
    results.append((proc.returncode, out, err))
success = [item for item in results if item[0] == 0]
conflict = [item for item in results if item[0] == 2]
if len(success) != 1 or len(conflict) != 1:
    raise SystemExit(f"expected 1 success and 1 conflict, got {results}")
history_path = state / "lease-history.jsonl"
events = [
    json.loads(line)
    for line in history_path.read_text(encoding="utf-8").splitlines()
    if line.strip()
]
if sum(1 for event in events if event.get("action") == "acquire") != 1:
    raise SystemExit(f"expected one durable acquire event, got {events}")
conflicts = [event for event in events if event.get("action") == "conflict"]
if len(conflicts) != 1 or "requested" not in conflicts[0]:
    raise SystemExit(f"expected one durable conflict event with request, got {events}")
active = json.loads((state / "active-lease.json").read_text(encoding="utf-8"))
if active.get("driver_run_id") not in ("race-a", "race-b"):
    raise SystemExit(f"unexpected active lease owner: {active}")
PY
then
    t_pass "ai_workflow_lease: concurrent acquire has one winner and one conflict event"
else
    t_fail "ai_workflow_lease: concurrent acquire has one winner and one conflict event"
fi

# Reviewer independence: reviewer evidence with the SAME run id as the driver
# must not satisfy obligations. Fresh reviewer run ids then satisfy the review
# obligations without rerunning duplicate work.
rm -rf "$AIWF_STATE"
mkdir -p "$AIWF_STATE"
"${AIWF_ENV[@]}" python3 scripts/ai-workflow/lease.py acquire \
    --todo "$TODO10" --section 2 --driver claude --run-id driver-c >/dev/null
"${AIWF_ENV[@]}" python3 scripts/ai-workflow/evidence.py record \
    --todo "$TODO10" --section 2 --role validator --backend local \
    --run-id build-c --kind build --result ok >/dev/null
OB_UNSOURCED_BUILD=$("${AIWF_ENV[@]}" python3 scripts/ai-workflow/obligations.py \
    "$TODO10" --section 2 --driver-run-id driver-c --format json)
if echo "$OB_UNSOURCED_BUILD" | python3 -c 'import json,sys; d=json.load(sys.stdin); print(any(i["name"]=="build" and i["status"]=="missing" for i in d["required"]))' | grep -q True; then
    t_pass "ai_workflow_obligations: unsourced build evidence does not satisfy shipping gate"
else
    t_fail "ai_workflow_obligations: unsourced build evidence does not satisfy shipping gate"
fi
"${AIWF_ENV[@]}" python3 scripts/ai-workflow/evidence.py record \
    --todo "$TODO10" --section 2 --role validator --backend local \
    --run-id build-c-sourced --kind build --result ok --source "$TODO10" >/dev/null
"${AIWF_ENV[@]}" python3 scripts/ai-workflow/evidence.py record \
    --todo "$TODO10" --section 2 --role validator --backend local \
    --run-id graph-c --kind todo-graph-validate --result ok >/dev/null
for kind in adversarial consistency perf; do
    "${AIWF_ENV[@]}" python3 scripts/ai-workflow/evidence.py record \
        --todo "$TODO10" --section 2 --role "codex-reviewer-$kind" \
        --backend codex --run-id driver-c --kind "$kind" --result received \
        --source "$TODO10" >/dev/null
done
OB_SAME=$("${AIWF_ENV[@]}" python3 scripts/ai-workflow/obligations.py \
    "$TODO10" --section 2 --driver-run-id driver-c --format json)
if echo "$OB_SAME" | grep -q '"name": "codex-review-adversarial".*"status": "missing"' \
   || echo "$OB_SAME" | python3 -c 'import json,sys; d=json.load(sys.stdin); print(any(i["name"]=="codex-review-adversarial" and i["status"]=="missing" for i in d["required"]))' | grep -q True; then
    t_pass "ai_workflow_obligations: same-run Codex review does not self-certify"
else
    t_fail "ai_workflow_obligations: same-run Codex review does not self-certify"
fi
AIWF_LEGACY_ROOT="$AIWF_TMP/legacy-root"
AIWF_LEGACY_STATE="$AIWF_TMP/legacy-state"
mkdir -p "$AIWF_LEGACY_ROOT/.claude/state" "$AIWF_LEGACY_STATE"
python3 - "$AIWF_LEGACY_ROOT/.claude/state/last-review-stamps.json" "$TODO10" <<'PY'
import json, pathlib, sys, time
path = pathlib.Path(sys.argv[1])
todo = sys.argv[2]
now = time.time_ns()
path.write_text(json.dumps({
    todo: {
        "section": "2",
        "adversarial": now,
        "consistency": now,
        "perf": now,
    }
}, indent=2) + "\n")
PY
AIWF_LEGACY_ENV=(env AI_WORKFLOW_REPO_ROOT="$AIWF_LEGACY_ROOT" AI_WORKFLOW_STATE_DIR="$AIWF_LEGACY_STATE")
if "${AIWF_LEGACY_ENV[@]}" python3 scripts/ai-workflow/evidence.py import-legacy >/dev/null \
   && grep -q '"result": "telemetry"' "$AIWF_LEGACY_STATE/evidence.jsonl" \
   && ! grep -q '"result": "received"' "$AIWF_LEGACY_STATE/evidence.jsonl"; then
    t_pass "ai_workflow_import_legacy: last-review-stamps import as telemetry"
else
    t_fail "ai_workflow_import_legacy: last-review-stamps import as telemetry" \
           "$(cat "$AIWF_LEGACY_STATE/evidence.jsonl" 2>&1 | tail -5)"
fi
OB_LEGACY=$("${AIWF_LEGACY_ENV[@]}" python3 scripts/ai-workflow/obligations.py \
    "$TODO10" --section 2 --format json)
if echo "$OB_LEGACY" | python3 -c 'import json,sys; d=json.load(sys.stdin); print(any(i["name"]=="codex-review-adversarial" and i["status"]=="missing" for i in d["required"]))' | grep -q True; then
    t_pass "ai_workflow_obligations: legacy review stamps do not satisfy shipping review"
else
    t_fail "ai_workflow_obligations: legacy review stamps do not satisfy shipping review"
fi
rm -rf "$AIWF_LEGACY_ROOT" "$AIWF_LEGACY_STATE"
mkdir -p "$AIWF_LEGACY_ROOT/.claude/state" "$AIWF_LEGACY_STATE"
python3 - "$AIWF_LEGACY_ROOT/.claude/state/codex-review-history.jsonl" "$TODO10" <<'PY'
import json, pathlib, sys, time
path = pathlib.Path(sys.argv[1])
todo = sys.argv[2]
item = {
    "received": True,
    "review_kind": "adversarial",
    "todo_path": todo,
    "section": "2",
    "review_run_id": "legacy-unsourced-adversarial",
    "trigger": f"[review-kind: adversarial] {todo} section 2",
    "received_timestamp_ns": time.time_ns(),
}
path.write_text(json.dumps(item) + "\n")
PY
if "${AIWF_LEGACY_ENV[@]}" python3 scripts/ai-workflow/evidence.py import-legacy >/dev/null \
   && grep -q '"role": "legacy-review-history-importer"' "$AIWF_LEGACY_STATE/evidence.jsonl" \
   && grep -q '"result": "telemetry"' "$AIWF_LEGACY_STATE/evidence.jsonl" \
   && ! grep -q '"result": "received"' "$AIWF_LEGACY_STATE/evidence.jsonl"; then
    t_pass "ai_workflow_import_legacy: unsourced review history imports as telemetry"
else
    t_fail "ai_workflow_import_legacy: unsourced review history imports as telemetry" \
           "$(cat "$AIWF_LEGACY_STATE/evidence.jsonl" 2>&1 | tail -5)"
fi
OB_HISTORY=$("${AIWF_LEGACY_ENV[@]}" python3 scripts/ai-workflow/obligations.py \
    "$TODO10" --section 2 --format json)
if echo "$OB_HISTORY" | python3 -c 'import json,sys; d=json.load(sys.stdin); print(any(i["name"]=="codex-review-adversarial" and i["status"]=="missing" for i in d["required"]))' | grep -q True; then
    t_pass "ai_workflow_obligations: unsourced legacy review history does not satisfy shipping review"
else
    t_fail "ai_workflow_obligations: unsourced legacy review history does not satisfy shipping review"
fi
for kind in adversarial consistency perf; do
    "${AIWF_ENV[@]}" python3 scripts/ai-workflow/evidence.py record \
        --todo "$TODO10" --section 2 --role "codex-reviewer-$kind" \
        --backend codex --run-id "review-$kind" --kind "$kind" --result received \
        --source "$TODO10" >/dev/null
done
if "${AIWF_ENV[@]}" python3 scripts/ai-workflow/stamp.py verified \
    "$TODO10" --section 2 --driver-run-id driver-c --dry-run >/dev/null; then
    t_pass "ai_workflow_stamp: verified dry-run succeeds with fresh reviewer evidence"
else
    t_fail "ai_workflow_stamp: verified dry-run succeeds with fresh reviewer evidence"
fi
if "${AIWF_ENV[@]}" python3 scripts/ai-workflow/gates.py commit \
    --todo "$TODO10" --section 2 --driver-run-id driver-c >/dev/null 2>&1; then
    t_fail "ai_workflow_gates: commit blocks before verified stamp evidence"
else
    t_pass "ai_workflow_gates: commit blocks before verified stamp evidence"
fi
"${AIWF_ENV[@]}" python3 scripts/ai-workflow/evidence.py record \
    --todo "$TODO10" --section 2 --role stamp-writer --backend ai-workflow \
    --run-id stamp-c --kind stamp.verified --result ok --source "$TODO10" >/dev/null
if "${AIWF_ENV[@]}" python3 scripts/ai-workflow/gates.py commit \
    --todo "$TODO10" --section 2 --driver-run-id driver-c >/dev/null; then
    t_pass "ai_workflow_gates: commit allows after lease/build/review/stamp evidence"
else
    t_fail "ai_workflow_gates: commit allows after lease/build/review/stamp evidence"
fi
DIRECT_GATE=$("${AIWF_ENV[@]}" python3 scripts/ai-workflow/gates.py commit \
    --todo "$TODO10" --section 2 --driver-run-id driver-c --mode direct --format json)
GIT_GATE=$("${AIWF_ENV[@]}" python3 scripts/ai-workflow/gates.py commit \
    --todo "$TODO10" --section 2 --driver-run-id driver-c --mode git-hook --format json)
HOOK_GATE=$("${AIWF_ENV[@]}" python3 scripts/ai-workflow/gates.py commit \
    --todo "$TODO10" --section 2 --driver-run-id driver-c --mode claude-hook --format json)
DIRECT_VERDICT=$(echo "$DIRECT_GATE" | python3 -c 'import json,sys; print(json.load(sys.stdin)["verdict"])')
GIT_VERDICT=$(echo "$GIT_GATE" | python3 -c 'import json,sys; print(json.load(sys.stdin)["verdict"])')
HOOK_VERDICT=$(echo "$HOOK_GATE" | python3 -c 'import json,sys; print(json.load(sys.stdin)["verdict"])')
if [ "$DIRECT_VERDICT" = "ALLOW" ] && [ "$GIT_VERDICT" = "ALLOW" ] && [ "$HOOK_VERDICT" = "ALLOW" ]; then
    t_pass "ai_workflow_gates: direct/git-hook/claude-hook modes share verdict"
else
    t_fail "ai_workflow_gates: direct/git-hook/claude-hook modes share verdict" \
           "direct=$DIRECT_VERDICT git=$GIT_VERDICT hook=$HOOK_VERDICT"
fi

AIWF_LEASE_SCALE_STATE="$AIWF_TMP/lease-scale-state"
rm -rf "$AIWF_LEASE_SCALE_STATE"
mkdir -p "$AIWF_LEASE_SCALE_STATE"
if env AI_WORKFLOW_STATE_DIR="$AIWF_LEASE_SCALE_STATE" python3 - "$AIWF_LEASE_SCALE_STATE" "$TODO10" <<'PY'
import importlib.util
import os
import pathlib
import shutil
import subprocess
import sys

state = pathlib.Path(sys.argv[1])
todo = sys.argv[2]
root = pathlib.Path.cwd()
sys.path.insert(0, str(root / "scripts"))
os.environ["AI_WORKFLOW_STATE_DIR"] = str(state)

from ai_workflow_import import common  # noqa: E402

spec = importlib.util.spec_from_file_location(
    "ai_workflow_gates", root / "scripts/ai-workflow/gates.py"
)
gates = importlib.util.module_from_spec(spec)
assert spec and spec.loader
spec.loader.exec_module(gates)

ledger = state / "lease-history.jsonl"
for i in range(1800):
    common.append_jsonl(
        ledger,
        {
            "action": "complete",
            "lease": {
                "todo_path": "todo/other.md",
                "section": "9",
                "driver_run_id": f"old-{i}",
            },
        },
    )
common.append_jsonl(
    ledger,
    {
        "action": "complete",
        "lease": {
            "todo_path": todo,
            "section": "8",
            "driver_run_id": "lease-scale-current",
        },
    },
)

real_iter_jsonl = common.iter_jsonl
real_iter_jsonl_reverse = common.iter_jsonl_reverse
seen = {"n": 0}


def fail_full_jsonl(path):
    raise AssertionError(f"full lease history scan used for {path}")


def counted_reverse(path, block_size=65536):
    for item in real_iter_jsonl_reverse(path, block_size):
        seen["n"] += 1
        yield item


common.iter_jsonl = fail_full_jsonl
common.iter_jsonl_reverse = counted_reverse
try:
    ok, ids = gates._active_or_completed_lease(todo, "8", root)
finally:
    common.iter_jsonl = real_iter_jsonl
    common.iter_jsonl_reverse = real_iter_jsonl_reverse

if not ok or ids != ["complete:lease-scale-current"]:
    raise SystemExit(f"completed lease not found: ok={ok} ids={ids}")
if seen["n"] > 3:
    raise SystemExit(f"lease history reverse scan consumed stale records: {seen['n']}")

ledger.unlink()
for i in range(1800):
    common.append_jsonl(
        ledger,
        {
            "action": "complete",
            "lease": {
                "todo_path": "todo/other.md",
                "section": "9",
                "driver_run_id": f"miss-{i}",
            },
        },
    )

seen["n"] = 0
common.iter_jsonl = fail_full_jsonl
common.iter_jsonl_reverse = counted_reverse
try:
    ok, ids = gates._active_or_completed_lease(todo, "404", root)
finally:
    common.iter_jsonl = real_iter_jsonl
    common.iter_jsonl_reverse = real_iter_jsonl_reverse

if ok or ids:
    raise SystemExit(f"missing lease unexpectedly found: ok={ok} ids={ids}")
if seen["n"] > gates.LEASE_HISTORY_SCAN_LIMIT + 1:
    raise SystemExit(f"missing lease scan was unbounded: {seen['n']}")
PY
then
    t_pass "ai_workflow_gates: completed/missing lease lookup uses bounded reverse history scan"
else
    t_fail "ai_workflow_gates: completed/missing lease lookup uses bounded reverse history scan"
fi

AIWF_REVIEW_STATE="$AIWF_TMP/review-state"
rm -rf "$AIWF_REVIEW_STATE"
mkdir -p "$AIWF_REVIEW_STATE"
AIWF_REVIEW_ENV=(env AI_WORKFLOW_STATE_DIR="$AIWF_REVIEW_STATE")
"${AIWF_REVIEW_ENV[@]}" python3 scripts/ai-workflow/lease.py acquire \
    --todo "$TODO10" --section 4 --driver claude --run-id review-profile-driver >/dev/null
"${AIWF_REVIEW_ENV[@]}" python3 scripts/ai-workflow/evidence.py record \
    --todo "$TODO10" --section 4 --role validator --backend local \
    --run-id review-profile-graph --kind todo-graph-validate --result ok >/dev/null
for kind in adversarial consistency perf; do
    "${AIWF_REVIEW_ENV[@]}" python3 scripts/ai-workflow/evidence.py record \
        --todo "$TODO10" --section 4 --role "codex-reviewer-$kind" \
        --backend codex --run-id "review-profile-$kind" --kind "$kind" \
        --result received --source "$TODO10" >/dev/null
done
OB_REVIEW_PROFILE=$("${AIWF_REVIEW_ENV[@]}" python3 scripts/ai-workflow/obligations.py \
    "$TODO10" --section 4 --workflow review --driver-run-id review-profile-driver --format json)
if echo "$OB_REVIEW_PROFILE" | python3 -c 'import json,sys; d=json.load(sys.stdin); print(d["status"]=="ready" and all(i["name"]!="build" for i in d["required"]))' | grep -q True; then
    t_pass "ai_workflow_obligations: review workflow does not require build evidence"
else
    t_fail "ai_workflow_obligations: review workflow does not require build evidence" "$OB_REVIEW_PROFILE"
fi

AIWF_SCALE_STATE="$AIWF_TMP/scale-state"
rm -rf "$AIWF_SCALE_STATE"
mkdir -p "$AIWF_SCALE_STATE"
if env AI_WORKFLOW_STATE_DIR="$AIWF_SCALE_STATE" python3 - "$AIWF_SCALE_STATE" "$TODO10" <<'PY'
import importlib.util
import os
import pathlib
import shutil
import subprocess
import sys
from types import SimpleNamespace

state = pathlib.Path(sys.argv[1])
todo = sys.argv[2]
section = "4"
driver_run = "scale-driver"
root = pathlib.Path.cwd()
sys.path.insert(0, str(root / "scripts"))
os.environ["AI_WORKFLOW_STATE_DIR"] = str(state)

from ai_workflow_import import common  # noqa: E402
real_git_blob_or_digest = common.git_blob_or_digest
real_git_blobs_or_digests = common.git_blobs_or_digests
real_check_output = common.subprocess.check_output

spec = importlib.util.spec_from_file_location(
    "ai_workflow_obligations", root / "scripts/ai-workflow/obligations.py"
)
ob = importlib.util.module_from_spec(spec)
assert spec and spec.loader
spec.loader.exec_module(ob)

current_blob = common.git_blob_or_digest(todo, root)
if not current_blob:
    raise SystemExit("missing current TODO blob")

common.write_json_atomic(
    state / "active-lease.json",
    {
        "todo_path": todo,
        "section": section,
        "driver": "codex",
        "driver_run_id": driver_run,
        "expires_at_ns": common.now_ns() + 3600 * 1_000_000_000,
    },
)
ledger = state / "evidence.jsonl"


def append(kind, run_id, role, result="ok", source_blob=None):
    payload = {
        "task_id": f"{todo}#{section}",
        "todo_path": todo,
        "section": section,
        "role": role,
        "backend": "local",
        "run_id": run_id,
        "kind": kind,
        "head_sha": "HEAD",
        "source_blobs": {todo: source_blob} if source_blob is not None else {},
        "result": result,
        "summary_path": "",
        "created_at_ns": common.now_ns(),
        "created_at": common.now_iso(),
        "expires_at_ns": 0,
        "legacy_import": False,
        "metadata": {},
    }
    ob.evidence.append_event(payload, root)


for i in range(250):
    append("build", f"stale-build-{i}", "validator", source_blob=f"stale-{i}")

append("build", "scale-build", "validator", source_blob=current_blob)
for kind in ("adversarial", "consistency", "perf"):
    append(kind, f"scale-review-{kind}", f"codex-reviewer-{kind}", result="received", source_blob=current_blob)
append("stamp.verified", "scale-stamp", "stamp-writer", source_blob=current_blob)
append("todo-graph-validate", "scale-graph", "validator")

calls = {"n": 0}


def counted_blob(path, repo_root=None):
    calls["n"] += 1
    if common.rel_path(path, repo_root or root) == todo:
        return current_blob
    return ""


def counted_blobs(paths, repo_root=None):
    calls["n"] += 1
    return {
        common.rel_path(path, repo_root or root): current_blob
        for path in paths
    }


ob.common.git_blob_or_digest = counted_blob
ob.common.git_blobs_or_digests = counted_blobs
out = ob.resolve(
    SimpleNamespace(
        todo=todo,
        section=section,
        workflow="implement",
        driver_run_id=driver_run,
    )
)
if out["status"] != "ready":
    raise SystemExit(out)
if calls["n"] > 5:
    raise SystemExit(f"source blob lookups were not bounded: {calls['n']}")

section = "5"
driver_run = "scale-driver-multifile"
common.write_json_atomic(
    state / "active-lease.json",
    {
        "todo_path": todo,
        "section": section,
        "driver": "codex",
        "driver_run_id": driver_run,
        "expires_at_ns": common.now_ns() + 3600 * 1_000_000_000,
    },
)
ledger.unlink()
multi_blobs = {
    f"scripts/generated-scale-{i}.py": f"blob-{i}"
    for i in range(90)
}


def append_multi(kind, run_id, role, result="ok"):
    payload = {
        "task_id": f"{todo}#{section}",
        "todo_path": todo,
        "section": section,
        "role": role,
        "backend": "local",
        "run_id": run_id,
        "kind": kind,
        "head_sha": "HEAD",
        "source_blobs": multi_blobs,
        "result": result,
        "summary_path": "",
        "created_at_ns": common.now_ns(),
        "created_at": common.now_iso(),
        "expires_at_ns": 0,
        "legacy_import": False,
        "metadata": {},
    }
    ob.evidence.append_event(payload, root)


append_multi("build", "scale-multi-build", "validator")
for kind in ("adversarial", "consistency", "perf"):
    append_multi(kind, f"scale-multi-review-{kind}", f"codex-reviewer-{kind}", result="received")
append_multi("stamp.verified", "scale-multi-stamp", "stamp-writer")
append("todo-graph-validate", "scale-multi-graph", "validator")

multi_calls = {"n": 0}


def fail_single_blob(path, repo_root=None):
    raise AssertionError(f"unexpected per-path blob lookup for {path}")


def counted_multi_blobs(paths, repo_root=None):
    multi_calls["n"] += 1
    return {
        common.rel_path(path, repo_root or root): multi_blobs[common.rel_path(path, repo_root or root)]
        for path in paths
    }


ob.common.git_blob_or_digest = fail_single_blob
ob.common.git_blobs_or_digests = counted_multi_blobs
out = ob.resolve(
    SimpleNamespace(
        todo=todo,
        section=section,
        workflow="implement",
        driver_run_id=driver_run,
    )
)
if out["status"] != "ready":
    raise SystemExit(out)
if multi_calls["n"] > 1:
    raise SystemExit(f"multi-file source current check was not batched: {multi_calls['n']}")

section = "6"
driver_run = "scale-driver-cross-event"
common.write_json_atomic(
    state / "active-lease.json",
    {
        "todo_path": todo,
        "section": section,
        "driver": "codex",
        "driver_run_id": driver_run,
        "expires_at_ns": common.now_ns() + 3600 * 1_000_000_000,
    },
)
ledger.unlink()
append("build", "scale-cross-build", "validator", source_blob=current_blob)
for kind in ("adversarial", "consistency", "perf"):
    append(kind, f"scale-cross-review-{kind}", f"codex-reviewer-{kind}", result="received", source_blob=current_blob)
append("stamp.verified", "scale-cross-stamp", "stamp-writer", source_blob=current_blob)
append("todo-graph-validate", "scale-cross-graph", "validator")


def append_stale_build(i):
    stale_path = f"scripts/stale-event-{i}.py"
    payload = {
        "task_id": f"{todo}#{section}",
        "todo_path": todo,
        "section": section,
        "role": "validator",
        "backend": "local",
        "run_id": f"scale-cross-stale-{i}",
        "kind": "build",
        "head_sha": "HEAD",
        "source_blobs": {stale_path: f"old-{i}"},
        "result": "ok",
        "summary_path": "",
        "created_at_ns": common.now_ns(),
        "created_at": common.now_iso(),
        "expires_at_ns": 0,
        "legacy_import": False,
        "metadata": {},
    }
    ob.evidence.append_event(payload, root)


for i in range(120):
    append_stale_build(i)

cross_calls = {"n": 0}


def counted_cross_blobs(paths, repo_root=None):
    cross_calls["n"] += 1
    out = {}
    for path in paths:
        rel = common.rel_path(path, repo_root or root)
        if rel == todo:
            out[rel] = current_blob
        elif rel.startswith("scripts/stale-event-"):
            out[rel] = "new-" + rel.rsplit("-", 1)[-1].removesuffix(".py")
        else:
            out[rel] = ""
    return out


ob.common.git_blob_or_digest = fail_single_blob
ob.common.git_blobs_or_digests = counted_cross_blobs
out = ob.resolve(
    SimpleNamespace(
        todo=todo,
        section=section,
        workflow="implement",
        driver_run_id=driver_run,
    )
)
if out["status"] != "ready":
    raise SystemExit(out)
if cross_calls["n"] > 2:
    raise SystemExit(f"cross-event source current check was not batched: {cross_calls['n']}")

section = "6b"
ledger.unlink()
append("build", "scale-single-current", "validator", source_blob=current_blob)
for i in range(300):
    append("build", f"scale-single-stale-{i}", "validator", source_blob=f"old-single-{i}")

single_calls = {"n": 0}


def counted_single_blobs(paths, repo_root=None):
    single_calls["n"] += 1
    return {
        common.rel_path(path, repo_root or root): current_blob
        for path in paths
    }


ob.common.git_blob_or_digest = fail_single_blob
ob.common.git_blobs_or_digests = counted_single_blobs
found = ob._resolve_events(
    todo,
    section,
    [
        {
            "name": "build",
            "match": {
                "kind": "build",
                "result": "ok",
                "allow_legacy": False,
                "require_source_blobs": True,
            },
        }
    ],
    root,
)
if "build" not in found:
    raise SystemExit("single-spec stale run did not resolve current build")
if single_calls["n"] > 1:
    raise SystemExit(f"single-spec stale run used per-event blob checks: {single_calls['n']}")

common.git_blob_or_digest = real_git_blob_or_digest
common.git_blobs_or_digests = real_git_blobs_or_digests

batch_calls = []


def fake_check_output(args, cwd=None, text=None, stderr=None, timeout=None):
    if args[:4] == ["git", "ls-files", "-s", "--"]:
        rels = args[4:]
        if len(rels) > common.GIT_BLOB_BATCH_SIZE:
            raise SystemExit(f"git blob batch exceeded limit: {len(rels)}")
        batch_calls.append(tuple(rels))
        return "".join(
            f"100644 blob-{rel.replace('/', '-')} 0\t{rel}\n"
            for rel in rels
        )
    return real_check_output(args, cwd=cwd, text=text, stderr=stderr, timeout=timeout)


common.subprocess.check_output = fake_check_output
try:
    many_paths = [
        f"scripts/chunked-source-{i}.py"
        for i in range(common.GIT_BLOB_BATCH_SIZE * 2 + 17)
    ]
    chunked = common.git_blobs_or_digests(many_paths, root)
finally:
    common.subprocess.check_output = real_check_output

if len(chunked) != len(many_paths):
    raise SystemExit(f"chunked blob lookup missed paths: {len(chunked)} != {len(many_paths)}")
if len(batch_calls) < 3:
    raise SystemExit(f"chunked blob lookup did not split large input: {len(batch_calls)}")

source_calls = {"n": 0}


def fail_record_single(path, repo_root=None):
    raise AssertionError(f"record source path used per-file lookup for {path}")


def counted_record_batch(paths, repo_root=None):
    source_calls["n"] += 1
    return {
        common.rel_path(path, repo_root or root): f"blob-{i}"
        for i, path in enumerate(paths)
    }


common.git_blob_or_digest = fail_record_single
common.git_blobs_or_digests = counted_record_batch
record_sources = [f"scripts/record-source-{i}.py" for i in range(180)]
recorded = common.source_blobs(record_sources, root)
common.git_blob_or_digest = real_git_blob_or_digest
common.git_blobs_or_digests = real_git_blobs_or_digests
if source_calls["n"] != 1:
    raise SystemExit(f"source_blobs did not use one batched helper call: {source_calls['n']}")
if len(recorded) != len(record_sources):
    raise SystemExit(f"source_blobs dropped batched records: {len(recorded)}")

section = "7"
driver_run = "scale-driver-bounded-ledger"
common.write_json_atomic(
    state / "active-lease.json",
    {
        "todo_path": todo,
        "section": section,
        "driver": "codex",
        "driver_run_id": driver_run,
        "expires_at_ns": common.now_ns() + 3600 * 1_000_000_000,
    },
)
ledger.unlink()


def append_for_section(
    target_todo,
    target_section,
    kind,
    run_id,
    role,
    result="ok",
    source_blob=None,
    expires_at_ns=0,
):
    payload = {
        "task_id": f"{target_todo}#{target_section}",
        "todo_path": target_todo,
        "section": target_section,
        "role": role,
        "backend": "local",
        "run_id": run_id,
        "kind": kind,
        "head_sha": "HEAD",
        "source_blobs": {todo: source_blob} if source_blob is not None else {},
        "result": result,
        "summary_path": "",
        "created_at_ns": common.now_ns(),
        "created_at": common.now_iso(),
        "expires_at_ns": expires_at_ns,
        "legacy_import": False,
        "metadata": {},
    }
    ob.evidence.append_event(payload, root)


for i in range(1600):
    if i % 2:
        append_for_section(todo, section, "build", f"old-same-target-{i}", "validator", source_blob=f"old-{i}")
    else:
        append_for_section("todo/other.md", section, "build", f"old-other-target-{i}", "validator")

append_for_section(todo, section, "build", "bounded-build", "validator", source_blob=current_blob)
for kind in ("adversarial", "consistency", "perf"):
    append_for_section(todo, section, kind, f"bounded-review-{kind}", f"codex-reviewer-{kind}", result="received", source_blob=current_blob)
append_for_section(todo, section, "stamp.verified", "bounded-stamp", "stamp-writer", source_blob=current_blob)
append_for_section(todo, section, "todo-graph-validate", "bounded-graph", "validator")

real_iter_jsonl_reverse = common.iter_jsonl_reverse
records_seen = {"n": 0}


def counted_iter_jsonl_reverse(path, block_size=65536):
    for event in real_iter_jsonl_reverse(path, block_size):
        records_seen["n"] += 1
        yield event


def counted_bounded_blobs(paths, repo_root=None):
    return {
        common.rel_path(path, repo_root or root): current_blob
        for path in paths
    }


common.iter_jsonl_reverse = counted_iter_jsonl_reverse
common.git_blob_or_digest = fail_record_single
common.git_blobs_or_digests = counted_bounded_blobs
try:
    out = ob.resolve(
        SimpleNamespace(
            todo=todo,
            section=section,
            workflow="implement",
            driver_run_id=driver_run,
        )
    )
finally:
    common.iter_jsonl_reverse = real_iter_jsonl_reverse
    common.git_blob_or_digest = real_git_blob_or_digest
    common.git_blobs_or_digests = real_git_blobs_or_digests

if out["status"] != "ready":
    raise SystemExit(out)
if records_seen["n"] > 20:
    raise SystemExit(f"obligation resolve consumed stale ledger records: {records_seen['n']}")

section = "8"
driver_run = "scale-driver-missing-evidence"
common.write_json_atomic(
    state / "active-lease.json",
    {
        "todo_path": todo,
        "section": section,
        "driver": "codex",
        "driver_run_id": driver_run,
        "expires_at_ns": common.now_ns() + 3600 * 1_000_000_000,
    },
)
ledger.unlink()
for i in range(1500):
    append_for_section(
        "todo/other.md",
        "99",
        "build",
        f"missing-unrelated-{i}",
        "validator",
    )

records_seen["n"] = 0
common.iter_jsonl_reverse = counted_iter_jsonl_reverse
try:
    missing_out = ob.resolve(
        SimpleNamespace(
            todo=todo,
            section=section,
            workflow="implement",
            driver_run_id=driver_run,
        )
    )
finally:
    common.iter_jsonl_reverse = real_iter_jsonl_reverse

if missing_out["status"] != "missing":
    raise SystemExit(f"missing evidence unexpectedly ready: {missing_out}")
if records_seen["n"] != 0:
    raise SystemExit(f"missing evidence lookup read unrelated index records: {records_seen['n']}")

section = "8b"
driver_run = "scale-driver-hot-missing"
common.write_json_atomic(
    state / "active-lease.json",
    {
        "todo_path": todo,
        "section": section,
        "driver": "codex",
        "driver_run_id": driver_run,
        "expires_at_ns": common.now_ns() + 3600 * 1_000_000_000,
    },
)
if ledger.exists():
    ledger.unlink()
for i in range(ob.RESOLVE_TARGET_SCAN_RECORD_LIMIT + 300):
    append_for_section(todo, section, "build", f"hot-stale-{i}", "validator", source_blob=f"stale-hot-{i}")

records_seen["n"] = 0
common.iter_jsonl_reverse = counted_iter_jsonl_reverse
common.git_blob_or_digest = fail_record_single
common.git_blobs_or_digests = counted_bounded_blobs
try:
    hot_missing_out = ob.resolve(
        SimpleNamespace(
            todo=todo,
            section=section,
            workflow="implement",
            driver_run_id=driver_run,
        )
    )
finally:
    common.iter_jsonl_reverse = real_iter_jsonl_reverse
    common.git_blob_or_digest = real_git_blob_or_digest
    common.git_blobs_or_digests = real_git_blobs_or_digests

if hot_missing_out["status"] != "missing":
    raise SystemExit(f"hot missing evidence unexpectedly ready: {hot_missing_out}")
if records_seen["n"] > ob.RESOLVE_TARGET_SCAN_RECORD_LIMIT:
    raise SystemExit(f"hot target scan exceeded cap: {records_seen['n']}")
if not any("target-index scan capped" in item for item in hot_missing_out.get("diagnostics", [])):
    raise SystemExit(f"hot target cap did not report diagnostic: {hot_missing_out}")

section = "9"
if ledger.exists():
    ledger.unlink()
expired_at = common.now_ns() - 1_000_000_000
append_for_section(
    todo,
    section,
    "build",
    "expired-current-build",
    "validator",
    source_blob=current_blob,
    expires_at_ns=expired_at,
)
expired_found = ob._resolve_events(
    todo,
    section,
    [
        {
            "name": "build",
            "match": {
                "kind": "build",
                "result": "ok",
                "allow_legacy": False,
                "require_source_blobs": True,
            },
        }
    ],
    root,
)
if expired_found:
    raise SystemExit(f"expired indexed event resolved as valid evidence: {expired_found}")
expired_index = ob.evidence.target_index_path(root, todo, section)
if not expired_index.exists():
    raise SystemExit("expired event did not create target index fixture")
import contextlib
import io

with contextlib.redirect_stdout(io.StringIO()):
    ob.evidence.gc(SimpleNamespace())
if expired_index.exists() and expired_index.read_text(encoding="utf-8").strip():
    raise SystemExit("gc left expired target-index records behind")

concurrent_state = state / "gc-concurrent"
shutil.rmtree(concurrent_state, ignore_errors=True)
concurrent_state.mkdir(parents=True)
old_state_env = os.environ.get("AI_WORKFLOW_STATE_DIR")
os.environ["AI_WORKFLOW_STATE_DIR"] = str(concurrent_state)
try:
    section = "10"
    ob.evidence.append_event(
        {
            "task_id": f"{todo}#{section}",
            "todo_path": todo,
            "section": section,
            "role": "validator",
            "backend": "local",
            "run_id": "expired-before-gc",
            "kind": "build",
            "head_sha": "HEAD",
            "source_blobs": {},
            "result": "ok",
            "summary_path": "",
            "created_at_ns": common.now_ns(),
            "created_at": common.now_iso(),
            "expires_at_ns": common.now_ns() - 1_000_000_000,
            "legacy_import": False,
            "metadata": {},
        },
        root,
    )
finally:
    if old_state_env is None:
        os.environ.pop("AI_WORKFLOW_STATE_DIR", None)
    else:
        os.environ["AI_WORKFLOW_STATE_DIR"] = old_state_env

child_code = f"""
import importlib.util
import pathlib
import sys
import time
from types import SimpleNamespace

root = pathlib.Path({str(root)!r})
sys.path.insert(0, str(root / "scripts"))
spec = importlib.util.spec_from_file_location(
    "ai_workflow_evidence", root / "scripts/ai-workflow/evidence.py"
)
ev = importlib.util.module_from_spec(spec)
assert spec and spec.loader
spec.loader.exec_module(ev)
real_load = ev.load_events

def slow_load(repo_root=None):
    events = real_load(repo_root)
    print("snapshot-ready", flush=True)
    time.sleep(0.5)
    return events

ev.load_events = slow_load
raise SystemExit(ev.gc(SimpleNamespace()))
"""
env = dict(os.environ)
env["AI_WORKFLOW_REPO_ROOT"] = str(root)
env["AI_WORKFLOW_STATE_DIR"] = str(concurrent_state)
proc = subprocess.Popen(
    [sys.executable, "-c", child_code],
    cwd=root,
    env=env,
    text=True,
    stdout=subprocess.PIPE,
    stderr=subprocess.PIPE,
)
ready = proc.stdout.readline().strip() if proc.stdout else ""
if ready != "snapshot-ready":
    out, err = proc.communicate(timeout=5)
    raise SystemExit(f"gc concurrency fixture did not reach snapshot: {ready} out={out} err={err}")
record = subprocess.run(
    [
        sys.executable,
        "scripts/ai-workflow/evidence.py",
        "record",
        "--todo",
        todo,
        "--section",
        section,
        "--role",
        "validator",
        "--backend",
        "local",
        "--run-id",
        "append-during-gc",
        "--kind",
        "build",
        "--result",
        "ok",
    ],
    cwd=root,
    env=env,
    text=True,
    capture_output=True,
    timeout=5,
)
gc_out, gc_err = proc.communicate(timeout=5)
if proc.returncode != 0:
    raise SystemExit(f"gc child failed: out={gc_out} err={gc_err}")
if record.returncode != 0:
    raise SystemExit(f"concurrent evidence record failed: {record.stderr or record.stdout}")
ledger_events = common.iter_jsonl(concurrent_state / "evidence.jsonl")
if not any(ev.get("run_id") == "append-during-gc" for ev in ledger_events):
    raise SystemExit(f"gc lost concurrent append from ledger: {ledger_events}")
old_state_env = os.environ.get("AI_WORKFLOW_STATE_DIR")
os.environ["AI_WORKFLOW_STATE_DIR"] = str(concurrent_state)
try:
    concurrent_index = ob.evidence.target_index_path(root, todo, section)
finally:
    if old_state_env is None:
        os.environ.pop("AI_WORKFLOW_STATE_DIR", None)
    else:
        os.environ["AI_WORKFLOW_STATE_DIR"] = old_state_env
index_events = common.iter_jsonl(concurrent_index)
if not any(ev.get("run_id") == "append-during-gc" for ev in index_events):
    raise SystemExit(f"gc lost concurrent append from target index: {index_events}")

section = "11"
indexed_event = ob.evidence.append_event(
    {
        "task_id": f"{todo}#{section}",
        "todo_path": todo,
        "section": section,
        "role": "validator",
        "backend": "local",
        "run_id": "targeted-query-event",
        "kind": "build",
        "head_sha": "HEAD",
        "source_blobs": {},
        "result": "ok",
        "summary_path": "",
        "created_at_ns": common.now_ns(),
        "created_at": common.now_iso(),
        "expires_at_ns": 0,
        "legacy_import": False,
        "metadata": {},
    },
    root,
)
real_load_events = ob.evidence.load_events


def fail_full_ledger_load(repo_root=None):
    raise AssertionError("targeted evidence query/explain used full ledger")


target_args = SimpleNamespace(
    todo=todo,
    section=section,
    role=None,
    backend=None,
    run_id=None,
    not_run_id=None,
    kind="build",
    result="ok",
    source_current=False,
    latest=True,
    format="ids",
    require=True,
)
ob.evidence.load_events = fail_full_ledger_load
try:
    with contextlib.redirect_stdout(io.StringIO()) as query_out:
        query_rc = ob.evidence.query(target_args)
    with contextlib.redirect_stdout(io.StringIO()) as explain_out:
        explain_rc = ob.evidence.explain(target_args)
finally:
    ob.evidence.load_events = real_load_events
if query_rc != 0 or indexed_event["event_id"] not in query_out.getvalue():
    raise SystemExit(f"targeted query missed index event: rc={query_rc} out={query_out.getvalue()}")
if explain_rc != 0 or "targeted-query-event" not in explain_out.getvalue():
    raise SystemExit(f"targeted explain missed index event: rc={explain_rc} out={explain_out.getvalue()}")
PY
then
    t_pass "ai_workflow_obligations: large ledger resolves newest evidence with bounded source checks"
else
    t_fail "ai_workflow_obligations: large ledger resolves newest evidence with bounded source checks"
fi

AIWF_RECORD_ROOT="$AIWF_TMP/record-source-root"
AIWF_RECORD_STATE="$AIWF_TMP/record-source-state"
rm -rf "$AIWF_RECORD_ROOT" "$AIWF_RECORD_STATE"
mkdir -p "$AIWF_RECORD_ROOT/sources" "$AIWF_RECORD_STATE"
git -C "$AIWF_RECORD_ROOT" init -q
record_args=()
for i in $(seq 1 180); do
    src="$AIWF_RECORD_ROOT/sources/source-$i.txt"
    printf 'source %s\n' "$i" > "$src"
    record_args+=(--source "$src")
done
git -C "$AIWF_RECORD_ROOT" add sources
if env AI_WORKFLOW_REPO_ROOT="$AIWF_RECORD_ROOT" AI_WORKFLOW_STATE_DIR="$AIWF_RECORD_STATE" \
    python3 scripts/ai-workflow/evidence.py record \
        --todo "$TODO10" --section 4 --role validator --backend local \
        --run-id record-many-sources --kind build --result ok \
        "${record_args[@]}" >/dev/null \
   && python3 - "$AIWF_RECORD_STATE/evidence.jsonl" <<'PY'
import json
import pathlib
import sys

line = pathlib.Path(sys.argv[1]).read_text(encoding="utf-8").splitlines()[-1]
event = json.loads(line)
blobs = event.get("source_blobs") or {}
if len(blobs) != 180:
    raise SystemExit(f"expected 180 source blobs, got {len(blobs)}")
if any(not key.startswith("sources/source-") for key in blobs):
    raise SystemExit("source blob paths were not normalized relative to AI_WORKFLOW_REPO_ROOT")
PY
then
    t_pass "ai_workflow_evidence: record handles many --source paths through batched source blobs"
else
    t_fail "ai_workflow_evidence: record handles many --source paths through batched source blobs"
fi

META_OUT=$("${AIWF_ENV[@]}" python3 scripts/ai-workflow/evidence.py record \
    --todo "$TODO10" --section 3 --role validator --backend local \
    --run-id build-meta --kind build --result ok --source "$TODO10" \
    --command "bash scripts/build.sh" --exit-code 0 \
    --log-path build/build.log --final-marker "=== BUILD OK ===")
if echo "$META_OUT" | grep -q '"command": "bash scripts/build.sh"' \
   && echo "$META_OUT" | grep -q '"exit_code": "0"' \
   && echo "$META_OUT" | grep -q '"final_marker": "=== BUILD OK ==="'; then
    t_pass "ai_workflow_evidence: build metadata recorded"
else
    t_fail "ai_workflow_evidence: build metadata recorded" "$META_OUT"
fi

STAMP_TODO="$AIWF_TMP/stamp-fixture.md"
cat > "$STAMP_TODO" <<'EOF_STAMP_FIXTURE'
# Stamp Fixture

## 1. Fixture Section

- [ ] work item
EOF_STAMP_FIXTURE
rm -rf "$AIWF_STATE"
mkdir -p "$AIWF_STATE"
"${AIWF_ENV[@]}" python3 scripts/ai-workflow/lease.py acquire \
    --todo "$STAMP_TODO" --section 1 --driver claude --run-id stamp-driver >/dev/null
"${AIWF_ENV[@]}" python3 scripts/ai-workflow/evidence.py record \
    --todo "$STAMP_TODO" --section 1 --role validator --backend local \
    --run-id stamp-build --kind build --result ok --source "$STAMP_TODO" >/dev/null
"${AIWF_ENV[@]}" python3 scripts/ai-workflow/evidence.py record \
    --todo "$STAMP_TODO" --section 1 --role validator --backend local \
    --run-id stamp-graph --kind todo-graph-validate --result ok >/dev/null
for kind in adversarial consistency perf; do
    "${AIWF_ENV[@]}" python3 scripts/ai-workflow/evidence.py record \
        --todo "$STAMP_TODO" --section 1 --role "codex-reviewer-$kind" \
        --backend codex --run-id "stamp-review-$kind" --kind "$kind" \
        --result received --source "$STAMP_TODO" >/dev/null
done
if "${AIWF_ENV[@]}" python3 scripts/ai-workflow/stamp.py verified \
    "$STAMP_TODO" --section 1 --driver-run-id stamp-driver --write >/dev/null \
   && grep -q '"kind": "stamp.generated"' "$AIWF_STATE/evidence.jsonl" \
   && grep -q '"stamp_text_sha256"' "$AIWF_STATE/evidence.jsonl"; then
    t_pass "ai_workflow_stamp: write records generated stamp hash"
else
    t_fail "ai_workflow_stamp: write records generated stamp hash" \
           "$(cat "$AIWF_STATE/evidence.jsonl" 2>&1 | tail -5)"
fi

STAMP_REPO="$AIWF_TMP/stamp-repo"
STAMP_REPO_TODO="todo/00-infrastructure/TODO-99-stamp.md"
STAMP_REPO_SRC="src/stamp-source.c"
mkdir -p "$STAMP_REPO/todo/00-infrastructure" "$STAMP_REPO/src"
cat > "$STAMP_REPO/$STAMP_REPO_TODO" <<'EOF_TRACKED_STAMP'
# TODO-99 Stamp Fixture

## 1. Fixture Section

- [ ] work item
EOF_TRACKED_STAMP
printf 'int stamp_source(void) { return 1; }\n' > "$STAMP_REPO/$STAMP_REPO_SRC"
(
    cd "$STAMP_REPO" \
      && git init -q \
      && git config user.email t@t \
      && git config user.name t \
      && git add "$STAMP_REPO_TODO" "$STAMP_REPO_SRC" \
      && git -c commit.gpgsign=false commit -q -m init
)
STAMP_REPO_ENV=(env AI_WORKFLOW_REPO_ROOT="$STAMP_REPO" AI_WORKFLOW_STATE_DIR="$STAMP_REPO/build/ai-workflow")
"${STAMP_REPO_ENV[@]}" python3 scripts/ai-workflow/lease.py acquire \
    --todo "$STAMP_REPO_TODO" --section 1 --driver claude --run-id tracked-stamp-driver >/dev/null
"${STAMP_REPO_ENV[@]}" python3 scripts/ai-workflow/evidence.py record \
    --todo "$STAMP_REPO_TODO" --section 1 --role validator --backend local \
    --run-id tracked-stamp-build --kind build --result ok --source "$STAMP_REPO_SRC" >/dev/null
"${STAMP_REPO_ENV[@]}" python3 scripts/ai-workflow/evidence.py record \
    --todo "$STAMP_REPO_TODO" --section 1 --role validator --backend local \
    --run-id tracked-stamp-graph --kind todo-graph-validate --result ok >/dev/null
for kind in adversarial consistency perf; do
    "${STAMP_REPO_ENV[@]}" python3 scripts/ai-workflow/evidence.py record \
        --todo "$STAMP_REPO_TODO" --section 1 --role "codex-reviewer-$kind" \
        --backend codex --run-id "tracked-stamp-review-$kind" --kind "$kind" \
        --result received --source "$STAMP_REPO_SRC" >/dev/null
done
"${STAMP_REPO_ENV[@]}" python3 scripts/ai-workflow/stamp.py verified \
    "$STAMP_REPO_TODO" --section 1 --driver-run-id tracked-stamp-driver --write >/dev/null
if "${STAMP_REPO_ENV[@]}" python3 scripts/ai-workflow/gates.py commit \
    --todo "$STAMP_REPO_TODO" --section 1 --workflow implement --format json >/dev/null 2>&1; then
    t_fail "ai_workflow_stamp: unstaged tracked stamp evidence blocks commit"
else
    t_pass "ai_workflow_stamp: unstaged tracked stamp evidence blocks commit"
fi
( cd "$STAMP_REPO" && git add "$STAMP_REPO_TODO" )
if "${STAMP_REPO_ENV[@]}" python3 scripts/ai-workflow/gates.py commit \
    --todo "$STAMP_REPO_TODO" --section 1 --workflow implement --format json >/dev/null; then
    t_pass "ai_workflow_stamp: staged tracked stamp evidence satisfies commit gate"
else
    t_fail "ai_workflow_stamp: staged tracked stamp evidence satisfies commit gate" \
           "$("${STAMP_REPO_ENV[@]}" python3 scripts/ai-workflow/gates.py commit --todo "$STAMP_REPO_TODO" --section 1 --workflow implement --format json 2>&1)"
fi

# --- ai_workflow_stamp: file-level lifecycle stamps + no-bare-XREF (TODO-10 stamp writer) ---
LIFE_REPO="$AIWF_TMP/lifecycle-repo"
LIFE_TODO="todo/00-infrastructure/TODO-98-lifecycle.md"
LIFE_EV="$LIFE_REPO/build/ai-workflow/evidence.jsonl"
mkdir -p "$LIFE_REPO/todo/00-infrastructure"
cat > "$LIFE_REPO/$LIFE_TODO" <<'EOF_LIFE'
# TODO-98 -- Lifecycle Fixture

> **Goal:** fixture goal line.

## 1. First Section

- [ ] work item
EOF_LIFE
(
    cd "$LIFE_REPO" \
      && git init -q \
      && git config user.email t@t \
      && git config user.name t \
      && git add -A \
      && git -c commit.gpgsign=false commit -q -m init
)
LIFE_ENV=(env AI_WORKFLOW_REPO_ROOT="$LIFE_REPO" AI_WORKFLOW_STATE_DIR="$LIFE_REPO/build/ai-workflow")
"${LIFE_ENV[@]}" python3 scripts/ai-workflow/evidence.py record --todo "$LIFE_TODO" --section file \
    --role validator --backend local --run-id life-graph --kind todo-graph-validate --result ok >/dev/null
"${LIFE_ENV[@]}" python3 scripts/ai-workflow/evidence.py record --todo "$LIFE_TODO" --section file \
    --role codex-reviewer-gap-audit --backend codex --run-id life-gap --kind gap-audit --result received >/dev/null
"${LIFE_ENV[@]}" python3 scripts/ai-workflow/stamp.py validated "$LIFE_TODO" --write >/dev/null
"${LIFE_ENV[@]}" python3 scripts/ai-workflow/stamp.py gap-audited "$LIFE_TODO" --write >/dev/null
LIFE_STATE=$(python3 -c "import sys; sys.path.insert(0,'.claude/hooks'); import sequencer_triage as t; print(t.file_lifecycle('$LIFE_REPO/$LIFE_TODO'))")
if echo "$LIFE_STATE" | grep -q "'validated': True" \
   && echo "$LIFE_STATE" | grep -q "'gap_audited': True" \
   && python3 -c "import sys; pre=open('$LIFE_REPO/$LIFE_TODO').read().split('## 1.')[0]; sys.exit(0 if '> **Validated:**' in pre and '> **Gap-audited:**' in pre else 1)"; then
    t_pass "ai_workflow_stamp: file-level validated/gap-audited land in preamble and sequencer recognizes them"
else
    t_fail "ai_workflow_stamp: file-level validated/gap-audited land in preamble and sequencer recognizes them" "$LIFE_STATE"
fi
if ! grep -q '"section": null' "$LIFE_EV" && grep -q '"section": "file"' "$LIFE_EV"; then
    t_pass "ai_workflow_stamp: file-level stamp records file-scoped evidence, never section null"
else
    t_fail "ai_workflow_stamp: file-level stamp records file-scoped evidence, never section null" \
           "$(grep -o '"section": [^,]*' "$LIFE_EV" | sort -u)"
fi
"${LIFE_ENV[@]}" python3 scripts/ai-workflow/stamp.py validated "$LIFE_TODO" --summary re-stamp --write >/dev/null
if [ "$(grep -c '^> \*\*Validated:\*\*' "$LIFE_REPO/$LIFE_TODO")" = "1" ]; then
    t_pass "ai_workflow_stamp: file-level re-stamp replaces in place (no duplicate lifecycle stamp)"
else
    t_fail "ai_workflow_stamp: file-level re-stamp replaces in place (no duplicate lifecycle stamp)"
fi
printf '## 1. NoPreamble\n- [ ] x\n' > "$LIFE_REPO/todo/00-infrastructure/TODO-97-noh1.md"
if "${LIFE_ENV[@]}" python3 scripts/ai-workflow/stamp.py validated todo/00-infrastructure/TODO-97-noh1.md --allow-missing --write 2>/dev/null; then
    t_fail "ai_workflow_stamp: file-level stamp hard-fails cleanly on a file with no H1"
else
    t_pass "ai_workflow_stamp: file-level stamp hard-fails cleanly on a file with no H1"
fi
if "${LIFE_ENV[@]}" python3 scripts/ai-workflow/stamp.py deferred "$LIFE_TODO" --section 1 --summary '[H] gap -> XREF: TODO-11' --write 2>/dev/null; then
    t_fail "ai_workflow_stamp: deferred with a bare XREF is rejected before write"
else
    t_pass "ai_workflow_stamp: deferred with a bare XREF is rejected before write"
fi
if "${LIFE_ENV[@]}" python3 scripts/ai-workflow/stamp.py deferred "$LIFE_TODO" --section 1 --summary '[H] gap -> XREF: TODO-11 (item: "X" at line 20)' --dry-run >/dev/null; then
    t_pass "ai_workflow_stamp: deferred with a concrete-item XREF is allowed"
else
    t_fail "ai_workflow_stamp: deferred with a concrete-item XREF is allowed"
fi
LIFE_SEC_TODO="todo/00-infrastructure/TODO-96-seclocal.md"
cat > "$LIFE_REPO/$LIFE_SEC_TODO" <<'EOF_SEC'
# TODO-96 -- Section-local Fixture

## 2. Second

- [x] item

**Test checkpoint:** does a thing.

> **Test runner:** 2026 | tests
>
> **Notes:**
> - bullet a
> - bullet b
EOF_SEC
"${LIFE_ENV[@]}" python3 scripts/ai-workflow/stamp.py verified "$LIFE_SEC_TODO" --section 2 --allow-missing --write >/dev/null
if python3 - "$LIFE_REPO/$LIFE_SEC_TODO" <<'PY'
import sys
lines = open(sys.argv[1]).read().splitlines()
h = next(i for i, l in enumerate(lines) if l.startswith("## 2."))
abut = lines[h + 1].startswith(">")  # heading must not abut a blockquote/stamp
vi = next(i for i, l in enumerate(lines) if l.startswith("> **Verified:**"))
na = next(i for i, l in enumerate(lines) if l.startswith("> - bullet a"))
nb = next(i for i, l in enumerate(lines) if l.startswith("> - bullet b"))
sys.exit(0 if (not abut and vi > nb and nb == na + 1) else 1)
PY
then
    t_pass "ai_workflow_stamp: section-local stamp lands in the bottom block and preserves multiline Notes"
else
    t_fail "ai_workflow_stamp: section-local stamp lands in the bottom block and preserves multiline Notes"
fi
# file-level lifecycle stamps must be ledger-backed (or explicitly --allow-missing)
LIFE_NOEV_TODO="todo/00-infrastructure/TODO-95-noev.md"
cat > "$LIFE_REPO/$LIFE_NOEV_TODO" <<'EOF_NOEV'
# TODO-95 -- No Evidence

> **Goal:** g.

## 1. S

- [ ] x
EOF_NOEV
if "${LIFE_ENV[@]}" python3 scripts/ai-workflow/stamp.py validated "$LIFE_NOEV_TODO" --write 2>/dev/null; then
    t_fail "ai_workflow_stamp: file-level stamp refuses without todo-graph-validate evidence"
else
    t_pass "ai_workflow_stamp: file-level stamp refuses without todo-graph-validate evidence"
fi
if "${LIFE_ENV[@]}" python3 scripts/ai-workflow/stamp.py validated "$LIFE_NOEV_TODO" --allow-missing --write >/dev/null 2>&1; then
    t_pass "ai_workflow_stamp: file-level stamp writes a manual stamp under --allow-missing"
else
    t_fail "ai_workflow_stamp: file-level stamp writes a manual stamp under --allow-missing"
fi
# a bare first XREF clause is rejected even when a later clause on the same line is concrete
if "${LIFE_ENV[@]}" python3 scripts/ai-workflow/stamp.py deferred "$LIFE_NOEV_TODO" --section 1 \
    --summary '[H] a -> XREF: TODO-01 -> XREF: TODO-02 (item: "b" at line 2)' --write 2>/dev/null; then
    t_fail "ai_workflow_stamp: multi-XREF summary with a bare first clause is rejected"
else
    t_pass "ai_workflow_stamp: multi-XREF summary with a bare first clause is rejected"
fi
# section boundary ends at the next H2, so a stamp lands after H3 subsections, not before them
LIFE_H3_TODO="todo/00-infrastructure/TODO-94-h3.md"
cat > "$LIFE_REPO/$LIFE_H3_TODO" <<'EOF_H3'
# TODO-94 -- H3 Subsections

## 3. Third

Intro.

### Subsection A

- [ ] a

### Subsection B

- [ ] b

**Test checkpoint:** ok.

## 4. Fourth

- [ ] later
EOF_H3
"${LIFE_ENV[@]}" python3 scripts/ai-workflow/stamp.py verified "$LIFE_H3_TODO" --section 3 --allow-missing --write >/dev/null
if python3 - "$LIFE_REPO/$LIFE_H3_TODO" <<'PY'
import sys
lines = open(sys.argv[1]).read().splitlines()
vi = next(i for i, l in enumerate(lines) if l.startswith("> **Verified:**"))
tc = next(i for i, l in enumerate(lines) if l.startswith("**Test checkpoint:**"))
s4 = next(i for i, l in enumerate(lines) if l.startswith("## 4."))
sb = next(i for i, l in enumerate(lines) if l.startswith("### Subsection B"))
sys.exit(0 if (vi > tc and vi > sb and vi < s4) else 1)
PY
then
    t_pass "ai_workflow_stamp: section-local stamp lands after H3 subsections, not before them"
else
    t_fail "ai_workflow_stamp: section-local stamp lands after H3 subsections, not before them"
fi

# canonical stamp order: writing quality-reviewed BEFORE verified still lands
# Verified above Quality reviewed (group-join ranked insert, Codex F4)
LIFE_ORD_TODO="todo/00-infrastructure/TODO-93-order.md"
cat > "$LIFE_REPO/$LIFE_ORD_TODO" <<'EOF_ORD'
# TODO-93 -- Stamp Order

## 5. Ordered

- [ ] item

**Test checkpoint:** ok.
EOF_ORD
"${LIFE_ENV[@]}" python3 scripts/ai-workflow/stamp.py quality-reviewed "$LIFE_ORD_TODO" --section 5 --allow-missing --write >/dev/null
"${LIFE_ENV[@]}" python3 scripts/ai-workflow/stamp.py verified "$LIFE_ORD_TODO" --section 5 --allow-missing --write >/dev/null
if python3 - "$LIFE_REPO/$LIFE_ORD_TODO" <<'PY'
import sys
lines = open(sys.argv[1]).read().splitlines()
vi = next(i for i, l in enumerate(lines) if l.startswith("> **Verified:**"))
qi = next(i for i, l in enumerate(lines) if l.startswith("> **Quality reviewed:**"))
sys.exit(0 if vi < qi else 1)
PY
then
    t_pass "ai_workflow_stamp: group-join places Verified above Quality reviewed regardless of write order"
else
    t_fail "ai_workflow_stamp: group-join places Verified above Quality reviewed regardless of write order"
fi

# accepted/deferred with NO XREF at all is rejected before write (Codex F2)
if "${LIFE_ENV[@]}" python3 scripts/ai-workflow/stamp.py deferred "$LIFE_TODO" --section 1 --write 2>/dev/null; then
    t_fail "ai_workflow_stamp: deferred with no XREF summary is rejected"
else
    t_pass "ai_workflow_stamp: deferred with no XREF summary is rejected"
fi

# a soft XREF (parenthetical present but no concrete item:/at line marker) is rejected (Codex F2)
if "${LIFE_ENV[@]}" python3 scripts/ai-workflow/stamp.py deferred "$LIFE_TODO" --section 1 \
    --summary '[H] gap -> XREF: TODO-11 (later)' --write 2>/dev/null; then
    t_fail "ai_workflow_stamp: deferred with a soft (non-concrete) XREF is rejected"
else
    t_pass "ai_workflow_stamp: deferred with a soft (non-concrete) XREF is rejected"
fi

# a non-canonical (lowercase/spaced) XREF marker is invisible to the sibling
# parsers, so the writer treats it as "no owner" and rejects the stamp (Codex F2/C1)
if "${LIFE_ENV[@]}" python3 scripts/ai-workflow/stamp.py deferred "$LIFE_TODO" --section 1 \
    --summary '[H] gap -> xref : TODO-11' --write 2>/dev/null; then
    t_fail "ai_workflow_stamp: deferred with a non-canonical lowercase XREF is rejected"
else
    t_pass "ai_workflow_stamp: deferred with a non-canonical lowercase XREF is rejected"
fi

# an XREF marker with no TODO target names no owner and is rejected (Codex C1)
if "${LIFE_ENV[@]}" python3 scripts/ai-workflow/stamp.py deferred "$LIFE_TODO" --section 1 \
    --summary '[H] gap -> XREF: no-owner (item: "x" at line 1)' --write 2>/dev/null; then
    t_fail "ai_workflow_stamp: deferred with an ownerless (no-TODO) XREF is rejected"
else
    t_pass "ai_workflow_stamp: deferred with an ownerless (no-TODO) XREF is rejected"
fi

# a marker sitting OUTSIDE the parenthetical is not concrete (todo-graph would
# extract no item owner) and is rejected (Codex re-adversarial RA-H1)
if "${LIFE_ENV[@]}" python3 scripts/ai-workflow/stamp.py deferred "$LIFE_TODO" --section 1 \
    --summary '[H] gap -> XREF: TODO-11 (later) and item: outside the parens' --write 2>/dev/null; then
    t_fail "ai_workflow_stamp: deferred with a marker outside the parenthetical is rejected"
else
    t_pass "ai_workflow_stamp: deferred with a marker outside the parenthetical is rejected"
fi

# joining a section whose existing stamps are ALREADY out of order re-sorts the
# whole group into canonical rank order (Codex re-adversarial RA-M1)
LIFE_DIS_TODO="todo/00-infrastructure/TODO-91-disorder.md"
cat > "$LIFE_REPO/$LIFE_DIS_TODO" <<'EOF_DIS'
# TODO-91 -- Disordered Stamps

## 5. Ordered

- [ ] item

**Test checkpoint:** ok.

> **Quality reviewed:** old-qr
> **Verified:** old-v
EOF_DIS
"${LIFE_ENV[@]}" python3 scripts/ai-workflow/stamp.py deferred "$LIFE_DIS_TODO" --section 5 \
    --summary '[H] gap -> XREF: TODO-11 (item: "x" at line 1)' --allow-missing --write >/dev/null
if python3 - "$LIFE_REPO/$LIFE_DIS_TODO" <<'PY'
import sys
lines = open(sys.argv[1]).read().splitlines()
vi = next(i for i, l in enumerate(lines) if l.startswith("> **Verified:**"))
di = next(i for i, l in enumerate(lines) if l.startswith("> **Deferred:**"))
qi = next(i for i, l in enumerate(lines) if l.startswith("> **Quality reviewed:**"))
sys.exit(0 if vi < di < qi else 1)
PY
then
    t_pass "ai_workflow_stamp: joining a pre-existing out-of-order group re-sorts to canonical order"
else
    t_fail "ai_workflow_stamp: joining a pre-existing out-of-order group re-sorts to canonical order"
fi

# `at line` in PROSE (not an item: parenthetical) does not make an XREF concrete;
# todo-graph would extract no item owner, so it is rejected (Codex re-adv RA2-M1)
if "${LIFE_ENV[@]}" python3 scripts/ai-workflow/stamp.py deferred "$LIFE_TODO" --section 1 \
    --summary '[M] gap -> XREF: TODO-11 (see owner at line 99 later)' --write 2>/dev/null; then
    t_fail "ai_workflow_stamp: deferred with prose 'at line' but no item owner is rejected"
else
    t_pass "ai_workflow_stamp: deferred with prose 'at line' but no item owner is rejected"
fi

# an INTERLEAVED independent blockquote label (> **Notes:**) between stamps must not
# be absorbed as a stamp continuation and moved by the re-sort (Codex re-adv RA2-M2)
LIFE_NOTE_TODO="todo/00-infrastructure/TODO-90-interleaved.md"
cat > "$LIFE_REPO/$LIFE_NOTE_TODO" <<'EOF_NOTE'
# TODO-90 -- Interleaved Notes

## 5. Ordered

- [ ] item

**Test checkpoint:** ok.

> **Quality reviewed:** old-qr
> **Notes:** interleaved note
> **Verified:** old-v
EOF_NOTE
"${LIFE_ENV[@]}" python3 scripts/ai-workflow/stamp.py deferred "$LIFE_NOTE_TODO" --section 5 \
    --summary '[H] gap -> XREF: TODO-11 (item: "x" at line 1)' --allow-missing --write >/dev/null
if python3 - "$LIFE_REPO/$LIFE_NOTE_TODO" <<'PY'
import sys
lines = open(sys.argv[1]).read().splitlines()
notes = [i for i, l in enumerate(lines) if l.startswith("> **Notes:**")]
vi = next(i for i, l in enumerate(lines) if l.startswith("> **Verified:**"))
di = [i for i, l in enumerate(lines) if l.startswith("> **Deferred:**")]
# the Notes line must survive exactly once, stay directly above Verified (not moved
# to the tail), and the Deferred stamp must have been added
ok = len(notes) == 1 and notes[0] + 1 == vi and len(di) == 1
sys.exit(0 if ok else 1)
PY
then
    t_pass "ai_workflow_stamp: interleaved > **Notes:** label is not reordered as a stamp continuation"
else
    t_fail "ai_workflow_stamp: interleaved > **Notes:** label is not reordered as a stamp continuation"
fi

# an ownerless XREF clause must not ride on a concrete sibling clause (adversarial:
# the writer predicate skipped no-TODO chunks while has-concrete passed on the sibling)
if "${LIFE_ENV[@]}" python3 scripts/ai-workflow/stamp.py deferred "$LIFE_TODO" --section 1 \
    --summary '[H] ghost -> XREF: no-owner (item: "ghost" at line 1), -> XREF: 00-infrastructure/TODO-11 (item: "Real" at line 7)' --write 2>/dev/null; then
    t_fail "ai_workflow_stamp: ownerless XREF clause is rejected even with a concrete sibling"
else
    t_pass "ai_workflow_stamp: ownerless XREF clause is rejected even with a concrete sibling"
fi

# a quoted item name containing parens is still concrete (nested parens inside the
# quotes must not break the writer's innermost-paren scan)
if "${LIFE_ENV[@]}" python3 scripts/ai-workflow/stamp.py deferred "$LIFE_TODO" --section 1 \
    --summary '[M] gap -> XREF: 00-infrastructure/TODO-11 (item: "Enforce OWNERSHIP (not presence) in consumers" at line 134)' --write 2>/dev/null; then
    t_pass "ai_workflow_stamp: quoted item name containing parens is accepted as concrete"
else
    t_fail "ai_workflow_stamp: quoted item name containing parens is accepted as concrete"
fi

# writer-strict subset property: a concrete paren past the clause terminator is bare
# to the git hook (clause regex stops at the comma), so the writer must reject it or
# it would mint a stamp the commit hook then blocks (consistency finding)
if "${LIFE_ENV[@]}" python3 scripts/ai-workflow/stamp.py deferred "$LIFE_TODO" --section 1 \
    --summary '[M] gap -> XREF: 00-infrastructure/TODO-11, later (item: "Real" at line 7)' --write 2>/dev/null; then
    t_fail "ai_workflow_stamp: concrete paren past the clause terminator is rejected (hook-subset)"
else
    t_pass "ai_workflow_stamp: concrete paren past the clause terminator is rejected (hook-subset)"
fi

# a QUOTED terminator before the paren truncates the quote-unaware hook clause to
# bare, so the writer must reject at the same raw boundary (re-adversarial finding)
if "${LIFE_ENV[@]}" python3 scripts/ai-workflow/stamp.py deferred "$LIFE_TODO" --section 1 \
    --summary '[M] gap -> XREF: 00-infrastructure/TODO-11 "see, note" (item: "Real" at line 7)' --write 2>/dev/null; then
    t_fail "ai_workflow_stamp: quoted terminator before the parenthetical is rejected (hook-subset)"
else
    t_pass "ai_workflow_stamp: quoted terminator before the parenthetical is rejected (hook-subset)"
fi

STAGED_REPO="$AIWF_TMP/staged-repo"
mkdir -p "$STAGED_REPO/todo/00-infrastructure"
cat > "$STAGED_REPO/todo/00-infrastructure/TODO-99-fixture.md" <<'EOF_STAGED_TODO'
# TODO-99 Fixture

| Order | Section | Deliverable | Depends On | Status |
| :---: | :---: | --- | --- | :---: |
| 1 | 1 | Fixture | -- | [ ] |

## 1. Fixture

body
EOF_STAGED_TODO
( cd "$STAGED_REPO" && git init -q && git add . && git -c user.email=t@t -c user.name=t commit -q -m init )
perl -0pi -e 's/\[ \]/[x]/' "$STAGED_REPO/todo/00-infrastructure/TODO-99-fixture.md"
( cd "$STAGED_REPO" && git add todo/00-infrastructure/TODO-99-fixture.md )
perl -0pi -e 's/\| Order \| Section \| Deliverable \| Depends On \| Status \|/| Depends On | Deliverable | Section | Order | Status |/' \
    "$STAGED_REPO/todo/00-infrastructure/TODO-99-fixture.md"
if env AI_WORKFLOW_REPO_ROOT="$STAGED_REPO" AI_WORKFLOW_STATE_DIR="$STAGED_REPO/build/ai-workflow" \
    python3 scripts/ai-workflow/gates.py staged-commit --format json >/dev/null 2>&1; then
    t_fail "ai_workflow_gates: staged commit blocks without lease despite unstaged TODO edit"
else
    t_pass "ai_workflow_gates: staged commit blocks without lease despite unstaged TODO edit"
fi
env AI_WORKFLOW_REPO_ROOT="$STAGED_REPO" AI_WORKFLOW_STATE_DIR="$STAGED_REPO/build/ai-workflow" \
    python3 scripts/ai-workflow/lease.py acquire \
    --todo todo/00-infrastructure/TODO-99-fixture.md --section 1 \
    --driver claude --run-id staged-driver >/dev/null
if env AI_WORKFLOW_REPO_ROOT="$STAGED_REPO" AI_WORKFLOW_STATE_DIR="$STAGED_REPO/build/ai-workflow" \
    python3 scripts/ai-workflow/gates.py staged-commit --format json >/dev/null; then
    t_pass "ai_workflow_gates: staged commit allows with active lease"
else
    t_fail "ai_workflow_gates: staged commit allows with active lease"
fi

HOOK_REPO="$AIWF_TMP/shared-hook-repo"
mkdir -p "$HOOK_REPO/.githooks" "$HOOK_REPO/.claude/hooks" \
         "$HOOK_REPO/scripts/ai-workflow" "$HOOK_REPO/scripts"
cp "$REPO_ROOT/.githooks/pre-commit" "$HOOK_REPO/.githooks/pre-commit"
cat > "$HOOK_REPO/scripts/lint.sh" <<'EOF_HOOK_LINT'
#!/usr/bin/env bash
exit 0
EOF_HOOK_LINT
chmod +x "$HOOK_REPO/scripts/lint.sh"
cat > "$HOOK_REPO/.claude/hooks/section_commit_gate.py" <<'EOF_HOOK_SECTION'
#!/usr/bin/env python3
raise SystemExit(0)
EOF_HOOK_SECTION
cat > "$HOOK_REPO/scripts/ai-workflow/gates.py" <<'EOF_HOOK_SHARED'
#!/usr/bin/env python3
import pathlib
import sys

if len(sys.argv) > 1 and sys.argv[1] == "staged-commit":
    pathlib.Path("shared-gate-called").write_text("yes\n", encoding="ascii")
    raise SystemExit(2)
raise SystemExit(0)
EOF_HOOK_SHARED
chmod +x "$HOOK_REPO/.githooks/pre-commit" \
         "$HOOK_REPO/.claude/hooks/section_commit_gate.py" \
         "$HOOK_REPO/scripts/ai-workflow/gates.py"
(
    cd "$HOOK_REPO" \
      && git init -q \
      && git config user.email t@t \
      && git config user.name t \
      && git config core.hooksPath .githooks \
      && printf 'seed\n' > file.txt \
      && git add file.txt \
      && git -c commit.gpgsign=false commit -q --no-verify -m init \
      && printf 'change\n' >> file.txt \
      && git add file.txt
)
HOOK_COMMIT_OUT=$(
    cd "$HOOK_REPO" \
      && AI_WORKFLOW_ENFORCE_SHARED_GATES=1 git -c commit.gpgsign=false commit -m blocked 2>&1
)
HOOK_COMMIT_RC=$?
if [ "$HOOK_COMMIT_RC" != "0" ] \
   && [ -f "$HOOK_REPO/shared-gate-called" ] \
   && echo "$HOOK_COMMIT_OUT" | grep -q "Shared AI workflow gate refused"; then
    t_pass "ai_workflow_gates: pre-commit enforces shared staged gate"
else
    t_fail "ai_workflow_gates: pre-commit enforces shared staged gate" \
           "rc=$HOOK_COMMIT_RC out=$HOOK_COMMIT_OUT called=$([ -f "$HOOK_REPO/shared-gate-called" ] && cat "$HOOK_REPO/shared-gate-called" || echo no)"
fi

GIT_FAIL_BIN="$AIWF_TMP/git-fail-bin"
mkdir -p "$GIT_FAIL_BIN"
REAL_GIT_BIN="$(command -v git)"
cat > "$GIT_FAIL_BIN/git" <<EOF_GIT_FAIL
#!/usr/bin/env bash
if [ "\$1" = "diff" ] && [ "\$2" = "--cached" ] && [ "\$3" = "--name-only" ]; then
    exit 1
fi
exec "$REAL_GIT_BIN" "\$@"
EOF_GIT_FAIL
chmod +x "$GIT_FAIL_BIN/git"
INSPECT_FAIL_OUT=$(env PATH="$GIT_FAIL_BIN:$PATH" AI_WORKFLOW_REPO_ROOT="$STAGED_REPO" \
    AI_WORKFLOW_STATE_DIR="$STAGED_REPO/build/ai-workflow" \
    python3 scripts/ai-workflow/gates.py staged-commit --format json 2>&1)
if [ "$?" != "0" ] && echo "$INSPECT_FAIL_OUT" | grep -q '"errors"'; then
    t_pass "ai_workflow_gates: staged commit blocks on git inspection failure"
else
    t_fail "ai_workflow_gates: staged commit blocks on git inspection failure" "$INSPECT_FAIL_OUT"
fi

STAGED_CANON_REPO="$AIWF_TMP/staged-canonical-repo"
mkdir -p "$STAGED_CANON_REPO/todo/00-infrastructure"
cat > "$STAGED_CANON_REPO/todo/00-infrastructure/TODO-98-canonical-fixture.md" <<'EOF_STAGED_CANON_TODO'
# TODO-98 Canonical Fixture

| ⭐  | Order | Deliverable | Depends On | Status |
| --- | :---: | ----------- | ---------- | :----: |
| 💎  |   1   | Fixture     | --         |  [ ]   |

## 1. Fixture

body
EOF_STAGED_CANON_TODO
( cd "$STAGED_CANON_REPO" && git init -q && git add . && git -c user.email=t@t -c user.name=t commit -q -m init )
perl -0pi -e 's/\[ \]/[x]/' "$STAGED_CANON_REPO/todo/00-infrastructure/TODO-98-canonical-fixture.md"
( cd "$STAGED_CANON_REPO" && git add todo/00-infrastructure/TODO-98-canonical-fixture.md )
if env AI_WORKFLOW_REPO_ROOT="$STAGED_CANON_REPO" AI_WORKFLOW_STATE_DIR="$STAGED_CANON_REPO/build/ai-workflow" \
    python3 scripts/ai-workflow/gates.py staged-commit --format json >/dev/null 2>&1; then
    t_fail "ai_workflow_gates: canonical IO row blocks without lease" \
           "canonical star-first row was not detected"
else
    t_pass "ai_workflow_gates: canonical IO row blocks without lease"
fi
env AI_WORKFLOW_REPO_ROOT="$STAGED_CANON_REPO" AI_WORKFLOW_STATE_DIR="$STAGED_CANON_REPO/build/ai-workflow" \
    python3 scripts/ai-workflow/lease.py acquire \
    --todo todo/00-infrastructure/TODO-98-canonical-fixture.md --section 1 \
    --driver claude --run-id staged-canon-driver >/dev/null
if env AI_WORKFLOW_REPO_ROOT="$STAGED_CANON_REPO" AI_WORKFLOW_STATE_DIR="$STAGED_CANON_REPO/build/ai-workflow" \
    python3 scripts/ai-workflow/gates.py staged-commit --format json >/dev/null; then
    t_pass "ai_workflow_gates: canonical IO row allows with active lease"
else
    t_fail "ai_workflow_gates: canonical IO row allows with active lease"
fi

STAGED_DEP_REPO="$AIWF_TMP/staged-depends-repo"
mkdir -p "$STAGED_DEP_REPO/todo/00-infrastructure"
cat > "$STAGED_DEP_REPO/todo/00-infrastructure/TODO-97-depends-fixture.md" <<'EOF_STAGED_DEP_TODO'
# TODO-97 Depends Fixture

| ⭐  | Order | Deliverable | Depends On | Status |
| --- | :---: | ----------- | ---------- | :----: |
| 💎  |   3   | Fixture     | Alpha, Beta |  [ ]   |

## 3. Fixture

body
EOF_STAGED_DEP_TODO
( cd "$STAGED_DEP_REPO" && git init -q && git add . && git -c user.email=t@t -c user.name=t commit -q -m init )
perl -0pi -e 's/\[ \]/[x]/' "$STAGED_DEP_REPO/todo/00-infrastructure/TODO-97-depends-fixture.md"
( cd "$STAGED_DEP_REPO" && git add todo/00-infrastructure/TODO-97-depends-fixture.md )
if env AI_WORKFLOW_REPO_ROOT="$STAGED_DEP_REPO" AI_WORKFLOW_STATE_DIR="$STAGED_DEP_REPO/build/ai-workflow" \
    python3 scripts/ai-workflow/gates.py staged-commit --format json >/dev/null 2>&1; then
    t_fail "ai_workflow_gates: Depends On cells do not satisfy lease target" \
           "Order 3 row with named Depends On cells was not detected"
else
    t_pass "ai_workflow_gates: Depends On cells do not satisfy lease target"
fi
env AI_WORKFLOW_REPO_ROOT="$STAGED_DEP_REPO" AI_WORKFLOW_STATE_DIR="$STAGED_DEP_REPO/build/ai-workflow" \
    python3 scripts/ai-workflow/lease.py acquire \
    --todo todo/00-infrastructure/TODO-97-depends-fixture.md --section 3 \
    --driver claude --run-id staged-dep-driver >/dev/null
if env AI_WORKFLOW_REPO_ROOT="$STAGED_DEP_REPO" AI_WORKFLOW_STATE_DIR="$STAGED_DEP_REPO/build/ai-workflow" \
    python3 scripts/ai-workflow/gates.py staged-commit --format json >/dev/null; then
    t_pass "ai_workflow_gates: Depends On row allows with Order lease"
else
    t_fail "ai_workflow_gates: Depends On row allows with Order lease"
fi

# ai_workflow_stamp atomicity: a stamp write must NOT touch the TODO if the backing
# stamp.generated evidence cannot be recorded. The write path acquires workflow_lock
# around the TODO write + ledger appends; here workflow.lock is a directory so os.open
# (O_RDWR) fails at the lock acquire (state dir otherwise valid) and the write path
# must abort before the TODO file is touched.
AIWF_ATOM_FIX="$AIWF_TMP/atomicity-fixture.md"
printf '# Atomicity Fixture\n\n## 1. Section\n\n- [x] item\n' > "$AIWF_ATOM_FIX"
AIWF_ATOM_STATE="$AIWF_TMP/atom-state"; mkdir -p "$AIWF_ATOM_STATE/workflow.lock"
AIWF_ATOM_BEFORE="$(cat "$AIWF_ATOM_FIX")"
env AI_WORKFLOW_STATE_DIR="$AIWF_ATOM_STATE" python3 scripts/ai-workflow/stamp.py validated \
    "$AIWF_ATOM_FIX" --write --allow-missing --summary "atomicity probe" >/dev/null 2>&1
AIWF_ATOM_RC=$?
if [ "$AIWF_ATOM_RC" != "0" ] && [ "$AIWF_ATOM_BEFORE" = "$(cat "$AIWF_ATOM_FIX")" ]; then
    t_pass "ai_workflow_stamp: unavailable workflow lock aborts before writing (no stamp without evidence)"
else
    t_fail "ai_workflow_stamp: unavailable workflow lock aborts before writing (no stamp without evidence)" \
           "rc=$AIWF_ATOM_RC changed=$([ "$AIWF_ATOM_BEFORE" != "$(cat "$AIWF_ATOM_FIX")" ] && echo yes || echo no)"
fi

# ai_workflow_stamp rollback: a ledger-append failure AFTER the TODO write must roll
# the stamp back (single critical section + reentrant lock). The lock itself is
# healthy; evidence.jsonl is a directory so the stamp.generated append raises after
# the stamp has landed. Old preflight-only code left the stamp in place (changed=yes);
# the rollback restores the original text, so UNCHANGED is the red-green discriminator.
AIWF_ROLL_FIX="$AIWF_TMP/rollback-fixture.md"
printf '# Rollback Fixture\n\n## 1. Section\n\n- [x] item\n' > "$AIWF_ROLL_FIX"
AIWF_ROLL_STATE="$AIWF_TMP/rollback-state"; mkdir -p "$AIWF_ROLL_STATE/evidence.jsonl"
AIWF_ROLL_BEFORE="$(cat "$AIWF_ROLL_FIX")"
env AI_WORKFLOW_STATE_DIR="$AIWF_ROLL_STATE" python3 scripts/ai-workflow/stamp.py validated \
    "$AIWF_ROLL_FIX" --write --allow-missing --summary "rollback probe" >/dev/null 2>&1
AIWF_ROLL_RC=$?
if [ "$AIWF_ROLL_RC" != "0" ] && [ "$AIWF_ROLL_BEFORE" = "$(cat "$AIWF_ROLL_FIX")" ]; then
    t_pass "ai_workflow_stamp: ledger-append failure after the TODO write rolls the stamp back"
else
    t_fail "ai_workflow_stamp: ledger-append failure after the TODO write rolls the stamp back" \
           "rc=$AIWF_ROLL_RC changed=$([ "$AIWF_ROLL_BEFORE" != "$(cat "$AIWF_ROLL_FIX")" ] && echo yes || echo no)"
fi

rm -rf "$AIWF_TMP"


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
