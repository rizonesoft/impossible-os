#!/usr/bin/env bash
# test-secureboot-smoke.sh -- non-interactive Secure Boot smoke test
# for the Unified Kernel Image (UKI) and the split-path BOOTX64.EFI
# signing pipeline. Verifies signature + UKI section structure offline
# without launching QEMU (avoids the interactive MokManager step that
# scripts/debug/kernel/run-secureboot.bat requires).
#
# What this checks (offline; no actual QEMU boot needed):
#   1. build/tools/BOOTX64.EFI exists, has a sbsign signature, and
#      verifies against keys/MOK.cer.
#   2. build/tools/BOOTX64.UKI.efi exists, has a sbsign signature, and
#      verifies against keys/MOK.cer.
#   3. The UKI artifact's PE section table contains .linux, .cmdline,
#      .osrel sections (objdump -h grep). All three must be present
#      for the bootloader's detect_uki_sections() to fire the UKI
#      fast path.
#   4. Shim chain COVERAGE, reported explicitly in every state: no shim
#      pinned (the normal state since aab6b6f64), a partial shim/ dir, a
#      shim signed by the expired MS UEFI CA 2011, and -- when the chain
#      is otherwise covered -- that the staged ESP really carries it.
#      An uncovered chain is always named in the summary and never hides
#      behind a bare PASS.
#
# What this does NOT do:
#   - Actually boot QEMU with Secure Boot enabled (interactive MOK
#     enrollment via MokManager). Use scripts/debug/kernel/run-secureboot.bat
#     for the manual end-to-end flow.
#   - Re-run sbsign. The smoke trusts the build pipeline's output.
#
# Environment:
#   REQUIRE_SHIM=1        turn an uncovered Secure Boot chain into a hard
#                         failure. Off by default because dev and CI builds
#                         legitimately ship no shim; any path that PUBLISHES
#                         an image and advertises Secure Boot must set it.
#   SHIM_TRUST_MODE       ms-ca (default) = a Microsoft-re-signed distro shim;
#                         mok-dev = a shim we built ourselves, verified against
#                         keys/MOK.cer instead of an MS CA generation.
#   SHIM_CA_TEST_TODAY    override "today" for the CA-expiry check (tests).
#   DISK_IMG / MOK_KEY    override the disk image / signing key locations.
#
# Exit 0: all checks pass. Exit 1: any check fails. Exit 2: required
# tooling missing (sbverify, llvm-objdump-19).

set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
EFI="$REPO_ROOT/build/tools/BOOTX64.EFI"
UKI="$REPO_ROOT/build/tools/BOOTX64.UKI.efi"
# Approach A (build idempotency): signatures live at DISTINCT paths; the
# canonical EFI/UKI stay UNSIGNED so the incremental UKI objcopy is idempotent.
# The signed artifacts (shipped to the ESP) are what must carry a valid sig.
EFI_SIGNED="${EFI_SIGNED:-$REPO_ROOT/build/tools/BOOTX64.signed.efi}"
UKI_SIGNED="${UKI_SIGNED:-$REPO_ROOT/build/tools/BOOTX64.UKI.signed.efi}"
MOK_CRT="${MOK_CRT:-$REPO_ROOT/keys/MOK.cer}"

PASS=0
FAIL=0
FAILURES=()

t_ok()   { PASS=$((PASS + 1)); echo "  PASS  $1"; }
t_fail() { FAIL=$((FAIL + 1)); FAILURES+=("$1"); echo "  FAIL  $1${2:+ -- $2}"; }

echo "=================================================================="
echo " Secure Boot smoke -- BOOTX64.EFI + BOOTX64.UKI.efi signatures"
echo "=================================================================="

# Tooling check
for cmd in sbverify llvm-objdump-19; do
    if ! command -v "$cmd" >/dev/null 2>&1; then
        echo "[ERROR] required tool '$cmd' not found on PATH" >&2
        echo "  install: sudo apt install sbsigntools llvm-19" >&2
        exit 2
    fi
