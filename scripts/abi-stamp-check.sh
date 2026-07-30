#!/usr/bin/env bash
# abi-stamp-check.sh -- the $(ABI_STAMP) recipe body, out of the Makefile.
#
# Computes a cheap key over a complete superset of scripts/gen-user-abi.py's
# read-set, skips the 0.835s `--check` when the key is unchanged, and
# publishes both the contract digest (the stamp itself) and the key under a
# `flock` so concurrent `make` PROCESSES invoking this same target (the
# canonical scripts/build.sh wrapper drives 5 of them) serialize instead of
# racing on shared temp files.
#
# Fixed by EIGHT re-adversarial rounds (2026-07-30, round 8 the hard cap on
# this section's review) against an earlier in-Makefile version:
#
# Round 1 (three gaps):
#   1. [High] All invocations shared literal `.key.new` / `.tmp` names with no
#      lock, so process A's in-flight check could be overwritten by process
#      B's key and A could publish B's unvalidated inputs as if checked.
#      Fixed: `flock` around the whole compute-check-publish transaction,
#      `mktemp` for every temp file.
#   2. [High] The key recorded $(CC) but not scripts/gen-user-abi.py's
#      ABI_CLANG override (CLANG_ENV in that script), so ABI_CLANG=<other
#      compiler> changed what the generator certified against without moving
#      the key. Fixed: resolve the SAME override precedence here and hash the
#      resolved compiler binary's content, not just $(CC)'s path/size/mtime.
#   3. [Medium] Headers were concatenated back-to-back with no per-file
#      delimiter, so bytes moved across a sorted-adjacent header boundary
#      could reproduce an identical concatenated stream. Fixed: hash each
#      header individually and fold in "digest  path" records (sha256sum's
#      own format already delimits path from content), which cannot alias
#      across a boundary the way raw concatenation can.
#
# Round 2 (against round 1's own fix, two more gaps):
#   4. [High] Round 1's compiler identity fix hashed only the FILE at the
#      resolved path. An ABI_CLANG wrapper script can stay byte-identical
#      while the compiler IT execs into changes underneath, leaving the key
#      unchanged. Fixed: fingerprint the compiler's own reported BEHAVIOR
#      (`--version` plus a `-dM -E -` predefined-macro dump against empty
#      input) alongside the file digest -- a swapped downstream compiler
#      changes its own version string and macro set even if the wrapper
#      bytes in front of it do not.
#   5. [Low] The cleanup trap was armed only after BOTH `mktemp` calls
#      succeeded, so a first-mktemp-succeeds-second-fails sequence under
#      `set -e` exited with no trap active and leaked the stamp temp file.
#      Fixed: initialize both temp-file variables empty and arm the trap
#      before either `mktemp` call (`rm -f ""` is a safe no-op).
#
# Round 3 (against round 2's own fix, one gap plus a genuine residual):
#   6. [High] Round 2's behavior probe ran `-dM -E -` with NO flags, which
#      reports HOST-default predefined macros -- a different invocation than
#      the generator's real `--target=x86_64-elf -ffreestanding` one (measured:
#      398 host macros vs 383 real-target macros, genuinely different sets).
#      Fixed: probe under the real KFLAGS/UFLAGS the generator actually
#      certifies against. The same finding also raised a residual this round
#      does NOT fully close: a byte-stable wrapper could route only these
#      probes to one compiler while routing the real compile to a different
#      downstream binary via a same-version SHARED LIBRARY swap (this host's
#      clang-19 links ~190 MiB of libclang-cpp/libLLVM/libicu). Full content-
#      hashing that runtime closure would reintroduce most of the 0.835s this
#      section removes, and defending a deliberately adversarial wrapper is
#      outside this section's threat model (an accidental `make CC=<other>`
#      mismatch, not a bypass). Mitigated, not closed: resolved shared-library
#      PATHS plus size+mtime (metadata only) are folded into the key as a
#      cheap tripwire, documented in-code as an accepted residual.
#
# Round 4 (against round 3's own fix, one gap plus a systemic pattern bug):
#   7. [High] The key's file glob matched only `*.h`, but this repo already
#      includes fragments via `*.inc` (e.g.
#      include/kernel/firmware_quirks_table.inc). A generator-relevant header
#      adopting that pattern would leave the key blind to a real input --
#      the exact fail-open class this section exists to remove. Fixed:
#      glob matches `*.h` AND `*.inc`.
#   8. [Medium] `VAR="$(cmd)"` as a STANDALONE statement, followed on the next
#      line by a separate `[ -n "$VAR" ] || { echo ...; exit 1; }` check, is
#      broken under `set -e`: a failing command substitution in a standalone
#      assignment aborts the script immediately, before the following line's
#      diagnostic ever runs. Empirically reproduced (stubbing sha256sum/
#      shasum to fail): the intended `[ABI] cannot digest ...` message never
#      printed. This is the exact class of bug round 3 already fixed once for
#      the `ldd` probe specifically, just missed in four other spots
#      (CC_DIGEST, CC_BEHAVIOR_DIGEST, HDR_DIGESTS, KEY, DIGEST). Fixed:
#      `|| true` on every one of them, so the existing emptiness checks
#      actually get a chance to run and report a named error.
#
# Round 5 (against round 4's own fix, one gap plus a completeness gap):
#   9. [High] Round 4's blanket `|| true` fixed the silent-abort problem but
#      went too far the other way: it discards the REAL exit status, so a
#      pipeline that partially succeeds (a batch checksum failing partway
#      through, or -- confirmed directly -- `xargs` without `-r` still
#      running its command on ZERO matched files, reading stdin and
#      producing a valid-looking non-empty digest of NOTHING) passes an
#      emptiness-only check. Fixed: every digest/key assignment now brackets
#      the real command in `set +e` / `set -e` and checks BOTH the captured
#      exit code and emptiness, and the header/fragment `find` is piped
#      through `xargs -r` so an empty match set is a hard failure instead of
#      a bogus success.
#  10. [Medium] The dependency-closure completeness test (test-tooling.sh)
#      hardcoded `include/build_info.h` as the one known exclusion instead
#      of reading the Makefile's actual `ABI_KEY_EXCLUDE` value -- the exact
#      hardcoded-duplicate-drift class already fixed once for
#      `ABI_INCLUDE_ROOTS` in round 2. Fixed: the test reads `ABI_KEY_EXCLUDE`
#      from the Makefile the same way it already reads `ABI_INCLUDE_ROOTS`.
#
# Round 6 (against the header-hashing design itself, one gap):
#  11. [High] `find | xargs sha256sum` follows symlinks and records only
#      "digest path" -- but scripts/gen-user-abi.py deliberately REFUSES a
#      symlinked contract or facade (os.lstat + S_ISLNK checks at
#      gen-user-abi.py:1329-1340 and 1899-1912). Replacing the contract or a
#      facade with a symlink to byte-identical content left the key
#      completely unchanged, so a cache hit skipped `--check` entirely and
#      bypassed the generator's own deliberate refusal. Fixed: the hashing
#      pass is now `-type f` only (regular files), and a SEPARATE `-type l`
#      pass folds a "SYMLINK path" marker into the same digest input,
#      independent of target content -- any conversion between regular file
#      and symlink for a tracked path always moves the key.
#
# Round 7 (against round 6's own fix, one gap plus two hardening items):
#  12. [High] `find` without `-L` never descends into a symlinked DIRECTORY,
#      and round 6's marker pass only matched `*.h`/`*.inc` leaf NAMES, so an
#      arbitrarily-named symlinked directory under a root was invisible to
#      BOTH passes -- files reached only that way could drift forever with no
#      upstream refusal to fall back on (unlike the contract/facade case).
#      Also caught verifying this: round 6's marker was content-BLIND ("it's
#      a symlink", nothing about the target), so a non-contract/facade leaf
#      symlink's target content changing later was invisible too. Fixed: leaf
#      markers now embed a content digest ("SYMLINK:<digest> path"); a
#      SEPARATE unfiltered scan refuses outright on any symlink under the
#      roots that is not a `*.h`/`*.inc` leaf (most importantly a symlinked
#      directory) rather than trying to safely and recursively hash arbitrary
#      symlinked-directory content, which is a materially larger undertaking
#      than this section's threat model justifies.
#  13. [Medium] The round-6 symlink regression created its same-content
#      target INSIDE the same hashed root, so the key moved because a new
#      regular header was added -- independent of whether the symlink logic
#      worked at all. Fixed: the target now lives outside every root passed
#      to the script.
#  14. [Medium] `exec 9>"$LOCK"` follows symlinks and truncates on open BEFORE
#      `flock` ever runs; a stale symlink at that predictable path could
#      truncate an unrelated file, and a FIFO there could block the whole
#      build indefinitely with no diagnostic. Fixed: explicit `-L` / `-f`
#      checks refuse a symlink or non-regular lock path before ever opening it.
#
# Round 8 (against round 7's own fix, one gap -- HARD CAP, this section's
# review doctrine stops the re-adversarial loop at 8 rounds):
#  15. [High] Round 7 classified symlinks by NAME (any `*.h`/`*.inc`-named
#      symlink went to the leaf-digest pass, everything else was refused), so
#      a directory symlink NAMED e.g. "vendor.h" bypassed the refusal: it took
#      the leaf-digest path, `sha256sum` failed against a directory, and the
#      fallback produced a STABLE "SYMLINK:unreadable" marker that never moved
#      regardless of what changed inside that directory. Fixed: classify by
#      the DEREFERENCED type instead of the name -- a symlink is eligible for
#      the leaf-digest marker only when its name matches `*.h`/`*.inc` AND it
#      dereferences to a REGULAR FILE (`[ -f ]` follows symlinks); every other
#      symlink under the roots (a directory target regardless of name, a
#      broken link, or a non-matching name) is refused.
#
# `set -euo pipefail` replaces the old recipe's `2>/dev/null` swallowing:
# a find/xargs/sha256sum failure now aborts instead of silently hashing a
# partial input.
#
# Usage: abi-stamp-check.sh <stamp> <contract-header> <cc> <kflags> <uflags> \
#                            <key-exclude-path> <include-root>...
set -euo pipefail

