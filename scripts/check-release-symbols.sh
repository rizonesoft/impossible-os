#!/usr/bin/env bash
# =============================================================================
# check-release-symbols.sh -- prove a release kernel.exe was cut at
# KERNEL_TESTS=off and carries no flavor-gated test seam or test TU
#
# A KERNEL_TESTS=off build prunes the test seams and test translation units, but
# nothing proves a SHIPPED image was cut that way. This gate is that proof. It
# runs three independent checks and is designed to FAIL LOUDLY on a test-flavor
# (KERNEL_TESTS=on) image:
#
#   PART A  Seam-symbol gate. Compile every kernel TU twice (-D vs -U
#           KERNEL_TESTS, identical otherwise) via compile_commands.json,
#           diff the defined-symbol sets to derive the seam inventory
#           INDEPENDENTLY of the release link, and assert build/kernel.map
#           carries none of it. (scripts/lib/release-seam-inventory.py.)
#   PART A2 ADVISORY test-only-reference audit. Surfaces a test-only helper
#           defined UNCONDITIONALLY in a production TU (invisible to PART A's
#           flavor-diff) via a compiler-derived "referenced only by pruned test
#           objects" inventory. Over-reports by design, so it NEVER blocks a
#           release. (scripts/lib/test-only-ref-audit.py; needs the ON-flavor
#           compile DB -- see test_ref_audit_advisory below.)
#   PART B  Link-input gate. Read the `ld.lld --trace` input list that the
#           KERNEL_BIN link emits as a byproduct (build/kernel.link-trace.txt,
#           co-generated with kernel.exe so it cannot drift) and assert no
#           src/kernel/test/ object and none of the test-only extra TUs reached
#           the link. (kernel.map proves symbols; the trace proves link inputs.)
#   PART C  Provenance gate. Assert build/kernel.exe carries the non-alloc
#           `.ipos.provenance` marker (src/kernel/provenance_release.c, linked
#           only at KERNEL_TESTS=off). `--verify-provenance <exe>` runs this
#           check standalone; packaging paths use it to REJECT an unstamped
#           kernel.
#
# SCOPE: Part A's flavor-diff catches symbols that DIFFER between the two
# flavors -- i.e. everything gated by `#ifdef KERNEL_TESTS`. A test-only helper
# defined UNCONDITIONALLY in a production TU (present in both flavors) would be
# invisible to the diff; the release test-surface exclusion guarded the known such helpers, so
# they are now flavor-gated and Part A covers them. The advisory PART A2
# (test_ref_audit_advisory below) surfaces a FUTURE unguarded one from a
# compiler-derived "referenced only by pruned test objects" inventory. That
# inventory over-reports (see PART A2) so it is ADVISORY, never a release gate;
# this proof therefore asserts "no flavor-gated test surface leaked", not "zero
# test-referenced symbols".
#
# Usage:
#   scripts/check-release-symbols.sh                     # full gate (A+B+C)
#   scripts/check-release-symbols.sh --verify-provenance <kernel.exe>
#
# Exit 0 = clean release image; non-zero = a failure (with detail on stderr).
# =============================================================================
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

BUILD_DIR="build"
KERNEL_EXE="$BUILD_DIR/kernel.exe"
KERNEL_MAP="$BUILD_DIR/kernel.map"
TRACE_TXT="$BUILD_DIR/kernel.link-trace.txt"
TRACE_SHA="$BUILD_DIR/kernel.link-trace.sha"
CC_JSON="compile_commands.json"

PROV_SECTION=".ipos.provenance"
PROV_MAGIC="IPOS-RELEASE-PROVENANCE-V1"
PROV_FLAVOR="flavor=off"

NM="${LLVM_NM:-llvm-nm-19}"
READELF="${LLVM_READELF:-llvm-readelf-19}"

# Test-only translation units that live OUTSIDE src/kernel/test/ (pruned via the
# Makefile KERNEL_TESTS_EXTRA_TUS list); their objects must never reach the link.
EXTRA_TEST_OBJS=(
    "$BUILD_DIR/kernel/fs/ntfs/ntfs_test.o"
    "$BUILD_DIR/kernel/fs/ixfs/ixfs_test.o"
    "$BUILD_DIR/kernel/main/test_threads.o"
)

err()  { printf '[check-release-symbols] ERROR: %s\n' "$*" >&2; }
note() { printf '[check-release-symbols] %s\n' "$*" >&2; }
ok()   { printf '[check-release-symbols] PASS: %s\n' "$*" >&2; }

