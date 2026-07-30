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

# Query-surface output bounds (test_query_bounds.sh). Separate suite from
# test_build.sh: that one owns subcommand SEMANTICS, this one owns what
# comes OUT of them (default limit, fail-closed ceiling, envelope fields,
# deterministic pages, and the MCP surface never returning an unbounded
# set). Shelled out the same way so failures bubble into the aggregate.
QUERY_BOUNDS_TEST="$REPO_ROOT/scripts/todo-graph/tests/test_query_bounds.sh"
if [ -x "$QUERY_BOUNDS_TEST" ]; then
    QB_OUT=$("$QUERY_BOUNDS_TEST" 2>&1)
    QB_RC=$?
    QB_SUMMARY=$(printf '%s\n' "$QB_OUT" | grep -E '^== test_query_bounds' | tail -1)
    if [ "$QB_RC" = "0" ]; then
        t_pass "scripts/todo-graph/tests/test_query_bounds.sh PASS (${QB_SUMMARY:-summary unavailable})"
    else
        t_fail "scripts/todo-graph/tests/test_query_bounds.sh FAIL (${QB_SUMMARY:-run directly for details})"
    fi
else
    t_fail "scripts/todo-graph/tests/test_query_bounds.sh not found or not executable"
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

    # 30-34. UNSPACED control operators. shlex.split only breaks on whitespace,
    #     so `true&&codex ...` and `cd /x;codex ...` tokenized as ONE glued
    #     token and slipped past this hook entirely. Measured 2026-07-27: the
    #     spaced forms blocked, the unspaced ones did not -- a real bypass of a
    #     hook whose whole job is to be un-bypassable. The spaced variants are
    #     kept alongside so a future tokenizer change cannot fix one shape
    #     while regressing the other.
    _flag_probe 2 "block unspaced && before codex --model" \
        'true&&codex exec --model gpt-5.5 prompt'
    _flag_probe 2 "block unspaced ; before codex --model" \
        'cd /tmp;codex exec --model gpt-5.5 prompt'
    _flag_probe 2 "block spaced ; before codex --model" \
        'cd /tmp; codex exec --model gpt-5.5 prompt'
    _flag_probe 2 "block spaced && before codex --model" \
        'cd /tmp && codex exec --model gpt-5.5 prompt'
    # A ';' INSIDE a quoted argument is data, not an operator. The padding must
    #     never reach into quotes or it becomes a false-positive machine.
    _flag_probe 0 "allow quoted semicolon as data (non-codex)" \
        'echo "a;b" && ls'
fi

# ============================================================================
# broker_dispatch_required (review-kind dispatches must use the broker)
# ============================================================================
# A 12.95h run sent 17 of 54 Codex dispatches down the raw script -- 12 of 19
# re-adversarial rounds, the round that decides whether a finding is resolved.
# The direct path was measured truncating a review to a 544-byte capture and
# persists no artifact, so that evidence cannot be re-read without a full
# re-dispatch. Gated in the UNATTENDED run only; interactive keeps the script.
BROKER_HOOK="$REPO_ROOT/.claude/hooks/broker_dispatch_required.py"
if [ ! -f "$BROKER_HOOK" ]; then
    t_fail "broker_dispatch_required hook missing: $BROKER_HOOK"
else
    _broker_probe() {
        # Args: <expected_rc> <description> <env-assignments> <command-string>
        local want="$1" desc="$2" envs="$3" cmd="$4"
        local payload rc
        payload=$(python3 -c "import json,sys; print(json.dumps({'tool_name':'Bash','tool_input':{'command':sys.argv[1]}}))" "$cmd" 2>/dev/null || echo '{}')
        # Scrub the two gate variables from the AMBIENT env before applying
        # $envs. Without this the "interactive" probe inherits the live
        # OVERNIGHT_SEQUENCER_RUN=1 of an overnight run and blocks, so the
        # pack could never go green inside the very run it protects.
        printf '%s' "$payload" | env -u OVERNIGHT_SEQUENCER_RUN -u BROKER_DISPATCH_OVERRIDE \
            $envs python3 "$BROKER_HOOK" >/dev/null 2>&1
        rc=$?
        if [ "$rc" = "$want" ]; then
            t_pass "broker_dispatch: $desc (rc=$rc)"
        else
            t_fail "broker_dispatch: $desc (want $want, got $rc)"
        fi
    }
    _BD="bash scripts/codex-dispatch.sh '[review-kind: re-adversarial] todo/x body'"
    _BB="bash scripts/overnight/review-broker-codex-dispatch.sh '[review-kind: re-adversarial] todo/x body'"
    _broker_probe 2 "unattended: direct + review-kind BLOCKS" \
        "OVERNIGHT_SEQUENCER_RUN=1" "$_BD"
    _broker_probe 0 "unattended: broker + review-kind allowed" \
        "OVERNIGHT_SEQUENCER_RUN=1" "$_BB"
    # The broker basename ENDS IN codex-dispatch.sh; a suffix test would
    #     misclassify it as direct and block the mandated path.
    _broker_probe 0 "unattended: non-review-kind dispatch untouched" \
        "OVERNIGHT_SEQUENCER_RUN=1" "bash scripts/codex-dispatch.sh 'no marker'"
    _broker_probe 2 "unattended: direct behind 'cd X;' still BLOCKS" \
        "OVERNIGHT_SEQUENCER_RUN=1" "cd /tmp;$_BD"
    _broker_probe 0 "unattended: override allows" \
        "OVERNIGHT_SEQUENCER_RUN=1 BROKER_DISPATCH_OVERRIDE=1" "$_BD"
    # Interactive is the whole point of scoping this to the unattended run.
    _broker_probe 0 "interactive: direct + review-kind allowed" \
        "X=1" "$_BD"
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

# --- memmap layout gate (host) ----------------------------------------------
# include/kernel/mm/memmap.h pins the address-space layout with _Static_assert,
# but an assert cannot evaluate a static-inline call, so the translation
# helpers have no compile-time net. Worse, clang-19 (the kernel compiler) does
# NOT diagnose the wraparound that bites at the top of the address space -- not
# under -Wall -Wextra -Werror, and not even under -Weverything -- while host gcc
# catches it via -Wtype-limits. This gate is therefore the ONLY automated net
# for that bug class, and it runs on the host at zero kernel-image cost (which
# is what lets it run while the BSS ceiling blocks the in-kernel suite).
[ "$QUIET" = "0" ] && echo "" && echo -e "${DIM}[memmap_layout_gate]${NC}"
assert_exit_zero "memmap layout gate: constants + translation helpers" \
    bash "$REPO_ROOT/tools/memmap-check/check.sh"

# --- Check 16 tracked-secret guard ------------------------------------------
# Fake keys are GENERATED at runtime inside a throwaway git repo so this
# file's own source never carries a live-looking key pattern.
[ "$QUIET" = "0" ] && echo "" && echo -e "${DIM}[lint_secret_guard]${NC}"
SEC_TMP="$(mktemp -d)"
SEC_REPO="$SEC_TMP/repo"
mkdir -p "$SEC_REPO/scripts/lint" "$SEC_REPO/docs" "$SEC_REPO/src"
cp "$REPO_ROOT/scripts/lint.sh" "$SEC_REPO/scripts/lint.sh"
cp -r "$REPO_ROOT/scripts/lint/." "$SEC_REPO/scripts/lint/" 2>/dev/null || true
( cd "$SEC_REPO" && git init -q && git config user.email t@t && git config user.name t )
echo "clean doc" > "$SEC_REPO/docs/ok.md"
printf 'int sec_ok(void) { return 0; }\n' > "$SEC_REPO/src/ok.c"
( cd "$SEC_REPO" && git add -A && git -c commit.gpgsign=false commit -q -m seed )
SEC_OUT="$(cd "$SEC_REPO" && bash scripts/lint.sh src/ 2>&1)"
if echo "$SEC_OUT" | grep -q "tracked-secret"; then
    t_fail "lint Check 16 false-positive on clean synthetic repo" "$(echo "$SEC_OUT" | grep tracked-secret | head -2)"
else
    t_pass "lint Check 16 clean on clean synthetic repo"
fi
# tracked secrets.json basename is refused
echo '{}' > "$SEC_REPO/secrets.json"
( cd "$SEC_REPO" && git add secrets.json && git -c commit.gpgsign=false commit -q -m s1 )
SEC_OUT="$(cd "$SEC_REPO" && bash scripts/lint.sh src/ 2>&1)"; SEC_RC=$?
if [ "$SEC_RC" != "0" ] && echo "$SEC_OUT" | grep -q "tracked-secret: basename"; then
    t_pass "lint Check 16 refuses tracked secrets.json basename (rc=$SEC_RC)"
else
    t_fail "lint Check 16 missed tracked secrets.json" "rc=$SEC_RC"
fi
( cd "$SEC_REPO" && git rm -q secrets.json && git -c commit.gpgsign=false commit -q -m s2 )
# a live-looking key in tracked content is refused (key built at runtime)
python3 -c "print('key = ' + 'sk-or-v1-' + '0123456789abcdef' * 4)" > "$SEC_REPO/docs/leak.md"
( cd "$SEC_REPO" && git add docs/leak.md && git -c commit.gpgsign=false commit -q -m s3 )
SEC_OUT="$(cd "$SEC_REPO" && bash scripts/lint.sh src/ 2>&1)"; SEC_RC=$?
if [ "$SEC_RC" != "0" ] && echo "$SEC_OUT" | grep -q "live API-key pattern"; then
    t_pass "lint Check 16 refuses live-looking key in tracked content (rc=$SEC_RC)"
else
    t_fail "lint Check 16 missed tracked live key" "rc=$SEC_RC"
fi
# staged-then-cleaned-worktree bypass is CLOSED: a live key in the INDEX is
# refused even when the worktree copy was rewritten without restaging. The
# prior tracked leak is removed FIRST and the assertion names the staged file
# specifically, so this case proves the --cached scan on its own.
( cd "$SEC_REPO" && git rm -q docs/leak.md && git -c commit.gpgsign=false commit -q -m s4 )
python3 -c "print('key = ' + 'sk-or-v1-' + 'fedcba9876543210' * 4)" > "$SEC_REPO/docs/staged-leak.md"
( cd "$SEC_REPO" && git add docs/staged-leak.md )
echo "placeholder only" > "$SEC_REPO/docs/staged-leak.md"
SEC_OUT="$(cd "$SEC_REPO" && bash scripts/lint.sh src/ 2>&1)"; SEC_RC=$?
if [ "$SEC_RC" != "0" ] && echo "$SEC_OUT" | grep -q "docs/staged-leak.md: tracked-secret"; then
    t_pass "lint Check 16 catches a staged key with a cleaned worktree copy (rc=$SEC_RC)"
else
    t_fail "lint Check 16 missed the staged-only key" "rc=$SEC_RC"
fi
# -f: the fixture deliberately leaves staged != worktree != HEAD (that is the
# staged-only-leak case itself); plain --cached refuses that state and errored
# on every run ("staged content different from both the file and the HEAD").
( cd "$SEC_REPO" && git rm -q --cached -f docs/staged-leak.md && rm -f docs/staged-leak.md )

# skip env produces a visible WARN and passes
SEC_OUT="$(cd "$SEC_REPO" && SKIP_LINT_SECRETS=1 bash scripts/lint.sh src/ 2>&1)"
if echo "$SEC_OUT" | grep -q "Check 16 (tracked-secret guard) skipped"; then
    t_pass "lint Check 16 skip env emits visible WARN"
else
    t_fail "lint Check 16 skip env silent"
fi
rm -rf "$SEC_TMP"

# --- runner_bash_guard: subagent Bash hazard backstop -------------------------
[ "$QUIET" = "0" ] && echo "" && echo -e "${DIM}[runner_bash_guard]${NC}"
_RBG="$REPO_ROOT/.claude/hooks/runner_bash_guard.py"
_rbg_case() {  # <desc> <transcript> <command> <want_rc>
    local desc="$1" tp="$2" cmd="$3" want="$4" rc
    printf '{"tool_name":"Bash","transcript_path":"%s","tool_input":{"command":"%s"}}' "$tp" "$cmd" \
        | python3 "$_RBG" >/dev/null 2>&1; rc=$?
    if [ "$rc" = "$want" ]; then
        t_pass "runner_bash_guard: $desc (rc=$rc)"
    else
        t_fail "runner_bash_guard: $desc (want $want, got $rc)"
    fi
}
_rbg_case "subagent codex dispatch BLOCKED" "/x/agent-a1.jsonl" "bash scripts/codex-dispatch.sh review" 2
_rbg_case "subagent git push BLOCKED" "/x/agent-a1.jsonl" "git push origin main" 2
_rbg_case "subagent gh pr comment BLOCKED" "/x/agent-a1.jsonl" "gh pr comment 5 --body hi" 2
_rbg_case "subagent SKIP override BLOCKED" "/x/agent-a1.jsonl" "SKIP_REVIEW_HOOK=1 git status" 2
# proved bypass shapes from the 2026-07-02 re-adversarial (tokenizer-aware now)
_rbg_case "subagent git -C commit BLOCKED (global-opt walk)" "/x/agent-a1.jsonl" "git -C /tmp/r commit --no-verify -m x" 2
_rbg_case "subagent bash -lc codex BLOCKED (shell recursion)" "/x/agent-a1.jsonl" "bash -lc \\\"codex exec x\\\"" 2
_rbg_case "subagent gh -R pr comment BLOCKED (global-flag walk)" "/x/agent-a1.jsonl" "gh -R o/r pr comment 1 --body hi" 2
_rbg_case "subagent gh api -XPOST BLOCKED (attached method)" "/x/agent-a1.jsonl" "gh api repos/x/y -XPOST" 2
_rbg_case "subagent gh api --method=POST BLOCKED" "/x/agent-a1.jsonl" "gh api repos/x/y --method=POST" 2
_rbg_case "subagent eval BLOCKED (indirection)" "/x/agent-a1.jsonl" "eval git-push-hidden" 2
_rbg_case "subagent git -c shell-alias smuggle BLOCKED" "/x/agent-a1.jsonl" "git -c alias.x=codex-body x" 2
_rbg_case "subagent git -c plain-alias smuggle BLOCKED" "/x/agent-a1.jsonl" "git -c alias.k=commit-body k" 2
_rbg_case "subagent git -c core.pager hook BLOCKED (deny-by-default)" "/x/agent-a1.jsonl" "git -c core.pager=cat log --oneline -3" 2
_rbg_case "subagent git -c core.fsmonitor hook BLOCKED" "/x/agent-a1.jsonl" "git -c core.fsmonitor=body status --short" 2
_rbg_case "subagent separate --config-env alias BLOCKED" "/x/agent-a1.jsonl" "git --config-env alias.x=ALIAS_BODY x" 2
_rbg_case "subagent GIT_CONFIG_* alias injection BLOCKED" "/x/agent-a1.jsonl" "GIT_CONFIG_COUNT=1 GIT_CONFIG_KEY_0=alias.x GIT_CONFIG_VALUE_0=body git x" 2
_rbg_case "subagent GIT_SSH_COMMAND injection BLOCKED" "/x/agent-a1.jsonl" "GIT_SSH_COMMAND=codexbody git fetch" 2
_rbg_case "subagent GIT_EXTERNAL_DIFF injection BLOCKED" "/x/agent-a1.jsonl" "GIT_EXTERNAL_DIFF=body git diff HEAD" 2
_rbg_case "subagent benign env + git status allowed" "/x/agent-a1.jsonl" "FOO=1 git status --short" 0
_rbg_case "subagent gh api GET allowed" "/x/agent-a1.jsonl" "gh api repos/x/y/actions/runs" 0
_rbg_case "subagent git branch --list allowed" "/x/agent-a1.jsonl" "git branch --list" 0
_rbg_case "subagent build.sh allowed" "/x/agent-a1.jsonl" "bash scripts/build.sh" 0
_rbg_case "subagent git log allowed" "/x/agent-a1.jsonl" "git log --oneline -5" 0
_rbg_case "subagent gh run view allowed" "/x/agent-a1.jsonl" "gh run view 12 --log-failed" 0
_rbg_case "main-session git commit untouched" "/x/c3c5bd55-mainsession.jsonl" "git commit -m x" 0
_rbg_case "main-session codex dispatch untouched" "/x/c3c5bd55-mainsession.jsonl" "bash scripts/codex-dispatch.sh review" 0
_rbg_out=$(printf 'not json' | python3 "$_RBG" 2>&1); _rbg_rc=$?
if [ "$_rbg_rc" = "0" ] && [ -z "$_rbg_out" ]; then
    t_pass "runner_bash_guard: fail-open on malformed payload"
else
    t_fail "runner_bash_guard: malformed payload not fail-open (rc=$_rbg_rc)"
fi

# --- Check 14 agent tool-allowlist: runner class ------------------------------
# Synthetic agents in a throwaway repo prove the two-class contract: analyst
# with Bash fails; runner marker + {Bash,Read,Grep,Glob} passes; runner with
# Edit or WebSearch fails (runners cannot mutate files or do web research).
[ "$QUIET" = "0" ] && echo "" && echo -e "${DIM}[lint_agent_runner_class]${NC}"
ARC_TMP="$(mktemp -d)"
ARC_REPO="$ARC_TMP/repo"
mkdir -p "$ARC_REPO/scripts/lint" "$ARC_REPO/.claude/agents" "$ARC_REPO/src"
cp "$REPO_ROOT/scripts/lint.sh" "$ARC_REPO/scripts/lint.sh"
cp -r "$REPO_ROOT/scripts/lint/." "$ARC_REPO/scripts/lint/" 2>/dev/null || true
printf 'int arc_ok(void) { return 0; }\n' > "$ARC_REPO/src/ok.c"
( cd "$ARC_REPO" && git init -q && git config user.email t@t && git config user.name t \
    && git add -A && git -c commit.gpgsign=false commit -q -m seed )
_arc_case() {  # <desc> <want_flag:yes|no> <agent-body-file-content> [filename]
    # Check 14 runs only in no-arg (full-repo) lint mode; other checks may
    # error on the bare synthetic repo, so assert on the agent-file error
    # line presence/absence rather than the overall exit code. The optional
    # 4th arg lets the runner pass-case use a ROSTER name (the roster gate
    # means a non-roster filename can never be a valid runner).
    local desc="$1" want_flag="$2" body="$3" fname="${4:-synthetic.md}" out
    printf '%s' "$body" > "$ARC_REPO/.claude/agents/$fname"
    out=$(cd "$ARC_REPO" && bash scripts/lint.sh 2>&1) || true
    if [ "$want_flag" = "yes" ]; then
        if echo "$out" | grep -q "$fname.*\(allowlist\|RUNNER_ROSTER\|hard rules\)"; then
            t_pass "lint_agent_runner_class: $desc (flagged)"; else
            t_fail "lint_agent_runner_class: $desc" "expected flag, got: $(echo "$out" | grep "$fname" | head -1)"; fi
    else
        if echo "$out" | grep -q "$fname"; then
            t_fail "lint_agent_runner_class: $desc" "unexpected flag: $(echo "$out" | grep "$fname" | head -1)"; else
            t_pass "lint_agent_runner_class: $desc (clean)"; fi
    fi
    rm -f "$ARC_REPO/.claude/agents/$fname"
}
_arc_case "analyst with Bash is flagged" yes '---
name: synthetic
tools: Bash, Read
---
body'
_arc_case "rostered runner + marker + hard rules + Bash/Read/Grep/Glob passes" no '---
name: checks-runner
tools: Bash, Read, Grep, Glob
---
<!-- agent-class: runner -->
## Forbidden -- hard rules
body' checks-runner.md
_arc_case "rostered runner missing hard-rules section is flagged" yes '---
name: checks-runner
tools: Bash, Read, Grep, Glob
---
<!-- agent-class: runner -->
body' checks-runner.md
_arc_case "runner with Edit is flagged" yes '---
name: synthetic
tools: Bash, Read, Edit
---
<!-- agent-class: runner -->
body'
_arc_case "runner marker NOT in roster is flagged (spoof guard)" yes '---
name: synthetic
tools: Bash, Read, Grep, Glob
---
<!-- agent-class: runner -->
## Forbidden -- hard rules
body'
_arc_case "runner with WebSearch is flagged" yes '---
name: synthetic
tools: Bash, Read, WebSearch
---
<!-- agent-class: runner -->
body'
rm -rf "$ARC_TMP"

# --- codex-bg-dispatch argument contract -----------------------------------
[ "$QUIET" = "0" ] && echo "" && echo -e "${DIM}[codex_bg_dispatch_contract]${NC}"
_bg_multi="$(bash "$REPO_ROOT/scripts/codex-bg-dispatch.sh" '[review-kind: design] todo/00-infrastructure/TODO-08-automation-hardening.md ok' extra 2>&1 >/dev/null || true)"
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

# Positive paths (BLOCK/WARN) via a synthetic tree: the hook resolves its repo
# root from its own location, so a copied hook + synthetic sequencer-run.json
# exercises the SECTIONS-phase logic without touching the real state.
AD_TMP="$(mktemp -d)"
mkdir -p "$AD_TMP/.claude/hooks" "$AD_TMP/.claude/state"
cp "$_AD_GATE" "$AD_TMP/.claude/hooks/agent_dispatch_required.py"
printf '{"active": true, "phase": "SECTIONS"}' > "$AD_TMP/.claude/state/sequencer-run.json"
_ad_pos() {  # <desc> <want_rc> <want_grep>
    local desc="$1" want_rc="$2" want_grep="$3" err rc
    err=$(printf '%s' "$PAYLOAD" | env -u SKIP_AGENT_DISPATCH_HOOK OVERNIGHT_SEQUENCER_RUN=1         python3 "$AD_TMP/.claude/hooks/agent_dispatch_required.py" 2>&1 1>/dev/null)
    rc=$?
    if [ "$rc" = "$want_rc" ] && echo "$err" | grep -q "$want_grep"; then
        t_pass "agent_dispatch_gate: $desc (rc=$rc)"
    else
        t_fail "agent_dispatch_gate: $desc (want rc=$want_rc + '$want_grep', got rc=$rc err='$err')"
    fi
}
# fail-open on malformed shapes even in SECTIONS phase (non-object JSON,
# non-dict tool_input) and no BLOCK outside kernel-ish paths (docs .c copy)
for BAD in '[]' '{"tool_name":"Edit","tool_input":[]}'; do
    PAYLOAD="$BAD"
    err=$(printf '%s' "$PAYLOAD" | env -u SKIP_AGENT_DISPATCH_HOOK OVERNIGHT_SEQUENCER_RUN=1 \
        python3 "$AD_TMP/.claude/hooks/agent_dispatch_required.py" 2>&1 1>/dev/null); rc=$?
    if [ -z "$err" ] && [ "$rc" = "0" ]; then
        t_pass "agent_dispatch_gate: fail-open on malformed payload $BAD"
    else
        t_fail "agent_dispatch_gate: malformed payload $BAD not fail-open (rc=$rc err='$err')"
    fi
done
PAYLOAD='{"tool_name":"Edit","tool_input":{"file_path":"src/kernel/sched.c"}}'
_ad_pos "BLOCKs kernel source edit with no fresh dispatch" 2 "agent-dispatch BLOCK"
PAYLOAD='{"tool_name":"Edit","tool_input":{"file_path":"scripts/tool.py"}}'
_ad_pos "WARNs (not blocks) non-kernel source edit" 0 "agent-dispatch reminder"
# a fresh dispatch record clears both paths
python3 -c "import json,time,sys; json.dump({'timestamp_ns': time.time_ns(), 'subagent_type': 'kernel-explorer'}, open(sys.argv[1],'w'))" "$AD_TMP/.claude/state/last-agent-dispatch.json"
PAYLOAD='{"tool_name":"Edit","tool_input":{"file_path":"src/kernel/sched.c"}}'
err=$(printf '%s' "$PAYLOAD" | env -u SKIP_AGENT_DISPATCH_HOOK OVERNIGHT_SEQUENCER_RUN=1     python3 "$AD_TMP/.claude/hooks/agent_dispatch_required.py" 2>&1 1>/dev/null); rc=$?
if [ -z "$err" ] && [ "$rc" = "0" ]; then
    t_pass "agent_dispatch_gate: fresh dispatch record clears the BLOCK"
else
    t_fail "agent_dispatch_gate: fresh dispatch did not clear (rc=$rc err='$err')"
fi
rm -rf "$AD_TMP"

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
# these probes mutate the LIVE session state file (the hooks resolve their
# state path from their own location) -- back it up and restore after, or
# every suite run silently destroys the session review-receipt state (this
# was the root cause of the recurring 'last-codex-review.json missing' gate
# blocks and likely the 2026-06-14 'hook dead' incident)
RRG_STATE_BACKUP=""
if [ -f "$STATE_FILE" ]; then
    RRG_STATE_BACKUP="$STATE_FILE.suite-backup.$$"
    cp "$STATE_FILE" "$RRG_STATE_BACKUP"