STAMP="$1"; shift
CONTRACT="$1"; shift
CC="$1"; shift
KFLAGS="$1"; shift
UFLAGS="$1"; shift
EXCLUDE="$1"; shift
ROOTS=("$@")

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

LOCK="${STAMP}.lock"
mkdir -p "$(dirname "$STAMP")"
# `exec 9>"$LOCK"` follows symlinks and TRUNCATES on open before `flock` ever
# runs -- a stale symlink at this predictable, otherwise-ignored build/ path
# would silently truncate whatever REGULAR file it points to, and a FIFO at
# that path would block the `exec` itself indefinitely (a build-wide DoS with
# no diagnostic). Refuse both BEFORE the open (re-adversarial finding, round
# 7): `-L` catches the link regardless of what it points to (even a
# nonexistent target), and the `-e && ! -f` pair catches anything else
# non-regular already sitting at that path (FIFO, socket, directory).
if [ -L "$LOCK" ]; then
    echo "[ABI] refusing lock path $LOCK: it is a symlink" >&2
    exit 1
fi
if [ -e "$LOCK" ] && [ ! -f "$LOCK" ]; then
    echo "[ABI] refusing lock path $LOCK: not a regular file" >&2
    exit 1
fi
exec 9>"$LOCK"
flock 9
# Test-only observability hook: no-op unless a test sets this env var, so it
# never changes production behavior. Gives the lock regression test a
# POSITIVE, timing-independent proof that this invocation is past the flock
# acquisition point, instead of inferring "blocked" from elapsed wall time
# (a slow but unlocked run looks identical to a genuinely blocked one under a
# time-based check -- re-adversarial finding, round 3).
[ -n "${ABI_STAMP_CHECK_LOCK_MARKER:-}" ] && touch "$ABI_STAMP_CHECK_LOCK_MARKER"