done

if [ ! -f "$EFI" ]; then
    t_fail "BOOTX64.EFI missing" "run: bash scripts/build.sh"
fi
if [ ! -f "$UKI" ]; then
    t_fail "BOOTX64.UKI.efi missing" "run: bash scripts/build.sh"
fi
if [ ! -f "$MOK_CRT" ]; then
    echo "[skip] keys/MOK.cer not found -- dev build without local signing keys"
    echo "  signature verification skipped; UKI structural checks still run"
fi

# Verification state. Each flag is set ONLY by a check that actually succeeded --
# never by an artifact merely existing. Check 4 builds chain coverage out of
# these, so a skipped verification can never be mistaken for a passed one.
LOADER_VERIFIED=0

# 1. Signed BOOTX64.signed.efi verifies against MOK -- this is the artifact the
#    system-disk recipe ships to the ESP. When keys are present it MUST exist.
if [ -f "$MOK_CRT" ]; then
    if [ ! -f "$EFI_SIGNED" ]; then
        t_fail "BOOTX64.signed.efi missing" "MOK key present but signing produced no signed loader"
    elif sbverify --cert "$MOK_CRT" "$EFI_SIGNED" 2>&1 | grep -q "Signature verification OK"; then
        t_ok "BOOTX64.signed.efi signature verifies against keys/MOK.cer"
        LOADER_VERIFIED=1
    else
        t_fail "BOOTX64.signed.efi signature does NOT verify against MOK.cer"
    fi
fi

# 1b. The canonical (unsigned) BOOTX64.EFI must still carry a dumpable .sbat so
#     the incremental UKI objcopy stays idempotent (TODO-02 build idempotency).
if [ -f "$EFI" ]; then
    if llvm-objdump-19 -h "$EFI" 2>/dev/null | grep -qE "[[:space:]]\.sbat([[:space:]]|$)"; then
        t_ok "canonical BOOTX64.EFI carries a dumpable .sbat (unsigned, idempotent input)"
    else
        t_fail "canonical BOOTX64.EFI missing dumpable .sbat" "input may have been signed in place"
    fi
fi

# 2. Signed BOOTX64.UKI.signed.efi verifies against MOK.
if [ -f "$MOK_CRT" ]; then
    if [ ! -f "$UKI_SIGNED" ]; then
        t_fail "BOOTX64.UKI.signed.efi missing" "MOK key present but signing produced no signed UKI"
    elif sbverify --cert "$MOK_CRT" "$UKI_SIGNED" 2>&1 | grep -q "Signature verification OK"; then
        t_ok "BOOTX64.UKI.signed.efi signature verifies against keys/MOK.cer"
    else
        t_fail "BOOTX64.UKI.signed.efi signature does NOT verify against MOK.cer"
    fi
fi