fi

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

    # Cleanup: restore the pre-suite session state instead of deleting it
    rm -f "$STATE_FILE" "$STATE_DIR"/*.tmp 2>/dev/null
    if [ -n "$RRG_STATE_BACKUP" ] && [ -f "$RRG_STATE_BACKUP" ]; then
        mv "$RRG_STATE_BACKUP" "$STATE_FILE"
    fi
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

    # 9: review far older than the legacy TTL but content-bound (trigger_blobs
    #    match the staged tree) -> ALLOW. Content-addressed receipts
    #    (2026-07-11): a review's validity is its blob binding, not a clock.
    _write_received_state "true" '["src/kernel/foo.c"]' "3600"
    _gate_run 0 "old review with matching content binding allows (receipts)" \
        '{"tool_name":"Bash","tool_input":{"command":"git commit -m foo"}}'

    # 9b: same old review, staged content edited AFTER the review (blob
    #     mismatch) -> block. The binding, not the age, is the receipt.
    (
        cd "$GATE_REPO"
        cat > src/kernel/foo.c <<'CMOD'
int seed_only(void) { return 1; }
int new_helper(int n) { return n + 2; }
CMOD
        git add src/kernel/foo.c
    )
    touch "$GATE_REPO/build/build.log"  # keep legacy build evidence fresh
    _gate_run 2 "old review with post-review edit blocks (blob mismatch)" \
        '{"tool_name":"Bash","tool_input":{"command":"git commit -m foo"}}'
    _stage_section_commit  # restore canonical staged content for later cases

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
# gap-audit recognition + trusted receipt-side review_run_id (TODO-10
# review-evidence integrity): the receipt hook must attribute gap-audit
# dispatches and must never trust a prompt-authored review run id.
GA_OUT=$(python3 - <<'PYGA'
import sys
sys.path.insert(0, ".claude/hooks")
import codex_review_completed as crc

def check(label, cond):
    print(("OK " if cond else "FAIL ") + label)

check("skill_maps_gap_audit", crc._detect_review_kind("codex-gap-audit", "") == "gap-audit")
check("marker_matches_gap_audit", crc._detect_review_kind("", "[review-kind: gap-audit] todo/x") == "gap-audit")
check("trigger_skills_include_gap_audit", "codex-gap-audit" in crc.CODEX_TRIGGER_SKILLS)

# Control-operator shapes. A dispatch behind an UNSPACED operator used to be
# invisible to BOTH the review-kind detector and the receipt binder, because
# shlex.split leaves an unspaced ';' or '&&' glued to its neighbour and the
# segment walk never sees a separator token. Fails closed for these two (a
# performed review reads as no review, so correct work is BLOCKED and the
# operator is trained to reach for SKIP_*_HOOK). `cd <repo>; <dispatch>` is a
# shape the runner emits constantly, so this is not exotic.
from _review_kind import detect_review_kind_from_cmd as drk
_D = "bash scripts/codex-dispatch.sh '[review-kind: design] todo/x body'"
check("kind_plain", drk(_D) == "design")
check("kind_spaced_and", drk("cd /tmp && " + _D) == "design")
check("kind_spaced_semi", drk("cd /tmp; " + _D) == "design")
check("kind_unspaced_semi", drk("cd /tmp;" + _D) == "design")
check("kind_unspaced_and", drk("true&&" + _D) == "design")
check("kind_piped", drk(_D + " 2>&1 | tail -5") == "design")
# A ';' inside the quoted PROMPT is data, not an operator -- padding must never
# reach into quotes or the prompt itself would be split.
check("kind_semicolon_inside_prompt",
      drk("bash scripts/codex-dispatch.sh '[review-kind: design] todo/x a;b'") == "design")
# The broker is the shape the sequencer doctrine mandates; it is recognized
# because its basename ENDS IN codex-dispatch.sh. Asserted so that stays true.
check("kind_broker_wrapper",
      drk("bash scripts/overnight/review-broker-codex-dispatch.sh "
          "'[review-kind: adversarial] todo/x body'") == "adversarial")
# Receipt binding must agree with detection on every shape above -- a dispatch
# the gate recognizes but the receipt does not would leave the gate unsatisfiable.
check("receipt_unspaced_semi", crc._is_codex_bash_trigger("cd /tmp;" + _D))
check("receipt_unspaced_and", crc._is_codex_bash_trigger("true&&" + _D))
d, r, hint = crc._detect_run_metadata("review-run-id: forged-by-prompt-123", 42, "adversarial")
check("receipt_generates_trusted_run_id", r == "codex-review-adversarial-42" and hint == "forged-by-prompt-123")
d2, r2, h2 = crc._detect_run_metadata("", 43, "perf")
check("receipt_run_id_without_prompt_hint", r2 == "codex-review-perf-43" and h2 == "")
import json, pathlib, tempfile
# the non-review codex e / codex exec automation aliases are NOT review
# dispatches: they must never mint reviewer evidence via the receipt path
import _codex_dispatch as cd
check("codex_e_alias_not_review_dispatch",
      cd.extract_dispatch_prompt("codex e '[review-kind: adversarial] todo/x'") == ""
      and cd.extract_dispatch_prompt("codex exec '[review-kind: adversarial] todo/x'") == ""
      and cd.extract_dispatch_prompt("codex review '[review-kind: adversarial] todo/x'") != "")
# background dispatches are detected and their receipts mirror as TELEMETRY,
# never result=received (a receive can precede background-job completion)
check("background_dispatch_detected",
      cd.is_background_dispatch("node /x/codex-companion.mjs task --background --json 'p'")
      and cd.is_background_dispatch("bash scripts/codex-bg-dispatch.sh 'p'")
      and not cd.is_background_dispatch("bash scripts/codex-dispatch.sh 'p'"))
# argv-based, not raw-text: a FOREGROUND dispatch whose prompt merely mentions
# the bg wrapper (e.g. a review of the background-dispatch code) stays foreground
check("fg_prompt_mentioning_bg_wrapper_stays_foreground",
      not cd.is_background_dispatch(
          "bash scripts/codex-dispatch.sh '[review-kind: adversarial] todo/x review scripts/codex-bg-dispatch.sh behavior'"))
# the bg wrapper is a RECOGNIZED trigger (proper attribution, telemetry
# semantics) -- not an invisible dispatch
check("bg_wrapper_classified_with_prompt",
      cd.extract_dispatch_prompt("bash scripts/codex-bg-dispatch.sh '[review-kind: perf] todo/x b'")
      == "[review-kind: perf] todo/x b")
# e2e: a background dispatch records trigger state marked background and
# NEVER populates last-review-stamps.json (the four-dispatch proof surface)
import os, subprocess as sp
with tempfile.TemporaryDirectory() as tmp:
    root = pathlib.Path(tmp)
    sp.run(["git", "init", "-q"], cwd=tmp, check=True, capture_output=True)
    hook = pathlib.Path.cwd() / ".claude/hooks/codex_review_completed.py"
    payload = json.dumps({
        "tool_name": "Bash",
        "tool_input": {"command": "bash scripts/codex-bg-dispatch.sh '[review-kind: adversarial] todo/00-infrastructure/TODO-08-automation-hardening.md body'"},
    })
    sp.run([sys.executable, str(hook)], input=payload, text=True, cwd=tmp, capture_output=True)
    stamps = root / ".claude" / "state" / "last-review-stamps.json"
    state_f = root / ".claude" / "state" / "last-codex-review.json"
    st = json.loads(state_f.read_text()) if state_f.exists() else {}
    check("bg_dispatch_no_stamp_proof",
          not stamps.exists() and st.get("background_dispatch") is True)
PYGA
)
GA_OK=$(echo "$GA_OUT" | grep -c "^OK ")
if [ "$GA_OK" = "20" ]; then
    echo "$GA_OUT" | grep "^OK " | while IFS= read -r line; do
        t_pass "review_receipt: $line"
    done
    PASS=$((PASS + 20))
else
    t_fail "review_receipt: gap-audit / trusted run-id coverage incomplete" "ok=$GA_OK out=$GA_OUT"
fi

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

    # Test R2 (INVERTED post-background-telemetry): `task --background
    # --json <prompt>` still classifies (state records the kind, marked
    # background) but must NEVER populate the four-dispatch stamp proof --
    # a background launch proves only that a job started.
    rm -f "$FD_STAMPS"
    PAYLOAD='{"tool_name":"Bash","tool_input":{"command":"node /abs/codex-companion.mjs task --background --json \"[review-kind: perf] todo/00-infrastructure/TODO-99-fdgate-fixture.md S1 perf review prompt\""}}'
    (cd "$FD_REPO" && printf '%s' "$PAYLOAD" | \
        python3 ".claude/hooks/codex_review_completed.py" >/dev/null 2>&1)
    if python3 -c "
import json, os, sys
if os.path.exists('$FD_STAMPS'):
    s = json.load(open('$FD_STAMPS'))
    if isinstance(s.get('$FD_TODO_PATH', {}).get('perf'), int):
        sys.exit(1)
r = json.load(open('$FD_REVIEW'))
sys.exit(0 if r.get('background_dispatch') is True and r.get('review_kind') == 'perf' else 1)
" 2>/dev/null; then
        t_pass "four_dispatch_gate: R2 background task classifies but records NO stamp proof"
    else
        t_fail "four_dispatch_gate: R2 background task leaked into stamp proof or lost attribution"
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

    # Test B: stamp-only SKIP PRESERVES last-codex-review.json `received` and
    # records the skip in the audit log.
    #
    # This asserted the opposite until 2026-07-26. The reset was deliberately
    # removed on 2026-07-14 (Canary #2, B3, see the rationale comment in
    # section_commit_gate.py): a stamp-only commit touches only TODO markdown
    # and does NOT change the source the code review passed, so `received:
    # true` still legitimately stands and the immediately-following rollover
    # plus receiving-review gate need it to persist -- resetting produced a
    # reset -> rollover-refused -> receiving-review-re-block repair loop. The
    # original worry (a later section commit reusing a stale `received`) is
    # already blocked by _review_evidence content-binding, which fails a
    # later commit whose staged blobs differ from the review's trigger_blobs
    # regardless of the flag. The paper trail moved to skip-log.jsonl, so the
    # assertion moved with it.
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
        SKIP_REVIEW_HOOK_REASON="testing B: stamp-only SKIP preserves received" \
        python3 ".claude/hooks/section_commit_gate.py" >/dev/null 2>&1; echo $?)
    if [ "$SKIP_RC" = "0" ] && python3 -c "
import json, pathlib, sys
s = json.load(open('$FD_REVIEW'))
if s.get('received') is not True:
    sys.exit(1)                      # B3: the passed code review must persist
log = pathlib.Path('$FD_REPO/.claude/state/skip-log.jsonl')
if not log.exists():
    sys.exit(1)                      # the paper trail replaced the reset
recs = [json.loads(l) for l in log.read_text().splitlines() if l.strip()]
sys.exit(0 if any('preserves received' in json.dumps(r) for r in recs) else 1)
" 2>/dev/null; then
        t_pass "four_dispatch_gate: B stamp-only SKIP preserves received + logs the skip (B3)"
    else
        t_fail "four_dispatch_gate: B stamp-only SKIP contract broken (rc=$SKIP_RC)"
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
        rm -f "$FD_REPO/.claude/state/.fdgate_d_ready"
        python3 - <<'PY' &
import fcntl, os, time, pathlib
p = pathlib.Path(".claude/state/last-review-stamps.lock")
p.parent.mkdir(parents=True, exist_ok=True)
fd = os.open(str(p), os.O_RDWR | os.O_CREAT, 0o644)
fcntl.flock(fd, fcntl.LOCK_EX)
# signal readiness ONLY after the lock is actually held -- a fixed sleep
# races under suite load (holder python startup can exceed it), letting the
# foreground call grab the lock uncontended and fail both assertions.
pathlib.Path(".claude/state/.fdgate_d_ready").write_text("held")
time.sleep(4)
fcntl.flock(fd, fcntl.LOCK_UN)
os.close(fd)
PY
        HOLDER_PID=$!
        # wait (bounded) for the holder to actually hold the lock
        _fd_waited=0
        while [ ! -f "$FD_REPO/.claude/state/.fdgate_d_ready" ] && [ "$_fd_waited" -lt 100 ]; do
            sleep 0.05
            _fd_waited=$((_fd_waited + 1))
        done
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
    rm -f "/tmp/fdgate_d_result.$$" "$FD_REPO/.claude/state/.fdgate_d_ready"

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

# (b2) interactive_offload_router routes offloadable prompt shapes to the
#      right specialist agent and stays silent on ordinary prompts.
IOR_HOOK="$REPO_ROOT/.claude/hooks/interactive_offload_router.py"
IOR_OUT="$(printf '%s' '{"prompt":"validate TODO-13 for structural gaps"}' | \
    python3 "$IOR_HOOK" 2>/dev/null)"
if echo "$IOR_OUT" | grep -q "offload hint" && \
   echo "$IOR_OUT" | grep -q "todo-validation-mapper"; then
    t_pass "offload_router_todo  validate-TODO prompt routes to todo-validation-mapper"
else
    t_fail "offload_router_todo  expected todo-validation-mapper hint, got: $IOR_OUT"
fi
IOR_OUT2="$(printf '%s' '{"prompt":"run the full test suite"}' | \
    python3 "$IOR_HOOK" 2>/dev/null)"
if echo "$IOR_OUT2" | grep -q "checks-runner"; then
    t_pass "offload_router_checks  run-tests prompt routes to checks-runner"
else
    t_fail "offload_router_checks  expected checks-runner hint, got: $IOR_OUT2"
fi
IOR_OUT3="$(printf '%s' '{"prompt":"fix this typo in the README"}' | \
    python3 "$IOR_HOOK" 2>/dev/null)"
if [ -z "$IOR_OUT3" ]; then
    t_pass "offload_router_clean  ordinary prompt produces no systemMessage"
else
    t_fail "offload_router_clean  expected empty output, got: $IOR_OUT3"
fi

# (b3) inline_churn_monitor: warns at 25 inline ops, resets on Agent dispatch.
ICM_HOOK="$REPO_ROOT/.claude/hooks/inline_churn_monitor.py"
ICM_STATE="$REPO_ROOT/.claude/state/inline-churn.json"
ICM_BACKUP=""
[ -f "$ICM_STATE" ] && ICM_BACKUP="$(cat "$ICM_STATE")"
rm -f "$ICM_STATE"
ICM_WARN=""
for _i in $(seq 1 25); do
    ICM_WARN="$(printf '%s' '{"tool_name":"Bash"}' | python3 "$ICM_HOOK" 2>/dev/null)"
done
if echo "$ICM_WARN" | grep -q "inline-churn" && echo "$ICM_WARN" | grep -q "25 inline"; then
    t_pass "inline_churn_warn  systemMessage emitted at op 25"
else
    t_fail "inline_churn_warn  expected churn warning at op 25, got: $ICM_WARN"
fi
ICM_RESET_OUT="$(printf '%s' '{"tool_name":"Agent"}' | python3 "$ICM_HOOK" 2>/dev/null)"
ICM_COUNT="$(python3 -c "import json; print(json.load(open('$ICM_STATE'))['count'])" 2>/dev/null)"
if [ -z "$ICM_RESET_OUT" ] && [ "$ICM_COUNT" = "0" ]; then
    t_pass "inline_churn_reset  Agent dispatch resets the counter silently"
else
    t_fail "inline_churn_reset  expected silent reset to 0, got count=$ICM_COUNT out=$ICM_RESET_OUT"
fi
rm -f "$ICM_STATE"
[ -n "$ICM_BACKUP" ] && printf '%s' "$ICM_BACKUP" > "$ICM_STATE"

# (b4) build_offload_reminder: BLOCKs bare build.sh in overnight SECTIONS with
#      no fresh Agent dispatch; silent interactively.
#      The hook was promoted from REMINDER (systemMessage on stdout) to BLOCK
#      (exit 2 + message on stderr) when the offload routing was enforced.
#      These captures therefore read the COMBINED stream and assert the exit
#      code, so the assertion tracks the contract rather than the channel a
#      given generation happened to use.
BOR_HOOK="$REPO_ROOT/.claude/hooks/build_offload_reminder.py"
BOR_SEQ="$REPO_ROOT/.claude/state/sequencer-run.json"
BOR_DISP="$REPO_ROOT/.claude/state/last-agent-dispatch.json"
BOR_SEQ_BAK=""; BOR_DISP_BAK=""
[ -f "$BOR_SEQ" ] && BOR_SEQ_BAK="$(cat "$BOR_SEQ")"
[ -f "$BOR_DISP" ] && BOR_DISP_BAK="$(cat "$BOR_DISP")"
rm -f "$BOR_SEQ" "$BOR_DISP"
BOR_OUT="$(printf '%s' '{"tool_name":"Bash","tool_input":{"command":"bash scripts/build.sh"}}' | \
    python3 "$BOR_HOOK" 2>&1)"
if [ -z "$BOR_OUT" ]; then
    t_pass "build_offload_interactive  silent when sequencer guard inactive"
else
    t_fail "build_offload_interactive  expected silence, got: $BOR_OUT"
fi
printf '%s' '{"active": true, "phase": "SECTIONS"}' > "$BOR_SEQ"
# SESSION SCOPING (2026-07-28): the gate now requires the run's IDENTITY, not
# just its state -- OVERNIGHT_SEQUENCER_RUN=1, the same discriminator
# run_phase_guard.is_headless() uses -- because keying on the global cursor
# alone meant every interactive operator session inherited the run's phase
# gates. These cases simulate the RUN, so they must export it; the
# operator-not-gated case is asserted separately below.
export OVERNIGHT_SEQUENCER_RUN=1
BOR_OUT2="$(printf '%s' '{"tool_name":"Bash","tool_input":{"command":"bash scripts/test.sh QUIET=1"}}' | \
    python3 "$BOR_HOOK" 2>&1)"; BOR_RC2=$?
if echo "$BOR_OUT2" | grep -q "build-offload" && [ "$BOR_RC2" = "2" ]; then
    t_pass "build_offload_sections  BLOCKs bare test.sh in SECTIONS phase (exit 2)"
else
    t_fail "build_offload_sections  expected build-offload BLOCK (exit 2), got rc=$BOR_RC2: $BOR_OUT2"
fi
# Freshness must be checks-runner-SPECIFIC (regression fixed 2026-07-05): a
# fresh dispatch of an UNRELATED agent type must NOT suppress the reminder --
# the original bug treated any recent Agent dispatch as cover, letting 99
# in-context build/test calls slip through a live overnight run silently.
NOW_NS="$(date +%s%N)"
printf '{"timestamp_ns": %s, "subagent_type": "kernel-explorer", "by_type": {"kernel-explorer": {"timestamp_ns": %s}}}' \
    "$NOW_NS" "$NOW_NS" > "$BOR_DISP"
BOR_OUT3="$(printf '%s' '{"tool_name":"Bash","tool_input":{"command":"bash scripts/build.sh"}}' | \
    python3 "$BOR_HOOK" 2>&1)"; BOR_RC3=$?
if echo "$BOR_OUT3" | grep -q "build-offload" && [ "$BOR_RC3" = "2" ]; then
    t_pass "build_offload_type_specific  unrelated-agent dispatch does NOT suppress the reminder"
else
    t_fail "build_offload_type_specific  expected BLOCK despite unrelated dispatch, got rc=$BOR_RC3: $BOR_OUT3"
fi
printf '{"timestamp_ns": %s, "subagent_type": "checks-runner", "by_type": {"checks-runner": {"timestamp_ns": %s}}}' \
    "$NOW_NS" "$NOW_NS" > "$BOR_DISP"
BOR_OUT4="$(printf '%s' '{"tool_name":"Bash","tool_input":{"command":"bash scripts/build.sh"}}' | \
    python3 "$BOR_HOOK" 2>&1)"
if [ -z "$BOR_OUT4" ]; then
    t_pass "build_offload_type_specific  fresh checks-runner dispatch DOES suppress the reminder"
else
    t_fail "build_offload_type_specific  expected silence after checks-runner dispatch, got: $BOR_OUT4"
fi
# An OPERATOR session must NOT inherit the run's gate, even with the run
# active in SECTIONS. This is the half that was missing: the gate keyed on the
# global cursor, so a read-only operator command was blocked by a rule whose
# rationale is explicitly the RUN's context budget.
unset OVERNIGHT_SEQUENCER_RUN
BOR_OUT5="$(printf '%s' '{"tool_name":"Bash","tool_input":{"command":"bash scripts/build.sh"}}' | \
    python3 "$BOR_HOOK" 2>&1)"; BOR_RC5=$?
if [ -z "$BOR_OUT5" ] && [ "$BOR_RC5" = "0" ]; then
    t_pass "build_offload_operator_scope  operator session NOT gated by the run's phase"
else
    t_fail "build_offload_operator_scope  operator session was gated, rc=$BOR_RC5: $BOR_OUT5"
fi
# A read-only command naming two suite scripts must never read as an invocation
# (the `\b` matched the DOT inside `build.sh`, so `grep a.sh b.sh` was BLOCKed).
export OVERNIGHT_SEQUENCER_RUN=1
BOR_OUT6="$(printf '%s' '{"tool_name":"Bash","tool_input":{"command":"grep -n x scripts/build.sh scripts/test.sh"}}' | \
    python3 "$BOR_HOOK" 2>&1)"; BOR_RC6=$?
if [ -z "$BOR_OUT6" ] && [ "$BOR_RC6" = "0" ]; then
    t_pass "build_offload_readonly  grep naming two suite scripts is not an invocation"
else
    t_fail "build_offload_readonly  read-only grep was blocked, rc=$BOR_RC6: $BOR_OUT6"
fi
unset OVERNIGHT_SEQUENCER_RUN
rm -f "$BOR_SEQ" "$BOR_DISP"
[ -n "$BOR_SEQ_BAK" ] && printf '%s' "$BOR_SEQ_BAK" > "$BOR_SEQ"
[ -n "$BOR_DISP_BAK" ] && printf '%s' "$BOR_DISP_BAK" > "$BOR_DISP"

# (b5) review_dispatch_gate + xref_dispatch_reminder: embedded selftests are
#      the full fixture suites (tempdir state, block/allow/fail-open cases).
if python3 "$REPO_ROOT/.claude/hooks/review_dispatch_gate.py" --selftest >/dev/null 2>&1; then
    t_pass "review_dispatch_gate_selftest  9-case fixture suite green"
else
    t_fail "review_dispatch_gate_selftest  embedded selftest failed"
fi
if python3 "$REPO_ROOT/.claude/hooks/xref_dispatch_reminder.py" --selftest >/dev/null 2>&1; then
    t_pass "xref_dispatch_reminder_selftest  fixture suite green"
else
    t_fail "xref_dispatch_reminder_selftest  embedded selftest failed"
fi
if python3 "$REPO_ROOT/.claude/hooks/slice_read_reminder.py" --selftest >/dev/null 2>&1; then
    t_pass "slice_read_reminder_selftest  fixture suite green"
else
    t_fail "slice_read_reminder_selftest  embedded selftest failed"
fi
if python3 "$REPO_ROOT/.claude/hooks/read_offload_reminder.py" --selftest >/dev/null 2>&1; then
    t_pass "read_offload_reminder_selftest  fixture suite green"
else
    t_fail "read_offload_reminder_selftest  embedded selftest failed"
fi
if python3 "$REPO_ROOT/.claude/hooks/edit_retry_reminder.py" --selftest >/dev/null 2>&1; then
    t_pass "edit_retry_reminder_selftest  fixture suite green"
else
    t_fail "edit_retry_reminder_selftest  embedded selftest failed"
fi
if python3 "$REPO_ROOT/.claude/hooks/cd_prefix_reminder.py" --selftest >/dev/null 2>&1; then
    t_pass "cd_prefix_reminder_selftest  redundant-prefix reminder green"
else
    t_fail "cd_prefix_reminder_selftest  embedded selftest failed"
fi
# The boot-validation matrix must stay wired and must keep covering BOTH engines
# at BOTH cpu counts. The gate it replaces had silently run one CPU on one engine
# for its whole life, which is how an SMP regression shipped green.
# .claude/settings.json wires every hook, so an edit that neuters one must run
# the runner suite and re-arm the canary. It was in NEITHER manifest until
# 2026-07-28, which meant "turn this gate off" was a silent, ungated change.
_cp_full=$(printf '%s\n' '.claude/settings.json' | bash "$REPO_ROOT/scripts/overnight/control-plane-match.sh" 2>/dev/null)
_cp_flow=$(printf '%s\n' '.claude/settings.json' | bash "$REPO_ROOT/scripts/overnight/control-plane-match.sh" --flow-critical 2>/dev/null)
if [ -n "$_cp_full" ] && [ -n "$_cp_flow" ]; then
    t_pass "settings_json_control_plane  wiring file is manifest + flow-critical"
else
    t_fail "settings_json_control_plane  settings.json escapes a control-plane gate" \
           "full='$_cp_full' flow='$_cp_flow'"
fi

if [ -x "$REPO_ROOT/scripts/test-smoke-matrix.sh" ]; then
    t_pass "smoke_matrix_present  scripts/test-smoke-matrix.sh is executable"
else
    t_fail "smoke_matrix_present  scripts/test-smoke-matrix.sh missing or not executable"
fi
_sm_legs=$(grep -oE 'SMOKE_MATRIX_LEGS:-[^}]*' "$REPO_ROOT/scripts/test-smoke-matrix.sh" 2>/dev/null | head -1)
if printf '%s' "$_sm_legs" | grep -q 'kvm:1' \
   && printf '%s' "$_sm_legs" | grep -q 'kvm:2' \
   && printf '%s' "$_sm_legs" | grep -q 'tcg:1' \
   && printf '%s' "$_sm_legs" | grep -q 'tcg:2'; then
    t_pass "smoke_matrix_legs  default covers kvm+tcg x 1+2 cpus"
else
    t_fail "smoke_matrix_legs  default legs incomplete" "got: $_sm_legs"
fi
if grep -q 'test-smoke-matrix\.sh' "$REPO_ROOT/.claude/skills/implement-todo-section/SKILL.md" 2>/dev/null; then
    t_pass "smoke_matrix_wired  implement-todo-section calls the matrix"
else
    t_fail "smoke_matrix_wired  implement-todo-section still calls the single-config smoke test"
fi

if python3 "$REPO_ROOT/.claude/hooks/todo_wrap_reminder.py" --selftest >/dev/null 2>&1; then
    t_pass "todo_wrap_reminder_selftest  hard-wrap detector green"
else
    t_fail "todo_wrap_reminder_selftest  embedded selftest failed"
fi
if python3 "$REPO_ROOT/scripts/todo-staged-check.py" --selftest >/dev/null 2>&1; then
    t_pass "todo_staged_check_selftest  commit-time item-length backstop green"
else
    t_fail "todo_staged_check_selftest  embedded selftest failed"
fi
# The backstop must be WIRED, not merely present: its whole reason for existing
# is that the PreToolUse hook is bypassed by python3-via-Bash writes, and an
# unwired backstop reproduces exactly that gap one layer down.
if grep -q 'todo-staged-check\.py' "$REPO_ROOT/.githooks/pre-commit" 2>/dev/null; then
    t_pass "todo_staged_check_wired  called from .githooks/pre-commit"
else
    t_fail "todo_staged_check_wired  not called from .githooks/pre-commit"
fi
if python3 "$REPO_ROOT/scripts/todo-reflow.py" --selftest >/dev/null 2>&1; then
    t_pass "todo_reflow_selftest  reflow is content-preserving + idempotent"
else
    t_fail "todo_reflow_selftest  embedded selftest failed"
fi
# The reflow must never REFUSE (exit 2) across the real tree: a refusal means it
# would have changed content, not just line breaks, and that is the failure mode
# worth catching before validate-todo-file runs --write on someone's TODO.
_reflow_rc=0
python3 "$REPO_ROOT/scripts/todo-reflow.py" --check $(find "$REPO_ROOT/todo" -name '*.md') \
    >/dev/null 2>&1 || _reflow_rc=$?
if [ "$_reflow_rc" = "2" ]; then
    t_fail "todo_reflow_no_refusals  reflow would alter content in some todo file"
else
    t_pass "todo_reflow_no_refusals  content-preserving across every todo/ file"
fi

# search_offload_gate is RETIRED (2026-07-28): the headless run it was scoped to
# has no Grep tool, so its remediation was impossible, and Bash-grep vs Grep-tool
# output is identical in context so the saving never existed. The invariant worth
# testing is no longer "does it work" but "is it still unwired" -- a revival
# should have to argue with the measurement, not slip back in via settings.json.
# The leading `/` matters: websearch_offload_gate.py (live, unrelated) CONTAINS
# "search_offload_gate.py" as a substring, so a bare match would fail forever.
if grep -q '/search_offload_gate\.py' "$REPO_ROOT/.claude/settings.json" 2>/dev/null; then
    t_fail "search_offload_gate_retired  re-wired in settings.json" \
           "retired 2026-07-28 -- see docs/infrastructure/hook-codes.md SEARCH-OFFLOAD"
else
    t_pass "search_offload_gate_retired  still unwired from settings.json"
fi
if python3 "$REPO_ROOT/.claude/hooks/agent_coverage_gate.py" --selftest >/dev/null 2>&1; then
    t_pass "agent_coverage_gate_selftest  dispatch-replaces-read gate green"
else
    t_fail "agent_coverage_gate_selftest  embedded selftest failed"
fi
if python3 "$REPO_ROOT/.claude/hooks/_advisory_budget.py" --selftest >/dev/null 2>&1; then
    t_pass "advisory_budget_selftest  per-session injection budget green"
else
    t_fail "advisory_budget_selftest  embedded selftest failed"
fi
# T1-4: the budget is for ADVISORY hooks only. A BLOCK's fire means it BLOCKED,
# so budgeting one would punch a hole in a gate -- assert no blocking hook
# imports it.
_ab_block_users=""
for _h in "$REPO_ROOT"/.claude/hooks/*.py; do
    grep -q '_advisory_budget' "$_h" 2>/dev/null || continue
    grep -qE '^\s*(return 2|sys\.exit\(2\))' "$_h" 2>/dev/null && \
        _ab_block_users="$_ab_block_users $(basename "$_h")"
done
if [ -z "$_ab_block_users" ]; then
    t_pass "advisory_budget_no_block_users  no blocking hook is rate-limited"
else
    t_fail "advisory_budget_no_block_users  BLOCK hook(s) import the budget:$_ab_block_users"
fi
if python3 "$REPO_ROOT/.claude/hooks/read_cache_block.py" --selftest >/dev/null 2>&1; then
    t_pass "read_cache_block_selftest  fixture suite green"
else
    t_fail "read_cache_block_selftest  embedded selftest failed"
fi
# The read gate's correctness depends on pre_compact_flush deleting its table:
# the block asserts "already in context", which a compaction falsifies. Assert
# the wiring directly so the two files cannot drift apart silently.
if grep -q '_READ_CACHE_REL' "$REPO_ROOT/.claude/hooks/pre_compact_flush.py"; then
    t_pass "read_cache_precompact_clear  pre_compact_flush unlinks read-cache.json"
else
    t_fail "read_cache_precompact_clear  pre_compact_flush no longer clears the read cache"
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
    if [ "$IPG_OK_COUNT" = "4" ]; then
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
#   (scripts/xref_grammar.py) that the git hook (accepted_xref_block),
#   the stamp writer (stamp.py), and todo-graph all route through. Guards the
#   tier classification (bare/soft/concrete) + parse + writer predicates so the
#   three-way XREF-grammar drift that stalled the unification cannot reappear.
# ============================================================================
[ "$QUIET" = "0" ] && echo "" && echo -e "${DIM}[xref_grammar]${NC}"

XREF_OUT=$(python3 - <<'PYEOF'
import sys
sys.path.insert(0, "scripts")
import xref_grammar as xref

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
# a canonical item name containing a raw terminator stays writable: the
# structural paren + marker sit before the hook boundary (round-3 finding) --
# and the hook agrees (its clause ends at the same spot with ( and item: before it)
for i, t in enumerate((
    f'[M] gap -> XREF: 00-infrastructure/TODO-11 {S}2 (item: "Real, extra" at line 7)',
    f'[M] gap -> XREF: 00-infrastructure/TODO-11 {S}2 (item: "a; b] c" at line 9)',
)):
    check(f"writer_accepts_punctuated_item_name_{i}",
          not xref.writer_bare_xrefs(t) and xref.writer_has_concrete(t)
          and not xref.bare_clauses(t))
# a genuinely UNCLOSED parenthetical is refused by the writer even though the
# lenient hook lets it through -- writer strictness, round-4 finding
UNCLOSED = '[M] gap -> XREF: 00-infrastructure/TODO-11 (item: junk with no closing paren'
check("writer_rejects_unclosed_parenthetical",
      bool(xref.writer_bare_xrefs(UNCLOSED)) and not xref.writer_has_concrete(UNCLOSED))
# a marker hidden INSIDE the quoted item name is not structural
QMARK = '[M] gap -> XREF: 00-infrastructure/TODO-11 ("item: fake inside quotes" at line 3)'
check("writer_rejects_quoted_only_marker",
      bool(xref.writer_bare_xrefs(QMARK)) and not xref.writer_has_concrete(QMARK))
# a paren+marker inside quoted PROSE (quote opened before the paren) is not a
# structural owner parenthetical (round-5 finding: global quote state)
QPROSE = 'XREF: 00-infrastructure/TODO-11 "prefix (item: fake" blah ") still quoted"'
check("writer_rejects_paren_inside_quoted_prose",
      bool(xref.writer_bare_xrefs(QPROSE)) and not xref.writer_has_concrete(QPROSE))
# writer-accept implies a graph-consumable edge shape: arrow + domain path
# immediately followed by section + concrete item parenthetical (consistency +
# final-adversarial findings); cross-checked against todo-graph's actual regex
sys.path.insert(0, "scripts/todo-graph")
import build as tg
GOOD = f'[M] g -> XREF: 00-infrastructure/TODO-11 {S}2 (item: "Real" at line 7)'
check("writer_accept_implies_graph_edge_shape",
      xref.writer_has_concrete(GOOD) and not xref.writer_bare_xrefs(GOOD)
      and tg.XREF_CLAUSE_RE.search(GOOD) is not None)
NONCANON = '[M] g -> XREF: TODO-11 (item: "X" at line 20)'
check("writer_rejects_sectionless_or_bare_domain",
      bool(xref.writer_bare_xrefs(NONCANON)) and not xref.writer_has_concrete(NONCANON))
# a section marker NOT adjacent to the target makes no graph edge -> writer rejects
ADJ = f'[M] g -> XREF: 00-infrastructure/TODO-11 (item: "X" at line 20) see {S}1'
check("writer_rejects_non_adjacent_section",
      bool(xref.writer_bare_xrefs(ADJ)) and not xref.writer_has_concrete(ADJ)
      and tg.XREF_CLAUSE_RE.search(ADJ) is None)
# the arrow prefix is part of the edge grammar -> writer rejects arrowless clauses
NOARROW = f'[M] g XREF: 00-infrastructure/TODO-11 {S}2 (item: "Real" at line 7)'
check("writer_rejects_arrowless_clause",
      bool(xref.writer_bare_xrefs(NOARROW)) and not xref.writer_has_concrete(NOARROW)
      and tg.XREF_CLAUSE_RE.search(NOARROW) is None)
# helper; is HOOK-tier only: its semicolon is a raw terminator, so the writer can
# never accept it -- pinned as a documented decision (new stamps use the item form)
HLP = f'[M] g -> XREF: 00-infrastructure/TODO-11 {S}2 (helper; add kmem helper)'
check("writer_rejects_helper_marker",
      bool(xref.writer_bare_xrefs(HLP)) and not xref.writer_has_concrete(HLP))
# subset property fuzz: any summary the WRITER accepts must have no hook-BLOCK
# clause (writer-accepts implies hook-accepts, for every generated shape)
import random as _random
_random.seed(11)
_frag = ["XREF:", "-> XREF:", " 00-x/TODO-11", f" 00-x/TODO-11 {S}2", " no-owner",
         ",", ";", "]", ' "q, uote"', ' (item: "Real" at line 7)',
         ' (item: "a, b" at line 3)', " (later)", ' (item: junk unclosed',
         ' "prose (item: fake"', " item: loose", " text"]
_viol = [s for s in ("".join(_random.choice(_frag) for _ in range(_random.randint(1, 10)))
                     for _ in range(4000))
         if xref.writer_has_concrete(s) and not xref.writer_bare_xrefs(s)
         and xref.bare_clauses(s)]
check("writer_subset_property_fuzz", not _viol)
PYEOF
)
XREF_OK=$(echo "$XREF_OUT" | grep -c "^OK ")
if [ "$XREF_OK" = "32" ]; then
    echo "$XREF_OUT" | grep "^OK " | while IFS= read -r line; do
        t_pass "xref_grammar: $line"
    done
    PASS=$((PASS + 32))
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
# KERNEL_TESTS release-flavor knob (kernel-security-hardening: release-build
# test-surface exclusion)
# ============================================================================
# All checks use `make -n` dry-runs: they assert the FLAVOR CONTRACT (which
# flags reach the compiler, which sources reach the link) without paying for a
# real compile. The proof that a shipped image carries no test surface is a
# separate gate (the release-flavor proof / seam-inventory work), not these checks.

# Last -D/-U KERNEL_TESTS flag on a rule's compile line ("" if none). clang
# applies -D/-U left to right, so the LAST flag is the effective one.
_kt_last_flag() {  # <target> [make-vars...]
    local target="$1"; shift
    make -Bn "$@" "$target" 2>/dev/null \
        | grep -oE '\-[DU]KERNEL_TESTS' | tail -1
}

_kt_case() {  # <desc> <want> <target> [make-vars...]
    local desc="$1" want="$2" target="$3"; shift 3
    local got
    got="$(_kt_last_flag "$target" "$@")"
    if [ "$got" = "$want" ]; then
        t_pass "$desc"
    else
        t_fail "$desc" "effective flag: '${got:-<none>}' (want '$want')"
    fi
}

# Default flavor is the test flavor: the suite must keep working with no knob.
_kt_case "KERNEL_TESTS: default flavor defines the macro" \
         "-DKERNEL_TESTS" "build/kernel/mm/pmm.o"
_kt_case "KERNEL_TESTS=on defines the macro" \
         "-DKERNEL_TESTS" "build/kernel/mm/pmm.o" KERNEL_TESTS=on
_kt_case "KERNEL_TESTS=off undefines the macro" \
         "-UKERNEL_TESTS" "build/kernel/mm/pmm.o" KERNEL_TESTS=off

# SECURITY (Codex design review 2026-07-17): the documented KERNEL_EXTRA_CFLAGS
# pass-through must NOT be able to re-enable the test surface under the release
# flavor. The flavor flag is appended last with `override`, so it wins.
_kt_case "KERNEL_TESTS=off beats KERNEL_EXTRA_CFLAGS=-DKERNEL_TESTS" \
         "-UKERNEL_TESTS" "build/kernel/mm/pmm.o" \
         KERNEL_TESTS=off KERNEL_EXTRA_CFLAGS=-DKERNEL_TESTS
_kt_case "KERNEL_TESTS=off beats a command-line CFLAGS override" \
         "-UKERNEL_TESTS" "build/kernel/mm/pmm.o" \
         KERNEL_TESTS=off CFLAGS=-DKERNEL_TESTS

# The INTERNAL flag variables are `override` too: a plain assignment would lose
# to a command-line value and re-enable the seams under an apparent release
# build. Each of these is a bypass that existed before the override landed.
_kt_case "KERNEL_TESTS=off beats a KERNEL_TESTS_FLAG override" \
         "-UKERNEL_TESTS" "build/kernel/mm/pmm.o" \
         KERNEL_TESTS=off KERNEL_TESTS_FLAG=-DKERNEL_TESTS
_kt_case "KERNEL_TESTS=off beats a SIMD_CFLAGS override" \
         "-UKERNEL_TESTS" "build/kernel/icon_store.o" \
         KERNEL_TESTS=off SIMD_CFLAGS=-DKERNEL_TESTS
_kt_case "KERNEL_TESTS=off beats an AVX2_CFLAGS override" \
         "-UKERNEL_TESTS" "build/kernel/mm/memops.o" \
         KERNEL_TESTS=off AVX2_CFLAGS=-DKERNEL_TESTS
_kt_case "KERNEL_TESTS=off beats an AVX512_CFLAGS override" \
         "-UKERNEL_TESTS" "build/kernel/mm/memops_avx512.o" \
         KERNEL_TESTS=off AVX512_CFLAGS=-DKERNEL_TESTS

# The flavor stamp must not become make's default goal: it is declared before
# the kernel targets, and a bare `make` that builds only a stamp would let an
# operator believe a release kernel was produced.
_kt_goal="$(make -p 2>/dev/null | grep -m1 '^.DEFAULT_GOAL' || true)"
case "$_kt_goal" in
    *kernel-tests.stamp*)
        t_fail "KERNEL_TESTS stamp is not make's default goal" "$_kt_goal" ;;
    *)
        t_pass "KERNEL_TESTS stamp is not make's default goal" ;;
esac

# The SIMD/AVX rules derive their own CFLAGS variants and declare their own
# prerequisites; they must inherit the flavor like the pattern rule does.
_kt_case "KERNEL_TESTS=off reaches the SIMD-derived explicit rules" \
         "-UKERNEL_TESTS" "build/kernel/icon_store.o" KERNEL_TESTS=off

# An invalid flavor fails fast at parse time rather than building something
# unintended (mirrors the BUILD_ALT_BOOT contract).
# Capture first: this file runs under `set -o pipefail`, so piping make
# straight into grep would surface make's intended rc=2 as the pipeline's
# status and invert the result.
_kt_bogus_out="$(make KERNEL_TESTS=bogus kernel 2>&1)" && _kt_bogus_rc=0 || _kt_bogus_rc=$?
if [ "$_kt_bogus_rc" != "0" ] \
   && echo "$_kt_bogus_out" | grep -q "KERNEL_TESTS must be one of: on, off"; then
    t_pass "KERNEL_TESTS=bogus fails fast with a named error"
else
    t_fail "KERNEL_TESTS=bogus fails fast with a named error" \
           "rc=$_kt_bogus_rc; output: $(echo "$_kt_bogus_out" | head -1)"
fi

# Source pruning: the define alone still compiles/links every test TU, so the
# release flavor must drop them from the build entirely.
_kt_test_tus() {  # <flavor> -> count of src/kernel/test objects in the build
    make -n KERNEL_TESTS="$1" kernel 2>/dev/null \
        | grep -oE 'build/kernel/test/[a-z_0-9]+\.o' | sort -u | wc -l
}
_kt_on_tus="$(_kt_test_tus on)"
_kt_off_tus="$(_kt_test_tus off)"
if [ "$_kt_off_tus" = "0" ]; then
    t_pass "KERNEL_TESTS=off prunes every src/kernel/test/ TU from the build"
else
    t_fail "KERNEL_TESTS=off prunes every src/kernel/test/ TU from the build" \
           "$_kt_off_tus test objects still in the build"
fi
if [ "$_kt_on_tus" -gt 0 ]; then
    t_pass "KERNEL_TESTS=on keeps the src/kernel/test/ TUs ($_kt_on_tus objects)"
else
    t_fail "KERNEL_TESTS=on keeps the src/kernel/test/ TUs" "test suite pruned from the test flavor"
fi

# Stamp invalidation: the flavor stamp must be a REAL prerequisite of EVERY C
# object rule. An order-only prereq (`| stamp`) does not trigger a rebuild, so
# a flip would silently relink objects compiled under the opposite flavor.
#
# The match accepts the stamp ANYWHERE among the real prerequisites (the
# `[^|]*` runs stop at the order-only separator, so a `| $(KERNEL_TESTS_STAMP)`
# is still correctly rejected -- that is the property under test). It used to
# require the stamp IMMEDIATELY before the `|`, which turned the legitimate
# addition of a second real stamp into a false failure: the pattern rule at
# Makefile:1672 carries `$(KERNEL_TESTS_STAMP) $(EXCEPT_TELEMETRY_STAMP) |`
# and was reported as unstamped despite the stamp being a real prereq.
_kt_c_rules="$(grep -cE '^(\$\(BUILD_DIR\)/\S+\.o|\$\(LZ4_FULL_OBJ\)): \$\(SRC_DIR\)/\S+\.c ' Makefile)"
_kt_stamped="$(grep -cE '^(\$\(BUILD_DIR\)/\S+\.o|\$\(LZ4_FULL_OBJ\)): \$\(SRC_DIR\)/\S+\.c [^|]*\$\(KERNEL_TESTS_STAMP\)[^|]*\|' Makefile)"
if [ "$_kt_c_rules" = "$_kt_stamped" ] && [ "$_kt_stamped" -gt 0 ]; then
    t_pass "KERNEL_TESTS flavor stamp is a real prereq of all $_kt_stamped C object rules"
else
    t_fail "KERNEL_TESTS flavor stamp is a real prereq of all C object rules" \
           "$_kt_stamped of $_kt_c_rules C rules carry the stamp"
fi

# ============================================================================
# Release-flavor test-surface exclusion: test-only TUs OUTSIDE src/kernel/test/
# ============================================================================
# A directory-only prune (above) misses test-only TUs that sit beside
# production files under an ordinary filename. These dry-run checks assert
# the Makefile-level pruning contract for the three explicitly-listed extra
# TUs; the #ifdef KERNEL_TESTS guards inside partition.c/boot_tests.c/
# tpm_transport.c are compile-time (invisible to `make -n`) and their actual
# symbol-absence is proven by the release-flavor gate (section 27), not here.

_kt_extra_tu_present() {  # <flavor> <object-path> -> 1 if present in the plan
    # Capture to a variable and match with a bash `case`, never pipe `make -n`
    # (or its captured output) into `grep -q`: under this script's
    # `set -o pipefail`, grep -q's early exit on first match can SIGPIPE the
    # upstream writer, and pipefail then reports THAT non-zero exit instead
    # of grep's success -- silently forcing every case to "0" regardless of
    # the real content (caught 2026-07-17: all 4 of these checks false-failed
    # under the real script but passed standalone without pipefail active).
    # A pure bash `case` match has no subprocess pipe, so no pipefail hazard.
    local _plan="" _flavor="$1" _needle="$2"
    _plan="$(make -n KERNEL_TESTS="$_flavor" kernel 2>/dev/null)"
    case "$_plan" in
        *"$_needle"*) echo 1 ;;
        *)            echo 0 ;;
    esac
}
for _kt_extra in \
    "build/kernel/fs/ntfs/ntfs_test.o" \
    "build/kernel/fs/ixfs/ixfs_test.o" \
    "build/kernel/main/test_threads.o"
do
    _kt_on_present="$(_kt_extra_tu_present on "$_kt_extra")"
    _kt_off_present="$(_kt_extra_tu_present off "$_kt_extra")"
    if [ "$_kt_on_present" = "1" ] && [ "$_kt_off_present" = "0" ]; then
        t_pass "KERNEL_TESTS=off prunes $_kt_extra (present at on, absent at off)"
    else
        t_fail "KERNEL_TESTS=off prunes $_kt_extra" \
               "on=$_kt_on_present off=$_kt_off_present (want on=1 off=0)"
    fi
done

# shell_loader_func moved OUT of test_threads.c into its own production TU
# (2026-07-17): it must survive BOTH flavors, since it is the release
# flavor's only path to a shell (boot_desktop.c task_create()s it).
_kt_shell_on="$(_kt_extra_tu_present on "build/kernel/main/shell_loader.o")"
_kt_shell_off="$(_kt_extra_tu_present off "build/kernel/main/shell_loader.o")"
if [ "$_kt_shell_on" = "1" ] && [ "$_kt_shell_off" = "1" ]; then
    t_pass "shell_loader.o (production cmd.exe launcher) survives both flavors"
else
    t_fail "shell_loader.o survives both flavors" \
           "on=$_kt_shell_on off=$_kt_shell_off (want on=1 off=1)"
fi

# ============================================================================
# Hook repo-root resolution (regression pin, 2026-07-28)
# ============================================================================
# Five hooks find the repo root by walking up to the first directory that
# CONTAINS a `.claude`. A hook that mis-joins a `.claude/...`-relative state
# path onto `.claude/` itself creates `.claude/.claude/`, which then satisfies
# that test one level early -- so those hooks resolve the root to `.claude` and
# every gate keyed on repo state silently stops firing. No error, no log; the
# only symptom was two BLOCK tests here reporting rc=0. Pin both halves.

if [ -d "$REPO_ROOT/.claude/.claude" ]; then
    t_fail "no doubled .claude/.claude directory" \
           "exists -- it breaks _repo_root() in 5 hooks (see search_offload_gate._valve)"
else
    t_pass "no doubled .claude/.claude directory"
fi

_rr_bad=""
for _h in build_offload_reminder rotate_hint agent_dispatch_required \
          session_brief_inject verify_cadence_reminder; do
    _rr=$(cd "$REPO_ROOT" && python3 -c "
import importlib.util, sys
s = importlib.util.spec_from_file_location('$_h', '.claude/hooks/$_h.py')
m = importlib.util.module_from_spec(s); s.loader.exec_module(m)
print(m._repo_root())" 2>/dev/null)
    [ "$_rr" = "$REPO_ROOT" ] || _rr_bad="$_rr_bad $_h=$_rr"
done
if [ -z "$_rr_bad" ]; then
    t_pass "hook _repo_root() resolves to the repo root (5 hooks)"
else
    t_fail "hook _repo_root() resolves to the repo root" "wrong:$_rr_bad"
fi

# ============================================================================
# gen-user-abi constant extraction (TODO-04 s23 + s26)
# ============================================================================
# Every ABI fixture below plants scratch state under build/. Confirm ONCE, up
# front, that build/ is a real directory in this repository and not a symlink,
# and refuse the whole group otherwise -- a symlinked build/ would put every
# mkdtemp, plant and cleanup outside the checkout.
#
# This is a REAL-DIRECTORY check, not a defense against a concurrent process
# swapping build/ between this test and a later mkdir. Guarding that would need
# dir_fd + O_NOFOLLOW plumbing through every fixture, and it buys nothing here:
# an actor able to do it already has write access to this script, the Makefile
# and the sources. What it does close is the realistic case -- residue or a
# hand-made symlink left where a scratch directory is expected.
if [ -L "$REPO_ROOT/build" ] || { [ -e "$REPO_ROOT/build" ] && [ ! -d "$REPO_ROOT/build" ]; }; then
    t_fail "gen_user_abi: build/ is a real directory for ABI scratch state" \
           "build/ is a symlink or not a directory; refusing to plant fixture state through it"
    _GUA_SCRATCH_OK=0
else
    # build/ is GITIGNORED, so it does not exist on a clean checkout -- and CI
    # runs this suite BEFORE build.sh (.github/workflows/build.yml runs
    # test-tooling at the lint stage, build.sh several steps later). Creating it
    # here is what keeps every scratch mkdtemp below from failing with ENOENT on
    # exactly that path.
    mkdir -p "$REPO_ROOT/build"
    t_pass "gen_user_abi: build/ is a real directory for ABI scratch state"
    _GUA_SCRATCH_OK=1
fi
# The generator's single-literal rule extracts the first UNSIGNED integer
# literal from a define body, so it would read `(-1000)` as +1000 and
# `(BASE - 1)` as +1 -- silently WRONG values, which is worse than the
# hand-copy drift the generated header exists to eliminate. Exit statuses
# therefore go through a restricted expression grammar with a fail-closed
# contract. These fixtures pin every refusal it promises, because a resolver
# that silently OMITS a constant just restores the hand-copy it replaced.
#
# s26 moved EXTRACTION onto clang (-dD -E in the kernel's own flag vector) and
# added CERTIFICATION (a generated _Static_assert per emitted value, compiled
# in every build flavor). That splits these fixtures into two kinds, and the
# distinction is the point:
#
#   REFUSAL fixtures  -- shapes the restricted grammar still will not resolve
#                        (unsupported operator, cyclic, int32 overflow, octal,
#                        multi-term). Unchanged: a refusal is a build failure.
#   AGREEMENT fixtures -- shapes the OLD scanner refused because IT could not
#                        read them (conditional arms, spliced comments, a `/*`
#                        inside a string literal). clang reads them correctly,
#                        so the assertion is now "the generator returns the
#                        value clang computes", which is strictly stronger than
#                        "the generator declines to guess".
#
# A shape moving from refusal to agreement is NOT a weakened guarantee: the
# value is now decided by the compiler that builds the kernel, and anything
# genuinely unpublishable (a value that differs per build flavor) is refused by
# the flavor sweep instead.

[ "$QUIET" = "0" ] && echo "" && echo -e "${DIM}[gen-user-abi exit-status resolver]${NC}"
GUA_OUT=$(cd "$REPO_ROOT" && python3 - <<'PYGUA'
import importlib.util, tempfile, os, sys, io, contextlib

spec = importlib.util.spec_from_file_location("gua", "scripts/gen-user-abi.py")
gua = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gua)

EXPORT = ('X_STATUS',)
RESOLVE = ('X_BASE',)

def run(source, export=EXPORT, resolve=RESOLVE):
    """Resolve `source` as a fixture header; returns (entries, errors)."""
    fd, path = tempfile.mkstemp(suffix=".h")
    try:
        with os.fdopen(fd, "w") as f:
            f.write(source)
        return gua.resolve_exit_statuses(path, export, resolve)
    finally:
        os.unlink(path)

def check(label, cond):
    print(("OK " if cond else "FAIL ") + label)

# --- resolution ---------------------------------------------------------
e, err = run("#define X_BASE (-1000)\n#define X_STATUS (X_BASE - 1)\n")
check("resolves_symbol_minus_literal", not err and e == [('X_STATUS', -1001, '(-1001)')])

# The exact trap parse_defines falls into: a bare negative literal must NOT
# come back positive.
e, err = run("#define X_BASE (0)\n#define X_STATUS (-1234)\n")
check("bare_negative_literal_keeps_sign", not err and e[0][1] == -1234)

e, err = run("#define X_BASE (-1000)\n#define X_STATUS (X_BASE + 5)\n")
check("resolves_symbol_plus_literal", not err and e[0][1] == -995)

e, err = run("#define X_BASE (-1000)\n#define X_STATUS X_BASE\n")
check("resolves_bare_symbol", not err and e[0][1] == -1000)

# Order independence: a macro that references a symbol defined BELOW it is
# legal C (macros expand at use), so file order must not decide resolvability.
e, err = run("#define X_STATUS (X_BASE - 1)\n#define X_BASE (-1000)\n")
check("resolves_forward_reference", not err and e[0][1] == -1001)

# A kernel-side value change must move the generated value (this is what
# make check-abi turns into a build failure).
e, err = run("#define X_BASE (-2000)\n#define X_STATUS (X_BASE - 1)\n")
check("value_change_propagates", not err and e[0][1] == -2001)

# The resolve-only symbol feeds the arithmetic and is never exported: it is
# not a ring-3 contract, and crt0 aborts every process on a hash change.
e, err = run("#define X_BASE (-1000)\n#define X_STATUS (X_BASE - 1)\n")
check("resolve_only_symbol_not_exported",
      not err and [n for n, _v, _l in e] == ['X_STATUS'])

# --- refusals (each must FAIL LOUD, never omit) -------------------------
_, err = run("#define X_BASE (-1000)\n#define X_STATUS (X_BASE * 2)\n")
check("refuses_unsupported_operator", bool(err))

_, err = run("#define X_BASE (-1000)\n#define X_STATUS (SOMETHING_ELSE - 1)\n")
check("refuses_unallowlisted_symbol", bool(err))

_, err = run("#define X_BASE (-1000)\n")
check("refuses_absent_name", bool(err) and any("not defined" in m for m in err))

# Duplicate definitions are refused by TWO complementary layers, and which one
# fires depends on the shape. A redefinition with a DIFFERENT body is already
# fatal to the kernel build itself -- the authoritative flag vector carries
# -Werror, so -Wmacro-redefined stops the query before the generator gets a
# chance to have an opinion. Asserting the compiler's refusal here rather than
# the generator's is deliberate: it pins that the query really does run under
# the kernel's own flags.
_, err = run("#define X_BASE (-1000)\n#define X_STATUS (X_BASE - 1)\n"
             "#define X_STATUS (X_BASE - 2)\n")
check("refuses_duplicate_definition",
      bool(err) and any("REFUSED" in m and "redefined" in m for m in err))

# The second layer covers what -Wmacro-redefined does NOT: clang accepts an
# IDENTICAL redefinition silently, and accepts #undef + redefine silently,
# reporting only the surviving value. Both exit 0, so without an explicit
# ownership check they would sail through. The guarantee worth keeping is not
# "clang did not complain" but "exactly one place owns this ABI number" -- two
# owners are one refactor away from disagreeing, and the generator would
# faithfully certify whichever won the include race.
_, err = run("#define X_BASE (-1000)\n#define X_STATUS (X_BASE - 1)\n"
             "#define X_STATUS (X_BASE - 1)\n")
check("refuses_identical_redefinition",
      bool(err) and any("IDENTICAL" in m for m in err))

_, err = run("#define X_BASE (-1000)\n#define X_STATUS (X_BASE - 1)\n"
             "#undef X_STATUS\n#define X_STATUS (X_BASE - 2)\n")
check("refuses_undef_then_redefine",
      bool(err) and any("#undef" in m for m in err))

# task.h carries 14 #ifdef KERNEL_TESTS blocks, one directly below the
# exit-status block. The old scanner refused ANY conditional definition because
# it could not evaluate the build configuration and would otherwise have picked
# a physical arm rather than the active one. clang picks the arm the kernel
# compiles, so the ACTIVE arm now resolves -- correctly, and only because the
# query runs in the kernel's own flag vector where KERNEL_TESTS is defined.
e, err = run("#define X_BASE (-1000)\n#ifdef KERNEL_TESTS\n"
             "#define X_STATUS (X_BASE - 1)\n#endif\n")
check("resolves_active_conditional_arm", not err and e[0][1] == -1001)

# The arm that is NOT compiled must not contribute a value: an #ifdef on a
# macro the kernel does not define leaves the constant undefined, and an
# undefined allowlisted name is a loud refusal, never an omission.
_, err = run("#define X_BASE (-1000)\n#ifdef NOT_A_KERNEL_FLAVOR_FLAG\n"
             "#define X_STATUS (X_BASE - 1)\n#endif\n")
check("refuses_inactive_conditional_arm",
      bool(err) and any("not defined" in m for m in err))

# #if 0 / #else: the old scanner saw two physical definitions and refused.
# clang takes the #else arm, which is what the kernel compiles.
e, err = run("#define X_BASE (-1000)\n#if 0\n#define X_STATUS (X_BASE - 1)\n#else\n"
             "#define X_STATUS (X_BASE - 2)\n#endif\n")
check("resolves_else_arm_not_dead_arm", not err and e[0][1] == -1002)

# C strips comments BEFORE recognising directives, so a conditional hidden
# behind one is a REAL directive -- clang honours it and the active arm wins.
e, err = run("#define X_BASE (-1000)\n/* gate */ #ifdef KERNEL_TESTS\n"
             "#define X_STATUS (X_BASE - 1)\n#endif\n")