# scripts/gen-user-abi.py:237/291 -- ABI_CLANG overrides CC when set. The key
# must resolve the compiler through the identical precedence or a changed
# ABI_CLANG is invisible to it.
EFFECTIVE_CC="${ABI_CLANG:-$CC}"
CC_PATH="$(command -v "$EFFECTIVE_CC" 2>/dev/null)" || {
    echo "[ABI] cannot resolve compiler '$EFFECTIVE_CC' (CC=$CC ABI_CLANG=${ABI_CLANG:-<unset>})" >&2
    exit 1
}
# set +e / set -e brackets a real exit-status capture: a bare `|| true` on
# the assignment (tried first, round 4) discards the pipeline's real status
# entirely, so a partial failure that still emits SOME output (a batch
# checksum failing partway through, or a misconfigured empty root set --
# `xargs` without `-r`/`--no-run-if-empty` runs its command on EMPTY input
# too, reading stdin and producing a valid-looking non-empty digest of
# nothing) would pass an emptiness-only check (re-adversarial finding, round
# 5). Capturing the real `$?` this way still avoids `set -e` aborting before
# the diagnostic below can run, which is what a bare standalone assignment
# (no `set +e` bracket, no `|| true`) does instead.
set +e
CC_DIGEST="$( { sha256sum "$CC_PATH" 2>/dev/null || shasum -a 256 "$CC_PATH"; } | awk '{print $1}')"
_ABI_RC=$?
set -e
[ "$_ABI_RC" -eq 0 ] && [ -n "$CC_DIGEST" ] || {
    echo "[ABI] cannot digest compiler binary $CC_PATH (rc=$_ABI_RC)" >&2
    exit 1
}