# -----------------------------------------------------------------------------
# PART C -- provenance marker check (also the --verify-provenance entry point).
# -----------------------------------------------------------------------------
verify_provenance() {
    local exe="$1"
    [[ -f "$exe" ]] || { err "provenance: file not found: $exe"; return 1; }

    # 1. Read the marker with a GENUINELY read-only extractor. `llvm-readelf -p`
    #    (string dump) never writes its input, so verifying a kernel can never
    #    perturb the very bytes it attests -- unlike an objcopy form that may
    #    rewrite the file in place. Section absent -> non-zero / empty -> refuse.
    local content rc_read
    content="$("$READELF" -p "$PROV_SECTION" "$exe" 2>/dev/null)"; rc_read=$?
    if [[ "$rc_read" -ne 0 || -z "$content" ]] || ! grep -qF "$PROV_MAGIC" <<<"$content"; then
        err "provenance: section $PROV_SECTION absent or unreadable in $exe"
        err "  -> refusing: this is a test-flavor (KERNEL_TESTS=on) or unstamped kernel."
        return 1
    fi
    if ! grep -qF "$PROV_FLAVOR" <<<"$content"; then
        err "provenance: $PROV_SECTION present but flavor mismatch: '$content'"
        return 1
    fi

    # 2. The marker must be NON-ALLOC: it must not be mapped into any numbered
    #    PT segment (it belongs under 'None' in the Section-to-Segment mapping).
    #    A loadable marker would perturb the boot image; a non-alloc one is
    #    invisible to the PT_LOAD-only bootloader (precedent: .comment).
    local mapping in_seg
    mapping="$("$READELF" -l "$exe" 2>/dev/null)" \
        || { err "provenance: readelf -l failed on $exe (cannot verify non-alloc)"; return 1; }
    [[ -n "$mapping" ]] || { err "provenance: empty program-header output for $exe"; return 1; }
    in_seg="$(awk -v s="$PROV_SECTION" '
        /Section to Segment mapping:/ { m=1; next }
        m && $1 ~ /^[0-9]+$/ { for (i=2; i<=NF; i++) if ($i == s) print "YES" }
    ' <<<"$mapping")"
    if [[ -n "$in_seg" ]]; then
        err "provenance: $PROV_SECTION is mapped into a load segment (must be non-alloc)"
        return 1
    fi

    ok "provenance: $exe carries $PROV_SECTION ($PROV_MAGIC $PROV_FLAVOR, non-alloc)"
    return 0
}

# -----------------------------------------------------------------------------
# PART A -- seam-symbol gate.
# -----------------------------------------------------------------------------
symbol_gate() {
    [[ -f "$KERNEL_MAP" ]] || { err "missing $KERNEL_MAP -- build the kernel first"; return 1; }
    [[ -f "$CC_JSON"    ]] || { err "missing $CC_JSON -- run 'bash scripts/build.sh clean' (bear generates it)"; return 1; }

    local workdir; workdir="$(mktemp -d)"
    # shellcheck disable=SC2064
    trap "rm -rf '$workdir'" RETURN

    local inv="$workdir/on_only.txt"
    if ! python3 "$REPO_ROOT/scripts/lib/release-seam-inventory.py" \
            --cc "$CC_JSON" --repo "$REPO_ROOT" --nm "$NM" \
            --workdir "$workdir" --out "$inv"; then
        err "seam-inventory derivation failed (see above)"
        return 1
    fi
    # Fail-closed: an empty inventory means the double-compile produced nothing
    # (no kernel TUs enumerated, or every compile silently no-op'd). A real tree
    # always has KERNEL_TESTS seams, so an empty set is a gate malfunction, never
    # a clean result.
    [[ -s "$inv" ]] || { err "PART A FAILED: seam inventory is empty -- the double-compile produced no symbols (gate malfunction)"; return 1; }

    # Defined symbols in the shipped map: llvm-nm -n output is "<addr> <type>
    # <name>"; a type letter of 'U' (undefined) has no address, so a defined
    # symbol line has exactly 3 fields with a non-'U' type. Collect the names.
    # Explicitly check the parse (with pipefail) rather than trusting errexit,
    # which is disabled inside a function invoked in a `||` aggregation context.
    local mapsyms="$workdir/map_defined.txt"
    if ! awk 'NF==3 && $2 != "U" && $2 != "u" { print $3 }' "$KERNEL_MAP" | sort -u > "$mapsyms"; then
        err "PART A FAILED: could not parse defined symbols from $KERNEL_MAP"; return 1
    fi
    [[ -s "$mapsyms" ]] || { err "PART A FAILED: no defined symbols parsed from $KERNEL_MAP (format error?)"; return 1; }

    # leaked = on_only INTERSECT map_defined (both sorted). A comm failure must
    # ABORT, never fall through to an empty leaked set that reads as "clean".
    local leaked="$workdir/leaked.txt"
    if ! comm -12 "$inv" "$mapsyms" > "$leaked"; then
        err "PART A FAILED: symbol intersection (comm) failed -- inputs not sorted?"; return 1
    fi

    if [[ -s "$leaked" ]]; then
        err "PART A FAILED: the shipped kernel.map carries $(wc -l < "$leaked") test-surface seam symbol(s):"
        sed 's/^/    /' "$leaked" >&2
        err "  -> this kernel was NOT cut at KERNEL_TESTS=off, or a seam is unguarded."
        return 1
    fi
    ok "PART A: kernel.map carries none of the $(wc -l < "$inv") seam symbols in the on_only inventory"
    return 0
}