check("resolves_conditional_behind_block_comment", not err and e[0][1] == -1001)

# ...and an #endif buried INSIDE a comment is not a directive at all, so the
# `#if 0` is still open and the define never happens. Undefined -> refusal.
_, err = run("#define X_BASE (-1000)\n#if 0\n/*\n#endif\n*/\n"
             "#define X_STATUS (X_BASE - 1)\n#endif\n")
check("endif_inside_block_comment_does_not_close",
      bool(err) and any("not defined" in m for m in err))

# Live tokens AFTER a block comment must not be silently dropped: this body
# compiles as -999, and resolving it to -1001 would put a value the kernel
# never uses into the generated contract AND the hash.
_, err = run("#define X_BASE (-1000)\n#define X_STATUS (X_BASE - 1) /* why */ + 2\n")
check("refuses_tokens_after_block_comment", bool(err))

# A trailing comment with nothing after it is the ordinary shape and must
# still resolve.
e, err = run("#define X_BASE (-1000)\n#define X_STATUS (X_BASE - 1)  /* why */\n")
check("strips_trailing_block_comment", not err and e[0][1] == -1001)

# Exit statuses are int32_t; a value Python can compute but C would truncate
# is a wrong answer with a certificate.
_, err = run("#define X_BASE (-2147483648)\n#define X_STATUS (X_BASE - 1)\n")
check("refuses_int32_underflow", bool(err) and any("int32" in m for m in err))

_, err = run("#define X_BASE (2147483647)\n#define X_STATUS (X_BASE + 1)\n")
check("refuses_int32_overflow", bool(err) and any("int32" in m for m in err))

# `int(x, 0)` RAISES on a leading-zero decimal, so without an explicit refusal
# the generator dies on a traceback instead of a named error.
_, err = run("#define X_BASE (-1000)\n#define X_STATUS (010)\n")
check("refuses_octal_literal", bool(err) and any("octal" in m for m in err))

# C translation phase 2 splices backslash-newline BEFORE comments are removed
# and BEFORE directives are recognised, so the generator does too. A body split
# across lines is one logical body, not a refusal.
e, err = run("#define X_BASE (-1000)\n#define X_STATUS (X_BASE \\\n - 1)\n")
check("resolves_spliced_body", not err and e[0][1] == -1001)

# The splice DELETES the backslash-newline, so a split identifier rejoins. A
# physical-line scanner never sees this as a definition at all.
e, err = run("#define X_BASE (-1000)\n#define X_STA\\\nTUS (X_BASE - 1)\n")
check("resolves_spliced_identifier", not err and e[0][1] == -1001)

# The shape that proves the phase ORDER: a continued `//` comment absorbs the
# following `#endif`, so the `#if 0` is still open and the define is inside it.
# clang leaves the constant UNDEFINED here; a physical-line scanner resolved it
# to a concrete value and would have put a number the kernel never uses into
# the contract and the hash. Now the compiler answers, so it is undefined.
_, err = run("#define X_BASE (-1000)\n#if 0\n// c \\\n#endif\n"
             "#define X_STATUS (X_BASE - 1)\n#endif\n")
check("spliced_line_comment_swallows_endif",
      bool(err) and any("not defined" in m for m in err))

# A backslash followed by TRAILING WHITESPACE splices too, so the same shape
# one space wider must reach the same conclusion.
_, err = run("#define X_BASE (-1000)\n#if 0\n// c \\   \n#endif\n"
             "#define X_STATUS (X_BASE - 1)\n#endif\n")
check("whitespace_tailed_continuation_also_splices",
      bool(err) and any("not defined" in m for m in err))

# A `/*` inside a STRING literal is two characters, not a comment opener, so
# the ACTIVE arm here is the #else one. The old scanner opened a synthetic
# comment, swallowed that arm, and certified -1001 with no error; clang expands
# it to -1002, which is now what the generator returns.
e, err = run('#define X_BASE (-1000)\n#if 0\n#define S "/*"\n'
             '#define X_STATUS (X_BASE - 1)\n#else\n'
             '#define X_STATUS (X_BASE - 2)\n#endif\n')
check("string_literal_does_not_open_comment", not err and e[0][1] == -1002)