# File-content hashing alone stops at wrapper bytes: an ABI_CLANG wrapper
# script can stay byte-identical while the compiler it execs into changes
# underneath, leaving CC_DIGEST unchanged. A behavioral fingerprint of
# whatever actually RUNS closes that. Round 2 probed with no flags at all
# (host-default macros); round 3's re-adversarial found that was probing a
# DIFFERENT invocation than the generator's real one -- host-default and
# `--target=x86_64-elf -ffreestanding` macro sets genuinely differ (measured:
# 398 vs 383 defines). Probe under BOTH real flag vectors the generator
# certifies against (kernel and ring-3), with -MMD/-MP stripped since they
# conflict with plain -E (side-effect dep files, no object).
_abi_strip_dep_flags() {
    local _f_tok _f_out=""
    for _f_tok in $1; do
        case "$_f_tok" in -MMD|-MP) continue ;; esac
        _f_out="$_f_out $_f_tok"
    done
    printf '%s' "$_f_out"
}
CC_VERSION="$("$CC_PATH" --version 2>/dev/null)" || {
    echo "[ABI] compiler --version probe failed for $CC_PATH" >&2
    exit 1
}
CC_KFLAGS_PROBE="$(_abi_strip_dep_flags "$KFLAGS")"
CC_UFLAGS_PROBE="$(_abi_strip_dep_flags "$UFLAGS")"
CC_MACROS_K="$(eval "$CC_PATH" "$CC_KFLAGS_PROBE" -dM -E - < /dev/null 2>/dev/null)" || {
    echo "[ABI] compiler predefined-macro probe (kernel flags) failed for $CC_PATH" >&2
    exit 1
}
CC_MACROS_U="$(eval "$CC_PATH" "$CC_UFLAGS_PROBE" -dM -E - < /dev/null 2>/dev/null)" || {
    echo "[ABI] compiler predefined-macro probe (user flags) failed for $CC_PATH" >&2
    exit 1
}

# A byte-stable wrapper could still route ONLY these behavioral probes to one
# downstream compiler while dispatching the real compile elsewhere, or a
# same-version shared library swap (this host's clang-19 links ~190 MiB of
# libclang-cpp/libLLVM/libicu) could change codegen without moving any of the
# above. Full-content-hashing that runtime closure would reintroduce most of
# the 0.835s this section removes, so instead the resolved shared-library
# PATHS plus each one's size+mtime (metadata only, not content) are folded in
# as a cheap tripwire against a swapped .so -- not a complete defense, an
# accepted residual: this section's threat model is an accidental `make
# CC=<other>` mismatch, not a deliberately adversarial wrapper. `ldd` missing
# or failing degrades this signal rather than hard-failing the whole gate --
# `ldd` on any wrapper SCRIPT (as opposed to the real compiler ELF binary)
# always reports "not a dynamic executable" and exits non-zero, which is the
# expected, common case (ABI_CLANG wrappers are the documented reason this
# override exists at all), not an error. `|| true` on the whole pipeline is
# REQUIRED under `set -o pipefail`: without it, ldd's ordinary "not a dynamic
# executable" exit silently aborted the ENTIRE script with no message printed
# (found running this exact scenario -- a wrapper-backed ABI_CLANG made the
# gate fail closed with zero diagnostics, the opposite of "degrades
# gracefully"). `ldd` on a non-ELF file is also NOT reliably safe to invoke
# bare: some implementations exec the target under LD_TRACE_LOADED_OBJECTS
# instead of refusing it, which can block on stdin or behave unpredictably.
# `< /dev/null` closes that door and `timeout` bounds the worst case so a
# wrapper can never hang the gate either.
CC_LIBS_META="$( { timeout 5 ldd "$CC_PATH" < /dev/null 2>/dev/null | awk '{print $3}' | while read -r _lib; do
    [ -n "$_lib" ] && [ -e "$_lib" ] && stat -c '%n %s %Y' "$_lib" 2>/dev/null