# 3. UKI section table carries .linux + .cmdline + .osrel
if [ -f "$UKI" ]; then
    SEC_DUMP="$(llvm-objdump-19 -h "$UKI" 2>&1)"
    for sec in .linux .cmdline .osrel; do
        if echo "$SEC_DUMP" | grep -qE "[[:space:]]${sec}[[:space:]]"; then
            t_ok "UKI section '$sec' present"
        else
            t_fail "UKI section '$sec' MISSING from PE table"
        fi
    done
    LINUX_SIZE_HEX="$(echo "$SEC_DUMP" | awk '/[[:space:]]\.linux[[:space:]]/ {print $3}')"
    LINUX_SIZE_DEC=$((16#$LINUX_SIZE_HEX))
    if [ "$LINUX_SIZE_DEC" -gt 1048576 ]; then
        t_ok "UKI .linux section size ${LINUX_SIZE_DEC} bytes (>1 MiB; plausible kernel)"
    else
        t_fail "UKI .linux section size ${LINUX_SIZE_DEC} bytes is implausibly small"
    fi
fi

# 4. Shim chain coverage -- pinning, CA generation, and what actually got staged.
#
# Fail-loud shim-coverage contract (UEFI hardening / Secure Boot roadmap).
# Before 2026-08-13 this block was a
# bare `if [ -f "$SHIM" ]`, so an UNPINNED shim produced no output at all and the
# script still printed `PASS N/N Secure Boot smoke checks` -- a verdict a reader
# takes as "the Secure Boot chain is covered" when nothing about the chain ran.
# The shim binaries were unpinned in aab6b6f64 (2026-07-01) because their MS UEFI
# CA 2011 expired 2026-06-30, so absence is the NORMAL state today and must be
# reported rather than assumed benign.
#
# Presence of the two shim files is NOT proof the chain shipped: the system-disk
# recipe stages the shim chain only when shimx64.efi + mmx64.efi + keys/MOK.key
# are ALL present (Makefile "EFI chain-load layout"), and deliberately falls back
# to a direct unsigned BOOTX64.EFI otherwise. So coverage is decided by the same
# predicate the Makefile uses, and confirmed against the staged ESP when a disk
# image exists.
SHIM="$REPO_ROOT/shim/shimx64.efi"
SHIM_MM="$REPO_ROOT/shim/mmx64.efi"
MOK_KEY="${MOK_KEY:-$REPO_ROOT/keys/MOK.key}"
DISK_IMG="${DISK_IMG:-$REPO_ROOT/build/system-disk.img}"
# Overridable so a test can simulate mtools being absent without hiding the
# host's real binary from PATH (the smoke needs coreutils from the same PATH).
MDIR_BIN="${MDIR_BIN:-mdir}"
MCOPY_BIN="${MCOPY_BIN:-mcopy}"
SFDISK_BIN="${SFDISK_BIN:-sfdisk}"
# Hard-fail on an uncovered chain instead of reporting it. Opt-in because a dev
# or CI build legitimately has no shim; any path that PUBLISHES an image and
# advertises Secure Boot must set it.
REQUIRE_SHIM="${REQUIRE_SHIM:-0}"
# Which trust anchor the pinned shim is expected to carry:
#   ms-ca   (default) a distro shim re-signed by Microsoft -- the stock Secure
#           Boot path, where firmware trusts the shim out of the box.
#   mok-dev a shim we built ourselves (scripts/secure-boot/build-shim.sh, MOK.cer
#           as VENDOR_CERT_FILE). It carries NO Microsoft signature, so it is
#           verified against keys/MOK.cer instead and firmware will not trust it
#           until the key is enrolled in db. Without this mode such a shim would
#           be rejected as "no recognized MS UEFI CA", failing the one
#           restoration path that is actually available today.
SHIM_TRUST_MODE="${SHIM_TRUST_MODE:-ms-ca}"
# Mirrors the graduated policy in scripts/sign-efi.sh. Duplicated deliberately:
# sourcing that script would execute its top-level signing pipeline. These are
# fixed historical dates, not a moving configuration.
MS_UEFI_CA_2011_EXPIRY="2026-06-30"
MS_UEFI_CA_2011_WARN_FROM="2026-05-01"
# An empty or malformed TODAY makes the lexicographic expiry comparison false,
# which would silently ACCEPT an expired 2011-CA shim. `set -e` is off, so a
# failed `date` would do exactly that -- validate the value instead of trusting it.
TODAY="${SHIM_CA_TEST_TODAY:-$(date -u +%Y-%m-%d 2>/dev/null)}"
if ! printf '%s' "$TODAY" | grep -qE '^[0-9]{4}-[0-9]{2}-[0-9]{2}$'; then
    echo "[ERROR] cannot establish the current date (got '"'"'$TODAY'"'"')" >&2
    echo "  the CA expiry check would silently pass without it" >&2
    exit 2
fi

SHIM_TRUSTED=0      # the shim's own trust anchor VERIFIED for the selected mode
ESP_VERIFIED=0      # the staged ESP was inspected and carries the chain
CHAIN_COVERED=0
UNCOVERED_WHY=""

# Verify ONE shim-side binary against the trust anchor the selected mode
# requires. Both shimx64.efi and mmx64.efi go through this: MokManager is an
# executable the firmware launches during enrollment, so trusting the shim while
# merely counting the file next to it is a hole, not a shortcut.
# $1 = path, $2 = short label. Returns 0 when trusted; sets UNCOVERED_WHY on
# failure and emits its own PASS/FAIL line.
# Digest of every input the ESP comparison trusts. Taken before extraction and
# again after, so a mid-flight rebuild cannot be mistaken for a verified state.
_verify_inputs_digest() {
    local f
    for f in "$DISK_IMG" "$SHIM" "$SHIM_MM" "$EFI_SIGNED"; do
        [ -f "$f" ] && sha256sum "$f" 2>/dev/null
    done | sha256sum 2>/dev/null | cut -d" " -f1
}

# An ms-ca shim must be covered by shim/SHA256SUMS. Absent file, missing entry
# or mismatching digest are all refusals -- "no hash file" is the state that let
# an arbitrary binary with plausible certificate metadata be staged.
_shim_hash_pinned() {
    local bin="$1" label="$2" sums="$REPO_ROOT/shim/SHA256SUMS" want have
    if ! command -v sha256sum >/dev/null 2>&1; then
        t_fail "cannot hash-pin $label: sha256sum not on PATH"
        UNCOVERED_WHY="$label is not hash-pinned (sha256sum unavailable)"
        return 1
    fi
    if [ ! -f "$sums" ]; then
        t_fail "$label is pinned but shim/SHA256SUMS is absent" \
               "an approved hash list must accompany a pinned vendor shim"
        UNCOVERED_WHY="shim/SHA256SUMS absent, so $label is not hash-pinned"
        return 1
    fi
    # sha256sum lines are "<digest>  <name>" ("<digest> *<name>" in binary mode).
    want="$(awk -v f="$(basename "$bin")" '$2 == f || $2 == "*" f { print $1; exit }' "$sums")"
    if [ -z "$want" ]; then
        t_fail "$label has no entry in shim/SHA256SUMS"
        UNCOVERED_WHY="$label has no approved hash entry"
        return 1
    fi
    have="$(sha256sum "$bin" 2>/dev/null | cut -d" " -f1)"
    if [ "$want" != "$have" ]; then
        t_fail "$label does not match its shim/SHA256SUMS entry" "approved $want, got ${have:-<unreadable>}"
        UNCOVERED_WHY="$label does not match its approved hash"
        return 1
    fi
    t_ok "$label matches its approved shim/SHA256SUMS entry"
    return 0
}

shim_anchor_verify() {
    local bin="$1" label="$2" out rc year
    # Capture the exit status: a tool failure is NOT "no recognized CA".
    # Conflating them is the fail-open that scripts/sign-efi.sh was fixed for.
    out="$(sbverify --list "$bin" 2>&1)" && rc=0 || rc=$?
    if [ "$rc" -ne 0 ]; then
        t_fail "sbverify --list failed (rc=$rc) on $label" \
               "tool failure is not proof of anything -- refusing to treat it as trusted"
        UNCOVERED_WHY="sbverify could not read $label (rc=$rc)"
        return 1
    fi
    if [ "$SHIM_TRUST_MODE" = "mok-dev" ]; then
        if [ ! -f "$MOK_CRT" ]; then
            t_fail "SHIM_TRUST_MODE=mok-dev but $MOK_CRT is absent" \
                   "a MOK-dev chain can only be verified against the MOK certificate"
            UNCOVERED_WHY="mok-dev mode with no MOK certificate to verify against"
            return 1
        fi
        if sbverify --cert "$MOK_CRT" "$bin" 2>&1 | grep -q "Signature verification OK"; then
            t_ok "MOK-dev $label verifies against keys/MOK.cer (firmware trusts it only once that key is in db)"
            return 0
        fi
        # scripts/secure-boot/build-shim.sh does NOT sign its output --
        # VENDOR_CERT_FILE embeds a cert for the shim's own MOK-list checks,
        # which is not an Authenticode signature. Its raw output therefore
        # cannot be a covered chain: firmware has nothing to verify.
        t_fail "MOK-dev $label carries no signature that keys/MOK.cer verifies" \
               "build-shim.sh does not sign its output -- sbsign it with a db-enrolled key first"
        UNCOVERED_WHY="self-built $label is unsigned (build-shim.sh does not sbsign), so nothing can verify it"
        return 1
    fi
    # ms-ca mode: `sbverify --list` ENUMERATES a signature table, it does not
    # validate it, so the issuer regex below is metadata rather than proof (the
    # pinned-CA-certificate fix for that is parked in the owning TODO section).
    # An approved SHA256SUMS entry is the hash half of that anchor and costs
    # nothing today: a vendor shim we pin is supposed to arrive with one, and
    # the disk recipe only checks it when the file happens to exist.
    if ! _shim_hash_pinned "$bin" "$label"; then
        return 1
    fi
    if printf '%s\n' "$out" | grep -qE "Microsoft Corporation UEFI CA 20[12][0-9]"; then
        year="$(printf '%s\n' "$out" | grep -oE 'Microsoft Corporation UEFI CA 20[12][0-9]' | grep -oE '20[12][0-9]' | sort -u | tail -1)"
        if [ "$year" = "2011" ] && [ "$TODAY" \> "$MS_UEFI_CA_2011_EXPIRY" ]; then
            t_fail "$label is signed by MS UEFI CA 2011, expired $MS_UEFI_CA_2011_EXPIRY" \
                   "scripts/sign-efi.sh aborts on this too; pin a 2023-CA-signed shim"
            UNCOVERED_WHY="pinned $label is signed by the expired MS UEFI CA 2011"
            return 1
        fi
        if [ "$year" = "2011" ] && ! [ "$TODAY" \< "$MS_UEFI_CA_2011_WARN_FROM" ]; then
            # Same graduated policy as scripts/sign-efi.sh: WARN from
            # 2026-05-01, hard fail after expiry (handled above).
            echo "  WARN  $label is signed only by the deprecated MS UEFI CA 2011 (expires $MS_UEFI_CA_2011_EXPIRY)"
        fi
        t_ok "$label is signed by Microsoft Corporation UEFI CA $year"
        return 0
    fi
    t_fail "$label present but no recognized MS UEFI CA generation" \
           "set SHIM_TRUST_MODE=mok-dev if this is a self-built shim"
    UNCOVERED_WHY="pinned $label names no recognized MS UEFI CA generation"
    return 1
}

if [ -f "$SHIM" ] && [ -f "$SHIM_MM" ]; then
    # BOTH binaries must clear the anchor. `&&` would short-circuit and skip
    # MokManager's check entirely, so each runs and the results are combined.
    SHIM_BIN_OK=0; MM_BIN_OK=0
    shim_anchor_verify "$SHIM" "shim/shimx64.efi" && SHIM_BIN_OK=1
    shim_anchor_verify "$SHIM_MM" "shim/mmx64.efi" && MM_BIN_OK=1
    if [ "$SHIM_BIN_OK" = "1" ] && [ "$MM_BIN_OK" = "1" ]; then
        SHIM_TRUSTED=1
    fi

    # The disk recipe stages the chain only with a signing key AND a signed
    # loader, and check 1 above is what proves the loader really verifies.
    # Existence of the files is not verification, so LOADER_VERIFIED gates this.
    if [ "$SHIM_TRUSTED" = "1" ]; then
        if [ ! -f "$MOK_KEY" ]; then
            UNCOVERED_WHY="shim pinned but keys/MOK.key absent -- the disk recipe stages direct (unsigned) boot, not the shim chain"
        elif [ ! -f "$MOK_CRT" ]; then
            UNCOVERED_WHY="shim + MOK key present but $MOK_CRT is absent, so no loader signature was verified"
        elif [ "$LOADER_VERIFIED" != "1" ]; then
            UNCOVERED_WHY="shim + MOK key present but the signed loader did not verify (see check 1)"
        else
            CHAIN_COVERED=1
        fi
    fi
elif [ -f "$SHIM" ] || [ -f "$SHIM_MM" ]; then
    # `make disk` refuses this outright; the smoke must not be softer than the
    # thing it is smoke-testing.
    t_fail "partial shim/ directory -- need BOTH shimx64.efi and mmx64.efi" \
           "the system-disk recipe errors out on a partial shim directory"
    UNCOVERED_WHY="partial shim/ directory"
else
    UNCOVERED_WHY="no shim pinned in shim/ (unpinned in aab6b6f64, 2026-07-01: MS UEFI CA 2011 expired $MS_UEFI_CA_2011_EXPIRY)"
    if [ "$REQUIRE_SHIM" != "1" ]; then
        echo "  UNPIN shim/shimx64.efi absent -- Secure Boot chain-load is NOT covered by this run"
        echo "        direct (unsigned) dev boot only; run scripts/secure-boot/build-shim.sh for a MOK-dev chain"
        echo "        set REQUIRE_SHIM=1 to make an uncovered chain a hard failure"
    fi
fi

# 4b. When a disk image exists it is the authority on what actually shipped --
#     shim files on disk say nothing about what `make disk` put in EFI/BOOT.
#     This check FAILS CLOSED: if an image is present and we cannot inspect it,
#     that is an unverifiable claim, not a covered chain.
if [ "$CHAIN_COVERED" = "1" ] && [ -f "$DISK_IMG" ]; then
    # The offset must come from the image's OWN partition table, not from the
    # sidecar: a stale or crafted .info can point at a decoy FAT holding
    # byte-identical artifacts while the bootable ESP differs. The sidecar is
    # kept only as a cross-check, and a disagreement is a failure.
    ESP_OFFSET=""
    ESP_OFFSET_SIDECAR=""
    [ -f "$DISK_IMG.info" ] && ESP_OFFSET_SIDECAR="$(grep -oE '^EFI_OFFSET=[0-9]+$' "$DISK_IMG.info" | head -1 | cut -d= -f2)"
    if command -v "$SFDISK_BIN" >/dev/null 2>&1; then
        # GPT type GUID of an EFI System Partition.
        ESP_OFFSET="$("$SFDISK_BIN" --json "$DISK_IMG" 2>/dev/null | python3 -c '
import json,sys
try:
    t = json.load(sys.stdin)["partitiontable"]
except Exception:
    sys.exit(0)
ss = int(t.get("sectorsize", 512))
for part in t.get("partitions", []):
    if str(part.get("type", "")).upper() == "C12A7328-F81F-11D2-BA4B-00A0C93EC93B":
        print(int(part["start"]) * ss)
        break
' 2>/dev/null)"
    fi
    if ! command -v "$MDIR_BIN" >/dev/null 2>&1 || ! command -v "$MCOPY_BIN" >/dev/null 2>&1; then
        t_fail "cannot verify the staged ESP of $DISK_IMG: mtools not on PATH" \
               "both $MDIR_BIN and $MCOPY_BIN are required -- install mtools"
        UNCOVERED_WHY="staged ESP unverifiable (mtools missing)"
    elif [ -z "$ESP_OFFSET" ]; then
        t_fail "cannot locate an EFI System Partition in $DISK_IMG's GPT" \
               "need $SFDISK_BIN to authenticate the ESP offset -- the .info sidecar is not a trust source"
        UNCOVERED_WHY="staged ESP unverifiable (no GPT-designated ESP found)"
    elif [ -n "$ESP_OFFSET_SIDECAR" ] && [ "$ESP_OFFSET_SIDECAR" != "$ESP_OFFSET" ]; then
        t_fail "$DISK_IMG.info EFI_OFFSET disagrees with the image's GPT" \
               "sidecar says $ESP_OFFSET_SIDECAR, GPT says $ESP_OFFSET -- stale metadata or a decoy partition"
        UNCOVERED_WHY="staged ESP unverifiable (sidecar/GPT offset mismatch)"
    else
        # -b prints exact full paths, one per line. The default listing is 8.3
        # columnar ("BOOTX64  EFI"), where a basename match would also accept
        # BOOTX64.BAD -- exactly the collision this must not have.
        ESP_LIST="$("$MDIR_BIN" -i "$DISK_IMG@@$ESP_OFFSET" -b ::/EFI/BOOT 2>&1)" && MDIR_RC=0 || MDIR_RC=$?
        if [ "$MDIR_RC" -ne 0 ]; then
            t_fail "cannot verify the staged ESP of $DISK_IMG: mdir exited $MDIR_RC" \
                   "$(printf '%s' "$ESP_LIST" | tail -1)"
            UNCOVERED_WHY="staged ESP unverifiable (mdir rc=$MDIR_RC)"
        else
            # A filename is not an artifact. Extract each staged component and
            # compare it byte-for-byte with the source we verified above --
            # otherwise a stale or replaced image with the right THREE NAMES
            # keeps the chain "covered" while shipping none of the verified bytes.
            ESP_MISSING=""
            ESP_MISMATCH=""
            ESP_INPUTS_DIGEST="$(_verify_inputs_digest)"
            # An unchecked mktemp is a fail-open with teeth: `set -e` is off, so
            # a failed `mktemp -d` (read-only or full TMPDIR) would leave ESP_TMP
            # empty, extraction would target /BOOTX64.EFI and friends, and every
            # cmp would then compare a file we just wrote against its own source
            # and pass. Validate the directory before anything can write to it.
            ESP_TMP="$(mktemp -d 2>/dev/null)" || ESP_TMP=""
            if [ -z "$ESP_TMP" ] || [ ! -d "$ESP_TMP" ]; then
                t_fail "cannot verify the staged ESP of $DISK_IMG: no writable temporary directory" \
                       "mktemp -d failed (TMPDIR=${TMPDIR:-/tmp})"
                UNCOVERED_WHY="staged ESP unverifiable (no temp dir to extract into)"
                ESP_MISMATCH=" (extraction not attempted)"
            else
                trap 'rm -rf "$ESP_TMP"' EXIT INT TERM
                for f in BOOTX64.EFI grubx64.efi mmx64.efi; do
                    if ! printf '%s\n' "$ESP_LIST" | grep -qixF "::/EFI/BOOT/$f"; then
                        ESP_MISSING="$ESP_MISSING $f"
                        continue
                    fi
                    case "$f" in
                        BOOTX64.EFI) ESP_SRC="$SHIM" ;;      # the shim is staged as BOOTX64.EFI
                        grubx64.efi) ESP_SRC="$EFI_SIGNED" ;; # our signed loader
                        mmx64.efi)   ESP_SRC="$SHIM_MM" ;;
                    esac
                    if ! "$MCOPY_BIN" -i "$DISK_IMG@@$ESP_OFFSET" -n "::/EFI/BOOT/$f" "$ESP_TMP/$f" >/dev/null 2>&1; then
                        ESP_MISMATCH="$ESP_MISMATCH $f(unreadable)"
                    elif ! cmp -s "$ESP_TMP/$f" "$ESP_SRC"; then
                        ESP_MISMATCH="$ESP_MISMATCH $f(bytes differ from $(basename "$ESP_SRC"))"
                    fi
                done
                    # A concurrent rebuild or re-sign can swap the image or the
                # source artifacts between their trust checks and this
                # comparison, so a match would then prove a state that never
                # existed at one instant. Re-digest the inputs and refuse if
                # anything moved under us.
                if [ -z "$ESP_MISSING" ] && [ -z "$ESP_MISMATCH" ] \
                   && [ "$(_verify_inputs_digest)" != "$ESP_INPUTS_DIGEST" ]; then
                    ESP_MISMATCH="$ESP_MISMATCH (inputs changed during verification)"
                fi
                rm -rf "$ESP_TMP"
                trap - EXIT INT TERM
            fi
            if [ -n "$ESP_MISSING" ]; then
                t_fail "staged ESP is missing chain component(s):$ESP_MISSING" \
                       "shim files are pinned but the packaged image does not chain-load"
                UNCOVERED_WHY="staged ESP is missing chain component(s):$ESP_MISSING"
            elif [ -n "$ESP_MISMATCH" ]; then
                t_fail "staged ESP component(s) are not the verified artifacts:$ESP_MISMATCH" \
                       "the packaged image does not carry the bytes this run verified"
                UNCOVERED_WHY="staged ESP component(s) differ from the verified artifacts:$ESP_MISMATCH"
            else
                t_ok "staged ESP carries the verified chain byte-for-byte (BOOTX64.EFI + grubx64.efi + mmx64.efi)"
                ESP_VERIFIED=1
            fi
        fi
    fi
    [ "$ESP_VERIFIED" = "1" ] || CHAIN_COVERED=0