# -----------------------------------------------------------------------------
# PART B -- link-input gate.
# -----------------------------------------------------------------------------
link_input_gate() {
    [[ -f "$KERNEL_EXE" ]] || { err "missing $KERNEL_EXE -- build the kernel first"; return 1; }
    [[ -f "$TRACE_TXT" ]]  || { err "missing $TRACE_TXT -- rebuild the kernel (the link emits it)"; return 1; }
    [[ -s "$TRACE_TXT" ]]  || { err "PART B FAILED: $TRACE_TXT is empty -- link trace not captured"; return 1; }
    [[ -f "$TRACE_SHA" ]]  || { err "missing $TRACE_SHA -- rebuild the kernel (the link records it)"; return 1; }

    # The trace + its sha record are BYPRODUCTS of the same link that produced
    # kernel.exe (one KERNEL_BIN recipe writes all three), so there is no relink
    # and no drift. Authenticate BOTH the shipped binary AND the trace itself.
    # `sha256sum -c` only verifies records that are PRESENT, so first require the
    # manifest to contain EXACTLY the two canonical records -- otherwise a
    # manifest with the trace record dropped would pass while the trace is
    # swapped for a clean-looking one. (This binds against ACCIDENTAL staleness/
    # truncation; an actor who can rewrite build/ can rewrite all three records,
    # which needs a signed CI attestation -- tracked in section 29.)
    local sha_paths
    sha_paths="$(awk '{print $2}' "$TRACE_SHA" 2>/dev/null | sort | tr '\n' ',')"
    if [[ "$sha_paths" != "build/kernel.exe,build/kernel.link-trace.txt," ]]; then
        err "PART B FAILED: $TRACE_SHA must list EXACTLY the 2 canonical records (build/kernel.exe + build/kernel.link-trace.txt); got: '$sha_paths'"
        return 1
    fi
    if ! ( cd "$REPO_ROOT" && sha256sum -c "$TRACE_SHA" ) >/dev/null 2>&1; then
        err "PART B FAILED: kernel.exe or its link trace does not match the recorded digest ($TRACE_SHA)."
        err "  -> the binary or trace was rebuilt/stale/tampered; rebuild so all three are co-generated."
        return 1
    fi

    # Assert no test object reached the link. `--trace` prints one input path per
    # line; a src/kernel/test/ object or one of the enumerated extra test TUs is
    # a leak that kernel.map alone (symbols, not link inputs) would not reveal.
    local bad=0 line
    while IFS= read -r line; do
        case "$line" in
            *"/kernel/test/"*.o) err "PART B FAILED: test-directory object linked: $line"; bad=1 ;;
        esac
    done < "$TRACE_TXT"

    for obj in "${EXTRA_TEST_OBJS[@]}"; do
        if grep -qF -- "$obj" "$TRACE_TXT"; then
            err "PART B FAILED: test-only extra TU linked: $obj"; bad=1
        fi
    done
    [[ "$bad" -eq 0 ]] || { err "  -> kernel.map proves symbols; this link trace proves link inputs -- a test object slipped the link."; return 1; }

    ok "PART B: link trace co-generated with kernel.exe; no test object among $(wc -l < "$TRACE_TXT") link inputs"
    return 0
}