done; } || true)"

set +e
CC_BEHAVIOR_DIGEST="$( { printf '%s\n' "$CC_VERSION" "$CC_MACROS_K" "$CC_MACROS_U" "$CC_LIBS_META"; } \
    | sha256sum | awk '{print $1}')"
_ABI_RC=$?
set -e
[ "$_ABI_RC" -eq 0 ] && [ -n "$CC_BEHAVIOR_DIGEST" ] || {
    echo "[ABI] cannot fingerprint compiler behavior for $CC_PATH (rc=$_ABI_RC)" >&2
    exit 1
}

# Per-file "digest  path" records instead of raw concatenation: unambiguous
# per the re-adversarial finding (see header comment above). Matches *.inc as
# well as *.h: this repo already includes fragment files that way (e.g.
# include/kernel/firmware_quirks_table.inc), and a generator-relevant header
# adopting that pattern would otherwise leave the key blind to a real input
# (re-adversarial finding, round 4) -- exactly the fail-open class this
# section exists to remove. `xargs -r` (no-run-if-empty) matters here: without
# it, zero matched headers still runs `sha256sum` with no file args, which
# reads stdin and produces a non-empty digest of NOTHING -- a misconfigured
# empty root set would otherwise look like a valid, non-empty result.
#
# `-type f` restricts the hashing pass to REGULAR files. scripts/gen-user-abi.py
# deliberately REFUSES a symlinked contract or facade (os.lstat + S_ISLNK,
# gen-user-abi.py:1329-1340 and 1899-1912) -- but `sha256sum` follows symlinks
# transparently and records only "digest path", so hashing them here the same
# way would let a plain-file-to-symlink conversion with identical target bytes
# leave the key COMPLETELY unchanged, cache-hitting past `--check` and
# bypassing the generator's own deliberate refusal (re-adversarial finding,
# round 6). Symlinks matched under the same names are instead recorded as a
# distinct "SYMLINK path" marker, independent of target content: any
# conversion between regular file and symlink for a tracked path always moves
# the key, forcing a real `--check` that then correctly refuses. Each pass's
# exit status is captured SEPARATELY and checked BEFORE combining -- folding
# them into one `{ pass1; pass2 } | sort` would let pass2 succeeding mask a
# pass1 failure, since a compound command's status is only its LAST command's.
set +e
HDR_HASHED="$(find "${ROOTS[@]}" \( -name '*.h' -o -name '*.inc' \) -type f \
    -not -path "$EXCLUDE" -print0 | xargs -0 -r sha256sum)"
_ABI_RC=$?
set -e
[ "$_ABI_RC" -eq 0 ] || {
    echo "[ABI] header hashing failed under: ${ROOTS[*]} (rc=$_ABI_RC)" >&2
    exit 1
}

# EVERY symlink under the roots is classified by its DEREFERENCED type, not
# by name: round 7 classified by NAME (any `*.h`/`*.inc`-named symlink went to
# the leaf-digest pass, anything else was refused), which a directory symlink
# NAMED e.g. "vendor.h" bypassed -- it took the leaf-digest path, `sha256sum`
# failed against a directory, and the fallback produced a STABLE
# "SYMLINK:unreadable" marker that never moved regardless of what changed
# inside that directory (re-adversarial finding, round 8, the hard cap on
# this section's review). Fixed: a symlink is only eligible for the
# leaf-digest marker when its name matches AND it dereferences to a REGULAR
# FILE (`[ -f ]` follows symlinks); every other symlink under the roots --
# a directory target regardless of name, a broken/dangling link, or any
# non-`*.h`/`*.inc` name -- is refused outright. `find` without `-L` never
# descends into a symlinked directory, so files reached only that way would
# otherwise be structurally invisible to the key; recursively and safely
# hashing arbitrary symlinked-directory content is a materially larger
# undertaking than this section's threat model (an accidental `make
# CC=<other>`-class mismatch, not deliberately hostile filesystem structure)
# justifies -- refuse-and-ask-a-human matches gen-user-abi.py's own posture on
# abnormal structure. The content digest on the leaf marker (not just a bare
# "it's a symlink" flag) matters separately: a symlinked *.h/*.inc that is NOT
# the generator's contract/facade has no upstream refusal at all
# (gen-user-abi.py's lstat guards cover only those three paths), so a
# content-blind marker would miss the target's content changing later while
# the symlink itself stays put (round 7).
set +e
_ABI_SYMLINK_CLASSIFY="$(find "${ROOTS[@]}" -type l -not -path "$EXCLUDE" -print0 \
    | while IFS= read -r -d '' _abi_sym; do
          case "$_abi_sym" in
              *.h|*.inc)
                  if [ -f "$_abi_sym" ]; then
                      _abi_sym_digest="$( { sha256sum -L "$_abi_sym" 2>/dev/null \
                          || cat "$_abi_sym" 2>/dev/null | shasum -a 256; } | awk '{print $1}')"
                      printf 'LEAF SYMLINK:%s  %s\n' "${_abi_sym_digest:-unreadable}" "$_abi_sym"
                      continue
                  fi
                  ;;
          esac
          printf 'REFUSED  %s\n' "$_abi_sym"
      done)"