# The same state must not break the ordinary case: a lone slash in a CHAR
# literal is not a comment either.
e, err = run("#define X_BASE (-1000)\n#define C '/'\n#define X_STATUS (X_BASE - 1)\n")
check("char_literal_slash_is_not_a_comment", not err and e[0][1] == -1001)

# A valid C header cannot carry a stray #endif. The scanner used to detect this
# itself as "my model of the file diverged from the compiler's"; now the
# compiler rejects the header outright and the generator surfaces that refusal
# rather than certifying a value out of a file clang would not accept.
_, err = run("#define X_BASE (-1000)\n#endif\n#define X_STATUS (X_BASE - 1)\n")
check("refuses_unmatched_endif",
      bool(err) and any("REFUSED" in m for m in err))

e, err = run("#define X_BASE (-0x3E8)\n#define X_STATUS (X_BASE - 0x1)\n")
check("resolves_hex_literals", not err and e[0][1] == -1001)

_, err = run("#define X_BASE (X_STATUS - 1)\n#define X_STATUS (X_BASE - 1)\n")
check("refuses_cyclic_expression", bool(err) and any("cyclic" in m for m in err))

_, err = run("#define X_BASE (-1000)\n#define X_STATUS ((X_BASE - 1)\n")
check("refuses_unbalanced_parens", bool(err))

_, err = run("#define X_BASE (-1000)\n#define X_STATUS (X_BASE) + (1)\n")
check("refuses_multi_term_expression", bool(err))

# --- ABI fingerprint boundary ------------------------------------------
# The header-text assertions below prove what is EMITTED; these prove what is
# HASHED, which is the half that decides whether a stale ring-3 binary is
# rejected at crt0. Without them, dropping the EXIT loop or feeding it the
# wrong list would pass every other fixture once the contract was regenerated.
def exit_entries(source):
    e, err = run(source)
    assert not err, err
    return e

_A = exit_entries("#define X_BASE (-1000)\n#define X_STATUS (X_BASE - 1)\n")
_B = exit_entries("#define X_BASE (-2000)\n#define X_STATUS (X_BASE - 1)\n")
_C = exit_entries("#define X_BASE (-2000)\n#define X_STATUS (-1001)\n")

def h(exit_status):
    return gua.compute_abi_hash([], [], exit_status, [], [])

check("hash_moves_on_exported_value_change", h(_A) != h(_B))
# A resolve-only helper is not a ring-3 contract: rewriting it while the
# EXPORTED value is unchanged must NOT skew every existing binary.
check("hash_stable_when_only_resolve_only_changes", h(_A) == h(_C))
# The EXIT tag must domain-separate: the same (name, literal) pair fed to the
# SYS slot has to produce a different fingerprint.
check("hash_exit_tag_is_domain_separated",
      gua.compute_abi_hash([], [], _A, [], []) !=
      gua.compute_abi_hash(_A, [], [], [], []))

# --- shipped repo invariants -------------------------------------------
entries, errors = gua.resolve_exit_statuses(
    gua.TASK_H, gua.EXIT_STATUS_EXPORT, gua.EXIT_STATUS_RESOLVE_ONLY)
check("repo_task_h_resolves_clean", not errors)
check("repo_exports_exec_image_destroyed",
      [n for n, _v, _l in entries] == ['TASK_EXIT_EXEC_IMAGE_DESTROYED'])
check("repo_exec_image_destroyed_is_negative",
      bool(entries) and entries[0][1] < 0)

# The SINGLE generated artifact -- user/include/abi_numbers.h is a static shim
# over it and carries no constants of its own, so reading the shim here would
# assert nothing.
header = open(gua.OUT_CONTRACT, encoding="utf-8").read()
check("generated_header_carries_constant",
      "#define TASK_EXIT_EXEC_IMAGE_DESTROYED" in header)
# The resolve-only base must stay OUT of the generated header, or a refactor
# of an internal allocation base becomes system-wide binary version skew.
check("generated_header_omits_resolve_only_base",
      "TASK_EXIT_REASON_BASE" not in header)

# --- s26: the compiler is the authority -------------------------------
# Everything above proves what the EXTRACTOR proposes. These prove the step
# that decides whether a proposal may be published at all. Without them the
# extractor could go back to being trusted on its own word, which is exactly
# the arrangement five adversarial rounds kept breaking.
_CLANG, _CLANG_ERR = gua.clang_binary()
check("clang_binary_resolves", _CLANG_ERR is None and bool(_CLANG))

_ONE_FLAVOR = [{'KERNEL_TESTS': 'on', 'EXCEPT_TELEMETRY': 'on',
                'BUILD_ALT_BOOT': 'off'}]

def certify(entries, kind=None, headers=None, flavors=None):
    return gua.certify_values(headers or [gua.TASK_H],
                              [('probe', kind or gua.CERTIFY_SIGNED32, entries)],
                              _CLANG, flavors=flavors)

# The value the repo actually ships must certify in EVERY build flavor.
check("certifies_real_exit_status",
      not certify([('TASK_EXIT_EXEC_IMAGE_DESTROYED', -1001, '(-1001)')]))

# A WRONG proposal is the whole point: it must fail the build, not be frozen
# into the generated contract plus an ABI hash.
check("certification_rejects_wrong_value",
      bool(certify([('TASK_EXIT_EXEC_IMAGE_DESTROYED', -9999, '(-9999)')],
                   flavors=_ONE_FLAVOR)))

# STATUS_* is published as an unsigned hex bit pattern while the kernel spells
# it ((NTSTATUS)0xC0000008), a NEGATIVE int32. A bare == would compare a
# negative int against a large unsigned and prove the wrong thing, so the
# comparison type is part of the claim.
check("certifies_ntstatus_bit_pattern",
      not certify([('STATUS_INVALID_HANDLE', 0xC0000008, '0xC0000008')],
                  kind=gua.CERTIFY_UNSIGNED32, headers=[gua.NTSTATUS_H]))
check("ntstatus_wrong_bit_pattern_rejected",
      bool(certify([('STATUS_INVALID_HANDLE', 0xC0000009, '0xC0000009')],
                   kind=gua.CERTIFY_UNSIGNED32, headers=[gua.NTSTATUS_H],
                   flavors=_ONE_FLAVOR)))

# The CONTRACT is committed once and included by every flavor, so a constant
# whose value depends on the flavor has no single publishable value. It must be
# refused rather than silently frozen at whichever flavor generated it.
def flavor_probe(source, entries):
    fd, path = tempfile.mkstemp(suffix=".h")
    try:
        with os.fdopen(fd, "w") as f:
            f.write(source)
        return gua.certify_values(
            [path], [('probe', gua.CERTIFY_SIGNED32, entries)], _CLANG)
    finally:
        os.unlink(path)

check("refuses_flavor_dependent_value",
      bool(flavor_probe("#ifdef KERNEL_TESTS\n#define X_FLAVOR 1\n"
                        "#else\n#define X_FLAVOR 2\n#endif\n",
                        [('X_FLAVOR', 1, '1')])))
check("accepts_flavor_invariant_value",
      not flavor_probe("#define X_STABLE 7\n", [('X_STABLE', 7, '7')]))

# Every flavor axis in the Makefile must be swept; dropping one silently
# narrows the invariance claim to the axes someone remembered.
_COMBOS, _COMBOS_ERR = gua.flavor_combinations()
check("flavor_sweep_covers_every_axis",
      _COMBOS_ERR is None and len(_COMBOS) == 12 and
      {k for c in _COMBOS for k in c} ==
      {'KERNEL_TESTS', 'EXCEPT_TELEMETRY', 'BUILD_ALT_BOOT'})
# The axis matrix and the compiler come from the MAKEFILE, not a Python mirror:
# a mirror would let a newly added axis silently narrow the invariance sweep
# while every check stayed green.
_CC_CFG, _AXES_CFG, _CFG_ERR = gua.abi_config()
check("axis_matrix_comes_from_the_makefile",
      _CFG_ERR is None and
      [a for a, _v in _AXES_CFG] ==
      ['KERNEL_TESTS', 'EXCEPT_TELEMETRY', 'BUILD_ALT_BOOT'])
check("compiler_comes_from_the_makefile",
      _CFG_ERR is None and _CC_CFG == 'clang-19')
# CONTRACT: the first state of each axis is the build DEFAULT, which is what
# lets extraction reuse the sweep's default pass instead of repeating it.
check("first_axis_state_is_the_build_default",
      _CFG_ERR is None and
      [v[0] for _a, v in _AXES_CFG] == ['on', 'on', 'off'] and
      _COMBOS[0] == {'KERNEL_TESTS': 'on', 'EXCEPT_TELEMETRY': 'on',
                     'BUILD_ALT_BOOT': 'off'})

# The flag vector is the Makefile's to define. If this script ever starts
# assembling its own, the translation context can drift from the kernel's
# while every generated artifact stays self-consistent -- the exact silent
# failure this section exists to remove.
_FLAGS, _FLAGS_ERR = gua.kernel_cpp_flags()
check("flag_vector_comes_from_the_makefile",
      _FLAGS_ERR is None and '-DCONFIG_SMP' in _FLAGS and
      '--target=x86_64-elf' in _FLAGS and '-Iinclude' in _FLAGS)
# The five ordered -I paths are part of the context, not decoration: a subset
# that merely looks equivalent can select a different definition. The committed
# ABI contract is deliberately NOT among them -- the shims reach it by a path
# relative to themselves, because a search path here would let the writable
# build tree shadow it (see the shadow fixture below).
check("flag_vector_carries_full_include_order",
      _FLAGS_ERR is None and
      [f for f in _FLAGS if f.startswith('-I')] ==
      ['-Iinclude', '-Isrc/kernel', '-Ibuild/generated', '-Isrc', '-Ibuild'])
# Dependency-file flags would litter stray .d files under -E/-fsyntax-only.
check("flag_vector_drops_dependency_only_flags",
      _FLAGS_ERR is None and not ({'-MMD', '-MP'} & set(_FLAGS)))
# Flavor overrides must actually reach the compiler, or the sweep is theatre.
_OFF, _ = gua.kernel_cpp_flags({'KERNEL_TESTS': 'off'})
check("flavor_override_changes_the_vector",
      _OFF is not None and '-UKERNEL_TESTS' in _OFF and
      '-DKERNEL_TESTS' not in _OFF)

# The differential that would have caught all five s23 findings in ONE pass:
# every constant the generator publishes, certified against the compiler in
# every flavor. Sampling a few names proves the mechanism; this proves the
# COVERAGE, which is the half that decays silently when a new allowlist entry
# lands. It re-derives the tables exactly as main() does.
_HDRS = [gua.SYSCALL_H, gua.SSDT_H, gua.NTSTATUS_H, gua.TASK_H]
_ev, _mac, _pperr = gua.run_preprocessor(_HDRS, _FLAGS, _CLANG)
check("full_table_preprocessor_query_succeeds", _pperr is None)
_sys, _sys_skip = gua.propose_defines(_mac, gua.SYSCALL_H, ('SYS_', 'FAULT_'))
_ssdt, _ssdt_skip = gua.propose_defines(_mac, gua.SSDT_H, 'SSDT_')
_nts, _nts_skip = gua.propose_defines(_mac, gua.NTSTATUS_H, 'STATUS_')
_exit, _exiterr = gua.resolve_exit_statuses(
    gua.TASK_H, gua.EXIT_STATUS_EXPORT, gua.EXIT_STATUS_RESOLVE_ONLY, _mac, _ev)
check("full_table_extraction_is_non_empty",
      not _exiterr and len(_sys) > 20 and len(_ssdt) > 20 and len(_nts) > 3)
# Nothing prefixed may be silently unreadable: a skipped name leaves BOTH the
# header and the FNV fingerprint while every artifact stays self-consistent.
check("no_prefixed_name_is_silently_skipped",
      not _sys_skip and not _ssdt_skip and not _nts_skip)
# Every allowlisted name must be PRESENT, not merely filtered: a rename yields
# a shorter table rather than an error unless this is checked.
check("every_allowlisted_name_is_present",
      not (set(gua.SSDT_USER_ALLOWLIST) - {n for n, _v, _l in _ssdt}) and
      not (set(gua.NTSTATUS_USER_ALLOWLIST) - {n for n, _v, _l in _nts}))
check("every_published_constant_is_compiler_certified",
      not gua.certify_values(_HDRS, [
          ('syscall', gua.CERTIFY_SIGNED32, _sys),
          ('ssdt', gua.CERTIFY_SIGNED32, _ssdt),
          ('ntstatus', gua.CERTIFY_UNSIGNED32, _nts),
          ('exit-status', gua.CERTIFY_SIGNED32, _exit),
      ], _CLANG))
# Ownership holds across the REAL include chain, not just fixture headers.
check("no_published_name_has_two_owners",
      not gua.check_single_ownership(
          _ev, [n for n, _v, _l in _sys + _ssdt + _nts]))

# --- s26: the gates are wired into PRODUCTION, not just callable -------
# Every check above calls a helper directly, so deleting the call from main()
# would leave all of them green -- and the repo's committed values are already
# correct, so even `--check` would stay quiet. These drive main() itself and
# assert BOTH the exit status and that no header was replaced.
_OUT_PAIR = (gua.OUT_CONTRACT,)

def main_with(patch, argv=('--check',)):
    """Run main() with one function replaced; returns (rc, headers_unchanged).

    Diagnostics are captured rather than printed: every injection below is
    SUPPOSED to fail, so its stderr is expected output, not suite noise. The
    captured text is asserted on separately so a refusal that fires with no
    explanation still counts as a failure."""
    before = [open(p, 'rb').read() for p in _OUT_PAIR]
    saved_argv, saved = sys.argv, {}
    for attr, fn in patch.items():
        saved[attr] = getattr(gua, attr)
        setattr(gua, attr, fn)
    sys.argv = ['gen-user-abi.py'] + list(argv)
    buf = io.StringIO()
    try:
        with contextlib.redirect_stderr(buf):
            rc = gua.main()
    except SystemExit as exc:
        rc = exc.code
    finally:
        sys.argv = saved_argv
        for attr, fn in saved.items():
            setattr(gua, attr, fn)
    after = [open(p, 'rb').read() for p in _OUT_PAIR]
    main_with.last_stderr = buf.getvalue()
    return (rc, before == after)


def refused(patch, must_say):
    """main() must exit 1, leave the generated contract untouched, and SAY why."""
    rc, unchanged = main_with(patch)
    return (rc, unchanged, must_say in main_with.last_stderr) == (1, True, True)

# Baseline: unpatched main() succeeds, so a failure below is the injection.
check("main_check_passes_unpatched", main_with({}) == (0, True))

# A wrong PROPOSED value must be caught by main()'s certification call. Without
# that call this returns 0 and the original silent-wrong-value class is back.
_real_propose = gua.propose_defines
def _wrong_value(macros, path, prefixes):
    entries, skipped = _real_propose(macros, path, prefixes)
    if entries and path == gua.SYSCALL_H:
        n, v, _l = entries[0]
        entries = [(n, v + 1, str(v + 1))] + entries[1:]
    return (entries, skipped)
check("main_rejects_a_wrong_proposed_value",
      refused({'propose_defines': _wrong_value},
              "does not match the generated header"))

# A skipped prefixed name must fail main(), not shrink the table: it would
# otherwise drop out of the ABI fingerprint too.
def _drop_one(macros, path, prefixes):
    entries, skipped = _real_propose(macros, path, prefixes)
    if entries and path == gua.SSDT_H:
        dropped = entries[0]
        entries = entries[1:]
        skipped = skipped + [(dropped[0], 'ALIAS_OF_SOMETHING_ELSE')]
    return (entries, skipped)
check("main_refuses_a_skipped_prefixed_name",
      refused({'propose_defines': _drop_one},
              "not a single integer literal"))

# An allowlisted name that disappeared must fail rather than yield a shorter
# table (the shape a kernel-side rename produces).
def _drop_allowlisted(macros, path, prefixes):
    entries, skipped = _real_propose(macros, path, prefixes)
    if path == gua.NTSTATUS_H:
        entries = [e for e in entries if e[0] not in gua.NTSTATUS_USER_ALLOWLIST]
    return (entries, skipped)
check("main_refuses_a_missing_allowlisted_name",
      refused({'propose_defines': _drop_allowlisted},
              "allowlist names absent"))

# Ownership must be enforced for SYS_/SSDT_/STATUS_, not only exit statuses.
_real_pp = gua.run_preprocessor
def _duplicate_owner(headers, flags, clang):
    events, final, err = _real_pp(headers, flags, clang)
    if err:
        return (events, final, err)
    name = next(n for n in final if n.startswith('SSDT_'))
    return (events + [('define', name, final[name])], final, None)
check("main_refuses_a_second_owner_for_a_published_name",
      refused({'run_preprocessor': _duplicate_owner},
              "Exactly one place must own"))

# A compiler that fails mid-sweep must fail main(), never be treated as clean.
def _broken_clang():
    return ('/nonexistent/clang-for-fixture', None)
check("main_refuses_when_the_compiler_cannot_run",
      refused({'clang_binary': _broken_clang}, "cannot run"))

# A flag vector that cannot be obtained is fail-closed too: without the kernel's
# own flags there is no authoritative translation context to read.
def _no_flags(flavor=None):
    return (None, 'fixture: make print-abi-cppflags unavailable')
check("main_refuses_without_a_flag_vector",
      refused({'kernel_cpp_flags': _no_flags},
              "print-abi-cppflags unavailable"))

# --- s26: certification refuses vacuous and out-of-range input ---------
# An empty assert set compiles trivially, so reporting success would tell a
# caller that lost its tables that the ABI is certified.
check("certification_refuses_empty_tables",
      bool(gua.certify_values([gua.TASK_H],
                              [('probe', gua.CERTIFY_SIGNED32, [])], _CLANG)))

# Both sides of an unsigned assert are 32-bit, so an oversized proposal would
# certify TRUNCATED while format_block emitted the untruncated literal --
# certifying one number and publishing another.
check("rejects_unsigned_value_wider_than_uint32",
      gua.certify_range_error('X', 0x1C0000008, gua.CERTIFY_UNSIGNED32, 't')
      is not None)
check("accepts_unsigned_value_at_uint32_max",
      gua.certify_range_error('X', 0xFFFFFFFF, gua.CERTIFY_UNSIGNED32, 't')
      is None)
check("rejects_negative_unsigned_value",
      gua.certify_range_error('X', -1, gua.CERTIFY_UNSIGNED32, 't') is not None)
check("rejects_signed_value_outside_int32",
      gua.certify_range_error('X', 1 << 31, gua.CERTIFY_SIGNED32, 't')
      is not None)
check("accepts_signed_value_at_int32_bounds",
      gua.certify_range_error('X', (1 << 31) - 1, gua.CERTIFY_SIGNED32, 't')
      is None and
      gua.certify_range_error('X', -(1 << 31), gua.CERTIFY_SIGNED32, 't')
      is None)
# An out-of-range proposal must be refused BEFORE an assert is generated, or
# the mask in _assert_line silently makes it pass.
check("out_of_range_value_never_reaches_an_assert",
      bool(gua.certify_values([gua.NTSTATUS_H],
           [('probe', gua.CERTIFY_UNSIGNED32, [('STATUS_SUCCESS', 0x1FFFFFFFF,
                                                '0x1FFFFFFFF')])],
           _CLANG, flavors=_ONE_FLAVOR)))

# --- s26 round 2: the INVENTORY is cross-flavor, not just the values ---
# The flavor sweep can only assert names the extraction pass discovered, so a
# constant defined under one flavor and not another was invisible to the
# candidates, the skip list, ownership, the header, the FNV hash AND all 12
# certification units -- every one of which still compiled.
def flavor_names(source, prefixes=('SYS_',)):
    fd, path = tempfile.mkstemp(suffix=".h")
    try:
        with os.fdopen(fd, "w") as f:
            f.write(source)
        seen, _ev, _mac, errs = gua.cross_flavor_names([path], _CLANG, prefixes)
        return (seen, errs)
    finally:
        os.unlink(path)

_seen, _ferrs = flavor_names("#ifndef KERNEL_TESTS\n"
                             "#define SYS_ONLY_WHEN_TESTS_OFF 90\n#endif\n"
                             "#define SYS_ALWAYS 5\n")
check("refuses_a_name_defined_in_only_some_flavors",
      any("SYS_ONLY_WHEN_TESTS_OFF" in e and "build flavors" in e
          for e in _ferrs))
check("accepts_an_unconditionally_defined_name",
      "SYS_ALWAYS" in _seen and
      not any("SYS_ALWAYS" in e for e in _ferrs))
check("cross_flavor_inventory_sees_every_flavor",
      _seen.get("SYS_ALWAYS") and len(_seen["SYS_ALWAYS"]) == 12)
# The shipped tree must itself be free of flavor-conditional ABI names.
check("repo_has_no_flavor_conditional_abi_name",
      not gua.cross_flavor_names(
          _HDRS, _CLANG, ('SYS_', 'FAULT_', 'SSDT_', 'STATUS_'))[3])
# The sweep must hand back the DEFAULT flavor's extraction, or main() would be
# preprocessing that context a second time for nothing.
_cf_seen, _cf_ev, _cf_mac, _cf_err = gua.cross_flavor_names(
    _HDRS, _CLANG, ('SYS_',))
# Every cross_flavor_names exit must be the documented FOUR-tuple: a two-value
# early return turned a config error into `ValueError: not enough values to
# unpack` at the caller instead of the intended diagnostic.
_empty = gua.cross_flavor_names(_HDRS, _CLANG, ('SYS_',), flavors=[])
check("empty_flavor_matrix_returns_four_values", len(_empty) == 4)
check("empty_flavor_matrix_is_refused",
      bool(_empty[3]) and 'empty' in _empty[3][0])
check("sweep_returns_the_default_flavor_extraction",
      not _cf_err and _cf_ev is not None and _cf_mac is not None and
      'SYS_WRITE' in _cf_mac)

# --- s26 round 2: TEB/KUSD offsets are compiler-certified --------------
# These feed the ABI hash, but were verified only by a REGEX over raw header
# text -- which accepts an assertion inside a comment or an inactive #if arm,
# and never notices a kernel-side assertion that was deleted. A field could
# move while a stale textual assertion held the fingerprint constant.
check("real_teb_and_kusd_offsets_certify",
      not gua.certify_values(
          [gua.TEB_HEADER, gua.KUSD_HEADER], [], _CLANG,
          offset_families=[('teb', 'TEB', gua.TEB_LAYOUT),
                           ('kusd', 'KUSER_SHARED_DATA', gua.KUSD_LAYOUT)]))
_BAD_TEB = [(f, o + 8 if f == 'ClientId' else o) for f, o in gua.TEB_LAYOUT]
check("a_moved_teb_field_fails_certification",
      bool(gua.certify_values([gua.TEB_HEADER], [], _CLANG, flavors=_ONE_FLAVOR,
                              offset_families=[('teb', 'TEB', _BAD_TEB)])))
_BAD_KUSD = [(f, o + 4 if f == 'TickCount' else o) for f, o in gua.KUSD_LAYOUT]
check("a_moved_kusd_field_fails_certification",
      bool(gua.certify_values([gua.KUSD_HEADER], [], _CLANG,
                              flavors=_ONE_FLAVOR,
                              offset_families=[('kusd', 'KUSER_SHARED_DATA',
                                                _BAD_KUSD)])))
# main() must apply BOTH, not merely be able to.
_real_certify = gua.certify_values
def _bad_layout_probe(headers, families, clang, flavors=None,
                      offset_families=None):
    # Bind the REAL function: calling gua.certify_values here would re-enter
    # this patched stand-in.
    return _real_certify(headers, families, clang, flavors,
                         [('teb', 'TEB', _BAD_TEB)])
check("main_certifies_the_hashed_struct_offsets",
      refused({'certify_values': _bad_layout_probe}, "offset does not match"))

# --- s26 round 3: one definition of the translation context ------------
# The flag vector had TWO spellings -- the print target and the generic compile
# rule each listed the five -I paths -- so "they match" was a hand-maintained
# claim. Both now expand $(KERNEL_TU_FLAGS); pin that neither re-inlines them.
_MK = open("Makefile", encoding="utf-8").read()
check("compile_rule_uses_the_shared_tu_vector",
      "$(CC) $(KERNEL_TU_FLAGS) -c $< -o $@" in _MK)
check("print_target_uses_the_shared_tu_vector",
      "@printf '%s\\n' $(KERNEL_TU_FLAGS)" in _MK)
check("tu_vector_has_exactly_one_definition",
      _MK.count("-I$(INCLUDE) -I$(KERNEL_DIR) -I$(GENERATED) \\") == 1)

# Flags are parsed LINE by line, not by whitespace: the targets emit one record
# per line precisely so a value containing a space survives, and split() would
# shred `-DX='a b'` into two arguments -- a translation context the kernel never
# used, agreed on by every generated artifact.
check("flag_vector_is_parsed_line_by_line",
      "proc.stdout.splitlines()" in
      open("scripts/gen-user-abi.py", encoding="utf-8").read())
_ml, _mlerr = gua.make_query('print-abi-config')
check("make_query_returns_whole_lines",
      _mlerr is None and any(ln.startswith('FLAVOR=') and ',' in ln
                             for ln in _ml))
check("make_query_names_a_missing_target",
      gua.make_query('print-abi-no-such-target')[1] is not None)

# Offset assertions are discovered in the PREPROCESSED text, so a commented-out
# or dead-arm assertion is not counted and a named-constant offset is seen.
# Production reads the assertions out of the PREPROCESSED layout headers.
# Preprocess the same two headers the generator does and require parity with
# the raw scan on the shipped tree (they agree today), so this fixture pins the
# WIRING rather than merely that a list comes back.
_LAY_EV, _LAY_MAC, _LAY_ERR = gua.run_preprocessor(
    [gua.TEB_HEADER, gua.KUSD_HEADER], _FLAGS, _CLANG)
check("layout_headers_preprocess_cleanly", _LAY_ERR is None and _LAY_EV.text)
_PP_TEB = gua.parse_kernel_asserts(gua.TEB_HEADER, 'TEB', _LAY_EV.text)
_PP_KUSD = gua.parse_kernel_asserts(gua.KUSD_HEADER, 'KUSER_SHARED_DATA',
                                    _LAY_EV.text)
check("preprocessed_scan_finds_the_shipped_teb_assertions",
      len(_PP_TEB) == len(gua.TEB_LAYOUT) and
      sorted(_PP_TEB) == sorted(gua.parse_kernel_asserts(gua.TEB_HEADER, 'TEB')))
check("preprocessed_scan_finds_the_shipped_kusd_assertions",
      len(_PP_KUSD) == len(gua.KUSD_LAYOUT))
# The property the coverage gate exists for: a field the kernel pins but the
# hash-input list omits must be refused, or it drifts outside the fingerprint.
check("unlisted_asserted_field_is_refused",
      bool(gua.check_coverage(list(gua.TEB_LAYOUT),
                              _PP_TEB + [('GhostField', 0x999)], 'TEB')))
check("main_reads_layout_assertions_from_preprocessed_text",
      "parse_kernel_asserts(TEB_HEADER, 'TEB', layout_text)" in
      open("scripts/gen-user-abi.py", encoding="utf-8").read())
# Comments and dead arms are removed by the PREPROCESSOR, not by the regex --
# which is exactly why the scan must run on preprocessed text. Drive the real
# path: a commented assertion and one in an inactive #if arm must both vanish,
# while the live one survives.
def _pp_asserts(source, struct='TEB'):
    fd, path = tempfile.mkstemp(suffix=".h")
    try:
        with os.fdopen(fd, "w") as f:
            f.write(source)
        ev, _mac, err = gua.run_preprocessor([path], _FLAGS, _CLANG)
        assert not err, err
        return gua.parse_kernel_asserts(path, struct, ev.text)
    finally:
        os.unlink(path)

check("commented_offset_assertion_is_not_counted",
      not _pp_asserts('struct TEB { int a; };\n'
                      '/* _Static_assert(__builtin_offsetof(TEB, a) == 0x99,'
                      ' "x"); */\n'))
check("inactive_arm_offset_assertion_is_not_counted",
      not _pp_asserts('struct TEB { int a; };\n#if 0\n'
                      '_Static_assert(__builtin_offsetof(TEB, a) == 0x99,'
                      ' "x");\n#endif\n'))
check("named_constant_offset_is_counted_after_expansion",
      _pp_asserts('struct TEB { int a; int b; };\n#define B_OFF 4\n'
                  '_Static_assert(__builtin_offsetof(TEB, b) == B_OFF, "x");\n')
      == [('b', 4)])
check("reversed_offset_comparison_is_counted",
      gua.parse_kernel_asserts(
          'x.h', 'TEB',
          '_Static_assert(0x30 == __builtin_offsetof(TEB, NtTib.Self), "x");')
      == [('NtTib.Self', 0x30)])
check("forward_offset_comparison_is_counted",
      gua.parse_kernel_asserts(
          'x.h', 'TEB',
          '_Static_assert(__builtin_offsetof(TEB, NtTib.Self) == 0x30, "x");')
      == [('NtTib.Self', 0x30)])

# --- s26: propose_defines skip contract --------------------------------
# The three body shapes that are NOT publishable integers must be REPORTED as
# skips rather than quietly omitted.
_SKIP_MACROS = {'SYS_ALIAS': 'SYS_OTHER', 'SYS_EMPTY': '',
                'SYS_FUNCLIKE': None, 'SYS_GOOD': '7',
                'SYS_TWO_LITERALS': '(1 + 2)'}