fi

if [ "$REQUIRE_SHIM" = "1" ]; then
    if [ "$CHAIN_COVERED" != "1" ]; then
        t_fail "REQUIRE_SHIM=1 but the Secure Boot chain is not covered" "$UNCOVERED_WHY"
    elif [ "$ESP_VERIFIED" != "1" ]; then
        # A caller demanding a covered chain is about to ship something. Verified
        # inputs are not a verified image, and "no image was built" is not proof.
        t_fail "REQUIRE_SHIM=1 but no packaged ESP was verified" \
               "inputs verify; build a disk image so the shipped chain can be confirmed"
    fi
fi

echo "=================================================================="
TOTAL=$((PASS + FAIL))
if [ "$FAIL" -eq 0 ]; then
    if [ "$CHAIN_COVERED" = "1" ] && [ "$ESP_VERIFIED" = "1" ]; then
        echo "  PASS  $TOTAL/$TOTAL Secure Boot smoke checks (shim chain COVERED, staged ESP verified)"
    elif [ "$CHAIN_COVERED" = "1" ]; then
        # Inputs verify, but no packaged image existed to confirm what shipped.
        echo "  PASS  $TOTAL/$TOTAL Secure Boot smoke checks (shim chain COVERED -- inputs only, no disk image to inspect)"
    else
        # Never let a bare PASS stand in for a chain that was never exercised.
        echo "  PASS  $TOTAL/$TOTAL Secure Boot smoke checks -- shim chain NOT COVERED"
        echo "        $UNCOVERED_WHY"
    fi
    echo "=================================================================="
    exit 0
else
    echo "  FAIL  $FAIL/$TOTAL Secure Boot smoke checks failed"
    for f in "${FAILURES[@]}"; do
        echo "        - $f"
    done
    # State coverage on the failing path too: a reader triaging a failure needs
    # to know whether the chain was even in play, not just which checks failed.
    if [ "$CHAIN_COVERED" = "1" ]; then
        echo "        shim chain COVERED (unrelated checks failed)"
    else
        echo "        shim chain NOT COVERED -- ${UNCOVERED_WHY:-see failures above}"
    fi
    echo "=================================================================="
    exit 1
fi