_ABI_RC=$?
set -e
[ "$_ABI_RC" -eq 0 ] || {
    echo "[ABI] symlink scan failed under: ${ROOTS[*]} (rc=$_ABI_RC)" >&2
    exit 1
}

_ABI_UNEXPECTED_SYMLINKS="$(printf '%s\n' "$_ABI_SYMLINK_CLASSIFY" \
    | sed -n 's/^REFUSED  //p')"
if [ -n "$_ABI_UNEXPECTED_SYMLINKS" ]; then
    echo "[ABI] refusing: unexpected symlink(s) under ${ROOTS[*]} (remove or extend this script deliberately):" >&2
    printf '%s\n' "$_ABI_UNEXPECTED_SYMLINKS" >&2
    exit 1
fi

HDR_SYMLINKS="$(printf '%s\n' "$_ABI_SYMLINK_CLASSIFY" | sed -n 's/^LEAF //p')"

HDR_DIGESTS="$(printf '%s\n%s\n' "$HDR_HASHED" "$HDR_SYMLINKS" | sort -k2)"
[ -n "$HDR_DIGESTS" ] || {
    echo "[ABI] no headers found under: ${ROOTS[*]}" >&2
    exit 1
}

set +e
KEY="$( { printf '%s\n' "$HDR_DIGESTS"; \
          printf 'CC=%s\nCCDIGEST=%s\nCCBEHAVIOR=%s\nKFLAGS=%s\nUFLAGS=%s\n' \
              "$EFFECTIVE_CC" "$CC_DIGEST" "$CC_BEHAVIOR_DIGEST" "$KFLAGS" "$UFLAGS"; \
          cat scripts/gen-user-abi.py Makefile scripts/abi-stamp-check.sh; \
        } | sha256sum | awk '{print $1}')"
_ABI_RC=$?
set -e
[ "$_ABI_RC" -eq 0 ] && [ -n "$KEY" ] || {
    echo "[ABI] cannot compute the ABI input key (rc=$_ABI_RC)" >&2
    exit 1
}

if [ -f "$STAMP" ] && [ -f "$STAMP.key" ] && [ "$KEY" = "$(cat "$STAMP.key")" ]; then
    exit 0
fi

python3 scripts/gen-user-abi.py --check

set +e
DIGEST="$( { sha256sum "$CONTRACT" 2>/dev/null || shasum -a 256 "$CONTRACT"; } | awk '{print $1}')"
_ABI_RC=$?
set -e
[ "$_ABI_RC" -eq 0 ] && [ -n "$DIGEST" ] || {
    echo "[ABI] cannot digest $CONTRACT (rc=$_ABI_RC)" >&2
    exit 1
}

TMP_STAMP=""
TMP_KEY=""
trap 'rm -f "$TMP_STAMP" "$TMP_KEY"' EXIT
TMP_STAMP="$(mktemp "${STAMP}.XXXXXX")"
TMP_KEY="$(mktemp "${STAMP}.key.XXXXXX")"

printf '%s\n' "$DIGEST" > "$TMP_STAMP"
printf '%s\n' "$KEY" > "$TMP_KEY"

if [ -f "$STAMP" ] && cmp -s "$TMP_STAMP" "$STAMP"; then
    rm -f "$TMP_STAMP" # unchanged -- do not touch the stamp's mtime
else
    mv "$TMP_STAMP" "$STAMP"
    echo "[ABI] $STAMP ($CONTRACT validated, digest changed)"
fi
mv "$TMP_KEY" "$STAMP.key"
trap - EXIT