_kept, _skips = gua.propose_defines(_SKIP_MACROS, gua.SYSCALL_H, 'SYS_')
check("skip_contract_keeps_only_plain_integers",
      [n for n, _v, _l in _kept] == ['SYS_GOOD'])
check("skip_contract_reports_alias_empty_funclike_and_multiliteral",
      {n for n, _b in _skips} ==
      {'SYS_ALIAS', 'SYS_EMPTY', 'SYS_FUNCLIKE', 'SYS_TWO_LITERALS'})
check("skip_contract_handles_an_empty_macro_map",
      gua.propose_defines({}, gua.SYSCALL_H, 'SYS_') == ([], []))

# clang absent is an explicit REFUSAL. A silent skip would turn the ABI drift
# gate off on exactly the hosts least likely to notice, and a fallback to the
# old scanner would re-open the class this work closed.
_saved = os.environ.get('ABI_CLANG')
os.environ['ABI_CLANG'] = 'clang-does-not-exist-for-this-fixture'
try:
    _p, _r = gua.clang_binary()
    check("absent_compiler_is_a_named_refusal",
          _p is None and _r is not None and 'not found on PATH' in _r)
    check("absent_compiler_names_the_remedy",
          _r is not None and 'setup.sh' in _r and 'ABI_CLANG' in _r)
finally:
    if _saved is None:
        del os.environ['ABI_CLANG']
    else:
        os.environ['ABI_CLANG'] = _saved
PYGUA
)
GUA_OK=$(echo "$GUA_OUT" | grep -c "^OK ")
GUA_EXPECT=109
if [ "$GUA_OK" = "$GUA_EXPECT" ]; then
    echo "$GUA_OUT" | grep "^OK " | while IFS= read -r line; do
        [ "$QUIET" = "0" ] && echo -e "  ${GREEN}PASS${NC}  gen_user_abi: $line"
    done
    PASS=$((PASS + GUA_EXPECT))
else
    t_fail "gen_user_abi: exit-status resolver coverage incomplete" \
           "ok=$GUA_OK/$GUA_EXPECT out=$GUA_OUT"
fi

# The canonical build path must RUN the drift check. `make all` lists check-abi
# as a prerequisite, but CLAUDE.md forbids raw `make` and scripts/build.sh drove
# `kernel`/`userland` directly, so the guard was off on the only path in use:
# a kernel-side constant change compiled into the kernel while the generated
# contract kept the old value, stayed self-consistent, and passed the crt0
# handshake. Pin that build.sh invokes the generator's --check.
if grep -qE 'gen-user-abi\.py --check' "$SCRIPT_DIR/build.sh"; then
    t_pass "gen_user_abi: scripts/build.sh runs the ABI drift check"
else
    t_fail "gen_user_abi: scripts/build.sh runs the ABI drift check" \
           "the canonical build path must not bypass check-abi"
fi

# The six cases below all drive the REAL main() --check / --write-shims logic,
# but against THROWAWAY contract and facade paths, by redirecting the module
# constants in-process. Nothing tracked is ever mutated.
#
# Earlier revisions corrupted the real contract and replaced the real facade
# with FIFOs and directories, restoring afterwards -- first by fallthrough, then
# under EXIT/INT/TERM traps. Neither survives SIGKILL, and an interrupted run
# really did leave this checkout in a state that stalled a later lint pass. Even
# a clean restore rewrote ctime and, after rm+cp, the inode. A subprocess cannot
# be redirected this way (a child resolves the generator's own constants), so
# these run in-process and assert main()'s exit status directly.
#
# The probe directory is UNIQUE per run and created exclusively: a fixed
# build/shimprobe could be left behind as a symlink by an interrupted run, and
# write_text() would then follow it straight out of the repository -- the exact
# data-loss class the repair case below exists to prove is closed. It sits TWO
# levels below the repo root so a facade's relative
# `../../abi/generated/abi_contract.h` resolves to the real contract, exactly as
# it does from user/include/ and include/kernel/.
_GUA_MAIN_OUT="SKIPPED: build/ unusable for scratch state"
[ "$_GUA_SCRATCH_OK" = "1" ] && _GUA_MAIN_OUT=$(cd "$REPO_ROOT" && python3 - <<'PYMAIN'
import contextlib, importlib.util, io, os, pathlib, stat, sys, tempfile

spec = importlib.util.spec_from_file_location("gua", "scripts/gen-user-abi.py")
gua = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gua)

problems = []
probe = pathlib.Path(tempfile.mkdtemp(prefix='shimprobe-',
                                      dir=os.path.join(gua.REPO_ROOT, 'build')))
REAL = (gua.OUT_CONTRACT, gua.SHIM_USER_H, gua.SHIM_KERNEL_H)
real_contract = pathlib.Path(gua.OUT_CONTRACT).read_text()

def run_main(argv):
    """main() with diagnostics captured -- every case here is SUPPOSED to
    fail, so its stderr is expected output, not suite noise."""
    saved = sys.argv
    sys.argv = ['gen-user-abi.py'] + list(argv)
    buf = io.StringIO()
    try:
        # stdout as well as stderr: --write-shims reports what it wrote, and
        # that chatter would otherwise land in this fixture's captured verdict.
        with contextlib.redirect_stderr(buf), contextlib.redirect_stdout(buf):
            rc = gua.main()
    except SystemExit as exc:
        rc = exc.code
    finally:
        sys.argv = saved
    return rc, buf.getvalue()

def reset():
    gua.OUT_CONTRACT, gua.SHIM_USER_H, gua.SHIM_KERNEL_H = REAL

# These cases assert CONTRACT/FACADE validation, which sits downstream of the
# flavor sweep and the certification compiles. Running the full 12-flavor
# inventory for each of them re-did ~39 clang subprocesses per case to reach the
# same starting point -- the bulk of this suite's 90s -> 3m11s regression. Pin
# the sweep to the DEFAULT flavor here; the full sweep is still exercised by the
# real --check on every build and by the s26 flavor fixtures that own it.
_real_flavors = gua.flavor_combinations
gua.flavor_combinations = lambda: ([{}], None)

def case(label, setup, want_needle):
    """Redirect, apply a hostile setup, require --check to REFUSE and say why."""
    d = pathlib.Path(tempfile.mkdtemp(prefix='c-', dir=probe))
    try:
        gua.OUT_CONTRACT = str(d / 'abi_contract.h')
        gua.SHIM_USER_H = str(d / 'abi_numbers.h')
        gua.SHIM_KERNEL_H = str(d / 'abi_hash.h')
        # A canonical starting point, so only the mutation under test differs.
        pathlib.Path(gua.OUT_CONTRACT).write_text(real_contract)
        pathlib.Path(gua.SHIM_USER_H).write_text(gua.CANONICAL_SHIM_USER)
        pathlib.Path(gua.SHIM_KERNEL_H).write_text(gua.CANONICAL_SHIM_KERNEL)
        setup(d)
        rc, err = run_main(['--check'])
        if rc != 1:
            problems.append(f'{label}: expected rc=1, got {rc}: {err.strip()[:200]}')
        elif want_needle not in err:
            problems.append(f'{label}: expected {want_needle!r}, got: {err.strip()[:200]}')
    finally:
        reset()

def corrupt_contract(d):
    p = pathlib.Path(gua.OUT_CONTRACT)
    t = p.read_text()
    c = t.replace('#define IMPOSSIBLE_OS_ABI_HASH   0x',
                  '#define IMPOSSIBLE_OS_ABI_HASH   0xDEADBEEF')
    if c == t:
        problems.append('corrupted-contract: literal not found; the template '
                        'was renamed and this fixture stopped asserting')
    p.write_text(c)

case('--check rejects a corrupted generated contract', corrupt_contract,
     'is stale vs kernel source')

case('--check rejects the comment-lexing facade bypass',
     lambda d: pathlib.Path(gua.SHIM_USER_H).write_text(
         gua.CANONICAL_SHIM_USER +
         '// /*\n#ifdef NULL\n#undef SYS_WRITE\n#define SYS_WRITE 999\n'
         '#endif\n// */\n'),
     'differs from the pinned facade')

def fifo_facade(d):
    p = pathlib.Path(gua.SHIM_USER_H)
    p.unlink()
    os.mkfifo(p)

case('a FIFO facade is refused, not blocked on', fifo_facade,
     'is not a regular file')

def dir_facade(d):
    p = pathlib.Path(gua.SHIM_USER_H)
    p.unlink()
    p.mkdir()

case('a directory facade names a remedy that works', dir_facade,
     'remove or move it first')

def fifo_contract(d):
    p = pathlib.Path(gua.OUT_CONTRACT)
    p.unlink()
    os.mkfifo(p)

case('a FIFO generated contract is refused, not blocked on', fifo_contract,
     'must be a regular file')

def stale_contract_including_fifo(d):
    fifo = d / 'stale-include.h'
    os.mkfifo(fifo)
    pathlib.Path(gua.OUT_CONTRACT).write_text(f'#include "{fifo}"\n')

case('a stale regular contract never reaches clang',
     stale_contract_including_fifo, 'is stale vs kernel source')

# The --write-shims BRANCH, not just the helper underneath it: argparse, the
# flag, and the loop that repairs BOTH facades. Calling publish_atomically
# directly would still pass if the command repaired only one facade, or skipped
# the branch entirely, while the gate tells users to run exactly that command.
d = pathlib.Path(tempfile.mkdtemp(prefix='w-', dir=probe))
sentinel = d / 'sentinel.txt'
sentinel.write_text('SENTINEL-MUST-SURVIVE\n')
try:
    gua.SHIM_USER_H = str(d / 'abi_numbers.h')
    gua.SHIM_KERNEL_H = str(d / 'abi_hash.h')
    # The ring-3 facade starts as a SYMLINK at an external file: repair must
    # replace the link, never write through it.
    os.symlink(sentinel, gua.SHIM_USER_H)
    rc, err = run_main(['--write-shims'])
    if rc != 0:
        problems.append(f'write-shims: expected rc=0, got {rc}: {err.strip()[:200]}')
    if sentinel.read_text() != 'SENTINEL-MUST-SURVIVE\n':
        problems.append('write-shims: overwrote a symlink target outside the '
                        'repository instead of replacing the link')
    for path, want in ((gua.SHIM_USER_H, gua.CANONICAL_SHIM_USER),
                       (gua.SHIM_KERNEL_H, gua.CANONICAL_SHIM_KERNEL)):
        st = os.lstat(path)
        if not stat.S_ISREG(st.st_mode):
            problems.append(f'write-shims: {path} is not a regular file after '
                            f'repair')
        elif pathlib.Path(path).read_text() != want:
            problems.append(f'write-shims: {path} does not hold canonical bytes '
                            f'after repair')
finally:
    reset()

gua.flavor_combinations = _real_flavors

for root, dirs, files in os.walk(probe, topdown=False):
    for n in files:
        os.unlink(os.path.join(root, n))
    for n in dirs:
        os.rmdir(os.path.join(root, n))
os.rmdir(probe)

print('OK' if not problems else '; '.join(problems))
PYMAIN
)
if [ "$_GUA_MAIN_OUT" = "OK" ]; then
    t_pass "gen_user_abi: --check and --write-shims refuse every hostile ABI file"
else
    t_fail "gen_user_abi: --check and --write-shims refuse every hostile ABI file" \
           "$_GUA_MAIN_OUT"
fi

# PUBLICATION ATOMICITY -- the property the old two-destination design could not
# offer. A timed SIGKILL is the wrong instrument: it is flaky and can miss the
# publication boundary indefinitely. Instead wrap os.replace and terminate
# DETERMINISTICALLY at each side of it, then assert the tree holds exactly one
# complete generation -- the old file or the new file, never a blend and never a
# leftover temp. Runs against a throwaway destination so the repo tree is never
# the experiment.
_GUA_ATOMIC_OUT="SKIPPED: build/ unusable for scratch state"
[ "$_GUA_SCRATCH_OK" = "1" ] && _GUA_ATOMIC_OUT=$(cd "$REPO_ROOT" && python3 - <<'PYATOMIC'
import importlib.util, os, pathlib, stat, tempfile

spec = importlib.util.spec_from_file_location("gua", "scripts/gen-user-abi.py")
gua = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gua)

OLD = "OLD-GENERATION\n"
NEW = "NEW-GENERATION\n"

class Crash(Exception):
    pass

problems = []

# Drive the PRODUCTION publisher, not a transcription of it. The fixture used
# to declare its own publish(), so it certified a copy: reordering the replace
# before the write, or replacing the wrong path, would not have failed it. The
# hooks fire immediately either side of the real os.replace.
for crash_at, expected in (('before', OLD), ('after', NEW)):
    d = tempfile.mkdtemp(dir=os.path.join(gua.REPO_ROOT, 'build'))
    dest = os.path.join(d, 'abi_contract.h')
    pathlib.Path(dest).write_text(OLD)
    os.chmod(dest, 0o644)
    hook = (lambda *_: (_ for _ in ()).throw(Crash())) if crash_at == 'before' \
        else None
    after = (lambda *_: (_ for _ in ()).throw(Crash())) if crash_at == 'after' \
        else None
    try:
        gua.publish_atomically(dest, NEW, before_replace=hook, after_replace=after)
    except Crash:
        pass
    got = pathlib.Path(dest).read_text()
    if got != expected:
        problems.append(f'crash-{crash_at}: destination is {got!r}, expected '
                        f'the complete {expected!r}')
    # Residue. This injects an EXCEPTION at each side of the commit point, and
    # the production publisher unwinds it by unlinking the staging file, so
    # neither side leaves anything -- which is a STRONGER property than the
    # section originally claimed and is what is asserted here.
    #
    # It is not the same as a SIGKILL. A signal runs no handler, so a real
    # abrupt termination before the commit point does leave the staging file
    # behind; that residue is inert (a uniquely-named .tmp nothing reads, which
    # can never take the published name) and is not reachable from this
    # injection. The distinction is recorded rather than papered over: a timed
    # SIGKILL was rejected as an instrument precisely because it is flaky.
    stray = [n for n in os.listdir(d) if n != 'abi_contract.h']
    if stray:
        problems.append(f'crash-{crash_at}: an exception during publication '
                        f'must unwind its staging file, found {stray}')
    # Mode must survive publication: mkstemp makes 0600, which would leave the
    # ABI headers private to the invoking user in a shared checkout.
    if crash_at == 'after':
        mode = stat.S_IMODE(os.stat(dest).st_mode)
        if mode != 0o644:
            problems.append(f'published mode is {mode:#o}, expected the '
                            f'destination mode 0o644 to be preserved')
    for n in os.listdir(d):
        os.unlink(os.path.join(d, n))
    os.rmdir(d)

# A brand-new destination lands at 0644 MODULO UMASK -- asserting group/other
# readability unconditionally would fail a developer running a legitimately
# restrictive umask (0027 -> 0640, 0077 -> 0600), turning a correct
# implementation into a red tooling gate.
for umask_val in (0o022, 0o027, 0o077):
    d = tempfile.mkdtemp(dir=os.path.join(gua.REPO_ROOT, 'build'))
    dest = os.path.join(d, 'abi_contract.h')
    old_umask = os.umask(umask_val)
    try:
        gua.publish_atomically(dest, NEW)
    finally:
        os.umask(old_umask)
    mode = stat.S_IMODE(os.stat(dest).st_mode)
    want = 0o644 & ~umask_val
    if mode != want:
        problems.append(f'a new destination under umask {umask_val:#o} '
                        f'published as {mode:#o}, expected {want:#o}')
    os.unlink(dest)
    os.rmdir(d)

# An EXISTING destination keeps its mode, including a deliberately private one:
# preservation must not silently widen a file someone restricted on purpose.
for existing in (0o600, 0o640, 0o644):
    d = tempfile.mkdtemp(dir=os.path.join(gua.REPO_ROOT, 'build'))
    dest = os.path.join(d, 'abi_contract.h')
    pathlib.Path(dest).write_text(OLD)
    os.chmod(dest, existing)
    gua.publish_atomically(dest, NEW)
    mode = stat.S_IMODE(os.stat(dest).st_mode)
    if mode != existing:
        problems.append(f'an existing {existing:#o} destination became '
                        f'{mode:#o}; publication must preserve it')
    os.unlink(dest)
    os.rmdir(d)

# The unchanged-content shortcut must be gated on lstat, not on open(). A
# destination symlinked at a target that ALREADY holds the canonical bytes
# compared equal through open(), so publication declined to act and left the
# abnormal link in place -- the repair command silently refusing to repair.
d = tempfile.mkdtemp(dir=os.path.join(gua.REPO_ROOT, 'build'))
dest = os.path.join(d, 'abi_contract.h')
target = os.path.join(d, 'target.h')
pathlib.Path(target).write_text(NEW)
os.symlink(target, dest)
gua.publish_atomically(dest, NEW)
if os.path.islink(dest):
    problems.append('a destination symlink whose target held the canonical '
                    'bytes was left in place; the shortcut followed the link')
if pathlib.Path(target).read_text() != NEW:
    problems.append('publication wrote through the destination symlink instead '
                    'of replacing it')
for n in os.listdir(d):
    os.unlink(os.path.join(d, n))
os.rmdir(d)

# Unchanged input must rewrite NOTHING observable -- same inode, same mtime.
# Republishing unconditionally would re-trigger every downstream make rule and
# file watcher for an ABI that did not move.
d = tempfile.mkdtemp(dir=os.path.join(gua.REPO_ROOT, 'build'))
dest = os.path.join(d, 'abi_contract.h')
gua.publish_atomically(dest, NEW)
before = os.stat(dest)
gua.publish_atomically(dest, NEW)
after = os.stat(dest)
if (before.st_ino, before.st_mtime_ns) != (after.st_ino, after.st_mtime_ns):
    problems.append('republishing identical content replaced the inode or '
                    'moved mtime; an unchanged generation must be a no-op')
os.unlink(dest)
os.rmdir(d)

# A checkout reached through a symlink is LEGITIMATE (a worktree under a
# symlinked home, or an absolute invocation via such a path). Only descendants
# must be link-free, so the trust anchor is the resolved root.
_alias = pathlib.Path(tempfile.mkdtemp()) / 'repo-alias'
_alias.symlink_to(gua.REPO_ROOT)
try:
    aliased = os.path.join(str(_alias), 'build', 'abi-alias-probe.h')
    gua.publish_atomically(aliased, NEW)
    if pathlib.Path(aliased).read_text() != NEW:
        problems.append('publication through a symlinked checkout root did not '
                        'write the expected content')
    os.unlink(aliased)
except RuntimeError as exc:
    problems.append(f'publication refused a legitimate symlinked checkout '
                    f'root: {exc}')
finally:
    _alias.unlink()
    os.rmdir(_alias.parent)

# Publication must REFUSE to leave the repository, at any path component -- the
# final-component symlink guard was not enough, because a symlinked parent
# directory resolves during staging and replacement just the same.
ext = tempfile.mkdtemp()
sentinel = pathlib.Path(ext) / 'abi_contract.h'
sentinel.write_text('SENTINEL-MUST-SURVIVE\n')
# A UNIQUE name, and never an unlink of a pre-existing entry: the fixed
# `build/abi-escape-probe` was removed before planting, so residue left by an
# interrupted run -- a symlink someone else's process owned, or one pointing
# outside the tree -- was deleted by a routine tooling run. mkdtemp gives a
# name that cannot already exist, and symlink() onto a fresh path inside it
# cannot clobber anything.
link_parent = os.path.join(tempfile.mkdtemp(prefix='abi-escape-',
                                            dir=os.path.join(gua.REPO_ROOT, 'build')),
                           'probe')
try:
    os.symlink(ext, link_parent)
    try:
        gua.publish_atomically(os.path.join(link_parent, 'abi_contract.h'), NEW)
        problems.append('publication through a symlinked parent directory was '
                        'permitted')
    except RuntimeError:
        pass
    if sentinel.read_text() != 'SENTINEL-MUST-SURVIVE\n':
        problems.append('publication overwrote a file outside the repository '
                        'through a symlinked parent directory')
finally:
    if os.path.islink(link_parent):
        os.unlink(link_parent)
    os.rmdir(os.path.dirname(link_parent))
    sentinel.unlink(missing_ok=True)
    os.rmdir(ext)

print('OK' if not problems else '; '.join(problems))
PYATOMIC
)
if [ "$_GUA_ATOMIC_OUT" = "OK" ]; then
    t_pass "gen_user_abi: publication is atomic at a single commit point"
else
    t_fail "gen_user_abi: publication is atomic at a single commit point" \
           "$_GUA_ATOMIC_OUT"
fi

# The two STATIC shims are ordinary committed source, so no rendering comparison
# can catch a one-line edit that detaches a side of the build from the generated
# contract. --check verifies them structurally; assert each refusal shape here.
_GUA_SHIM_OUT="SKIPPED: build/ unusable for scratch state"
[ "$_GUA_SCRATCH_OK" = "1" ] && _GUA_SHIM_OUT=$(cd "$REPO_ROOT" && python3 - <<'PYSHIM'
import importlib.util, os, pathlib, subprocess, sys, tempfile

spec = importlib.util.spec_from_file_location("gua", "scripts/gen-user-abi.py")
gua = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gua)

problems = []
_CLANG, _CLANG_ERR = gua.clang_binary()
if _CLANG_ERR:
    print(f'cannot resolve clang for shim validation: {_CLANG_ERR}')
    raise SystemExit(0)

# Every MUTATING case below runs against throwaway facades, never the tracked
# ones. Restoring a tracked file in a `finally` is not interruption-safe --
# SIGTERM or SIGKILL skips it entirely, leaving a corrupted facade behind, which
# is exactly the incident this section hit -- and even a clean restore rewrites
# timestamps. Pointing the generator's path constants at copies removes the
# whole hazard class instead of guarding it: the worst a signal can now leave is
# litter under build/.
#
# The probes sit TWO levels below the repo root so the facades' relative include
# `../../abi/generated/abi_contract.h` resolves to the real contract, exactly as
# it does from user/include/ and include/kernel/.
# UNIQUE per run, never a fixed reused name: an interrupted run could leave a
# fixed build/shimprobe behind as a symlink, and write_text() would then follow
# it straight out of the repository -- the very data-loss class these fixtures
# exist to prove is closed.
_PROBE = pathlib.Path(tempfile.mkdtemp(
    prefix='shimprobe-', dir=str(pathlib.Path(gua.REPO_ROOT) / 'build')))
_REAL_USER_H, _REAL_KERNEL_H = gua.SHIM_USER_H, gua.SHIM_KERNEL_H

# Read-only baseline against the REAL committed facades, before the redirect.
for _path, _want in ((_REAL_USER_H, True), (_REAL_KERNEL_H, False)):
    _r = gua.check_shim_form(_path, _want)
    if _r:
        problems.append(f'committed facade {_path} is not text-pinned: {_r}')

_probe_user = _PROBE / 'abi_numbers.h'
_probe_kernel = _PROBE / 'abi_hash.h'
with open(_probe_user, 'x', encoding='utf-8') as _f:
    _f.write(gua.CANONICAL_SHIM_USER)
with open(_probe_kernel, 'x', encoding='utf-8') as _f:
    _f.write(gua.CANONICAL_SHIM_KERNEL)
gua.SHIM_USER_H = str(_probe_user)
gua.SHIM_KERNEL_H = str(_probe_kernel)

# The AUTHORITATIVE vectors, not a hand-picked subset -- validating a shim under
# flags the real compile does not use is fail-open (production carries -O2, so
# __OPTIMIZE__ is defined there and not in a minimal vector).
_USER_VECS, _UV_ERR = gua._user_shim_vectors()
_KERN_VECS, _KV_ERR = gua._kernel_shim_vectors()
if _UV_ERR or _KV_ERR:
    problems.append(f'cannot obtain authoritative shim vectors: {_UV_ERR or ""} '
                    f'{_KV_ERR or ""}'.strip())
    _USER_VECS, _KERN_VECS = [], []

# A minimal vector must be REFUSED outright rather than silently guessed.
if not gua.check_shim(gua.SHIM_USER_H, True, _CLANG):
    problems.append('check_shim accepted a shim with no flag vector supplied')

def _vecs(path):
    return _USER_VECS if path == gua.SHIM_USER_H else _KERN_VECS

def expect(path, want_numbers, mutate, needle, label):
    """The mutation must be rejected under AT LEAST ONE authoritative vector --
    a flavor-guarded edit is invisible to the others by construction."""
    p = pathlib.Path(path)
    original = p.read_text()
    try:
        p.write_text(mutate(original))
        seen = []
        for flags, name in _vecs(path):
            seen += gua.check_shim(str(p), want_numbers, _CLANG, flags, name)
        if not any(needle in r for r in seen):
            problems.append(f'{label}: expected a reason containing {needle!r}, '
                            f'got {seen}')
    finally:
        p.write_text(original)

# Baseline: the committed shims are sound under EVERY authoritative vector.
for path, want in ((gua.SHIM_USER_H, True), (gua.SHIM_KERNEL_H, False)):
    for flags, name in _vecs(path):
        reasons = gua.check_shim(path, want, _CLANG, flags, name)
        if reasons:
            problems.append(f'committed shim {path} fails check_shim '
                            f'under [{name}]: {reasons}')

# Detaching the include, in each of the shapes a raw-text scan would miss.
expect(gua.SHIM_USER_H, True,
       lambda t: t.replace('#include "../../abi/generated/abi_contract.h"',
                           '/* detached */'),
       'does not resolve', 'detached-include')
expect(gua.SHIM_USER_H, True,
       lambda t: t.replace('#include "../../abi/generated/abi_contract.h"',
                           '#if 0\n#include "../../abi/generated/abi_contract.h"\n#endif'),
       'does not resolve', 'if0-guarded-include')
# Wrong visibility for the side: user without the tables, kernel with them.
expect(gua.SHIM_USER_H, True,
       lambda t: t.replace('#define ABI_CONTRACT_WANT_NUMBERS', '/* dropped */'),
       'does not resolve', 'user-shim-without-numbers')
expect(gua.SHIM_KERNEL_H, False,
       lambda t: t + '#define ABI_CONTRACT_WANT_NUMBERS\n'
                     '#include "../../abi/generated/abi_contract.h"\n',
       'does NOT publish for it', 'kernel-shim-with-numbers')
# A restated value -- and the two spellings that slipped past the raw-text scan:
# `#undef` plus `# define` with a space, which would have shipped ring 3 against
# a wrong syscall number while the fingerprint still agreed at the handshake.
# A BARE redefinition (no #undef) is rejected even harder than by value
# comparison: the authoritative vectors carry -Werror -Wmacro-redefined, so the
# preprocessor refuses the shim outright. Either rejection is a --check failure,
# so this needle matches the macro name rather than one specific message.
expect(gua.SHIM_USER_H, True,
       lambda t: t + '#define SYS_WRITE 999\n',
       'SYS_WRITE', 'hand-defined-constant')
expect(gua.SHIM_USER_H, True,
       lambda t: t + '#undef SYS_WRITE\n# define SYS_WRITE 999\n',
       'resolves SYS_WRITE', 'undef-then-spaced-redefine')
expect(gua.SHIM_KERNEL_H, False,
       lambda t: t + '#undef IMPOSSIBLE_OS_ABI_HASH\n',
       'does not resolve', 'undef-the-fingerprint')
expect(gua.SHIM_KERNEL_H, False,
       lambda t: t + '#undef IMPOSSIBLE_OS_ABI_HASH\n'
                     '#  define IMPOSSIBLE_OS_ABI_HASH 0xBADULL\n',
       'resolves IMPOSSIBLE_OS_ABI_HASH', 'hand-defined-hash')
# The FLAG-GUARDED shapes: invisible to any minimal preprocessing context, live
# in the real build. -O2 puts __OPTIMIZE__ in both production vectors, and the
# kernel adds flavor macros -- either would carry a wrong ABI value into real
# binaries with the fingerprint intact, so the crt0 handshake would still agree.
expect(gua.SHIM_USER_H, True,
       lambda t: t + '#ifdef __OPTIMIZE__\n#undef SYS_WRITE\n'
                     '# define SYS_WRITE 999\n#endif\n',
       'resolves SYS_WRITE', 'optimize-guarded-redefine')
expect(gua.SHIM_KERNEL_H, False,
       lambda t: t + '#ifdef KERNEL_TESTS\n#undef IMPOSSIBLE_OS_ABI_HASH\n'
                     '#define IMPOSSIBLE_OS_ABI_HASH 0xBADULL\n#endif\n',
       'resolves IMPOSSIBLE_OS_ABI_HASH', 'flavor-guarded-fingerprint-swap')

# --- FACADE TEXT-PINNING --------------------------------------------------
# Four parsing designs were bypassed here in succession -- a `#define `-prefix
# scan, the same under minimal flags, per-vector semantic comparison, and a
# directive allowlist over stripped comments. Each was Python guessing at C
# lexing and each lost to a different corner: `# define`, __OPTIMIZE__ guards,
# a guard on NULL that types.h supplies before the shim, line splicing,
# `// /*` hiding directives from a block-first regex, and `/*` inside an
# #include header-name collapsing to the required spelling. The facades carry
# no ABI data and never vary, so their canonical TEXT is pinned instead
# (line endings normalized; every other difference refused). A text comparison
# has no corners to find.
def expect_form(path, want_numbers, mutate, needle, label):
    p = pathlib.Path(path)
    original = p.read_text()
    try:
        p.write_text(mutate(original))
        reasons = gua.check_shim_form(str(p), want_numbers)
        if not any(needle in r for r in reasons):
            problems.append(f'form/{label}: expected a reason containing '
                            f'{needle!r}, got {reasons}')
    finally:
        p.write_text(original)