# -----------------------------------------------------------------------------
# PART A2 -- ADVISORY test-only-reference audit (release test-surface exclusion).
#
# PART A proves no `#ifdef KERNEL_TESTS`-gated seam leaks. It is blind to a
# test-only helper defined UNCONDITIONALLY in a production TU (present in both
# flavors). Section 29 guards the known such helpers -- once guarded they become
# flavor-gated and PART A covers them -- and this advisory surfaces a FUTURE
# unguarded one, with no hand-maintained symbol list.
#
# It is ADVISORY, never a hard gate: a compiler-derived "referenced only by test
# objects" set over-reports (a real API address-taken only within its own TU, or
# awaiting a production caller, looks test-only) and can under-report, so it can
# neither be asserted-empty nor fail a release (Codex design review 2026-07-17).
# It requires the ON-flavor compile DB (the release/off DB prunes test objects):
# pass RELEASE_ON_CC=<path>, else compile_commands.on.json, else the live
# compile_commands.json only if it still carries test objects; otherwise it
# advisory-skips. It NEVER changes the gate exit status.
# -----------------------------------------------------------------------------
test_ref_audit_advisory() {
    local on_cc=""
    if [[ -n "${RELEASE_ON_CC:-}" && -s "${RELEASE_ON_CC}" ]]; then
        on_cc="${RELEASE_ON_CC}"
    elif [[ -s "$BUILD_DIR/compile_commands.on.json" ]]; then
        on_cc="$BUILD_DIR/compile_commands.on.json"
    elif [[ -s "compile_commands.on.json" ]]; then
        on_cc="compile_commands.on.json"
    elif [[ -s "$CC_JSON" ]] && grep -q '/kernel/test/' "$CC_JSON"; then
        on_cc="$CC_JSON"
    fi
    if [[ -z "$on_cc" ]]; then
        note "PART A2 (advisory): SKIPPED -- no ON-flavor compile DB (set RELEASE_ON_CC or place compile_commands.on.json); the release/off DB prunes test objects."
        return 0
    fi
    [[ -f "$KERNEL_MAP" ]] || { note "PART A2 (advisory): SKIPPED -- $KERNEL_MAP absent"; return 0; }

    # Fail-open by contract: this advisory MUST NOT abort the release gate. Any
    # setup failure (mktemp under an unusable TMPDIR, etc.) is a warning + return
    # 0, so the authoritative A/B/C result is never masked (Codex adversarial F3).
    local wd
    if ! wd="$(mktemp -d 2>/dev/null)"; then
        note "PART A2 (advisory): SKIPPED -- mktemp failed (TMPDIR unusable); hard gate unaffected"
        return 0
    fi
    local out="$BUILD_DIR/test-only-ref-audit.txt"
    # Advisory: never propagate the auditor's rc to the gate.
    python3 "$REPO_ROOT/scripts/lib/test-only-ref-audit.py" \
        --cc "$on_cc" --map "$KERNEL_MAP" --repo "$REPO_ROOT" \
        --nm "$NM" --workdir "$wd" --out "$out" || true
    [[ -s "$out" ]] && note "PART A2 (advisory): full candidate inventory written to $out"
    rm -rf "$wd" 2>/dev/null || true
    return 0
}

# -----------------------------------------------------------------------------
main() {
    if [[ "${1:-}" == "--verify-provenance" ]]; then
        [[ -n "${2:-}" ]] || { err "--verify-provenance requires a <kernel.exe> path"; exit 2; }
        verify_provenance "$2"
        exit $?
    fi
    if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
        sed -n '2,30p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
        exit 0
    fi
    if [[ -n "${1:-}" ]]; then
        err "unknown argument: $1 (see --help)"; exit 2
    fi

    note "release-flavor proof for $KERNEL_EXE"
    local rc=0
    symbol_gate      || rc=1
    link_input_gate  || rc=1
    verify_provenance "$KERNEL_EXE" || rc=1
    test_ref_audit_advisory || true   # advisory only -- never affects rc
    if [[ "$rc" -eq 0 ]]; then
        ok "release proof PASSED: no flavor-gated test seam in the map, no test object linked, release provenance stamped"
    else
        err "release-flavor proof FAILED -- see the PART failures above"
    fi
    exit "$rc"
}

main "$@"