# (The committed facades were checked against their canonical text above,
# before the probe redirect; the probes start canonical by construction.)

# Every historical bypass, now refused by the same mechanism.
for payload, label in (
        ('#ifdef NULL\n#undef SYS_WRITE\n#define SYS_WRITE 999\n#endif\n',
         'preceding-header-macro-guard'),
        ('#undef SYS_WRITE\n# define SYS_WRITE 999\n', 'undef-then-spaced-redefine'),
        ('#ifdef __OPTIMIZE__\n#undef SYS_WRITE\n# define SYS_WRITE 9\n#endif\n',
         'optimize-guarded-redefine'),
        ('/* b *\\\n/\n#ifdef NULL\n#endif\n// */\n', 'line-splice-fake-comment-end'),
        ('// /*\n#ifdef NULL\n#undef SYS_WRITE\n#endif\n// */\n',
         'line-comment-hiding-block-marker'),
        ('/* unterminated\n#ifdef NULL\n#endif\n', 'unterminated-block-comment'),
        ('#include "types.h"\n', 'extra-include'),
        ('int sneaky = 1;\n', 'code-in-a-facade'),
):
    expect_form(gua.SHIM_USER_H, True, lambda t, pl=payload: t + pl,
                'differs from the pinned facade', label)

# In-place edits, not just appends -- these change no line count.
expect_form(gua.SHIM_USER_H, True,
            lambda t: t.replace('#include "../../abi/generated/abi_contract.h"',
                                '#include "../../abi/generated/abi_/*x*/contract.h"'),
            'differs from the pinned facade', 'header-name-comment-splice')
expect_form(gua.SHIM_USER_H, True,
            lambda t: t.replace('#include "../../abi/generated/abi_contract.h"',
                                '#include "generated/abi_contract.h"'),
            'differs from the pinned facade', 'search-form-include-spelling')
expect_form(gua.SHIM_USER_H, True,
            lambda t: t.replace('#define ABI_CONTRACT_WANT_NUMBERS\n', ''),
            'differs from the pinned facade', 'user-facade-without-numbers')
expect_form(gua.SHIM_KERNEL_H, False,
            lambda t: t.replace('#pragma once',
                                '#pragma once\n#define ABI_CONTRACT_WANT_NUMBERS'),
            'differs from the pinned facade', 'kernel-facade-with-numbers')
# A refusal must name the first differing LINE so a reader is not left diffing.
expect_form(gua.SHIM_USER_H, True,
            lambda t: t.replace('#pragma once', '#pragma  once'),
            ':13 differs', 'first-differing-line-is-named')

# A facade must be a REGULAR FILE, even when a symlink target happens to hold
# the exact canonical bytes: right today, following someone else's file
# tomorrow, entirely outside this gate's view.
_p = pathlib.Path(gua.SHIM_USER_H)
_orig = _p.read_bytes()
_orig_mode = _p.stat().st_mode
_canon = pathlib.Path(tempfile.mkdtemp()) / 'canonical.h'
_canon.write_bytes(_orig)
try:
    _p.unlink()
    _p.symlink_to(_canon)
    _reasons = gua.check_shim_form(str(_p), True)
    if not any('symlink' in r for r in _reasons):
        problems.append(f'form/symlink-with-canonical-bytes: a symlinked facade '
                        f'must be refused even when its target matches, got '
                        f'{_reasons}')
finally:
    if _p.is_symlink() or _p.exists():
        _p.unlink()
    _p.write_bytes(_orig)
    os.chmod(_p, _orig_mode)
    _canon.unlink(missing_ok=True)
    os.rmdir(_canon.parent)

# A CRLF working copy is byte-different and SEMANTICALLY IDENTICAL -- a line
# ending cannot change which directives are active. The text pin must not fail
# a checkout clang is perfectly happy with (.gitattributes keeps the repository
# side LF; this keeps a pre-existing clone from failing spuriously).
_p = pathlib.Path(gua.SHIM_USER_H)
_orig = _p.read_bytes()
_orig_mode = _p.stat().st_mode
try:
    _p.write_bytes(_orig.decode().replace('\\n', '\\r\\n').encode())
    _reasons = gua.check_shim_form(str(_p), True)
    if _reasons:
        problems.append(f'form/crlf-checkout: a CRLF facade must be accepted, '
                        f'got {_reasons}')
finally:
    _p.write_bytes(_orig)
    os.chmod(_p, _orig_mode)

# Facade repair must not FOLLOW a symlink: `open(path, "w")` truncated the LINK
# TARGET, verified to clobber a file outside the repository while leaving the
# link in place. publish_atomically stages and os.replaces, so the link itself
# is swapped -- exercised here in-process against the probe, because the
# --write-shims SUBCOMMAND resolves the generator's own path constants and would
# rewrite the tracked facade rather than this one.
_sentinel = pathlib.Path(tempfile.mkdtemp()) / 'sentinel.txt'
_sentinel.write_text('SENTINEL-MUST-SURVIVE\n')
try:
    _probe_user.unlink()
    _probe_user.symlink_to(_sentinel)
    gua.publish_atomically(str(_probe_user), gua.CANONICAL_SHIM_USER)
    if _sentinel.read_text() != 'SENTINEL-MUST-SURVIVE\n':
        problems.append('facade repair overwrote a symlink target outside the '
                        'repository instead of replacing the link')
    if _probe_user.is_symlink():
        problems.append('facade repair left the symlink in place, so the path '
                        'was followed rather than replaced')
    if _probe_user.read_text() != gua.CANONICAL_SHIM_USER:
        problems.append('facade repair did not restore the canonical bytes')
finally:
    if _probe_user.is_symlink() or _probe_user.exists():
        _probe_user.unlink()
    _probe_user.write_text(gua.CANONICAL_SHIM_USER)
    _sentinel.unlink(missing_ok=True)
    os.rmdir(_sentinel.parent)

gua.SHIM_USER_H, gua.SHIM_KERNEL_H = _REAL_USER_H, _REAL_KERNEL_H
for _n in _PROBE.iterdir():
    _n.unlink()
_PROBE.rmdir()

print('OK' if not problems else '; '.join(problems))
PYSHIM
)
if [ "$_GUA_SHIM_OUT" = "OK" ]; then
    t_pass "gen_user_abi: --check refuses a shim that detaches from the contract"
else
    t_fail "gen_user_abi: --check refuses a shim that detaches from the contract" \
           "$_GUA_SHIM_OUT"
fi

# INCLUSION ORDER must not decide whether ring-3 sees the number tables. The
# contract uses two independent guards precisely so a TU that already pulled in
# the kernel-visible half still gets the tables from a later include. Compile
# both orders through the real shims and require SYS_WRITE to resolve in each.
_GUA_ORDER_OK=1
for _order in "kernel_first" "user_first"; do
    _GUA_TU=$(mktemp -p "$REPO_ROOT/user/include" --suffix=.c abi-order-XXXXXX)
    if [ "$_order" = "kernel_first" ]; then
        printf '#include "../../include/kernel/abi_hash.h"\n#include "abi_numbers.h"\nint probe = SYS_WRITE;\nlong h = IMPOSSIBLE_OS_ABI_HASH;\n' > "$_GUA_TU"
    else
        printf '#include "abi_numbers.h"\n#include "../../include/kernel/abi_hash.h"\nint probe = SYS_WRITE;\nlong h = IMPOSSIBLE_OS_ABI_HASH;\n' > "$_GUA_TU"
    fi
    if ! (cd "$REPO_ROOT" && clang-19 --target=x86_64-elf -ffreestanding -nostdlib \
            -nostdinc -fsyntax-only "$_GUA_TU" >/dev/null 2>&1); then
        _GUA_ORDER_OK=0
        _GUA_ORDER_BAD="$_order"
    fi
    rm -f "$_GUA_TU"
done
if [ "$_GUA_ORDER_OK" = "1" ]; then
    t_pass "gen_user_abi: contract number tables are inclusion-order independent"
else
    t_fail "gen_user_abi: contract number tables are inclusion-order independent" \
           "SYS_WRITE did not resolve with include order: $_GUA_ORDER_BAD"
fi

# An ignored build tree must NOT be able to shadow the committed contract. The
# shims originally found it as `generated/abi_contract.h` via -Iabi, but
# -I$(BUILD_DIR) precedes that in the kernel vector and build/generated/ already
# exists, so planting build/generated/abi_contract.h won the lookup and compiled
# a bogus fingerprint into the kernel while --check -- which reads the committed
# artifact by absolute path -- stayed silent. The shims now include the artifact
# by a path relative to themselves. Plant the conflict and require the real hash.
# Plant and probe entirely in PYTHON with O_NOFOLLOW|O_CREAT|O_EXCL. The shell
# version used `[ -f ... ]` to decide whether the path was free, and both that
# test and the `>` redirection FOLLOW SYMLINKS: a dangling symlink left at
# build/generated/abi_contract.h read as "absent" and the redirection then
# created or truncated its target outside the checkout. A symlinked
# build/generated parent escaped the same way. O_EXCL refuses a path that
# already exists at all, O_NOFOLLOW refuses a final symlink, and the parent
# components are walked without following.
_GUA_SHADOW_OUT="SKIPPED: build/ unusable for scratch state"
[ "$_GUA_SCRATCH_OK" = "1" ] && _GUA_SHADOW_OUT=$(cd "$REPO_ROOT" && python3 - <<'PYSHADOW'
import os, pathlib, re, subprocess, tempfile

root = pathlib.Path.cwd()
real = (root / 'abi/generated/abi_contract.h').read_text()
m = re.search(r'0x[0-9A-F]{16}ULL', real)
if not m:
    print('could not read the committed hash literal'); raise SystemExit(0)
want = m.group(0)

gen = root / 'build' / 'generated'
probe = gen / 'abi_contract.h'

# Refuse to touch anything if a parent component is a symlink.
walk = gen
while walk != root:
    if walk.is_symlink():
        print(f'{walk} is a symlink; refusing to plant through it')
        raise SystemExit(0)
    walk = walk.parent
gen.mkdir(parents=True, exist_ok=True)

try:
    fd = os.open(probe, os.O_CREAT | os.O_EXCL | os.O_WRONLY | os.O_NOFOLLOW, 0o644)
except FileExistsError:
    print('build/generated/abi_contract.h already exists -- refusing to clobber it')
    raise SystemExit(0)
except OSError as exc:
    print(f'could not plant the shadow header safely: {exc}')
    raise SystemExit(0)

tu = None
try:
    with os.fdopen(fd, 'w') as f:
        f.write('#define IMPOSSIBLE_OS_ABI_HASH 0xDEADBEEFDEADBEEFULL\n')
    # The TU must EXPAND the macro: -E drops #define lines, so a header-only
    # translation unit preprocesses to nothing and the comparison below would
    # see an empty string and "fail" for the wrong reason.
    tu = pathlib.Path(tempfile.mkstemp(suffix='.c', prefix='abi-shadow-',
                                       dir=str(root))[1])
    tu.write_text('#include "include/kernel/abi_hash.h"\n'
                  'unsigned long long probe = IMPOSSIBLE_OS_ABI_HASH;\n')
    out = subprocess.run(
        ['clang-19', '--target=x86_64-elf', '-ffreestanding', '-nostdinc',
         '-Iinclude', '-Isrc/kernel', '-Ibuild/generated', '-Isrc', '-Ibuild',
         '-E', str(tu)], capture_output=True, text=True, cwd=str(root)).stdout
    seen = re.findall(r'0x[0-9A-F]+ULL', out)
    seen = seen[-1] if seen else '<none>'
    print('OK' if seen == want
          else f'kernel resolved {seen}, expected the committed {want}')
finally:
    probe.unlink(missing_ok=True)
    if tu is not None:
        tu.unlink(missing_ok=True)
PYSHADOW
)
if [ "$_GUA_SHADOW_OUT" = "OK" ]; then
    t_pass "gen_user_abi: an ignored build tree cannot shadow the ABI contract"
else
    t_fail "gen_user_abi: an ignored build tree cannot shadow the ABI contract" \
           "$_GUA_SHADOW_OUT"
fi

# The generated contract must be a REAL prerequisite of the grouped userland
# target. Those .exe targets compile their objects with inline recipe commands,
# so the -MMD depfiles describe object targets make never requests: unlike the
# kernel (whose .o files ARE real targets and do pick the contract up through
# their .d files), userland has no automatic path from an ABI change to a
# rebuild. Today the grouped target is rebuilt unconditionally anyway, because
# its first prerequisite `sysroot` is .PHONY -- so this listing is not what
# saves the current build, and no incremental-skew bug is being fixed here.
# It is listed because that protection is INCIDENTAL: the day the grouped
# target becomes properly incremental, an unlisted contract means the kernel
# rebuilds against a new hash while stale user binaries survive, and every
# ring-3 process then aborts at SYS_ABI_HANDSHAKE. Pin the explicit edge now.
if grep -qE '\$\(ABI_CONTRACT_H\)' "$REPO_ROOT/Makefile" && \
   awk '/^\$\(SYSROOT\)\/hello\.exe/,/^\t/' "$REPO_ROOT/Makefile" \
       | grep -q 'ABI_CONTRACT_H'; then
    t_pass "gen_user_abi: generated contract is a userland build prerequisite"
else
    t_fail "gen_user_abi: generated contract is a userland build prerequisite" \
           "the grouped userland target does not list \$(ABI_CONTRACT_H)"
fi

# The committed-ABI root must stay OFF every include vector. Putting it on one
# was the shadowing bug above: as soon as `generated/abi_contract.h` is resolved
# by SEARCH rather than by a path relative to the shim, whichever -I root comes
# first wins, and a writable build tree can come first. Re-adding -Iabi would
# make the shadow fixture pass while restoring the hazard for any NEW consumer
# that spells the include the search way, so pin the absence, not the presence.
_GUA_UFLAGS=$( (cd "$REPO_ROOT" && make print-user-cflags 2>/dev/null) | tr '\n' ' ')
case "$_GUA_UFLAGS" in
  *-O2*-Iuser/include*|*-Iuser/include*-O2*)
    t_pass "gen_user_abi: the ring-3 shim vector comes from the Makefile" ;;
  *)
    t_fail "gen_user_abi: the ring-3 shim vector comes from the Makefile" \
           "make print-user-cflags must expand USER_CFLAGS + -Iuser/include (got: $_GUA_UFLAGS)" ;;
esac

if grep -qE -- '-I\$\(ABI_DIR\)|-Iabi\b' "$REPO_ROOT/Makefile"; then
    t_fail "gen_user_abi: the committed-ABI root stays off the include vectors" \
           "an -Iabi search path reintroduces build-tree shadowing of the contract"
elif grep -q 'include "\.\./\.\./abi/generated/abi_contract\.h"' \
        "$REPO_ROOT/include/kernel/abi_hash.h" && \
     grep -q 'include "\.\./\.\./abi/generated/abi_contract\.h"' \
        "$REPO_ROOT/user/include/abi_numbers.h"; then
    t_pass "gen_user_abi: the committed-ABI root stays off the include vectors"
else
    t_fail "gen_user_abi: the committed-ABI root stays off the include vectors" \
           "both shims must include the contract by a path relative to themselves"
fi

# The generated CONTRACT must be in sync with kernel source at all times -- the
# same gate `make check-abi` runs, asserted here so a tooling run catches a
# forgotten regeneration without a full build. (One generated artifact since the
# single-commit-point publication landed; the two headers are static facades,
# checked separately by the text pin above.)
if (cd "$REPO_ROOT" && python3 scripts/gen-user-abi.py --check >/dev/null 2>&1); then
    t_pass "gen_user_abi: --check clean (generated contract matches kernel source)"
else
    t_fail "gen_user_abi: --check clean (generated contract matches kernel source)" \
           "run: python3 scripts/gen-user-abi.py"
fi

# ============================================================================
# Launcher record framing -- host-side parser gates
#
# test_forge.exe proves the LIVE path: a ring-3 binary printing launcher-
# shaped records cannot reach an artifact or a verdict. It deliberately does
# NOT forge a frame ANNOUNCEMENT, because the host fails a run on a nonce
# conflict and a fixture shipping in the default suite would then turn every
# run red by design. Those refusal paths are asserted here instead, against
# synthetic logs, where a red verdict IS the assertion.
# ============================================================================

if [ "$QUIET" = "0" ]; then
    echo ""
    echo -e "${CYAN}Launcher record framing (host parsers)${NC}"
fi

FRAME_TMP=$(mktemp -d)
trap 'rm -rf "$FRAME_TMP"' EXIT

# A minimal well-formed framed stream: announcement, one JSON binary record,
# the three run-level records, and the terminator. records= counts every
# framed record before the terminator (6), so a whole stream is 7 lines.
frame_log() {
    local nonce="$1" out="$2"
    {
        echo "[  1.000] [cpu:0] [ OK ] UTEST-${nonce}: [UTEST-FRAME] v=1 run=1"
        echo "[  1.010] [cpu:0] [ OK ] UTEST-${nonce}: [UTEST-JSON] {\"record_kind\":\"binary\",\"name\":\"test_real.exe\",\"type\":\"correctness\",\"status\":\"PASS\",\"time_ms\":5}"
        echo "[  1.020] [cpu:0] [ OK ] UTEST-${nonce}: [UTEST-JSON] {\"record_kind\":\"run_report\",\"asserts_passed\":1,\"asserts_failed\":0,\"skip_blocks\":0,\"skip_records\":0,\"binaries_reported\":1,\"binaries_invalid\":0,\"binaries_unreported\":0}"
        echo "[  1.030] [cpu:0] [ OK ] UTEST-${nonce}: [UTEST-JSON] {\"summary\":{\"passed\":1,\"failed\":0,\"skipped\":0,\"total\":1,\"time_ms\":5}}"
        echo "[  1.040] [cpu:0] [ OK ] UTEST-${nonce}: [UTEST-JSON] {\"record_kind\":\"run_meta\",\"aborted\":false,\"not_run\":0}"
        echo "[  1.050] [cpu:0] [ OK ] UTEST-${nonce}: === 1 passed, 0 failed, 0 skipped of 1 total ==="
        echo "[  1.060] [cpu:0] [ OK ] UTEST-${nonce}: [UTEST-FRAME-END] run=1 records=6"
    } > "$out"
}

HARVEST="$REPO_ROOT/scripts/utest-json-harvest.py"

# 1. Baseline: a genuine framed stream harvests cleanly.
frame_log "1a2b3c4d" "$FRAME_TMP/good.log"
if python3 "$HARVEST" "$FRAME_TMP/good.log" "$FRAME_TMP/good.json" >/dev/null 2>&1 &&
   python3 -c "import json,sys; d=json.load(open('$FRAME_TMP/good.json')); sys.exit(0 if d['summary']['total']==1 else 1)"; then
    t_pass "framing: a genuine framed stream harvests to a real summary"
else
    t_fail "framing: a genuine framed stream harvests to a real summary"
fi

# 2. UNFRAMED records are not records. This is the pre-section behaviour the
#    harvester used to accept verbatim: bare `[UTEST-JSON] ` anywhere on a
#    line was folded straight into the artifact.
sed -E 's/UTEST-1a2b3c4d: //' "$FRAME_TMP/good.log" > "$FRAME_TMP/unframed.log"
python3 "$HARVEST" "$FRAME_TMP/unframed.log" "$FRAME_TMP/unframed.json" >/dev/null 2>&1 && HRC=0 || HRC=$?
if [ "$HRC" != "0" ] &&
   python3 -c "import json,sys; d=json.load(open('$FRAME_TMP/unframed.json')); sys.exit(0 if d['summary'] is None else 1)"; then
    t_pass "framing: unframed records are refused, not harvested"
else
    t_fail "framing: unframed records are refused, not harvested" "harvester rc=$HRC"
fi

# 3. A GUESSED nonce is refused exactly like no frame at all -- the frame is
#    learned from the announcement, never pattern-matched.
frame_log "1a2b3c4d" "$FRAME_TMP/guess.log"
sed -i -E 's/^(.*)UTEST-1a2b3c4d: (\[UTEST-JSON\] \{"record_kind":"binary")/\1UTEST-deadbeef: \2/' \
    "$FRAME_TMP/guess.log"
if python3 "$HARVEST" "$FRAME_TMP/guess.log" "$FRAME_TMP/guess.json" >/dev/null 2>&1; then
    if python3 -c "
import json,sys
d=json.load(open('$FRAME_TMP/guess.json'))
names=[r.get('name') for r in d.get('testcases',[])]
sys.exit(0 if 'test_real.exe' not in names else 1)" 2>/dev/null; then
        t_pass "framing: a wrong-nonce record never reaches the artifact"
    else
        t_fail "framing: a wrong-nonce record never reaches the artifact" "record was harvested"
    fi
else
    # A refusal is also a correct outcome here: dropping the binary record
    # leaves the stream inconsistent with its own summary.
    t_pass "framing: a wrong-nonce record never reaches the artifact"
fi

# 4. The FIRST valid announcement wins. A later, different announcement must
#    not re-point the parser at an imitator's records.
frame_log "1a2b3c4d" "$FRAME_TMP/two.log"
{
    echo "[  2.000] [cpu:0] [ OK ] UTEST-deadbeef: [UTEST-FRAME] v=1 run=1"
    echo "[  2.010] [cpu:0] [ OK ] UTEST-deadbeef: [UTEST-JSON] {\"record_kind\":\"binary\",\"name\":\"forged.exe\",\"type\":\"correctness\",\"status\":\"PASS\",\"time_ms\":0}"
} >> "$FRAME_TMP/two.log"
python3 "$HARVEST" "$FRAME_TMP/two.log" "$FRAME_TMP/two.json" >/dev/null 2>&1 || true
if python3 -c "
import json,sys
d=json.load(open('$FRAME_TMP/two.json'))
blob=json.dumps(d)
sys.exit(0 if 'forged.exe' not in blob else 1)"; then
    t_pass "framing: the first announcement wins, a second one cannot re-point the parser"
else
    t_fail "framing: the first announcement wins, a second one cannot re-point the parser"
fi

# 5. Two legitimate launcher runs in one boot. The nonce is per-BOOT and
#    test_usermode_run() is documented safe to call repeatedly, so both runs
#    share a prefix; the artifact must describe the LAST COMPLETE run rather
#    than merging both into a duplicate-record refusal.
frame_log "1a2b3c4d" "$FRAME_TMP/run1.log"
# Second run, built from a COPY: appending a transform of the same file to
# itself makes sed chase its own growing output and never terminate.
sed -E 's/run=1/run=2/; s/test_real\.exe/test_second.exe/' \
    "$FRAME_TMP/run1.log" > "$FRAME_TMP/run2.log"
cat "$FRAME_TMP/run2.log" >> "$FRAME_TMP/run1.log"
if python3 "$HARVEST" "$FRAME_TMP/run1.log" "$FRAME_TMP/run1.json" >/dev/null 2>&1 &&
   python3 -c "
import json,sys
d=json.load(open('$FRAME_TMP/run1.json'))
names=[r.get('name') for r in d.get('testcases',[])]
sys.exit(0 if names==['test_second.exe'] else 1)"; then
    t_pass "framing: two launcher runs in one boot publish the last complete run"
else
    t_fail "framing: two launcher runs in one boot publish the last complete run"
fi

# 6. Reconciliation, ordinal binding and run selection are asserted against
#    the PRODUCTION parser (scripts/utest-frame.py) -- the one test.sh,
#    test-swtpm.sh and the harvester all call. The previous version of this
#    test recomputed `FRAMED != DECLARED + 1` inside itself, so the real
#    implementation could have been deleted while this stayed green.
FRAME_PARSER="$REPO_ROOT/scripts/utest-frame.py"

# 6a. A cut stream fails reconciliation.
frame_log "1a2b3c4d" "$FRAME_TMP/short.log"
grep -v 'record_kind":"run_meta' "$FRAME_TMP/short.log" > "$FRAME_TMP/short2.log"
if ! python3 "$FRAME_PARSER" "$FRAME_TMP/short2.log" >/dev/null 2>&1; then
    t_pass "framing: the parser refuses a cut stream"
else
    t_fail "framing: the parser refuses a cut stream"
fi

# 6b. A terminator whose ordinal does not close its announcement is refused.
#     Boot-wide summing accepted this: BEGIN run=1 ... END run=2 reconciled.
frame_log "1a2b3c4d" "$FRAME_TMP/ord.log"
sed -i -E 's/\[UTEST-FRAME-END\] run=1/[UTEST-FRAME-END] run=2/' "$FRAME_TMP/ord.log"
if ! python3 "$FRAME_PARSER" "$FRAME_TMP/ord.log" >/dev/null 2>&1; then
    t_pass "framing: the parser refuses a terminator with a mismatched ordinal"
else
    t_fail "framing: the parser refuses a terminator with a mismatched ordinal"
fi

# 6c. A complete run followed by an UNTERMINATED one still publishes the
#     complete one -- and never merges the two. Selecting the last
#     announcement and the last terminator independently got this backwards.
frame_log "1a2b3c4d" "$FRAME_TMP/trail.log"
sed -E 's/run=1/run=2/; s/test_real\.exe/test_trailing.exe/' \
    "$FRAME_TMP/trail.log" | grep -v 'UTEST-FRAME-END' > "$FRAME_TMP/trail2.log"
cat "$FRAME_TMP/trail2.log" >> "$FRAME_TMP/trail.log"
TRAIL=$(python3 "$FRAME_PARSER" "$FRAME_TMP/trail.log" 2>/dev/null || true)
if echo "$TRAIL" | grep -q '"ok": true' &&
   echo "$TRAIL" | python3 -c '
import json,sys
d=json.load(sys.stdin)
sys.exit(0 if d["last_complete"]["run"] == 1 else 1)'; then
    t_pass "framing: an unterminated trailing run never displaces the complete one"
else
    t_fail "framing: an unterminated trailing run never displaces the complete one"
fi

# 6d. Two COMPLETE runs: the newest wins, and the harvester agrees with the
#     parser about which one that is.
frame_log "1a2b3c4d" "$FRAME_TMP/two2.log"
sed -E 's/run=1/run=2/; s/test_real\.exe/test_second.exe/' \
    "$FRAME_TMP/two2.log" > "$FRAME_TMP/two2b.log"
cat "$FRAME_TMP/two2b.log" >> "$FRAME_TMP/two2.log"
if python3 "$FRAME_PARSER" "$FRAME_TMP/two2.log" 2>/dev/null |
       python3 -c '
import json,sys
d=json.load(sys.stdin)
sys.exit(0 if d["ok"] and d["last_complete"]["run"] == 2 else 1)'; then
    t_pass "framing: the newest complete run is the one selected"
else
    t_fail "framing: the newest complete run is the one selected"
fi

# 6f. A previous boot's crash replay must not be mistaken for this boot's
#     stream. klog_crash_recover() re-emits the PRIOR boot's entries with
#     their real UTEST tag, so a recovered COMPLETE run would otherwise be
#     learned as the frame and satisfy the completion gate before the current
#     launcher ran a single binary.
{
    echo "[CRASH-PREV] UTEST-99999999: [UTEST-FRAME] v=1 run=1"
    echo "[CRASH-PREV] UTEST-99999999: [UTEST-JSON] {\"record_kind\":\"binary\",\"name\":\"stale.exe\",\"type\":\"correctness\",\"status\":\"PASS\",\"time_ms\":1}"
    echo "[CRASH-PREV] UTEST-99999999: [UTEST-FRAME-END] run=1 records=2"
} > "$FRAME_TMP/crash.log"
frame_log "1a2b3c4d" "$FRAME_TMP/live.log"
cat "$FRAME_TMP/live.log" >> "$FRAME_TMP/crash.log"
if python3 "$FRAME_PARSER" "$FRAME_TMP/crash.log" 2>/dev/null |
       python3 -c '
import json,sys
d=json.load(sys.stdin)
sys.exit(0 if d["nonce"] == "1a2b3c4d" and d["ok"] else 1)'; then
    t_pass "framing: a previous boot's crash replay is not this boot's frame"
else
    t_fail "framing: a previous boot's crash replay is not this boot's frame"
fi

# 6g. A newer run that CLOSES without reconciling is fatal, and must not fall
#     back to an older run that happened to reconcile -- that publishes stale
#     results under a verdict the newest run never earned.
frame_log "1a2b3c4d" "$FRAME_TMP/stale.log"
sed -E 's/run=1/run=2/; s/records=6/records=99/' "$FRAME_TMP/stale.log" \
    > "$FRAME_TMP/stale2.log"
cat "$FRAME_TMP/stale2.log" >> "$FRAME_TMP/stale.log"
if ! python3 "$FRAME_PARSER" "$FRAME_TMP/stale.log" >/dev/null 2>&1; then
    t_pass "framing: a newer unreconciled run does not fall back to an older one"
else
    t_fail "framing: a newer unreconciled run does not fall back to an older one"
fi

# 6h. ONE foreign announcement is fatal. The main runner used to fail only at
#     two or more, so the normal forgery shape passed there while the other
#     two consumers refused it -- the exact drift the shared parser removes.
frame_log "1a2b3c4d" "$FRAME_TMP/one.log"
echo "[  9.000] [cpu:0] [ OK ] UTEST-deadbeef: [UTEST-FRAME] v=1 run=1" >> "$FRAME_TMP/one.log"
if ! python3 "$FRAME_PARSER" "$FRAME_TMP/one.log" >/dev/null 2>&1; then
    t_pass "framing: a single foreign announcement fails the frame"
else
    t_fail "framing: a single foreign announcement fails the frame"
fi

# 6i. A malformed numeric field must be refused, not crash the parser: an
#     unbounded digit run reaches int() and, past CPython's integer-string
#     limit, raises -- killing the harvester before it can write its refusal
#     envelope.
BIGRUN=$(python3 -c 'print("9" * 5000)')
echo "[  1.000] [cpu:0] [ OK ] UTEST-1a2b3c4d: [UTEST-FRAME] v=1 run=${BIGRUN}" \
    > "$FRAME_TMP/big.log"
python3 "$FRAME_PARSER" "$FRAME_TMP/big.log" >/dev/null 2>&1; BIGRC=$?
if [ "$BIGRC" -ne 0 ] && [ "$BIGRC" -ne 3 ]; then
    t_pass "framing: an oversized numeric field is refused, not a traceback"
else
    t_fail "framing: an oversized numeric field is refused, not a traceback" "rc=$BIGRC"
fi

# 6e. The production consumers must AGREE with the parser. A consumer that
#     validates differently is a consumer that passes what the others refuse.
if grep -q 'utest-frame.py' "$REPO_ROOT/scripts/test.sh" &&
   grep -q 'utest-frame.py' "$REPO_ROOT/scripts/test-swtpm.sh" &&
   grep -q 'utest-frame.py' "$REPO_ROOT/scripts/utest-json-harvest.py"; then
    t_pass "framing: all three host consumers use the canonical parser"
else
    t_fail "framing: all three host consumers use the canonical parser" \
           "a consumer still carries its own copy of the framing rules"
fi

# 6f. The INCREMENTAL resume must be equivalent to a full reparse -- on every
#     append prefix, not just the convenient ones. The poll resumes from a
#     byte offset once per second, and acceptance depends on far more than an
#     offset: a resume that dropped an earlier foreign nonce or a sticky
#     bad_close would let a later genuine terminator report GREEN where a full
#     reparse stays red. That is a false green, so it is asserted byte by
#     byte against an independent oracle (Python's own universal-newline
#     reader) rather than against the parser's own line splitter.
FRAME_DIFF_OUT="$FRAME_TMP/differential.txt"
if python3 "$REPO_ROOT/scripts/utest-frame-difftest.py" \
        > "$FRAME_DIFF_OUT" 2>&1; then
    FRAME_DIFF_N=$(grep -c '^ok ' "$FRAME_DIFF_OUT" || echo 0)
    t_pass "framing: incremental resume equals a full reparse ($FRAME_DIFF_N checks)"
else
    t_fail "framing: incremental resume equals a full reparse" \
           "$(grep -m3 '^FAIL' "$FRAME_DIFF_OUT" || tail -3 "$FRAME_DIFF_OUT")"
fi

# 6g. utest_frame_poll (scripts/test.sh) must never let a poll FAILURE look
#     like "no frame present": a config that does not require XML/JSON/TAP
#     accepts the kernel summary alone once the frame-presence check comes
#     back empty, and empty is exactly what a killed child or truncated
#     output also produces. It must also survive scripts/test.sh's own
#     `set -euo pipefail` -- an earlier draft used a bare `out="$(cmd)"; rc=$?`
#     that aborted the WHOLE SCRIPT the first time the parser legitimately
#     exited 1 or 2 (its ROUTINE not-complete-yet answer), confirmed with a
#     minimal repro outside any inherit_errexit shopt. The function is
#     extracted VERBATIM from scripts/test.sh (never hand-copied), so this
#     test can never silently drift from what actually ships; $PROJECT is
#     pointed at a fake tree so the child command is a stand-in script
#     instead of the real parser, with no PATH shadowing tricks needed.
UFP_SRC="$(sed -n '/^utest_frame_poll() {/,/^}/p' "$REPO_ROOT/scripts/test.sh")"
if [ -z "$UFP_SRC" ]; then
    t_fail "utest_frame_poll: extracted from scripts/test.sh" "function not found in scripts/test.sh"
else
    UFP_FAKE_PROJECT="$FRAME_TMP/ufp-fake-project"
    mkdir -p "$UFP_FAKE_PROJECT/scripts"
    UFP_LOG="$FRAME_TMP/ufp.log"
    : > "$UFP_LOG"

    ufp_run() {
        # $1 = fake utest-frame.py body (a python3 script's source), $2 = budget
        printf '%s\n' "$1" > "$UFP_FAKE_PROJECT/scripts/utest-frame.py"
        bash -c '
set -euo pipefail
PROJECT="'"$UFP_FAKE_PROJECT"'"
UTEST_POLL_STATE="'"$FRAME_TMP"'/ufp-state.json"
'"$UFP_SRC"'
utest_frame_poll "'"$UFP_LOG"'" "'"$2"'"
echo
echo "SURVIVED"
' 2>&1
    }

    # (a) errexit-safety: the parser's ROUTINE exit 2 ("no frame learned")
    #     must not abort the calling script.
    UFP_A=$(ufp_run 'import sys; print("{}"); sys.exit(2)' 5)
    if printf '%s' "$UFP_A" | tail -1 | grep -q '^SURVIVED$'; then
        t_pass "utest_frame_poll: survives set -e on the parser's routine exit 2"
    else
        t_fail "utest_frame_poll: survives set -e on the parser's routine exit 2" "$UFP_A"
    fi

    # (b) a killed child (budget exhausted) must report inconclusive, not
    #     something a "no frame ever" reading would accept.
    UFP_B=$(ufp_run 'import time; time.sleep(5)' 1)
    if printf '%s' "$UFP_B" | grep -q '__poll_inconclusive__' &&
       printf '%s' "$UFP_B" | tail -1 | grep -q '^SURVIVED$'; then
        t_pass "utest_frame_poll: a killed child reports inconclusive, not no-frame"
    else
        t_fail "utest_frame_poll: a killed child reports inconclusive, not no-frame" "$UFP_B"
    fi

    # (c) truncated-but-brace-matching output (a kill landing right after a
    #     nested object closes) must be rejected by real JSON validation, not
    #     waved through by a glob on the outer braces.
    UFP_C=$(ufp_run 'import sys; sys.stdout.write("{\"runs\": [{\"run\": 1}"); sys.exit(0)' 5)
    if printf '%s' "$UFP_C" | grep -q '__poll_inconclusive__'; then
        t_pass "utest_frame_poll: truncated-but-brace-matching output is rejected"
    else
        t_fail "utest_frame_poll: truncated-but-brace-matching output is rejected" "$UFP_C"
    fi

    # (d) a legitimate, complete, well-formed result must still pass through
    #     untouched -- the hardening above must not also reject good answers.
    UFP_D=$(ufp_run 'import sys; print("{\"prefix\": \"UTEST-abc12345: \", \"ok\": false}"); sys.exit(1)' 5)
    if printf '%s' "$UFP_D" | grep -q '"prefix": "UTEST-abc12345: "'; then
        t_pass "utest_frame_poll: a well-formed result still passes through"
    else
        t_fail "utest_frame_poll: a well-formed result still passes through" "$UFP_D"
    fi
fi

# 6h. The run-slice failure refusal must stay wired, and the per-run record
#     and alias publication must keep its ordering. The refusal shipped
#     alongside the streamed run-slice assembler, which exercised it ONCE, by
#     hand, by forcing --emit-run to fail; nothing re-checked it afterwards,
#     so a control-flow regression could quietly restore the mixed-run
#     artifact the branch exists to refuse.
#
#     This drives the PRODUCTION control flow, not just the document emitter.
#     Both the helper functions and the whole slice-failure REGION are
#     extracted VERBATIM from scripts/test.sh, so what runs here is what
#     ships: a rendering-only test would have proved the refusal renders while
#     saying nothing about whether --emit-run failure still reaches it, still
#     counts a failure, still marks the format published, or is still
#     protected from the later no-summary branch.
UAR_FNS=""
for _fn in utest_xml_identity_attrs utest_json_identity utest_xml_identity_props \
           utest_publish utest_alias_record utest_publish_leg_set \
           utest_commit_record \
           utest_publish_missing_refusals utest_finalize_record \
           utest_xml_refusal_doc utest_publish_xml_refusal; do
    _body="$(sed -n "/^${_fn}() {/,/^}/p" "$REPO_ROOT/scripts/test.sh")"
    if [ -z "$_body" ]; then
        t_fail "artifact record: extracted $_fn from scripts/test.sh" "function not found"
        UAR_FNS=""
        break
    fi
    UAR_FNS="$UAR_FNS
$_body"
done
# The extracted span STARTS at the XML_SRC assignment, not at
# XML_SLICE_FAILED=0. Hardcoding XML_SRC in the harness would let production
# drift back to a shared run-slice path -- the exact shared-state shape this
# section removed -- while this test kept passing against its own private one.
UAR_REGION="$(sed -n '/^    XML_SRC="\$RECORD_DIR\/runslice"$/,/^    fi  # XML_SLICE_FAILED guard/p' \
              "$REPO_ROOT/scripts/test.sh")"
if [ -z "$UAR_REGION" ]; then
    t_fail "artifact record: extracted the slice-failure region from scripts/test.sh" \
        "anchors not found -- XML_SRC assignment .. 'fi  # XML_SLICE_FAILED guard'"
elif [ -n "$UAR_FNS" ]; then
    UAR_TMP="$FRAME_TMP/uar"
    rm -rf "$UAR_TMP"
    mkdir -p "$UAR_TMP/build" "$UAR_TMP/scripts"
    # A fake parser that FAILS is how section 29 forced the branch by hand.
    # Automating that injection is the whole point of this sub-test.
    printf '%s\n' 'import sys; sys.exit(1)' > "$UAR_TMP/scripts/utest-frame.py"
    # A capture carrying a testcase payload: if the refusal ever falls back to
    # the whole capture again, this name shows up in the artifacts.
    UAR_LOG="$UAR_TMP/build/test.log"
    {
        printf 'UTEST-deadbeef: [UTEST-XML] <testcase name="leaked-from-capture" classname="x"/>\n'
        printf 'UTEST-deadbeef: [UTEST-XML-SUMMARY] tests=1 failures=0 skipped=0 time=1.0 aborted=0 not_run=0\n'
    } > "$UAR_LOG"

    UAR_OUT="$(bash -c '
set -euo pipefail
RED=""; YELLOW=""; CYAN=""; RESET=""
PROJECT="'"$UAR_TMP"'"
RECORD_DIR="'"$UAR_TMP"'/build/test-runs/run1"
mkdir -p "$RECORD_DIR"
TEST_LOG="'"$UAR_LOG"'"
XML_RECORD="$RECORD_DIR/test-results.xml"
JSON_RECORD="$RECORD_DIR/test-results.json"
IDENTITY_RECORD="$RECORD_DIR/test-run-identity.json"
RECORD_MARKER="$RECORD_DIR/record-complete.json"
XML_LEG_OUT="'"$UAR_TMP"'/build/test-results-leg.xml"
XML_OUT="'"$UAR_TMP"'/build/test-results.xml"
JSON_LEG_OUT="'"$UAR_TMP"'/build/test-results-leg.json"
JSON_OUT="'"$UAR_TMP"'/build/test-results.json"
IDENTITY_OUT="'"$UAR_TMP"'/build/test-run-identity.json"
RUN_ID="run1"; RUN_TS="t"; RUN_COMMIT="c"; RUN_LEG="leg"; RUN_LEG_SOURCE="derived"
RUN_ACCEL="tcg"; RUN_HOST="h"; RUN_HOSTNAME="hn"; RUN_QEMU="q"; SMP_CPUS_SAFE="1"
CI_PARITY=0
UTEST_FAIL=0; XML_PUBLISHED=0; XML_SUMMARY_OK=1; UTEST_FINALIZED=0
UF="UTEST-deadbeef: "
FRAME_COMPLETE_RUN="1"
'"$UAR_FNS"'
'"$UAR_REGION"'
utest_finalize_record complete
# XML_SRC comes from the extracted production span, never from this harness:
# report where production actually put it so a move back to a shared path is
# a test failure rather than an invisible drift.
echo "SLICE $XML_SRC"
echo "RESULT utest_fail=$UTEST_FAIL xml_published=$XML_PUBLISHED summary_ok=$XML_SUMMARY_OK"
' 2>&1)"

    uar_doc_ok() {
        # $1 = path. A published refusal must carry errors="1" and the
        # slice-specific message, and must NOT carry the capture payload.
        [ -f "$1" ] &&
        grep -q 'errors="1"' "$1" &&
        grep -q 'artifact would mix runs' "$1" &&
        ! grep -q 'leaked-from-capture' "$1"
    }

    if printf '%s' "$UAR_OUT" | grep -q 'RESULT utest_fail=1 xml_published=1 summary_ok=0'; then
        t_pass "slice-failure refusal: fails the run, marks XML published, stops assembly"
    else
        t_fail "slice-failure refusal: fails the run, marks XML published, stops assembly" "$UAR_OUT"
    fi

    # The run slice must live inside the per-run record, not at a shared path.
    if printf '%s' "$UAR_OUT" | grep -q "SLICE $UAR_TMP/build/test-runs/run1/"; then
        t_pass "run record: production puts the run slice inside the record directory"
    else
        t_fail "run record: production puts the run slice inside the record directory" "$UAR_OUT"
    fi

    if uar_doc_ok "$UAR_TMP/build/test-runs/run1/test-results.xml"; then
        t_pass "slice-failure refusal: errors=1 document lands in the run RECORD"
    else
        t_fail "slice-failure refusal: errors=1 document lands in the run RECORD" "$UAR_OUT"
    fi

    if uar_doc_ok "$UAR_TMP/build/test-results-leg.xml"; then
        t_pass "slice-failure refusal: errors=1 document lands at the LEG alias"
    else
        t_fail "slice-failure refusal: errors=1 document lands at the LEG alias" "$UAR_OUT"
    fi

    if uar_doc_ok "$UAR_TMP/build/test-results.xml"; then
        t_pass "slice-failure refusal: errors=1 document lands at the CANONICAL alias"
    else
        t_fail "slice-failure refusal: errors=1 document lands at the CANONICAL alias" "$UAR_OUT"
    fi

    # The no-summary and normal-assembly paths must not overwrite the precise
    # diagnosis with a generic one, and must not count a second failure for
    # the same event. utest_fail=1 above is half the proof; the message
    # surviving in the canonical alias is the other half.
    if grep -q 'artifact would mix runs' "$UAR_TMP/build/test-results.xml" 2>/dev/null &&
       ! grep -q 'counts unknown' "$UAR_TMP/build/test-results.xml" 2>/dev/null; then
        t_pass "slice-failure refusal: not overwritten by the no-summary branch"
    else
        t_fail "slice-failure refusal: not overwritten by the no-summary branch" "$UAR_OUT"
    fi

    # The record is only trustworthy once its commit marker names what it
    # holds. A directory that merely exists is abandoned staging.
    if [ -f "$UAR_TMP/build/test-runs/run1/record-complete.json" ] &&
       grep -q '"xml": "test-results.xml"' "$UAR_TMP/build/test-runs/run1/record-complete.json" &&
       grep -q '"run_id": "run1"' "$UAR_TMP/build/test-runs/run1/record-complete.json"; then
        t_pass "run record: the completion marker names the documents it holds"
    else
        t_fail "run record: the completion marker names the documents it holds" \
            "$(cat "$UAR_TMP/build/test-runs/run1/record-complete.json" 2>&1 || true)"
    fi

    # No staging file may survive publication -- a leftover .refusal.xml or
    # .tmp is the shared-state shape section 30 removed.
    UAR_STAGE="$(find "$UAR_TMP/build/test-runs/run1" -maxdepth 1 -name '.*' -type f 2>/dev/null | wc -l)"
    if [ "$UAR_STAGE" -eq 0 ]; then
        t_pass "run record: no staging file survives publication"
    else
        t_fail "run record: no staging file survives publication" \
            "$(find "$UAR_TMP/build/test-runs/run1" -maxdepth 1 -name '.*' -type f 2>&1)"
    fi

    # BEHAVIORAL fault injection, not a textual ordering grep. The structural
    # assertions below (i12c) only prove the commit call is written above the
    # alias calls; they passed while production published aliases after a
    # FAILED marker, because the failure was swallowed. These drive the real
    # functions and assert on the filesystem.
    uar_drive() {
        # $1 = extra shell setup injected before finalization, $2 = status
        bash -c '
set -euo pipefail
RED=""; YELLOW=""; CYAN=""; RESET=""
RECORD_DIR="'"$UAR_TMP"'/fi/rec"
XML_RECORD="$RECORD_DIR/test-results.xml"
JSON_RECORD="$RECORD_DIR/test-results.json"
IDENTITY_RECORD="$RECORD_DIR/test-run-identity.json"
RECORD_MARKER="$RECORD_DIR/record-complete.json"
XML_LEG_OUT="'"$UAR_TMP"'/fi/test-results-leg.xml"
JSON_LEG_OUT="'"$UAR_TMP"'/fi/test-results-leg.json"
XML_OUT="'"$UAR_TMP"'/fi/test-results.xml"
JSON_OUT="'"$UAR_TMP"'/fi/test-results.json"
IDENTITY_OUT="'"$UAR_TMP"'/fi/test-run-identity.json"
RUN_ID="rec"; RUN_TS="t"; RUN_COMMIT="c"; RUN_LEG="leg"; RUN_LEG_SOURCE="d"
RUN_ACCEL="tcg"; RUN_HOST="h"; RUN_HOSTNAME="hn"; RUN_QEMU="q"; SMP_CPUS_SAFE="1"
CI_PARITY=0; XML_MODE=0; JSON_MODE=0; HAS_XML=0; HAS_JSON=0
UTEST_FAIL=0; XML_PUBLISHED=1; JSON_PUBLISHED=1; UTEST_FINALIZED=0
'"$UAR_FNS"'
'"$1"'
utest_finalize_record "'"$2"'" || echo "FINALIZE_RC=$?"
echo "UTEST_FAIL=$UTEST_FAIL"
' 2>&1
    }
    uar_reset_fi() {
        rm -rf "$UAR_TMP/fi"
        mkdir -p "$UAR_TMP/fi/rec"
        printf '<testsuite/>\n' > "$UAR_TMP/fi/rec/test-results.xml"
        printf '{}\n' > "$UAR_TMP/fi/rec/test-results.json"
        printf '{}\n' > "$UAR_TMP/fi/rec/test-run-identity.json"
    }

    # (a) marker failure -> NOT ONE alias may appear. The injection points
    #     RECORD_MARKER at an unwritable location so the final rename fails.
    #     (Making the marker path a DIRECTORY does not work: `mv file dir/`
    #     succeeds by moving the file INTO it.)
    uar_reset_fi
    UAR_FI_A="$(uar_drive 'RECORD_MARKER=/proc/nonexistent-dir/marker.json' complete)"
    if [ ! -e "$UAR_TMP/fi/test-results.xml" ] &&
       [ ! -e "$UAR_TMP/fi/test-results-leg.xml" ] &&
       [ ! -e "$UAR_TMP/fi/test-run-identity.json" ] &&
       printf '%s' "$UAR_FI_A" | grep -q 'FINALIZE_RC=1'; then
        t_pass "run record: a failed commit marker publishes NO alias"
    else
        t_fail "run record: a failed commit marker publishes NO alias" \
            "$UAR_FI_A -- aliases: $(ls "$UAR_TMP/fi" 2>&1)"
    fi

    # (b) an INCOMPLETE record may reach the canonical alias (that names this
    #     invocation) but must never replace the leg alias, which means the
    #     latest COMPLETED run of that configuration.
    uar_reset_fi
    printf 'PREVIOUS-COMPLETED\n' > "$UAR_TMP/fi/test-results-leg.xml"
    UAR_FI_B="$(uar_drive 'true' incomplete)"
    if [ -f "$UAR_TMP/fi/test-results.xml" ] &&
       grep -q 'PREVIOUS-COMPLETED' "$UAR_TMP/fi/test-results-leg.xml"; then
        t_pass "run record: an incomplete record cannot replace the latest-completed leg alias"
    else
        t_fail "run record: an incomplete record cannot replace the latest-completed leg alias" \
            "$UAR_FI_B -- leg: $(cat "$UAR_TMP/fi/test-results-leg.xml" 2>&1)"
    fi

    # (c) a COMPLETE record does replace the leg alias -- the guard above must
    #     not have turned into a blanket refusal.
    uar_reset_fi
    printf 'PREVIOUS-COMPLETED\n' > "$UAR_TMP/fi/test-results-leg.xml"
    UAR_FI_C="$(uar_drive 'true' complete)"
    if [ -f "$UAR_TMP/fi/test-results-leg.xml" ] &&
       ! grep -q 'PREVIOUS-COMPLETED' "$UAR_TMP/fi/test-results-leg.xml" &&
       [ -f "$UAR_TMP/fi/rec/record-complete.json" ]; then
        t_pass "run record: a complete record does replace the leg alias"
    else
        t_fail "run record: a complete record does replace the leg alias" "$UAR_FI_C"
    fi

    # (d) finalization is idempotent: a second call (the EXIT guard after the
    #     normal path already finalized) must not re-publish or re-mark.
    uar_reset_fi
    UAR_FI_D="$(uar_drive 'utest_finalize_record complete; printf MARK > "$RECORD_MARKER"' complete)"
    if [ "$(cat "$UAR_TMP/fi/rec/record-complete.json" 2>/dev/null)" = "MARK" ]; then
        t_pass "run record: finalization is idempotent across a second call"
    else
        t_fail "run record: finalization is idempotent across a second call" "$UAR_FI_D"
    fi

    # (e) identity is the generation pointer: it must be published LAST, so a
    #     consumer resolving through it never sees it lead the documents.
    UAR_FINFN=$(awk '/^utest_finalize_record\(\)/{f=1} f{print} f&&/^\}/{exit}' \
                "$REPO_ROOT/scripts/test.sh")
    FIN_XML_LN=$(printf '%s\n' "$UAR_FINFN" | grep -n 'XML_RECORD' | head -1 | cut -d: -f1)
    FIN_ID_LN=$(printf '%s\n' "$UAR_FINFN" | grep -n 'IDENTITY_RECORD' | head -1 | cut -d: -f1)
    if [ -n "$FIN_XML_LN" ] && [ -n "$FIN_ID_LN" ] && [ "$FIN_ID_LN" -gt "$FIN_XML_LN" ]; then
        t_pass "run record: the identity alias is published last as the generation pointer"
    else
        t_fail "run record: the identity alias is published last as the generation pointer" "$UAR_FINFN"
    fi

    # (g) the leg alias SET must describe ONE run. A previous run that left
    #     both formats, followed by a complete XML-only run, must not leave
    #     the previous run's JSON beside the new XML -- the exact
    #     enumerate-and-be-misled pair this section exists to close.
    uar_reset_fi
    rm -f "$UAR_TMP/fi/rec/test-results.json"
    printf 'PREV-XML\n' > "$UAR_TMP/fi/test-results-leg.xml"
    printf 'PREV-JSON\n' > "$UAR_TMP/fi/test-results-leg.json"
    UAR_FI_G="$(uar_drive 'true' complete)"
    if [ ! -e "$UAR_TMP/fi/test-results-leg.json" ] &&
       [ -f "$UAR_TMP/fi/test-results-leg.xml" ] &&
       ! grep -q 'PREV-XML' "$UAR_TMP/fi/test-results-leg.xml"; then
        t_pass "run record: a format this run did not produce is dropped from the leg alias set"
    else
        t_fail "run record: a format this run did not produce is dropped from the leg alias set" \
            "$UAR_FI_G -- leg set: $(ls "$UAR_TMP/fi" 2>&1)"
    fi

    # (g2) TRANSITION-sensitive, not final-state. Inspecting only the end
    #      state cannot tell "removed the old set before publishing" from
    #      "published then cleaned up" -- both leave the same files, so an
    #      ordering revert would pass (g) untouched. This instruments `mv` to
    #      record what the leg set looked like at the instant the FIRST new
    #      document was published, and asserts no previous-generation file was
    #      still sitting there. Both formats present, which is the case the
    #      one-format drop never covered.
    uar_reset_fi
    printf 'PREV-XML\n' > "$UAR_TMP/fi/test-results-leg.xml"
    printf 'PREV-JSON\n' > "$UAR_TMP/fi/test-results-leg.json"
    UAR_FI_G2="$(uar_drive 'mv() {
  for _a in "$@"; do
    case "$_a" in
      *test-results-leg.xml|*test-results-leg.json)
        if [ ! -f "'"$UAR_TMP"'/fi/.probe" ]; then
          { grep -l PREV- "'"$UAR_TMP"'/fi"/test-results-leg.* 2>/dev/null || true; } > "'"$UAR_TMP"'/fi/.probe"
        fi ;;
    esac
  done
  command mv "$@"
}' complete)"
    if [ -f "$UAR_TMP/fi/.probe" ] && [ ! -s "$UAR_TMP/fi/.probe" ]; then
        t_pass "run record: the old leg set is gone before any new document is published"
    else
        t_fail "run record: the old leg set is gone before any new document is published" \
            "$UAR_FI_G2 -- stale present at first publish: $(cat "$UAR_TMP/fi/.probe" 2>&1)"
    fi

    # (g3) a staging failure must leave the PREVIOUS complete set intact and
    #      leak no staging file. The injected `cp` writes a PARTIAL
    #      destination before failing, which is what a real ENOSPC or I/O
    #      error does -- an injection that fails before writing anything
    #      could not tell a working cleanup from a deleted one, and the
    #      partial temp would otherwise persist inside the retained record.
    uar_reset_fi
    printf 'PREV-XML\n' > "$UAR_TMP/fi/test-results-leg.xml"
    printf 'PREV-JSON\n' > "$UAR_TMP/fi/test-results-leg.json"
    UAR_FI_G3="$(uar_drive 'cp() {
  for _a in "$@"; do
    case "$_a" in *.jleg.tmp) printf PARTIAL > "$_a"; return 1 ;; esac
  done
  command cp "$@"
}' complete)"
    if grep -q 'PREV-XML' "$UAR_TMP/fi/test-results-leg.xml" 2>/dev/null &&
       grep -q 'PREV-JSON' "$UAR_TMP/fi/test-results-leg.json" 2>/dev/null &&
       [ ! -e "$UAR_TMP/fi/rec/.xleg.tmp" ] && [ ! -e "$UAR_TMP/fi/rec/.jleg.tmp" ] &&
       printf '%s' "$UAR_FI_G3" | grep -q 'UTEST_FAIL=1'; then
        t_pass "run record: a leg staging failure keeps the previous set and leaks no staging file"
    else
        t_fail "run record: a leg staging failure keeps the previous set and leaks no staging file" \
            "$UAR_FI_G3 -- leg set: $(ls -a "$UAR_TMP/fi" "$UAR_TMP/fi/rec" 2>&1)"
    fi

    # (g4) a partially-cleared old set must stop publication outright: the
    #      whole point of clearing as a set is that no new document ever
    #      appears beside a survivor.
    uar_reset_fi
    printf 'PREV-XML\n' > "$UAR_TMP/fi/test-results-leg.xml"
    printf 'PREV-JSON\n' > "$UAR_TMP/fi/test-results-leg.json"
    UAR_FI_G4="$(uar_drive 'rm() {
  for _a in "$@"; do
    case "$_a" in *test-results-leg.xml) command rm -f "$@" 2>/dev/null; printf SURVIVOR > "'"$UAR_TMP"'/fi/test-results-leg.json"; return 0 ;; esac
  done
  command rm "$@"
}' complete)"
    if grep -q 'SURVIVOR' "$UAR_TMP/fi/test-results-leg.json" 2>/dev/null &&
       [ ! -e "$UAR_TMP/fi/test-results-leg.xml" ] &&
       printf '%s' "$UAR_FI_G4" | grep -qE 'UTEST_FAIL=[1-9]'; then
        t_pass "run record: a partially-cleared leg set publishes nothing beside the survivor"
    else
        t_fail "run record: a partially-cleared leg set publishes nothing beside the survivor" \
            "$UAR_FI_G4 -- leg set: $(ls "$UAR_TMP/fi" 2>&1)"
    fi

    # (h) an alias copy that FAILS must leave the destination absent, not
    #     holding the previous run's document beside this run's other format.
    #
    #     The failure is injected with a scoped `cp` shell function that fails
    #     only for the leg temp. An earlier draft used chmod 000 and SKIPPED
    #     as root -- reporting PASS while exercising nothing, so a root CI or
    #     container stayed green even if the destination-removal regressed.
    #     A skip is not a pass, and permissions are incidental to the
    #     invariant anyway.
    #     The move (not the staging copy) is what fails here, so the leg set
    #     has already been cleared: assert the destination is absent, that
    #     EXACTLY one failure was counted, and that the OTHER format still
    #     reached both its leg and canonical aliases from the current record
    #     -- an injected XML failure must not be able to hide a broken JSON
    #     publication behind an aggregate "at least one failure" check.
    uar_reset_fi
    printf 'PREV-XML\n' > "$UAR_TMP/fi/test-results-leg.xml"
    UAR_FI_H="$(uar_drive 'mv() { for _a in "$@"; do case "$_a" in *test-results-leg.xml) return 1 ;; esac; done; command mv "$@"; }' complete)"
    if [ ! -e "$UAR_TMP/fi/test-results-leg.xml" ] &&
       printf '%s' "$UAR_FI_H" | grep -q 'UTEST_FAIL=1' &&
       [ -f "$UAR_TMP/fi/test-results-leg.json" ] &&
       [ -f "$UAR_TMP/fi/test-results.json" ]; then
        t_pass "run record: a failed leg-alias publish leaves it absent, counts one failure, and spares the other format"
    else
        t_fail "run record: a failed leg-alias publish leaves it absent, counts one failure, and spares the other format" \
            "$UAR_FI_H -- leg set: $(ls "$UAR_TMP/fi" 2>&1)"
    fi

    # (f) a format the run OWED but never published gets its refusal INTO the
    #     record before the marker describes it -- otherwise the marker can
    #     say "json": null while cleanup later writes a json document.
    uar_reset_fi
    rm -f "$UAR_TMP/fi/rec/test-results.json"
    UAR_FI_F="$(uar_drive 'JSON_MODE=1; JSON_PUBLISHED=0' complete)"
    if [ -f "$UAR_TMP/fi/rec/test-results.json" ] &&
       grep -q 'run_incomplete' "$UAR_TMP/fi/rec/test-results.json" &&
       grep -q '"json": "test-results.json"' "$UAR_TMP/fi/rec/record-complete.json"; then
        t_pass "run record: an owed-but-unpublished format is refused before the marker names it"
    else
        t_fail "run record: an owed-but-unpublished format is refused before the marker names it" \
            "$UAR_FI_F -- marker: $(cat "$UAR_TMP/fi/rec/record-complete.json" 2>&1)"
    fi

    # Durability must be BOUNDED: every invocation leaves a record directory,
    # so an unpruned build/test-runs grows for as long as anyone runs tests.
    # The newest N survive and the run in progress is never a candidate.
    UAR_PRUNE="$(sed -n '/^utest_prune_records() {/,/^}/p' "$REPO_ROOT/scripts/test.sh")"
    if [ -z "$UAR_PRUNE" ]; then
        t_fail "run record: retention prunes to the newest N" "utest_prune_records not found"
    else
        UAR_PDIR="$UAR_TMP/prune"
        mkdir -p "$UAR_PDIR"
        for _i in 1 2 3 4 5; do mkdir -p "$UAR_PDIR/2026010${_i}T000000Z-1-aaaa"; done
        UAR_PRES="$(bash -c '
set -euo pipefail
RUNS_DIR="'"$UAR_PDIR"'"
RUN_ID="20260101T000000Z-1-aaaa"
UTEST_RECORD_KEEP=2
'"$UAR_PRUNE"'
utest_prune_records
ls -1 "$RUNS_DIR" | sort | tr "\n" " "
' 2>&1)"
        # keep=2 retains the two newest (…04, …05); …01 is the current RUN_ID
        # and must survive even though it is older than the cut.
        if printf '%s' "$UAR_PRES" | grep -q '20260101T000000Z-1-aaaa' &&
           printf '%s' "$UAR_PRES" | grep -q '20260104T000000Z-1-aaaa' &&
           printf '%s' "$UAR_PRES" | grep -q '20260105T000000Z-1-aaaa' &&
           ! printf '%s' "$UAR_PRES" | grep -q '20260102T000000Z-1-aaaa'; then
            t_pass "run record: retention prunes to the newest N and spares the running one"
        else
            t_fail "run record: retention prunes to the newest N and spares the running one" "$UAR_PRES"
        fi
    fi
fi

# 6i. Concurrent same-tree runs are an explicitly UNSUPPORTED mode, and the
#     refusal must not touch a single shared name. A second run that deleted
#     the first run's canonical artifacts before deferring would be worse than
#     the emergent interleaving it replaced.
if grep -q 'flock -n "\$UTEST_LOCK_FD"' "$REPO_ROOT/scripts/test.sh"; then
    t_pass "concurrency: scripts/test.sh takes an exclusive run lock"
else
    t_fail "concurrency: scripts/test.sh takes an exclusive run lock" "flock guard not found"
fi
# Ordering is the load-bearing part: the lock must be acquired BEFORE the
# canonical artifacts are cleared.
UAR_LOCK_LN="$(grep -n 'flock -n "\$UTEST_LOCK_FD"' "$REPO_ROOT/scripts/test.sh" | head -1 | cut -d: -f1)"
UAR_RM_LN="$(grep -n '^rm -f "\$XML_OUT" "\$JSON_OUT" "\$IDENTITY_OUT"$' "$REPO_ROOT/scripts/test.sh" | head -1 | cut -d: -f1)"
if [ -n "$UAR_LOCK_LN" ] && [ -n "$UAR_RM_LN" ] && [ "$UAR_LOCK_LN" -lt "$UAR_RM_LN" ]; then
    t_pass "concurrency: the run lock is taken before any shared name is cleared"
else
    t_fail "concurrency: the run lock is taken before any shared name is cleared" \
        "lock at line ${UAR_LOCK_LN:-none}, canonical clear at line ${UAR_RM_LN:-none}"
fi
# An orphaned QEMU must not inherit the lock descriptor: SIGKILL bypasses the
# cleanup trap, and an inherited fd would let the orphan hold the lock and
# wedge every later run.
if grep -q '9>&- &' "$REPO_ROOT/scripts/test.sh"; then
    t_pass "concurrency: QEMU is launched with the lock descriptor closed"
else
    t_fail "concurrency: QEMU is launched with the lock descriptor closed" "9>&- not found on the QEMU launch"
fi
# Leg aliases mean "latest COMPLETED run of this leg". No blanket clear may
# exist ANYWHERE -- not another configuration's (which would destroy a
# sequential matrix's accumulated results), and not an up-front clear of this
# leg's own (a run that then died would erase a perfectly good completed
# artifact). The ONLY legal removals live in utest_publish_leg_set: the
# set-level clear between staging and publication, and the fail-closed drop
# when a publish fails. Behaviour is asserted in 6h(g), 6h(g2), 6h(g3) and
# 6h(h); this pins that no OTHER clear crept back in.
UAR_LEG_RM_ALL="$(grep -c 'rm -f .*LEG_OUT' "$REPO_ROOT/scripts/test.sh")"
UAR_LEG_RM_OK="$(awk '/^utest_publish_leg_set\(\)/{a=1}
                      a && /rm -f .*LEG_OUT/{n++} a && /^\}/{a=0} END{print n+0}' \
                 "$REPO_ROOT/scripts/test.sh")"
if [ "$UAR_LEG_RM_ALL" -eq "$UAR_LEG_RM_OK" ] &&
   ! grep -q 'rm -f .*test-results-\*' "$REPO_ROOT/scripts/test.sh"; then
    t_pass "aliases: leg aliases are cleared only by the finalizer's coherence and fail-closed drops"
else
    t_fail "aliases: leg aliases are cleared only by the finalizer's coherence and fail-closed drops" \
        "$UAR_LEG_RM_ALL leg removals, $UAR_LEG_RM_OK inside the finalizer: $(grep -n 'rm -f .*LEG_OUT' "$REPO_ROOT/scripts/test.sh" 2>&1)"
fi

# 7. The disk sink must never SERIALIZE a raw subsystem tag. The frame nonce
#    is an authenticating value and every disk log lives in a directory ring
#    3 can open, so each rendering has to go through klog_disk_subsystem().
#    Checked generally rather than by output-shape: the first version of this
#    gate matched only buf_puts()/line[] formatting and therefore missed the
#    events.jsonl serializer, which kept publishing the real nonce while the
#    text logs withheld it. A lookup that returns a NUMBER (klog_get_dropped)
#    is not a disclosure and is the one allowed raw use.
RAW_SUBSYS=$(grep -n 'e->subsystem' "$REPO_ROOT/src/kernel/klog_disk.c" |
             grep -v 'klog_disk_subsystem' |
             grep -vc 'klog_get_dropped' || true)
if [ "${RAW_SUBSYS:-1}" -eq 0 ]; then
    t_pass "framing: no disk sink serializes a raw subsystem tag"
else
    t_fail "framing: no disk sink serializes a raw subsystem tag" \
           "unaliased e->subsystem reads: ${RAW_SUBSYS}"
fi

# 8. The live fixture must stay wired: it is the only ring-3 producer of
#    forged launcher records, and a run without it proves nothing.
if grep -q '^test_forge\.exe$' "$REPO_ROOT/tests/usermode.manifest" &&
   grep -q 'test_forge\.exe' "$REPO_ROOT/Makefile"; then
    t_pass "framing: the ring-3 forgery fixture is wired into the manifest and build"
else
    t_fail "framing: the ring-3 forgery fixture is wired into the manifest and build"
fi

# --- run identity + artifact publication lifecycle --------------------------
#
# The artifacts describe a run that has to be placeable: which commit, when,
# and on which leg. These fixtures pin the two properties that are easy to
# regress -- identity reaches EVERY envelope including the refusals, and no
# exit path may leave a previous run's success standing at a canonical path.

IDENT_TMP=$(mktemp -d)
trap 'rm -rf "$FRAME_TMP" "$IDENT_TMP"' EXIT

cat > "$IDENT_TMP/identity.json" <<'IDENTEOF'
{
 "schema": "utest-run-identity-v1",
 "timestamp": "2026-07-29T00:00:00Z",
 "commit": "0123456789abcdef0123456789abcdef01234567",
 "leg": "fixture-tcg-2cpu",
 "leg_source": "derived",
 "accel": "tcg",
 "cpus": 2
}
IDENTEOF

# i1. A clean harvest carries identity beside the summary, not instead of it.
frame_log "feedface" "$IDENT_TMP/good.log"
if python3 "$HARVEST" "$IDENT_TMP/good.log" "$IDENT_TMP/good.json" \
       --identity "$IDENT_TMP/identity.json" >/dev/null 2>&1 &&
   python3 -c "
import json,sys
d=json.load(open('$IDENT_TMP/good.json'))
ok = (d['run_identity']['leg'] == 'fixture-tcg-2cpu'
      and d['run_identity']['commit'].startswith('0123456789')
      and d['summary']['total'] == 1)
sys.exit(0 if ok else 1)"; then
    t_pass "identity: a clean harvest carries run identity alongside a real summary"
else
    t_fail "identity: a clean harvest carries run identity alongside a real summary"
fi

# i2. A REFUSED artifact is the one a dashboard most needs to place, so it
#     carries identity too -- without softening the refusal markers. An
#     identity-bearing envelope that lost `summary: null` would read as a
#     clean empty run.
sed -E 's/UTEST-feedface: //' "$IDENT_TMP/good.log" > "$IDENT_TMP/unframed.log"
python3 "$HARVEST" "$IDENT_TMP/unframed.log" "$IDENT_TMP/refused.json" \
    --identity "$IDENT_TMP/identity.json" >/dev/null 2>&1 && IRC=0 || IRC=$?
if [ "$IRC" != "0" ] && python3 -c "
import json,sys
d=json.load(open('$IDENT_TMP/refused.json'))
ok = (d['summary'] is None and d['summary_error']
      and d['run_identity']['leg'] == 'fixture-tcg-2cpu')
sys.exit(0 if ok else 1)"; then
    t_pass "identity: a refused artifact carries identity and stays refused"
else
    t_fail "identity: a refused artifact carries identity and stays refused" "rc=$IRC"
fi

# i3. Missing provenance must not manufacture a red. The artifact's gates are
#     about stream integrity; failing a good harvest because the identity file
#     is absent would turn a metadata gap into a false failure.
if python3 "$HARVEST" "$IDENT_TMP/good.log" "$IDENT_TMP/noident.json" \
       --identity "$IDENT_TMP/does-not-exist.json" >/dev/null 2>&1 &&
   python3 -c "
import json,sys
d=json.load(open('$IDENT_TMP/noident.json'))
sys.exit(0 if d['run_identity'] is None and d['summary']['total'] == 1 else 1)"; then
    t_pass "identity: an unreadable identity file publishes without it, not red"
else
    t_fail "identity: an unreadable identity file publishes without it, not red"
fi

# i4. The two-argument form still works. test-swtpm.sh and any ad-hoc caller
#     invoke the harvester without identity.
if python3 "$HARVEST" "$IDENT_TMP/good.log" "$IDENT_TMP/legacy.json" >/dev/null 2>&1; then
    t_pass "identity: the harvester's two-argument form is unchanged"
else
    t_fail "identity: the harvester's two-argument form is unchanged"
fi

# i5. Lifecycle ordering. The canonical artifacts must be invalidated before
#     ANY exit in the runner: the CI-parity preflight (absent QEMU), the
#     SMP_CPUS check and the UTEST_LEG refusal all exit, and each one is a
#     failed invocation that must not leave the last run's success behind.
LEGSH="$REPO_ROOT/scripts/test.sh"
if awk '/rm -f "\$XML_OUT" "\$JSON_OUT" "\$IDENTITY_OUT"/{inv=NR}
        /CI_PARITY=1 but/{ciexit=NR}
        /is not a CPU count/{cpuexit=NR}
        /is not a valid leg label/{legexit=NR}
        END{exit !(inv && ciexit && cpuexit && legexit &&
                   inv < ciexit && inv < cpuexit && inv < legexit)}' "$LEGSH"; then
    t_pass "identity: canonical artifacts are invalidated before every exit that can precede assembly"
else
    t_fail "identity: canonical artifacts are invalidated before every exit that can precede assembly"
fi

# i6. The guard is armed before the environment preflight, and the boot.conf
#     cleanup handler that REPLACES it re-calls the guard. Bash keeps one
#     handler per signal: if the second one forgot the guard, every failure
#     after boot.conf patching would go back to leaving a stale file.
if awk '/^trap .utest_artifact_guard \$\?. EXIT/{early=NR}
        /^# CI_PARITY=1 --/{pre=NR}
        END{exit !(early && pre && early < pre)}' "$LEGSH" &&
   awk '/^cleanup\(\)/{c=1} c && /utest_artifact_guard "\$ec"/{found=1}
        END{exit !found}' "$LEGSH"; then
    t_pass "identity: the artifact guard is armed before the preflight and survives trap replacement"
else
    t_fail "identity: the artifact guard is armed before the preflight and survives trap replacement"
fi

# i7. The -smp flag and the leg name must come from ONE variable. While the
#     flag was a bare literal, any identifier naming a CPU count was an
#     assertion nothing kept true.
if grep -q -- '-smp "\$SMP_CPUS"' "$LEGSH" && grep -q 'SMP_CPUS_SAFE}cpu' "$LEGSH"; then
    t_pass "identity: the -smp flag and the leg's CPU count share one variable"
else
    t_fail "identity: the -smp flag and the leg's CPU count share one variable"
fi

# i8. A cancelled run must NOT exit 0. The signal handler assigns before
#     calling cleanup, which leaves $? at 0, so a run killed mid-suite used to
#     report success to its caller while its own artifact said the run never
#     finished -- a green CI step over a run_incomplete document.
if grep -q "trap 'UTEST_SIGNALLED=1; cleanup 130' INT" "$LEGSH" &&
   grep -q "trap 'UTEST_SIGNALLED=1; cleanup 143' TERM" "$LEGSH" &&
   awk '/^cleanup\(\)/{c=1} c && /trap - EXIT INT TERM/{found=1} END{exit !found}' "$LEGSH"; then
    t_pass "identity: signals carry 128+signo and cleanup disarms its own traps"
else
    t_fail "identity: signals carry 128+signo and cleanup disarms its own traps"
fi

# i9. Publication state is tracked PER FORMAT. A single flag let a failure
#     landing between the XML publication and the JSON assembly suppress the
#     JSON refusal, leaving a requested artifact absent with nothing saying why.
if grep -q 'XML_PUBLISHED=0' "$LEGSH" && grep -q 'JSON_PUBLISHED=0' "$LEGSH" &&
   ! grep -q 'ARTIFACT_PUBLISHED' "$LEGSH" &&
   awk '/utest_publish_missing_refusals\(\)/{g=1}
        g && /XML_PUBLISHED:-0.*-eq 0/{x=1}
        g && /JSON_PUBLISHED:-0.*-eq 0/{j=1}
        END{exit !(x && j)}' "$LEGSH"; then
    t_pass "identity: XML and JSON publication state is tracked independently"
else
    t_fail "identity: XML and JSON publication state is tracked independently"
fi

# i10. Every identity field that reaches a filename, an XML attribute or an
#      unquoted JSON number is validated or filtered. SMP_CPUS=2x published
#      invalid JSON; a QEMU basename carrying a quote or ampersand broke both
#      formats. Filtering is what lets the emitters skip escaping.
if awk '/SMP_CPUS_VALID=0/{v=1} /-le 256/{b=1} END{exit !(v && b)}' "$LEGSH" &&
   grep -q "RUN_QEMU=\"\$(basename \"\$QEMU_BIN\" | tr -cd 'A-Za-z0-9._-')\"" "$LEGSH" &&
   grep -q "rev-parse HEAD 2>/dev/null | tr -cd 'a-f0-9'" "$LEGSH"; then
    t_pass "identity: CPU count is range-checked and every free-text field is charset-filtered"
else
    t_fail "identity: CPU count is range-checked and every free-text field is charset-filtered"
fi

# i11. UNTRACKED files must mark the tree dirty. The Makefile discovers
#      sources with find (ASM_SRCS/C_SRCS), so an untracked .c or .asm is
#      compiled INTO the image under test; `git diff --quiet HEAD` ignores it
#      exactly, which would attribute an unreproducible run to a clean SHA.
if grep -q 'git -C "\$PROJECT" status --porcelain' "$LEGSH" &&
   ! grep -q 'git -C "\$PROJECT" diff --quiet HEAD' "$LEGSH" &&
   grep -qE '^(ASM|C)_SRCS[[:space:]]*:=[[:space:]]*\$\(shell find' "$REPO_ROOT/Makefile"; then
    t_pass "identity: an untracked build input marks the commit dirty"
else
    t_fail "identity: an untracked build input marks the commit dirty"
fi

# i12. A landed RECORD must survive an alias-copy failure. utest_publish
#      returns 0 once the record exists, because the guard would otherwise see
#      the format as unpublished and move a run_incomplete refusal OVER a
#      fully assembled document -- destroying the evidence it exists to keep.
#      Section 30 made the per-run record the thing that must land, and BOTH
#      the leg and canonical pathnames aliases onto it; neither alias failure
#      may abort the publication, and each must still fail the run.
PUBFN=$(awk '/^utest_publish\(\)/{f=1} f{print} f&&/^\}/{exit}' "$LEGSH")
ALIASFN=$(awk '/^utest_alias_record\(\)/{f=1} f{print} f&&/^\}/{exit}' "$LEGSH")
LEGSETFN=$(awk '/^utest_publish_leg_set\(\)/{f=1} f{print} f&&/^\}/{exit}' "$LEGSH")
if printf '%s' "$PUBFN" | grep -q 'mv -f "\$staged" "\$record" || return 1' &&
   printf '%s' "$ALIASFN" | grep -q 'cp -f "\$record" "\$RECORD_DIR/\.\${tag}canon\.tmp"' &&
   [ "$(printf '%s' "$ALIASFN" | grep -c 'UTEST_FAIL=')" -eq 1 ] &&
   ! printf '%s' "$ALIASFN" | grep -q 'return 1' &&
   printf '%s' "$LEGSETFN" | grep -q 'UTEST_FAIL='; then
    t_pass "identity: a landed leg artifact survives an alias-copy failure"
else
    t_fail "identity: a landed leg artifact survives an alias-copy failure" "$PUBFN$ALIASFN$LEGSETFN"
fi

# i12f. The leg SET is published in two phases: every carried document is
#       staged BEFORE any stable name changes, then the whole old set is
#       removed, then the staged files move into place. Replacing them one at
#       a time paired the first new document with the other format's
#       previous-generation file for the whole gap between the operations.
LEGSET_STAGE=$(printf '%s\n' "$LEGSETFN" | grep -n 'cp -f' | head -1 | cut -d: -f1)
LEGSET_CLEAR=$(printf '%s\n' "$LEGSETFN" | grep -n 'rm -f "\$XML_LEG_OUT" "\$JSON_LEG_OUT"' | head -1 | cut -d: -f1)
LEGSET_MOVE=$(printf '%s\n' "$LEGSETFN" | grep -n 'mv -f "\$staged_x"' | head -1 | cut -d: -f1)
if [ -n "$LEGSET_STAGE" ] && [ -n "$LEGSET_CLEAR" ] && [ -n "$LEGSET_MOVE" ] &&
   [ "$LEGSET_STAGE" -lt "$LEGSET_CLEAR" ] && [ "$LEGSET_CLEAR" -lt "$LEGSET_MOVE" ]; then
    t_pass "aliases: the leg set is staged, then cleared as a set, then published"
else
    t_fail "aliases: the leg set is staged, then cleared as a set, then published" "$LEGSETFN"
fi

# i12b. There must be exactly ONE record-to-alias implementation. The JSON
#       harvester originally wrote its record directly and hand-rolled the
#       alias copies, which swallowed a leg-alias failure and left
#       JSON_PUBLISHED at 0 on a canonical failure -- so the EXIT guard then
#       overwrote an already-landed record with a run_incomplete refusal.
if [ "$(grep -c 'cp -f "\$record" "\$RECORD_DIR' "$LEGSH")" -eq 1 ] &&
   grep -q '"\$TEST_LOG" "\$RECORD_DIR/\.harvest\.json"' "$LEGSH" &&
   grep -q 'utest_publish "\$RECORD_DIR/\.harvest\.json" "\$JSON_RECORD"' "$LEGSH"; then
    t_pass "identity: JSON publication goes through the one shared record/alias path"
else
    t_fail "identity: JSON publication goes through the one shared record/alias path" \
        "the harvester must stage privately and publish through utest_publish"
fi

# i12c. Aliases may only appear after a SUCCESSFUL commit marker. An alias
#       published mid-run points into a record with no marker and possibly
#       only one of two requested formats, which is exactly what an alias
#       enumerator must never be able to consume.
#
#       Textual ordering alone is NOT the assertion. An earlier version of
#       this check verified only that the commit call was written above the
#       alias calls, and it passed while production swallowed the marker
#       failure with `|| true` and aliased anyway. The behavioral proof lives
#       in 6h(a); this pins the structure that makes it hold.
FINFN=$(awk '/^utest_finalize_record\(\)/{f=1} f{print} f&&/^\}/{exit}' "$LEGSH")
FIN_COMMIT=$(printf '%s\n' "$FINFN" | grep -n 'utest_commit_record' | head -1 | cut -d: -f1)
FIN_ALIAS=$(printf '%s\n' "$FINFN" | grep -n 'utest_alias_record' | head -1 | cut -d: -f1)
FIN_REFUSE=$(printf '%s\n' "$FINFN" | grep -n 'utest_publish_missing_refusals' | head -1 | cut -d: -f1)
if [ -n "$FIN_COMMIT" ] && [ -n "$FIN_ALIAS" ] && [ -n "$FIN_REFUSE" ] &&
   [ "$FIN_REFUSE" -lt "$FIN_COMMIT" ] && [ "$FIN_COMMIT" -lt "$FIN_ALIAS" ] &&
   printf '%s' "$FINFN" | grep -q 'if ! utest_commit_record' &&
   ! printf '%s' "$FINFN" | grep -q 'utest_commit_record .* || true' &&
   ! printf '%s' "$PUBFN" | grep -q 'utest_alias_record'; then
    t_pass "identity: aliases are published only after the record commit marker"
else
    t_fail "identity: aliases are published only after the record commit marker" "$FINFN"
fi

# i12d. A marker that cannot land must FAIL the run. A green exit over an
#       uncommitted record is the false-green the lifecycle exists to close.
COMMITFN=$(awk '/^utest_commit_record\(\)/{f=1} f{print} f&&/^\}/{exit}' "$LEGSH")
if printf '%s' "$COMMITFN" | grep -q 'UTEST_FAIL=' &&
   printf '%s' "$COMMITFN" | grep -q 'return 1'; then
    t_pass "identity: a record whose commit marker fails to land fails the run"
else
    t_fail "identity: a record whose commit marker fails to land fails the run" "$COMMITFN"
fi

# i12e. UTEST_RECORD_KEEP must be normalised in BASE 10. `08` passes a
#       digits-only check and `-ge`, then aborts the run in $(( )) under
#       set -e, where bash reads a leading zero as octal.
if grep -q 'keep=\$(( 10#\$keep ))' "$LEGSH"; then
    t_pass "run record: the retention bound is normalised in base 10"
else
    t_fail "run record: the retention bound is normalised in base 10" \
        "UTEST_RECORD_KEEP=08 would abort the run in arithmetic expansion"
fi

# i13. XML and JSON must project the SAME identity field set. A field in one
#      and not the other hands two consumers two different contracts for one
#      run. `schema` is the JSON envelope's own version marker, and
#      timestamp/hostname ride as <testsuite> attributes.
XML_FIELDS=$(awk '/^utest_xml_identity_props\(\)/{f=1} f&&/name="/{ \
    match($0, /name="[a-z_]+"/); print substr($0, RSTART+6, RLENGTH-7)} \
    f&&/^\}/{exit}' "$LEGSH" | sort)
JSON_FIELDS=$(awk '/^utest_json_identity\(\)/{f=1} f&&/printf/{ \
    if (match($0, /"[a-z_]+":/)) print substr($0, RSTART+1, RLENGTH-3)} \
    f&&/^\}/{exit}' "$LEGSH" | grep -vE '^(timestamp|hostname)$' | sort)
if [ -n "$XML_FIELDS" ] && [ "$XML_FIELDS" = "$JSON_FIELDS" ]; then
    t_pass "identity: the XML and JSON projections carry the same field set"
else
    t_fail "identity: the XML and JSON projections carry the same field set" \
           "xml=[$(echo $XML_FIELDS)] json=[$(echo $JSON_FIELDS)]"
fi

# i14. EVERY synthetic XML error document carries aborted/not_run. The
#      documented schema says their ABSENCE means the artifact predates the
#      completeness dimension, so a current refusal without them is misread as
#      an old one.
#
#      Section 30 replaced five duplicated inline refusal blocks with ONE
#      emitter, which turns this from a counting argument into a structural
#      one: assert that the sole emitter carries both properties plus the
#      identity projection, and that nothing else in the script hand-rolls an
#      errors="1" testsuite behind its back. A new refusal added as a fresh
#      inline block is exactly the drift this now catches.
REFUSALFN=$(awk '/^utest_xml_refusal_doc\(\)/{f=1} f{print} f&&/^\}/{exit}' "$LEGSH")
REFUSAL_OPENERS=$(grep -c 'errors="1" time="0"' "$LEGSH")
if printf '%s' "$REFUSALFN" | grep -q 'property name="aborted" value="true"' &&
   printf '%s' "$REFUSALFN" | grep -q 'property name="not_run" value="0"' &&
   printf '%s' "$REFUSALFN" | grep -q 'utest_xml_identity_props$' &&
   printf '%s' "$REFUSALFN" | grep -q 'errors="1"' &&
   [ "$REFUSAL_OPENERS" -eq 1 ]; then
    t_pass "identity: every synthetic XML error document carries the completeness properties"
else
    t_fail "identity: every synthetic XML error document carries the completeness properties" \
        "sole-emitter check failed; errors=1 openers in the script: $REFUSAL_OPENERS (expected 1)"
fi

# i15. Format obligations are settled before any fallible host-side parsing.
#      Deciding them at assembly time meant an exit during parsing published
#      one format's refusal and silently skipped the other's.
if awk '/^HAS_XML=0$/{x=NR} /^HAS_JSON=0$/{j=NR}
        /STRIPPED=\$\(sed/{if(!parse) parse=NR}
        END{exit !(x && j && parse && x < parse && j < parse)}' "$LEGSH"; then
    t_pass "identity: both format obligations are known before any host-side parsing"
else
    t_fail "identity: both format obligations are known before any host-side parsing"
fi

# i16. Provenance is metadata, not payload: a mistaken --identity path must
#      degrade to run_identity null, never kill the harvest or read unbounded.
mkdir -p "$IDENT_TMP/adir"
printf '"a scalar, not an object"' > "$IDENT_TMP/scalar.json"
python3 -c "open('$IDENT_TMP/huge.json','w').write('{\"x\":\"' + 'p'*200000 + '\"}')"
IDENT_OK=1
for BAD in "$IDENT_TMP/adir" "$IDENT_TMP/scalar.json" "$IDENT_TMP/huge.json"; do
    python3 "$HARVEST" "$IDENT_TMP/good.log" "$IDENT_TMP/bad.json" \
        --identity "$BAD" >/dev/null 2>&1 || IDENT_OK=0
    python3 -c "
import json,sys
d=json.load(open('$IDENT_TMP/bad.json'))
sys.exit(0 if d['run_identity'] is None and d['summary']['total']==1 else 1)" || IDENT_OK=0
done
if [ "$IDENT_OK" = "1" ]; then
    t_pass "identity: a directory, a scalar and an oversized identity all degrade to null"
else
    t_fail "identity: a directory, a scalar and an oversized identity all degrade to null"
fi

# i17. The documented leg grammar must be the one the runner enforces. The docs
#      previously advertised [a-z0-9-]{1,32}, which accepts a leading hyphen the
#      runner refuses -- an operator following the published grammar hit an
#      unexplained pre-build refusal.
if grep -qF '^[a-z0-9][a-z0-9-]{0,31}$' "$LEGSH" &&
   grep -qF '^[a-z0-9][a-z0-9-]{0,31}$' "$REPO_ROOT/docs/testing/usermode-output-formats.md" &&
   ! grep -qF '`[a-z0-9-]{1,32}`' "$REPO_ROOT/docs/testing/usermode-output-formats.md"; then
    t_pass "identity: the documented leg grammar matches the one the runner enforces"
else
    t_fail "identity: the documented leg grammar matches the one the runner enforces"
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
