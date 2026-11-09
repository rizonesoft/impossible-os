#!/usr/bin/env bash
# ============================================================================
# test-smoke.sh -- Automated QEMU smoke test for CI/CD
#
# Builds the OS, boots in QEMU headless mode, captures serial output,
# and checks for expected boot messages / absence of panics.
#
# Usage:  bash scripts/test-smoke.sh
#
# Exit codes:
#   0 = PASS (boot completed successfully)
#   1 = FAIL (panic, timeout, or build failure)
# ============================================================================

set -euo pipefail

case "${1:-}" in
    -h|--help)
        cat <<'EOF'
test-smoke.sh -- end-to-end boot smoke test

Usage:
  bash scripts/test-smoke.sh            Build + boot + pattern check (default)

Behavior:
  Builds the OS, boots build/system-disk.img headless in QEMU (KVM if
  /dev/kvm is writable, TCG fallback), and asserts that "Boot complete in"
  and "C:\>" appear on serial within TIMEOUT_SEC seconds.

Environment:
  TIMEOUT_SEC    QEMU wait budget in seconds (default: 30).

Exit codes:
  0 = PASS (boot completed successfully)
  1 = FAIL (panic, timeout, or build failure)

Artifacts:
  build/smoke-test.log            raw serial capture
  build/smoke-test.stripped.log   ANSI-stripped serial capture
EOF
        exit 0
        ;;
esac

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD="$REPO_ROOT/build"

DISK="$BUILD/system-disk.img"
OVMF_CODE="/usr/share/OVMF/OVMF_CODE_4M.fd"
OVMF_VARS_SRC="/usr/share/OVMF/OVMF_VARS_4M.fd"
OVMF_VARS_CP="$BUILD/OVMF_VARS_4M.fd"
SERIAL_LOG="$BUILD/smoke-test.log"
STRIPPED_LOG="$BUILD/smoke-test.stripped.log"
POST16_MANIFEST="$BUILD/post16-manifest.env"
TIMEOUT_SEC="${TIMEOUT_SEC:-30}"
# The SMOKE_MENU=keys dwell adds ~10 s to a boot, which a 1-CPU TCG leg cannot
# fit in 30 s.
case "${SMOKE_MENU:-0}" in keys|default) [ "$TIMEOUT_SEC" -lt 60 ] && TIMEOUT_SEC=60 ;; esac

# Legacy smoke test: superseded by `bash scripts/test.sh`. Retained as a light
# boot-only sanity check (no unit-test suite). Boots the canonical GPT disk
# image via AHCI, matching Makefile's run: target. Do not re-introduce the
# grub-mkrescue ISO path -- that is retired.

# Strip ANSI color and control sequences from the serial log into a derived
# file before pattern matching. The kernel emits `\x1b[31m[FAIL]...` etc., and
# OVMF emits screen-clear sequences that collide with any bracketed substring.
# Matching the stripped copy keeps patterns robust against future color edits.
strip_ansi() {
    sed -E 's/\x1b\[[0-9;]*[A-Za-z]//g; s/\x1b[=>]//g' "$SERIAL_LOG" > "$STRIPPED_LOG" 2>/dev/null || :
}

# ---- Colors ----
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
DIM='\033[0;90m'
NC='\033[0m'

# ---- Pass/Fail criteria ----
# PASS requires BOTH signals: the kernel's "Boot complete in" sentinel AND the
# shell's `C:\>` prompt. Either alone is insufficient (a kernel can print the
# sentinel and then panic in userland). All matches run against STRIPPED_LOG,
# so bracketed patterns are literal and ANSI colors never confuse grep.
PASS_PATTERNS_ALL=(
    "Boot complete in"
    'C:\>'
)
FAIL_PATTERNS=(
    "KERNEL PANIC"
    "ASSERT FAILED"
    "triple fault"
    "General Protection Fault"
    "Page Fault"
    "Double Fault"
    "[CRIT] ExitBootServices failed"
    "[FAIL] Kernel ELF corrupt"
    "[BOOT HALT]"
    # GENERIC fatal signals (added 2026-07-28). Every entry above names one
    # specific failure, which meant a NEW fatal shape shipped green: a WHPX
    # boot of main@238c49f6 halted at `cmd.exe` with
    #   [CRIT] sched: ring-3 task 4 thread 0 has TEB but kernel_gs_base=0
    #   [**] FATAL -- system halted
    # and this test passed, because neither line matched any specific pattern.
    #
    # `[CRIT]` is safe to blanket-fail on: it is the klog tag for LOG_FATAL,
    # documented in include/kernel/klog.h as "serial + framebuffer, then halt".
    # It is fatal BY DEFINITION, not a severity judgement -- and it appears
    # zero times in a healthy captured log (verified against the current
    # build/smoke-test.stripped.log).
    "[CRIT]"
    "FATAL -- system halted"
    "system halted"
)

# ---- Bootloader + kernel presence checks (verified after boot) ----
# Two layers, both must pass:
#
#   Layer 1 (CORE): POST16 code assertions from the source-of-truth manifest
#                   at build/post16-manifest.env. Emitted by the bootloader's
#                   post_code16() as "[BOOT] POST 0xNNNN" on serial. These
#                   survive printf/klog rename drift because the #define
#                   values in bootx64.c are the contract.
#
#   Layer 2 (RESIDUAL): a few string assertions that capture user-visible
#                       boot signals (ExitBootServices OK, Phase 0 BOOT_INFO
#                       banner, "Boot complete in", "C:\>"). These still
#                       drift if someone renames a printf, but they catch
#                       regressions the POST16 layer cannot (userland never
#                       emits POST16 codes).
#
# The previous BOOT_REQUIRED_PATTERNS string list is kept below as the
# fallback layer until the POST16 path has been proven on KVM + TCG +
# VirtualBox + bare metal (per the developer tooling roadmap smoke-test
# assertion section).

# Layer 1 core assertions: manifest is sourced AFTER the build step (see
# Step 1 below), because the build generates the manifest. Declare the
# arrays up-front so 'set -u' doesn't trip later; they get populated by
# sourcing build/post16-manifest.env post-build.
POST16_REQUIRED=()
POST16_REQUIRED_CODES=()

# Layer 2 residual string assertions: user-visible end-to-end signals.
BOOT_REQUIRED_STRING_SIGNALS=(
    "[BOOT] ExitBootServices OK"
    "[PHASE0] BOOT_INFO"
    "Boot complete in"
    'C:\>'
    # Bare-metal hardening signals (TODO-10 Unit Tests). Each is the serial
    # evidence for a gate whose absence is silent: an unparsed FADT probes
    # legacy ports that may not exist, an unallocated IST turns a #DF into a
    # triple fault, and a failed NX enable runs with no no-execute enforcement
    # after the page tables were already marked NX. None of the three is
    # visible from a unit test -- the values are platform state, not API
    # behavior -- so the boot log is where they are asserted.
    "IAPC_BOOT_ARCH:"
    "IST stacks:"
    "Verify: NX enabled"
)

# Fallback layer: legacy string-pattern list kept in place until POST16 path
# is proven across all four validation platforms. Logs residual diagnostics
# (one line per miss) but does NOT fail the smoke test; the core assertion
# is code-based. Remove after KVM+TCG+VBox+bare-metal all confirm stable.
BOOT_FALLBACK_STRINGS=(
    "[BOOT] ELF segment"
    "[BOOT] Kernel found at"
    "[BOOT] Watchdog: armed"
    "[BOOT] Watchdog: disarmed"
)

# These patterns must NOT appear on a clean firmware boot.
BOOT_ABSENT_PATTERNS=(
    "[BOOT] mmap: overlap resolved"
    "[WARN] mmap: carve at cap"
    "[WARN] Memory map entry"
    "[FAIL] Kernel ELF corrupt"
    "[CRIT] ExitBootServices failed"
)

echo -e "${CYAN}══════════════════════════════════════════════════${NC}"
echo -e "${CYAN}  Impossible OS -- Smoke Test${NC}"
echo -e "${CYAN}══════════════════════════════════════════════════${NC}"
echo ""

# The assertion body is a FUNCTION over a log path, not inline code, so the
# oracle itself can be exercised against synthetic transcripts (see
# SMOKE_ES_SELFTEST below). Two false passes were found by review that a live
# boot could not have surfaced, because a correct build never produces the
# transcript that would expose them. Sets ES_FAIL_MSG and returns 1 on a
# rejection rather than exiting, so the self-test can assert rejections.
es_check_log() {
    local log="$1"
    ES_FAIL_MSG=""
    ES_TIER_LINE=$(grep -oE "error-screen: tier=[a-z]+ [0-9]+x[0-9]+ qr=[a-z]+" \
                   "$log" 2>/dev/null | head -1)
    ES_RENDERED=$(grep -oE "error-screen: rendered=[a-z]+ qr=[a-z]+ fit=[a-z]+" \
                  "$log" 2>/dev/null | head -1)

    if ! grep -qF -- "[BOOT] error_screen_test=1 -- triggering boot_fatal" "$log" 2>/dev/null; then
        ES_FAIL_MSG="the injected error never fired (boot.conf fixture not applied?)"; return 1
    fi
    if ! grep -qF -- "[BOOT] error_screen_test: halting" "$log" 2>/dev/null; then
        ES_FAIL_MSG="boot_fatal did not reach its deliberate halt"; return 1
    fi
    [ -n "$ES_TIER_LINE" ] || { ES_FAIL_MSG="no error-screen capability line on serial"; return 1; }
    [ -n "$ES_RENDERED" ]  || { ES_FAIL_MSG="boot_fatal reported no render channel"; return 1; }

    # UNCONDITIONAL floor, whatever mode this leg forced. Asserting only that
    # the lines EXIST would pass on 'tier=none 0x0 qr=no' + 'rendered=none' --
    # a boot that painted nothing at all, which is precisely the state this
    # section exists to eliminate. QEMU always gives us a framebuffer, so a
    # 'none' here is a regression, never a platform limit.
    case "$ES_TIER_LINE" in
        *"tier=none"*) ES_FAIL_MSG="capability reported no tier: $ES_TIER_LINE"; return 1 ;;
        *" 0x0 "*)     ES_FAIL_MSG="capability reported a 0x0 mode: $ES_TIER_LINE"; return 1 ;;
    esac
    case "$ES_RENDERED" in
        "error-screen: rendered=none"*)
            ES_FAIL_MSG="boot_fatal painted nothing: $ES_RENDERED"; return 1 ;;
    esac
    # fit= is the renderer's own report that no field was truncated. A message
    # edited past its line budget in a later change stops being a screen that
    # quietly ends mid-instruction and becomes this failure.
    case "$ES_RENDERED" in
        *"fit=ok") : ;;
        *) ES_FAIL_MSG="a rendered field was truncated: $ES_RENDERED"; return 1 ;;
    esac

    # The capability line says what this display COULD carry; the render line
    # says what it DID. They are computed from the same predicate over the same
    # globals, so a disagreement is a real defect -- a layout downgrade between
    # init_gop and the fault, or a stale tier. Asserting equality unconditionally
    # is what makes the full-tier leg a regression guard rather than a smoke
    # test that accepts any render at all.
    ES_CAP_TIER=${ES_TIER_LINE#*tier=}; ES_CAP_TIER=${ES_CAP_TIER%% *}
    ES_GOT_TIER=${ES_RENDERED#*rendered=}; ES_GOT_TIER=${ES_GOT_TIER%% *}
    if [ "$ES_CAP_TIER" != "$ES_GOT_TIER" ]; then
        ES_FAIL_MSG="the display could carry '$ES_CAP_TIER' but rendered '$ES_GOT_TIER'"
        return 1
    fi
    # Same rule for the recovery route: when the capability line says the QR
    # fits, a fatal render that did not paint one has lost it. This is the
    # route the section is required NOT to break.
    ES_CAP_QR=${ES_TIER_LINE##*qr=}
    ES_GOT_QR=${ES_RENDERED##*qr=}; ES_GOT_QR=${ES_GOT_QR%% *}
    if [ "$ES_CAP_QR" = "fits" ] && [ "$ES_GOT_QR" != "yes" ]; then
        ES_FAIL_MSG="the QR fits on this display but was not painted: $ES_RENDERED"
        return 1
    fi

    if [ -n "$EXPECT_TIER" ]; then
        case "$ES_TIER_LINE" in
            *"tier=${EXPECT_TIER} "*) : ;;
            *) ES_FAIL_MSG="expected tier=${EXPECT_TIER}, capability said '$ES_TIER_LINE'"; return 1 ;;
        esac
        case "$ES_RENDERED" in
            "error-screen: rendered=${EXPECT_TIER} qr=yes fit=ok") : ;;
            *) ES_FAIL_MSG="expected 'rendered=${EXPECT_TIER} qr=yes fit=ok', got '$ES_RENDERED'"; return 1 ;;
        esac
    fi
    if [ -n "$FORCE_MODE" ]; then
        # The forced mode must actually have taken -- otherwise the leg is
        # asserting against whatever OVMF picked.
        case "$ES_TIER_LINE" in
            *" ${FORCE_MODE} qr="*) : ;;
            *) ES_FAIL_MSG="the forced mode ${FORCE_MODE} did not take: '$ES_TIER_LINE'"; return 1 ;;
        esac
    fi
    if [ "$LOWRES" = "1" ]; then
        case "$ES_TIER_LINE" in
            *"tier=compact ${LOWRES_MODE} qr=fits") : ;;
            *) ES_FAIL_MSG="expected 'tier=compact ${LOWRES_MODE} qr=fits', got '$ES_TIER_LINE'"; return 1 ;;
        esac
        [ "$ES_RENDERED" = "error-screen: rendered=compact qr=yes fit=ok" ] || {
            ES_FAIL_MSG="expected 'rendered=compact qr=yes fit=ok', got '$ES_RENDERED'"; return 1; }
    fi
    return 0
}

# ---- UKI-direct legs (SMOKE_UKI=signed|override) ------------------------------
# Every other leg boots the split-path BOOTX64.EFI. These boot a Unified Kernel
# Image directly: a fixture UKI packed by scripts/uki-pack.sh (the same packer
# the shipped BOOTX64.UKI.efi comes from) is installed as the ESP's
# EFI/BOOT/BOOTX64.EFI, so firmware launches the PE that carries the kernel,
# the cmdline and any signed payload inside one image.
#
#   signed    a 4096-byte .initrd is embedded. The leg must boot to the prompt
#             AND show the payload carried end to end: the loader copied it
#             ("[BOOT] UKI payload .initrd: bytes=4096 phys=0x..."), and the
#             kernel pinned that same range so the PMM cannot reuse it
#             ("boot_reserved: pinned UKI .initrd bytes=4096 phys=0x...").
#   override  the .cmdline carries a disk-payload override (SMOKE_UKI_OVERRIDE,
#             default `initrd=disk.img`). The loader must refuse it with
#             BOOT_ERR_UKI_DISK_OVERRIDE before any kernel handoff. A token
#             with the literal `<key>=` shape is refused by the pre-scan
#             ("...override token"); one that only normalises to a key, such
#             as `mod ule = disk.eif`, must be refused by the parser-level
#             gate ("...override key"), which is what this leg proves exists.
#
# The fixture UKI is NOT signed and these OVMF variables do not enforce Secure
# Boot, so the legs prove the loader's UKI path, not signature enforcement;
# scripts/test-secureboot-smoke.sh owns that.
#
# Like es_check_log, the verdicts are functions over a log path so the oracle
# can be exercised against synthetic transcripts (SMOKE_UKI_SELFTEST=1).
UKI_INITRD_BYTES=4096
# SHA-256 of the staged .initrd (bytes 0..255 repeating, UKI_INITRD_BYTES long),
# computed on the host from the same generator the fixture uses.
UKI_INITRD_SHA256="$(python3 -c 'import hashlib,sys; n=int(sys.argv[1]); print(hashlib.sha256(bytes(i & 0xFF for i in range(n))).hexdigest())' "$UKI_INITRD_BYTES")"
# The loader's self-measure size cap (SELF_MEASURE_SIZE_CAP, bootx64.c). An
# image above it is refused with status=over-size-cap by contract, which is the
# ONLY outcome the oracle accepts for such an image. Pinned against the source
# so the copy cannot drift silently.
SELF_MEASURE_CAP_BYTES=$((8 * 1024 * 1024))

# The parser key a cmdline token normalises to: everything before the first
# `=`, with whitespace removed (parse_boot_conf's key normalisation).
uki_override_key() {
    local tok="$1"
    printf '%s' "${tok%%=*}" | tr -d ' \t'
}

# Which layer must refuse the token: `token` when the raw bytes contain the
# literal `<key>=` the pre-scan looks for, else `key` (the parser gate).
uki_override_layer() {
    local tok="$1" key
    key="$(uki_override_key "$tok")"
    case "$tok" in
        "$key="*) echo token ;;
        *) echo key ;;
    esac
}

uki_hex_norm() {  # uppercase/lowercase hex -> lowercase, no leading zeros ("0" for zero)
    local h
    h="$(printf '%s' "$1" | tr 'A-F' 'a-f' | sed 's/^0*//')"
    printf '%s' "${h:-0}"
}

uki_check_signed_log() {
    local log="$1" bl kn bl_phys kn_phys
    UKI_FAIL_MSG=""
    if ! grep -qF -- '[BOOT] load_kernel: using UKI embedded `.linux` section' "$log"; then
        UKI_FAIL_MSG="loader did not take the UKI path (no embedded .linux line)"; return 1
    fi
    bl="$(grep -oE '\[BOOT\] UKI payload \.initrd: bytes=[0-9]+ phys=0x[0-9A-Fa-f]+' "$log" | head -1)"
    if [ -z "$bl" ]; then
        UKI_FAIL_MSG="loader did not report copying the .initrd payload"; return 1
    fi
    if [ "$(grep -cE '\[BOOT\] UKI payload \.initrd:' "$log")" != "1" ]; then
        UKI_FAIL_MSG="loader reported the .initrd copy more than once"; return 1
    fi
    case "$bl" in *" bytes=$UKI_INITRD_BYTES "*) ;; *)
        UKI_FAIL_MSG="loader copied the wrong size: $bl (staged $UKI_INITRD_BYTES)"; return 1 ;;
    esac
    if grep -qE '\[BOOT\] UKI payload \.(recovery|modules):' "$log"; then
        UKI_FAIL_MSG="loader reported a payload that was never staged"; return 1
    fi
    kn="$(grep -oE 'boot_reserved: pinned UKI \.initrd bytes=[0-9]+ phys=0x[0-9A-Fa-f]+' "$log" | head -1)"
    if [ -z "$kn" ]; then
        UKI_FAIL_MSG="kernel did not pin the .initrd copy (PMM may reuse its frames)"; return 1
    fi
    case "$kn" in *" bytes=$UKI_INITRD_BYTES "*) ;; *)
        UKI_FAIL_MSG="kernel pinned the wrong size: $kn"; return 1 ;;
    esac
    # Addresses are compared as unsigned digit strings: bash arithmetic is
    # signed 64-bit, so a high-bit address would go negative and pass any
    # upper-bound test. Leading zeros dropped, at most 9 hex digits (anything
    # longer is >= 64 GiB, far past the identity map), then the arithmetic is
    # exact.
    bl_phys="$(uki_hex_norm "${bl##*phys=0x}")"; kn_phys="$(uki_hex_norm "${kn##*phys=0x}")"
    if [ "$bl_phys" != "$kn_phys" ]; then
        UKI_FAIL_MSG="kernel pinned 0x$kn_phys but the loader copied to 0x$bl_phys"; return 1
    fi
    if [ "$bl_phys" = "0" ]; then
        UKI_FAIL_MSG="loader reported a copy at physical address 0"; return 1
    fi
    if [ "${#bl_phys}" -gt 9 ] || [ "$((16#$bl_phys + UKI_INITRD_BYTES))" -gt "$((16#100000000))" ]; then
        UKI_FAIL_MSG="loader placed the copy beyond the 4 GiB identity map: 0x$bl_phys"; return 1
    fi
    # The bytes the kernel holds must be the bytes that were staged: a copy
    # that moved the wrong source, a short copy, or a pin over the wrong range
    # all change this digest, and none of them changes the size or address.
    if ! grep -qF -- "boot_reserved: UKI .initrd received sha256=$UKI_INITRD_SHA256" "$log"; then
        UKI_FAIL_MSG="kernel's received .initrd digest is not the staged payload's ($UKI_INITRD_SHA256)"; return 1
    fi
    UKI_LOADER_LINE="$bl"; UKI_KERNEL_LINE="$kn"
    return 0
}

uki_check_override_log() {
    local log="$1" tok="$2" key layer title
    UKI_FAIL_MSG=""
    key="$(uki_override_key "$tok")"
    layer="$(uki_override_layer "$tok")"
    title="UKI cmdline contains disk-payload override $layer"
    if ! grep -qF -- '[BOOT] UKI .cmdline loaded (' "$log"; then
        UKI_FAIL_MSG="loader did not read the UKI .cmdline section"; return 1
    fi
    if ! grep -qF -- "[FATAL] UKI mode rejects out-of-UKI $key= override" "$log"; then
        UKI_FAIL_MSG="no rejection of the $key= override"; return 1
    fi
    if ! grep -qF -- "[CRIT] BOOT FATAL (0x00000013): $title" "$log"; then
        UKI_FAIL_MSG="refusal did not come from the $layer layer as BOOT_ERR_UKI_DISK_OVERRIDE (expected: $title)"; return 1
    fi
    # No handoff. The absence of kernel output alone cannot exclude a handoff
    # that faulted before logging, so the loader's own last line before the
    # jump is forbidden too.
    if grep -qF -- '[BOOT] jumping to kernel_main' "$log"; then
        UKI_FAIL_MSG="loader handed off to the kernel after refusing the override"; return 1
    fi
    if grep -qE '\[cpu:[0-9]+\]' "$log"; then
        UKI_FAIL_MSG="kernel output present: the boot continued past the refusal"; return 1
    fi
    UKI_FATAL_LINE="$(grep -F -- "[CRIT] BOOT FATAL (0x00000013)" "$log" | head -1)"
    return 0
}

if [ "${SMOKE_UKI_SELFTEST:-0}" = "1" ]; then
    uki_st_fails=0
    uki_st_log="$BUILD/uki-selftest.log"
    uki_expect() {  # $1 accept|reject, $2 label, $3 verdict fn, $4 token, rest = log lines
        local want="$1" label="$2" fn="$3" tok="$4" got; shift 4
        printf '%s\n' "$@" > "$uki_st_log"
        if "$fn" "$uki_st_log" "$tok"; then got=accept; else got=reject; fi
        if [ "$got" != "$want" ]; then
            echo -e "  ${RED}✗${NC} UKI oracle self-test: $label -- wanted $want, got $got ($UKI_FAIL_MSG)"
            uki_st_fails=$((uki_st_fails + 1))
        else
            echo -e "  ${GREEN}✓${NC} UKI oracle self-test: $label ($got)"
        fi
    }
    L_EMB='[BOOT] load_kernel: using UKI embedded `.linux` section'
    L_BL='[BOOT] UKI payload .initrd: bytes=4096 phys=0x000000007F000000'
    L_KN='[ 1.0] [cpu:0] [ OK ] mm: boot_reserved: pinned UKI .initrd bytes=4096 phys=0x7f000000'
    L_DG="[cpu:0] mm: boot_reserved: UKI .initrd received sha256=$UKI_INITRD_SHA256"
    uki_sig() {  # loader phys (16 hex), kernel phys, rest = extra lines
        local lp="$1" kp="$2"; shift 2
        uki_expect "$UKI_WANT" "$UKI_LABEL" uki_check_signed_log "" "$L_EMB" \
            "[BOOT] UKI payload .initrd: bytes=4096 phys=0x$lp" \
            "[cpu:0] mm: boot_reserved: pinned UKI .initrd bytes=4096 phys=0x$kp" "$@"
    }
    UKI_WANT=reject UKI_LABEL="signed: zero address";          uki_sig 0000000000000000 0 "$L_DG"
    UKI_WANT=reject UKI_LABEL="signed: high-bit address";      uki_sig 8000000000000000 8000000000000000 "$L_DG"
    UKI_WANT=reject UKI_LABEL="signed: address that wraps";    uki_sig FFFFFFFFFFFFF800 fffffffffffff800 "$L_DG"
    UKI_WANT=reject UKI_LABEL="signed: straddles 4 GiB";       uki_sig 00000000FFFFF800 fffff800 "$L_DG"
    UKI_WANT=accept UKI_LABEL="signed: ends exactly at 4 GiB"; uki_sig 00000000FFFFF000 fffff000 "$L_DG"
    UKI_WANT=reject UKI_LABEL="signed: received bytes differ"; uki_sig 000000007F000000 7f000000 \
        "[cpu:0] mm: boot_reserved: UKI .initrd received sha256=0000000000000000000000000000000000000000000000000000000000000000"
    UKI_WANT=reject UKI_LABEL="signed: no received digest";    uki_sig 000000007F000000 7f000000
    L_KN="$L_KN
$L_DG"
    uki_expect accept "signed: carried and pinned"       uki_check_signed_log "" "$L_EMB" "$L_BL" "$L_KN"
    uki_expect reject "signed: split path, not UKI"      uki_check_signed_log "" "$L_BL" "$L_KN"
    uki_expect reject "signed: loader never copied"      uki_check_signed_log "" "$L_EMB" "$L_KN"
    uki_expect reject "signed: kernel never pinned"      uki_check_signed_log "" "$L_EMB" "$L_BL"
    uki_expect reject "signed: wrong size copied"        uki_check_signed_log "" "$L_EMB" '[BOOT] UKI payload .initrd: bytes=4095 phys=0x000000007F000000' "$L_KN"
    uki_expect reject "signed: pinned a different range" uki_check_signed_log "" "$L_EMB" "$L_BL" '[cpu:0] mm: boot_reserved: pinned UKI .initrd bytes=4096 phys=0x7f001000'
    uki_expect reject "signed: copy above 4 GiB"         uki_check_signed_log "" "$L_EMB" '[BOOT] UKI payload .initrd: bytes=4096 phys=0x0000000100000000' '[cpu:0] boot_reserved: pinned UKI .initrd bytes=4096 phys=0x100000000'
    uki_expect reject "signed: unstaged payload copied"  uki_check_signed_log "" "$L_EMB" "$L_BL" "$L_KN" '[BOOT] UKI payload .modules: bytes=16 phys=0x000000007E000000'
    L_CMD='[BOOT] UKI .cmdline loaded (4500 bytes)'
    L_REJ='[FATAL] UKI mode rejects out-of-UKI initrd= override; disk payload is unsigned'
    L_TOK='[CRIT] BOOT FATAL (0x00000013): UKI cmdline contains disk-payload override token'
    L_KEY='[CRIT] BOOT FATAL (0x00000013): UKI cmdline contains disk-payload override key'
    L_MREJ='[FATAL] UKI mode rejects out-of-UKI module= override; disk payload is unsigned'
    uki_expect accept "override: literal refused"         uki_check_override_log "initrd=disk.img" "$L_CMD" "$L_REJ" "$L_TOK"
    uki_expect reject "override: refused then handed off" uki_check_override_log "initrd=disk.img" "$L_CMD" "$L_REJ" "$L_TOK" '[BOOT] jumping to kernel_main'
    uki_expect reject "override: refused, kernel ran"     uki_check_override_log "initrd=disk.img" "$L_CMD" "$L_REJ" "$L_TOK" '[ 0.5] [cpu:0] [ OK ] boot: phase 0'
    uki_expect reject "override: never refused"           uki_check_override_log "initrd=disk.img" "$L_CMD" '[BOOT] jumping to kernel_main'
    uki_expect reject "override: not the UKI path"        uki_check_override_log "initrd=disk.img" "$L_REJ" "$L_TOK"
    uki_expect reject "override: wrong fatal code"        uki_check_override_log "initrd=disk.img" "$L_CMD" "$L_REJ" '[CRIT] BOOT FATAL (0x00000012): UKI cmdline contains disk-payload override token'
    uki_expect accept "override: spaced key, parser gate" uki_check_override_log "mod ule = disk.eif" "$L_CMD" "$L_MREJ" "$L_KEY"
    uki_expect reject "override: spaced key, wrong layer" uki_check_override_log "mod ule = disk.eif" "$L_CMD" "$L_MREJ" "$L_TOK"
    if [ "$uki_st_fails" -gt 0 ]; then
        echo -e "${RED}UKI ORACLE SELF-TEST FAILED: $uki_st_fails case(s)${NC}"
        exit 1
    fi
    echo -e "${GREEN}  UKI ORACLE SELF-TEST PASSED${NC}"
    exit 0
fi

# ---- Oracle self-test (SMOKE_ES_SELFTEST=1) --------------------------------
# Runs es_check_log against synthetic transcripts and asserts its VERDICT.
# Boots nothing, so it costs milliseconds and can assert the rejections a
# correct build can never produce. The two ACCEPT cases at the end are the
# false passes review demonstrated against the previous inline oracle.
if [ "${SMOKE_ES_SELFTEST:-0}" = "1" ]; then
    es_selftest_fixture() {
        printf '%s\n' \
            "[BOOT] error-screen: tier=$1 $2 qr=$3" \
            "[BOOT] error_screen_test=1 -- triggering boot_fatal" \
            "[BOOT] error-screen: rendered=$4 qr=$5 fit=$6" \
            "[BOOT] error_screen_test: halting (screen stays visible)" \
            > "$BUILD/es-selftest.log"
    }
    es_selftest_fails=0
    es_expect() {  # $1 = expect (accept|reject), $2 = label, rest = fixture args
        local want="$1" label="$2"; shift 2
        es_selftest_fixture "$@"
        if es_check_log "$BUILD/es-selftest.log"; then got=accept; else got=reject; fi
        if [ "$got" != "$want" ]; then
            echo -e "  ${RED}✗${NC} oracle self-test: $label -- wanted $want, got $got ($ES_FAIL_MSG)"
            es_selftest_fails=$((es_selftest_fails + 1))
        else
            echo -e "  ${GREEN}✓${NC} oracle self-test: $label ($got)"
        fi
    }
    LOWRES=0; FORCE_MODE=""; EXPECT_TIER=""
    es_expect accept "healthy full-tier render"      full 800x600 fits full yes ok
    es_expect accept "healthy compact-tier render"   compact 640x480 fits compact yes ok
    es_expect reject "nothing rendered"              none 0x0 no none no ok
    es_expect reject "truncated field"               full 800x600 fits full yes truncated
    # The two false passes review demonstrated against the inline oracle.
    es_expect reject "QR lost though it fits"        full 800x600 fits full no ok
    es_expect reject "silent downgrade to compact"   full 800x600 fits compact yes ok
    EXPECT_TIER=full
    es_expect accept "expect-full leg, full render"  full 800x600 fits full yes ok
    es_expect reject "expect-full leg, downgraded"   compact 800x600 fits compact yes ok
    es_expect reject "expect-full leg, QR absent"    full 800x600 fits full no ok
    # Capability AND render both say no QR, so the agreement check above passes
    # it and only EXPECT_TIER's qr=yes clause can reject it. Without this case,
    # weakening that clause leaves every other self-test green.
    es_expect reject "expect-full leg, QR absent on both lines" full 800x600 no full no ok
    EXPECT_TIER=""
    LOWRES=1; LOWRES_MODE=640x480; FORCE_MODE=640x480; EXPECT_TIER=compact
    es_expect accept "lowres leg, correct render"    compact 640x480 fits compact yes ok
    es_expect reject "lowres leg, wrong forced mode" compact 800x600 fits compact yes ok
    if [ "$es_selftest_fails" -gt 0 ]; then
        echo -e "${RED}ORACLE SELF-TEST FAILED: $es_selftest_fails case(s)${NC}"
        exit 1
    fi
    echo -e "${GREEN}  ORACLE SELF-TEST PASSED${NC}"
    exit 0
fi

# ---- Step 1: Build ----
echo -e "${CYAN}[1/3]${NC} Building OS..."
cd "$REPO_ROOT"
# What the BootStoreReject record must do on this boot: written, unchanged or
# cleared. Anything but "written" is a LATER boot of the same machine, so it keeps
# the variable store the previous boot left, which the clean below would delete.
# A corrupt store on fresh variables can only write, so that fixture asserts it
# by default.
STORE_RECORD="${SMOKE_STORE_RECORD:-}"
if [ -z "$STORE_RECORD" ] && [ "${SMOKE_CORRUPT_STORE:-0}" = "1" ]; then
    STORE_RECORD=written
fi
PREVIOUS_VARS=""
# SMOKE_MENU=saved is the second boot of the machine SMOKE_MENU=default set a
# saved default on, so it keeps that boot's variable store too.
REUSED_VARS=0
if { [ -n "$STORE_RECORD" ] && [ "$STORE_RECORD" != "written" ]; } || [ "${SMOKE_MENU:-0}" = "saved" ]; then
    REUSED_VARS=1
    if [ ! -f "$OVMF_VARS_CP" ]; then
        echo -e "${RED}SMOKE TEST FAILED: SMOKE_STORE_RECORD=$STORE_RECORD / SMOKE_MENU=${SMOKE_MENU:-0} needs a previous boot's variable store${NC}"
        exit 1
    fi
    PREVIOUS_VARS="$(mktemp "${TMPDIR:-/tmp}/smoke-previous-vars.XXXXXX")"
    cp "$OVMF_VARS_CP" "$PREVIOUS_VARS"
fi
# A second boot of the same machine also keeps its disk's per-entry try
# counters: the first boot's kernel marked its entry good, and only the counter
# it names lets this boot's loader consume that mark. The fixture disk is rebuilt
# from the clean image, so the counters are carried across like the variables.
PREVIOUS_COUNTERS=""
if [ "${SMOKE_MENU:-0}" = "saved" ]; then
    prev_disk="$BUILD/system-disk.fixture.img"
    if [ ! -f "$prev_disk" ] || [ ! -f "$BUILD/system-disk.img.info" ]; then
        echo -e "${RED}SMOKE TEST FAILED: SMOKE_MENU=saved needs the previous boot's fixture disk${NC}"
        exit 1
    fi
    prev_off=$(. "$BUILD/system-disk.img.info"; echo "$EFI_OFFSET")
    PREVIOUS_COUNTERS="$(mktemp -d "${TMPDIR:-/tmp}/smoke-previous-counters.XXXXXX")"
    if ! mcopy -s -n -i "$prev_disk@@$prev_off" ::EFI/ImpossibleOS/counters "$PREVIOUS_COUNTERS/" 2>/dev/null \
        || [ -z "$(ls -A "$PREVIOUS_COUNTERS/counters" 2>/dev/null)" ]; then
        echo -e "${RED}SMOKE TEST FAILED: the previous boot left no try counter to carry over${NC}"
        rm -rf "$PREVIOUS_COUNTERS"
        exit 1
    fi
fi

bash scripts/build.sh clean >/dev/null 2>&1

BUILD_RESULT=$(tail -1 build/build.log 2>/dev/null || echo "UNKNOWN")
if [ "$BUILD_RESULT" != "=== BUILD OK ===" ]; then
    # The clean build deleted the store; put the kept one back so a retry after
    # the fix still boots the same machine, and the copy does not leak.
    [ -n "$PREVIOUS_VARS" ] && mv -f "$PREVIOUS_VARS" "$OVMF_VARS_CP"
    echo -e "${RED}SMOKE TEST FAILED: Build failed${NC}"
    echo -e "${DIM}  Check build/build.log for details${NC}"
    exit 1
fi
echo -e "  ${GREEN}✓${NC} Build succeeded"

# Source the freshly-generated POST16 manifest. Hard-fail on missing,
# empty, malformed, or length-mismatched arrays -- the manifest is the
# core assertion for this smoke test; skipping it silently would defeat
# the regression check.
if [ ! -s "$POST16_MANIFEST" ]; then
    echo -e "${RED}SMOKE TEST FAILED: $POST16_MANIFEST missing or empty${NC}"
    echo -e "${DIM}  The build should have regenerated it; see build/build.log${NC}"
    exit 1
fi
# shellcheck disable=SC1090
source "$POST16_MANIFEST"
if [ "${#POST16_REQUIRED[@]}" -eq 0 ]; then
    echo -e "${RED}SMOKE TEST FAILED: POST16_REQUIRED empty after sourcing $POST16_MANIFEST${NC}"
    echo -e "${DIM}  Manifest is malformed or corrupted. Run 'bash scripts/build.sh' to regenerate.${NC}"
    exit 1
fi
if [ "${#POST16_REQUIRED[@]}" -ne "${#POST16_REQUIRED_CODES[@]}" ]; then
    echo -e "${RED}SMOKE TEST FAILED: POST16_REQUIRED / POST16_REQUIRED_CODES length mismatch${NC}"
    echo -e "${DIM}  Names=${#POST16_REQUIRED[@]} Codes=${#POST16_REQUIRED_CODES[@]}. Manifest corrupted.${NC}"
    exit 1
fi
echo -e "  ${GREEN}✓${NC} POST16 manifest loaded (${#POST16_REQUIRED[@]} required codes)"

# ---- Step 2: Boot in QEMU ----
echo -e "${CYAN}[2/3]${NC} Booting in QEMU (headless, ${TIMEOUT_SEC}s timeout)..."

# Preflight
if [ ! -f "$DISK" ]; then
    echo -e "${RED}SMOKE TEST FAILED: $DISK not found${NC}"
    echo -e "${DIM}  Run 'bash scripts/build.sh' first.${NC}"
    exit 1
fi
if [ ! -f "$OVMF_CODE" ]; then
    echo -e "${RED}SMOKE TEST FAILED: OVMF not found at $OVMF_CODE${NC}"
    exit 1
fi

if [ -n "$PREVIOUS_VARS" ]; then
    mv -f "$PREVIOUS_VARS" "$OVMF_VARS_CP"
else
    cp "$OVMF_VARS_SRC" "$OVMF_VARS_CP"
fi

# ---- Optional corrupt-boot-entry-store fixture ----------------------------
# Proves the rejected-store notice actually renders instead of the machine
# falling back to the in-firmware entry in silence.
#
# Works on a COPY of the image, never the canonical one. The canonical image
# is what the smoke receipt and the rollover gate are bound to, so mutating it
# in place would certify a boot that never happened against bytes that no
# longer exist. The copy also keeps this off the receipt surface: the fixture
# needs no change to the Makefile or scripts/build.sh, which the unattended
# run may not edit.
CORRUPT_STORE="${SMOKE_CORRUPT_STORE:-0}"
# Stage a VALID store. The parser's runtime tables and struct copies only run on
# this path, so an image with no store cannot exercise them.
VALID_STORE="${SMOKE_VALID_STORE:-0}"
# Stage a valid last-known-good copy (bootentries.lkg.json) beside the store, so
# a corrupt primary boots the copy instead of the fallback (the store shadow).
# SMOKE_SHADOW_STORE=tmp instead stages a BROKEN published copy and a valid
# staging copy (bootentries.lkg.tmp): an interrupted publication.
# SMOKE_SHADOW_STORE=empty (with SMOKE_VALID_STORE=1) stages only an EMPTY
# staging copy: a write cut off before any byte landed. It is definitely not a
# store, so the loader must replace it, not hold off as for a failed read.
SHADOW_STORE="${SMOKE_SHADOW_STORE:-0}"
case "$SHADOW_STORE" in 0|1|tmp|empty) ;; *) echo -e "${RED}SMOKE TEST FAILED: SMOKE_SHADOW_STORE must be 0, 1, tmp or empty (got: $SHADOW_STORE)${NC}"; exit 1 ;; esac
if [ "$SHADOW_STORE" = "empty" ] && [ "${SMOKE_VALID_STORE:-0}" != "1" ]; then
    echo -e "${RED}SMOKE TEST FAILED: SMOKE_SHADOW_STORE=empty needs SMOKE_VALID_STORE=1${NC}"; exit 1
fi
SHADOW_NAME=bootentries.lkg.json
[ "$SHADOW_STORE" = "tmp" ] || [ "$SHADOW_STORE" = "empty" ] && SHADOW_NAME=bootentries.lkg.tmp
# Stage a valid store with TWO entries, so the boot menu draws, counts down and
# auto-boots its default. No other leg reaches the menu: one entry skips it.
# SMOKE_MENU=keys drives the same menu through the QEMU monitor instead: Down
# moves to the second entry, the leg then waits past the countdown deadline to
# prove the keypress stopped it, and Enter boots the second entry.
# SMOKE_MENU=default moves to the second entry, opens and closes help, presses
# d to make it the saved default, t to save a 5 s timeout, and e to boot it
# once with typed options;
# SMOKE_MENU=saved is that machine's next boot, which must pick the saved entry
# on its own and count down from the saved timeout.
MENU="${SMOKE_MENU:-0}"
case "$MENU" in 0|1|keys|default|saved) ;; *) echo -e "${RED}SMOKE TEST FAILED: SMOKE_MENU must be 0, 1, keys, default or saved (got: $MENU)${NC}"; exit 1 ;; esac
if [ "$MENU" != "0" ]; then VALID_STORE=1; fi
STORE_ENTRIES=1
[ "$MENU" != "0" ] && STORE_ENTRIES=2
# Countdown on the default entry: short for the timeout leg, and for the keys leg
# long enough that Down lands before it expires.
MENU_COUNTDOWN=2
{ [ "$MENU" = "keys" ] || [ "$MENU" = "default" ]; } && MENU_COUNTDOWN=4
# After Down, the keys leg holds Enter back this long past the original deadline,
# so a countdown that kept running would expire inside the window and fail.
MENU_KEYS_DWELL=$((MENU_COUNTDOWN + 6))
MONITOR_SOCK="$BUILD/smoke-monitor.sock"
# Send one key to the guest through the QEMU monitor. Returns non-zero when the
# monitor cannot be reached, so a dead socket fails the leg instead of reading
# as "the menu ignored the key".
monitor_sendkey() {
    python3 - "$MONITOR_SOCK" "$1" <<'PYEOF'
import socket, sys
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.settimeout(2.0)
s.connect(sys.argv[1])
s.sendall(("sendkey %s\n" % sys.argv[2]).encode())
try:
    s.recv(4096)
except socket.timeout:
    pass
s.close()
PYEOF
}
KEYS_STATE=wait_menu
KEYS_MOVED_AT=0
# S23 fixtures. SMOKE_LOWRES forces a GOP mode below the full error-screen
# layout floor so the compact renderer is the one under test; SMOKE_ERROR_SCREEN
# additionally makes the bootloader raise a real boot_fatal, which is the only
# way to observe that the compact screen and the QR recovery route coexist
# rather than the compact fill erasing the QR.
# SMOKE_FORCE_MODE writes a Resolution= into the fixture boot.conf and asserts
# nothing about the tier; SMOKE_LOWRES additionally asserts the render came out
# COMPACT. Keeping them separate is what makes an 800x600 full-tier regression
# leg runnable -- coupling the two meant forcing any mode also demanded a
# compact result, so the full layout could not be exercised at its own floor.
UKI_MODE="${SMOKE_UKI:-}"
UKI_OVERRIDE="${SMOKE_UKI_OVERRIDE:-initrd=disk.img}"
case "$UKI_MODE" in
    ""|signed|override) ;;
    *) echo -e "${RED}SMOKE TEST FAILED: SMOKE_UKI must be signed or override (got: $UKI_MODE)${NC}"; exit 1 ;;
esac
LOWRES="${SMOKE_LOWRES:-0}"
ERROR_SCREEN="${SMOKE_ERROR_SCREEN:-0}"
LOWRES_MODE="${SMOKE_LOWRES_MODE:-640x480}"
FORCE_MODE="${SMOKE_FORCE_MODE:-}"
# SMOKE_DEGRADE=rtc|fb forces one non-critical init to fail (resilient-boot roadmap):
# the fixture boot.conf carries cmdline=test_fail_<name>=1, and the leg passes
# only when the boot STILL reaches the prompt AND serial names the failure
# twice -- at the moment it happens and in the end-of-boot summary.
DEGRADE="${SMOKE_DEGRADE:-}"
case "$DEGRADE" in
    ""|rtc|fb) ;;
    *) echo -e "${RED}SMOKE TEST FAILED: SMOKE_DEGRADE must be rtc or fb (got: $DEGRADE)${NC}"; exit 1 ;;
esac
# SMOKE_EXPECT_TIER states the REQUIRED outcome rather than merely requiring
# the two reports to agree. Agreement alone accepts a downgrade: raising the
# full floor would make a forced 800x600 report compact on BOTH lines, and the
# unit boundary tests derive their inputs from that same constant, so nothing
# would have caught it.
EXPECT_TIER="${SMOKE_EXPECT_TIER:-}"
if [ "$LOWRES" = "1" ]; then FORCE_MODE="$LOWRES_MODE"; EXPECT_TIER="compact"; fi
# Any fixture that edits the image invalidates the smoke receipt, which
# fingerprints the CANONICAL build image: recording one here would certify the
# untouched image using evidence from a boot of different bytes.
IMAGE_MODIFIED=0
if [ "$CORRUPT_STORE" = "1" ] || [ "$VALID_STORE" = "1" ] || [ "$SHADOW_STORE" != "0" ] || [ -n "$FORCE_MODE" ] || [ "$ERROR_SCREEN" = "1" ] || [ -n "$UKI_MODE" ] || [ -n "$DEGRADE" ]; then
    if ! command -v mcopy >/dev/null 2>&1; then
        echo -e "${RED}SMOKE TEST FAILED: image fixtures need mtools (mcopy)${NC}"
        exit 1
    fi
    if [ ! -f "$DISK.info" ]; then
        echo -e "${RED}SMOKE TEST FAILED: $DISK.info missing (need EFI_OFFSET)${NC}"
        exit 1
    fi
    # shellcheck disable=SC1090
    . "$DISK.info"
    FIXTURE_DISK="$BUILD/system-disk.fixture.img"
    cp "$DISK" "$FIXTURE_DISK"
    DISK="$FIXTURE_DISK"
    IMAGE_MODIFIED=1
    mmd -i "$FIXTURE_DISK@@$EFI_OFFSET" ::EFI/ImpossibleOS 2>/dev/null || true
    if [ -n "$PREVIOUS_COUNTERS" ]; then
        if ! mcopy -s -o -i "$FIXTURE_DISK@@$EFI_OFFSET" "$PREVIOUS_COUNTERS/counters" ::EFI/ImpossibleOS/; then
            echo -e "${RED}SMOKE TEST FAILED: could not carry the try counters onto the fixture disk${NC}"
            rm -rf "$PREVIOUS_COUNTERS"
            exit 1
        fi
        rm -rf "$PREVIOUS_COUNTERS"
    fi
fi
if [ "$VALID_STORE" = "1" ] || [ "$SHADOW_STORE" != "0" ]; then
    VALID_JSON="$BUILD/bootentries.valid.json"
    python3 - "$VALID_JSON" "$STORE_ENTRIES" "$MENU_COUNTDOWN" <<'PYEOF'
import json, sys, zlib
entries = [{
    "id": "smoke-entry", "title": "Smoke", "kind": "split", "flags": ["active"],
    "sort_key": "00", "machine_id": "", "policy_tags": [],
    "payload": {"kernel": "\\boot\\kernel.exe"}}]
if sys.argv[2] == "2":
    # A short per-entry countdown on the default keeps the menu leg fast.
    entries[0]["timeout_override"] = int(sys.argv[3])
    # smoke-alt allows the menu's options editor, so the default leg can edit it.
    entries.append({
        "id": "smoke-alt", "title": "Smoke alternate", "kind": "split", "flags": ["active", "allow_editor"],
        "sort_key": "01", "machine_id": "", "policy_tags": [],
        "payload": {"kernel": "\\boot\\kernel.exe"}})
store = {"schema_version": 1, "crc32": "0x00000000", "entries": entries}
body = json.dumps(store).encode()
crc = zlib.crc32(body) & 0xFFFFFFFF
open(sys.argv[1], "wb").write(body.replace(b"0x00000000", b"0x%08X" % crc, 1))
PYEOF
    if [ "$VALID_STORE" = "1" ]; then
        mcopy -i "$DISK@@$EFI_OFFSET" -o "$VALID_JSON" \
              ::EFI/ImpossibleOS/bootentries.json
        echo -e "  ${DIM}valid-store fixture staged into $(basename "$DISK")${NC}"
    fi
    if [ "$SHADOW_STORE" = "empty" ]; then
        : > "$BUILD/bootentries.lkg.empty"
        mcopy -i "$DISK@@$EFI_OFFSET" -o "$BUILD/bootentries.lkg.empty" \
              "::EFI/ImpossibleOS/$SHADOW_NAME"
        echo -e "  ${DIM}empty staging-copy fixture staged into $(basename "$DISK")${NC}"
    elif [ "$SHADOW_STORE" != "0" ]; then
        mcopy -i "$DISK@@$EFI_OFFSET" -o "$VALID_JSON" \
              "::EFI/ImpossibleOS/$SHADOW_NAME"
        if [ "$SHADOW_STORE" = "tmp" ]; then
            # The published copy is torn: a valid prefix, no closing bytes.
            head -c 40 "$VALID_JSON" > "$BUILD/bootentries.lkg.torn.json"
            mcopy -i "$DISK@@$EFI_OFFSET" -o "$BUILD/bootentries.lkg.torn.json" \
                  ::EFI/ImpossibleOS/bootentries.lkg.json
        fi
        echo -e "  ${DIM}last-good-copy fixture ($SHADOW_NAME) staged into $(basename "$DISK")${NC}"
    fi
fi
if [ "$CORRUPT_STORE" = "1" ]; then
    CORRUPT_JSON="$BUILD/bootentries.corrupt.json"
    # A store that EXISTS and cannot be parsed. Deliberately not merely
    # absent: absence is the normal state of this image (the ESP staging
    # copies boot.conf only), and the notice must stay silent for it -- so a
    # fixture that removed the store would assert nothing.
    printf '%s\n' '{ "schema_version": 1, "entries": [ { "id": ' > "$CORRUPT_JSON"
    mcopy -i "$DISK@@$EFI_OFFSET" -o "$CORRUPT_JSON" \
          ::EFI/ImpossibleOS/bootentries.json
    echo -e "  ${DIM}corrupt-store fixture staged into $(basename "$DISK")${NC}"
fi
if [ -n "$FORCE_MODE" ] || [ "$ERROR_SCREEN" = "1" ] || [ -n "$DEGRADE" ]; then
    CONF_FIXTURE="$BUILD/boot.conf.fixture"
    if ! mcopy -i "$DISK@@$EFI_OFFSET" -o ::EFI/ImpossibleOS/boot.conf \
               "$CONF_FIXTURE" 2>/dev/null; then
        echo -e "${RED}SMOKE TEST FAILED: no boot.conf on the ESP to patch${NC}"
        exit 1
    fi
    # Drop any existing values for the keys we set, then append ours, so the
    # fixture does not depend on which duplicate the parser happens to keep.
    sed -i -E '/^[[:space:]]*(Resolution|error_screen_test)[[:space:]]*=/d' "$CONF_FIXTURE"
    if [ -n "$DEGRADE" ]; then
        # The injection key is reached through cmdline=, never as a bare
        # boot.conf line (see the test_* rows in src/kernel/config.c).
        sed -i -E '/^[[:space:]]*cmdline[[:space:]]*=/d' "$CONF_FIXTURE"
        printf 'cmdline=test_fail_%s=1\n' "$DEGRADE" >> "$CONF_FIXTURE"
        if [ "$DEGRADE" = "fb" ]; then
            # diag keeps VPD Tier 1 drawing through the whole boot, the
            # hardest case for "nothing paints through a refused layout".
            sed -i -E '/^[[:space:]]*postbars[[:space:]]*=/d' "$CONF_FIXTURE"
            printf 'postbars=diag\n' >> "$CONF_FIXTURE"
        fi
    fi
    if [ -n "$FORCE_MODE" ]; then
        printf 'Resolution=%s\n' "$FORCE_MODE" >> "$CONF_FIXTURE"
    fi
    if [ "$ERROR_SCREEN" = "1" ]; then
        printf 'error_screen_test=1\n' >> "$CONF_FIXTURE"
    fi
    mcopy -i "$DISK@@$EFI_OFFSET" -o "$CONF_FIXTURE" ::EFI/ImpossibleOS/boot.conf
    echo -e "  ${DIM}boot.conf fixture: $(tr '\n' ' ' < "$CONF_FIXTURE" | tail -c 60)${NC}"
fi

if [ -n "$UKI_MODE" ]; then
    UKI_FIX_DIR="$BUILD/uki-fixture"
    rm -rf "$UKI_FIX_DIR"; mkdir -p "$UKI_FIX_DIR/payloads"
    # The .cmdline is this image's own boot.conf, so the leg boots the same
    # configuration as every other leg; the override leg appends its token.
    if ! mcopy -i "$DISK@@$EFI_OFFSET" -o ::EFI/ImpossibleOS/boot.conf \
               "$UKI_FIX_DIR/cmdline.txt" 2>/dev/null; then
        echo -e "${RED}SMOKE TEST FAILED: no boot.conf on the ESP to pack as .cmdline${NC}"
        exit 1
    fi
    if [ "$UKI_MODE" = "override" ]; then
        printf '%s\n' "$UKI_OVERRIDE" >> "$UKI_FIX_DIR/cmdline.txt"
    else
        # The received-digest log is opt-in (it costs boot time on a real
        # payload); the signed leg turns it on to prove the copy intact.
        printf 'uki_digest=1\n' >> "$UKI_FIX_DIR/cmdline.txt"
        # Deterministic content, so a copy that moved the wrong bytes would
        # not be masked by a coincidentally equal random block.
        python3 -c 'import sys; sys.stdout.buffer.write(bytes(i & 0xFF for i in range(int(sys.argv[1]))))' \
            "$UKI_INITRD_BYTES" > "$UKI_FIX_DIR/payloads/initrd.img"
    fi
    if ! bash "$REPO_ROOT/scripts/uki-pack.sh" "$BUILD/tools/BOOTX64.EFI" "$BUILD/kernel.exe" \
            "$UKI_FIX_DIR/cmdline.txt" "$UKI_FIX_DIR/payloads" "$UKI_FIX_DIR/BOOTX64.UKI.efi" \
            > "$UKI_FIX_DIR/pack.log" 2>&1; then
        echo -e "${RED}SMOKE TEST FAILED: UKI fixture pack failed${NC}"
        sed 's/^/    /' "$UKI_FIX_DIR/pack.log"
        exit 1
    fi
    mcopy -i "$DISK@@$EFI_OFFSET" -o "$UKI_FIX_DIR/BOOTX64.UKI.efi" ::EFI/BOOT/BOOTX64.EFI
    echo -e "  ${DIM}UKI fixture ($UKI_MODE) installed as EFI/BOOT/BOOTX64.EFI in $(basename "$DISK")${NC}"
fi

# The deliberate-fatal leg never reaches userland, so the normal end-to-end
# markers can never appear and most of the generic FAIL set is EXPECTED output
# ([CRIT] BOOT FATAL and the halt banner are what the leg exists to produce).
# It therefore carries its own oracle: unrelated faults still fail, and the
# assertions below are the ones only a correct compact render can satisfy.
if [ "$ERROR_SCREEN" = "1" ]; then
    PASS_PATTERNS_ALL=(
        "[BOOT] error-screen: rendered="
        "[BOOT] error_screen_test: halting"
    )
    FAIL_PATTERNS=(
        "KERNEL PANIC"
        "ASSERT FAILED"
        "triple fault"
        "General Protection Fault"
        "Page Fault"
        "Double Fault"
        "[CRIT] ExitBootServices failed"
        "[FAIL] Kernel ELF corrupt"
        "[FAIL] init_gop"
    )
fi

# The override leg must stop in the loader, so the end-to-end markers can never
# appear and its [CRIT] BOOT FATAL line is the expected output. Unrelated faults
# still fail it; the no-handoff assertions are in uki_check_override_log.
if [ "$UKI_MODE" = "override" ]; then
    PASS_PATTERNS_ALL=(
        "[CRIT] BOOT FATAL (0x00000013)"
    )
    FAIL_PATTERNS=(
        "KERNEL PANIC"
        "ASSERT FAILED"
        "triple fault"
        "General Protection Fault"
        "Page Fault"
        "Double Fault"
        "[BOOT] jumping to kernel_main"
    )
fi
# A valid store boots a tracked entry, and the kernel marks it good only once
# the boot is accepted, a moment after the prompt. Wait for that mark rather
# than trusting the grace period: the next boot of this machine consumes it.
if [ "$VALID_STORE" = "1" ] && [ "$ERROR_SCREEN" != "1" ] && [ -z "$UKI_MODE" ]; then
    PASS_PATTERNS_ALL+=("BOOT: health: entry marked good (id=")
fi

# Boot the canonical GPT image via AHCI (matches Makefile run: target).
QEMU_FLAGS=(
    -drive "if=pflash,format=raw,readonly=on,file=$OVMF_CODE"
    -drive "if=pflash,format=raw,file=$OVMF_VARS_CP"
    -drive "id=disk0,file=$DISK,format=raw,if=none"
    -device "ich9-ahci,id=ahci0"
    -device "ide-hd,drive=disk0,bus=ahci0.0"
    -m 2G
    # SMP (2026-07-28). There was NO -smp flag here, so QEMU defaulted to ONE
    # cpu and this gate had never exercised SMP -- in a project whose stated
    # doctrine is "SMP-safe by default ... SMP from day one" (CLAUDE.md).
    #
    # That is not a theoretical gap. The exec commit-point work changed the
    # TEB <-> kernel_gs_base handoff across context switches, shipped with
    # this test green, and then halted on the operator's 2-CPU WHPX boot:
    #   [CRIT] sched: ring-3 task 4 thread 0 has TEB but kernel_gs_base=0
    # The single-CPU run reaches the IDENTICAL point (PID 4, same PEB/TEB) and
    # survives, because the race needs a timer tick to land in the window
    # between publishing the TEB and assigning kernel_gs_base -- which a second
    # CPU supplies and a single CPU does not.
    #
    # 2 matches the operator's WHPX configuration, which is the setup that
    # actually caught this. Override with SMOKE_SMP for a wider sweep.
    -smp "${SMOKE_SMP:-2}"
    -serial file:"$SERIAL_LOG"
    -no-reboot
    -no-shutdown
    -display none
)
if [ "$MENU" = "keys" ] || [ "$MENU" = "default" ]; then
    rm -f "$MONITOR_SOCK"
    QEMU_FLAGS+=(-monitor "unix:$MONITOR_SOCK,server,nowait")
fi

# Use KVM if available
if [ -c /dev/kvm ] && [ -w /dev/kvm ] && [ -z "${SMOKE_FORCE_TCG:-}" ]; then
    QEMU_FLAGS+=(-enable-kvm -cpu host)
    echo -e "  ${DIM}KVM acceleration enabled${NC}"
elif [ -n "${SMOKE_FORCE_TCG:-}" ]; then
    # SMOKE_FORCE_TCG (2026-07-28). CLAUDE.md is explicit that TCG is not just
    # a device-emulation net: it "catches timing-sensitive races that KVM's
    # speed hides", and the 2026-07-27 SYS_EXEC frame-handoff bug survived 297
    # commits and a green local suite precisely because KVM always landed the
    # tick that TCG did not. There was no way to ask this gate for that engine,
    # so the cheaper-and-blinder one was always used when /dev/kvm existed.
    echo -e "  ${DIM}TCG forced (SMOKE_FORCE_TCG) -- slower, catches timing races KVM hides${NC}"
fi

# Clear previous log
> "$SERIAL_LOG"

# Launch QEMU in background
qemu-system-x86_64 "${QEMU_FLAGS[@]}" &
QEMU_PID=$!

# Cleanup trap: kill QEMU on normal exit, SIGINT, SIGTERM. Idempotent so the
# existing post-polling kill (if it still runs) is harmless.
cleanup_qemu() {
    local ec=$?
    if [ -n "${QEMU_PID:-}" ] && kill -0 "$QEMU_PID" 2>/dev/null; then
        kill "$QEMU_PID" 2>/dev/null || true
        wait "$QEMU_PID" 2>/dev/null || true
    fi
    exit "$ec"
}
trap cleanup_qemu EXIT INT TERM

# Wait for boot with timeout, checking serial log periodically
BOOT_PASSED=false
BOOT_FAILED=false
FAIL_REASON=""

for i in $(seq 1 "$TIMEOUT_SEC"); do
    sleep 1
    strip_ansi

    # Check if QEMU crashed
    if ! kill -0 "$QEMU_PID" 2>/dev/null; then
        BOOT_FAILED=true
        FAIL_REASON="QEMU exited unexpectedly"
        break
    fi

    # Check for fail patterns (match against stripped log)
    for pattern in "${FAIL_PATTERNS[@]}"; do
        if grep -qF -- "$pattern" "$STRIPPED_LOG" 2>/dev/null; then
            BOOT_FAILED=true
            FAIL_REASON="Detected: $pattern"
            break 2
        fi
    done

    # Keys leg: Down once the menu is up, then Enter only after the original
    # countdown deadline has passed with the menu still waiting.
    if [ "$MENU" = "keys" ]; then
        case "$KEYS_STATE" in
        wait_menu)
            if grep -qF '[BOOT] menu: 2 entries, selected="smoke-entry"' "$STRIPPED_LOG" 2>/dev/null; then
                if ! monitor_sendkey down; then
                    BOOT_FAILED=true; FAIL_REASON="QEMU monitor unreachable for sendkey down"; break
                fi
                KEYS_STATE=wait_move
            fi ;;
        wait_move)
            if grep -qF '[BOOT] menu: 2 entries, selected="smoke-alt"' "$STRIPPED_LOG" 2>/dev/null; then
                KEYS_MOVED_AT=$i
                KEYS_STATE=dwell
            fi ;;
        dwell)
            if grep -qF -e '[BOOT] menu: countdown expired' -e '[BOOT] jumping to kernel_main' "$STRIPPED_LOG" 2>/dev/null; then
                BOOT_FAILED=true; FAIL_REASON="the menu booted on its own after a keypress (countdown not cancelled)"; break
            fi
            if [ $((i - KEYS_MOVED_AT)) -ge "$MENU_KEYS_DWELL" ]; then
                if ! monitor_sendkey ret; then
                    BOOT_FAILED=true; FAIL_REASON="QEMU monitor unreachable for sendkey ret"; break
                fi
                KEYS_STATE=done
            fi ;;
        esac
    fi
    # Default leg: Down, help open and closed, d, t (the 4 s countdown becomes a
    # saved 5 s), e with "test=0" typed, then Enter boots the edit. Each key
    # waits for the serial line of the one before, so none races the menu.
    if [ "$MENU" = "default" ]; then
        step_key=""; step_next=""
        case "$KEYS_STATE" in
        wait_menu) grep -qF '[BOOT] menu: 2 entries, selected="smoke-entry"' "$STRIPPED_LOG" 2>/dev/null \
                       && { step_key=down; step_next=wait_move; } ;;
        wait_move) grep -qF '[BOOT] menu: 2 entries, selected="smoke-alt"' "$STRIPPED_LOG" 2>/dev/null \
                       && { step_key=h; step_next=wait_help; } ;;
        wait_help) grep -qF '[BOOT] menu: help shown' "$STRIPPED_LOG" 2>/dev/null \
                       && { step_key=x; step_next=wait_close; } ;;
        wait_close) grep -qF '[BOOT] menu: help closed' "$STRIPPED_LOG" 2>/dev/null \
                       && { step_key=d; step_next=wait_saved; } ;;
        wait_saved) grep -qF '[BOOT] menu: d -- saved default smoke-alt' "$STRIPPED_LOG" 2>/dev/null \
                       && { step_key=t; step_next=wait_timeout; } ;;
        wait_timeout) grep -qF '[BOOT] menu: t -- LoaderConfigTimeout 5s' "$STRIPPED_LOG" 2>/dev/null \
                       && { step_key=e; step_next=wait_edit; } ;;
        wait_edit) if grep -qF '[BOOT] menu: e -- editing smoke-alt' "$STRIPPED_LOG" 2>/dev/null; then
                       # Type "test=0": a known key at its smoke value, since an
                       # unknown boot key halts the kernel. The editor polls
                       # every 50 ms, so the keys are spaced.
                       for k in t e s t equal 0; do
                           monitor_sendkey "$k" || { BOOT_FAILED=true; FAIL_REASON="QEMU monitor unreachable for sendkey $k"; break 2; }
                           sleep 0.3
                       done
                       step_key=ret; step_next=done
                   fi ;;
        esac
        if [ -n "$step_key" ]; then
            if ! monitor_sendkey "$step_key"; then
                BOOT_FAILED=true; FAIL_REASON="QEMU monitor unreachable for sendkey $step_key"; break
            fi
            KEYS_STATE=$step_next
        fi
    fi

    # PASS requires ALL patterns in PASS_PATTERNS_ALL (AND-set, not OR)
    all_found=true
    for pattern in "${PASS_PATTERNS_ALL[@]}"; do
        if ! grep -qF -- "$pattern" "$STRIPPED_LOG" 2>/dev/null; then
            all_found=false
            break
        fi
    done
    if [ "$all_found" = true ]; then
        BOOT_PASSED=true
        break
    fi

    # Progress indicator
    printf "\r  ${DIM}Waiting... %d/${TIMEOUT_SEC}s${NC}  " "$i"
done
printf "\r"

# Grace period after PASS markers fire so post-Phase-3 disk flushes (FAT32
# sector cache writeback for postcode.log / hwdump.txt / firmware-advisor.json /
# firmware-tables.json) actually persist to the disk image.  Without this the
# kernel logs the writes but the FAT32 cache never flushes before QEMU dies,
# leaving the disk image's bytes unchanged so consumers like `mtype` see all
# zeros.  3 seconds matches the longest observed post-"Boot complete" write
# (hwdump.txt at +3.4s in the test=1 boot log).  Skip on FAIL since waiting
# longer for a broken boot wastes operator time.
if [ "$BOOT_PASSED" = true ] && [ -z "${SMOKE_NO_GRACE:-}" ]; then
    GRACE_SEC="${SMOKE_GRACE_SEC:-3}"
    printf "  ${DIM}PASS markers detected; waiting %ds for disk flushes...${NC}  " "$GRACE_SEC"
    sleep "$GRACE_SEC"
    strip_ansi
    printf "\r"
fi

# ---- S23 deliberate-fatal verdict (SMOKE_ERROR_SCREEN=1) -------------------
# Runs BEFORE every later layer, because those layers assert an end-to-end boot
# this leg deliberately does not perform. Each assertion below is one a broken
# compact path fails: the rendered= tier is the bootloader's own report of which
# layout it painted, qr= is whether the recovery QR survived the compact fill
# that happens immediately before it, and the tier= capability line proves the
# forced sub-floor mode actually took rather than the fixture being ignored.
if [ "$ERROR_SCREEN" = "1" ]; then
    strip_ansi
    es_fail() {
        echo -e "${RED}SMOKE TEST FAILED (error-screen leg): $1${NC}"
        grep -E "error-screen:|error_screen_test|GOP: " "$STRIPPED_LOG" 2>/dev/null \
            | sed 's/^/    /' | head -12
        exit 1
    }
    [ "$BOOT_FAILED" = true ] && es_fail "unrelated fault: $FAIL_REASON"
    es_check_log "$STRIPPED_LOG" || es_fail "$ES_FAIL_MSG"
    echo -e "  ${GREEN}✓${NC} $ES_TIER_LINE"
    echo -e "  ${GREEN}✓${NC} $ES_RENDERED"
    echo ""
    echo -e "${GREEN}  ERROR-SCREEN LEG PASSED${NC}"
    echo -e "${DIM}  smoke receipt not recorded (fixture image)${NC}"
    exit 0
fi

# ---- UKI override verdict (SMOKE_UKI=override) ----------------------------
# Runs before the end-to-end layers, which assert a boot this leg must not
# perform. The grace period above already let any late handoff reach the log.
if [ "$UKI_MODE" = "override" ]; then
    strip_ansi
    [ "$BOOT_FAILED" = true ] && { echo -e "${RED}SMOKE TEST FAILED (UKI override leg): $FAIL_REASON${NC}"; exit 1; }
    if [ "$BOOT_PASSED" != true ] || ! uki_check_override_log "$STRIPPED_LOG" "$UKI_OVERRIDE"; then
        echo -e "${RED}SMOKE TEST FAILED (UKI override leg): ${UKI_FAIL_MSG:-no refusal within ${TIMEOUT_SEC}s}${NC}"
        grep -E "UKI|FATAL|jumping" "$STRIPPED_LOG" 2>/dev/null | sed 's/^/    /' | head -12
        exit 1
    fi
    echo -e "  ${GREEN}✓${NC} $UKI_FATAL_LINE"
    echo -e "  ${GREEN}✓${NC} no kernel handoff"
    echo ""
    echo -e "${GREEN}  UKI OVERRIDE LEG PASSED${NC} (token: $UKI_OVERRIDE)"
    echo -e "${DIM}  smoke receipt not recorded (fixture image)${NC}"
    exit 0
fi

# ---- SURVIVE-PAST-THE-PROMPT re-check (2026-07-28) -------------------------
# The poll loop BREAKS the instant PASS_PATTERNS_ALL match, so anything fatal
# after that instant was never examined. That is not a hypothetical: on the
# WHPX boot of main@238c49f6 the markers landed at 22.6s and 24.6s and the
# kernel halted at 25.2s -- INSIDE the grace window above -- and this test
# reported PASS. `cmd.exe` is the first ring-3 process, so a crash there is
# precisely the "does it reach userspace" failure this test exists to catch.
#
# Re-checking the fail patterns after the grace period costs nothing (the wait
# already happened for disk flushes) and converts the grace window from a blind
# spot into the survival assertion. SMOKE_NO_GRACE skips the wait, so re-check
# regardless of whether the wait ran.
if [ "$BOOT_PASSED" = true ]; then
    strip_ansi
    for pattern in "${FAIL_PATTERNS[@]}"; do
        if grep -qF -- "$pattern" "$STRIPPED_LOG" 2>/dev/null; then
            BOOT_PASSED=false
            BOOT_FAILED=true
            FAIL_REASON="Detected AFTER pass markers (crash past the prompt): $pattern"
            break
        fi
    done
fi

# ---- LOG MUST END CLEAN (2026-07-28) ---------------------------------------
# FAIL_PATTERNS is a denylist and therefore only ever catches fatal shapes
# somebody already enumerated -- which is exactly how the kernel_gs_base halt
# got through. This check is shape-based instead: whatever the kernel was
# saying as it stopped, it must not have been stopping. It costs one tail read
# and catches fatal shapes nobody has named yet.
if [ "$BOOT_PASSED" = true ]; then
    LAST_LINES="$(tail -5 "$STRIPPED_LOG" 2>/dev/null || true)"
    if printf '%s' "$LAST_LINES" | grep -qiE 'halt|panic|fatal|unrecoverable|triple fault|\[\*\*\]'; then
        BOOT_PASSED=false
        BOOT_FAILED=true
        FAIL_REASON="Log ends on a fatal shape (last 5 lines): $(printf '%s' "$LAST_LINES" | grep -iE 'halt|panic|fatal|unrecoverable|triple fault|\[\*\*\]' | head -1 | cut -c1-100)"
    fi
fi

# ---- Forced-degradation verdict (SMOKE_DEGRADE=rtc) -------------------------
# The boot has already passed every normal marker; this leg additionally needs
# the injected failure to have been recorded, not silently absorbed.
if [ -n "$DEGRADE" ] && [ "$BOOT_PASSED" = true ]; then
    strip_ansi
    DEG_UP="$(printf '%s' "$DEGRADE" | tr '[:lower:]' '[:upper:]')"
    if ! grep -qF "test_fail_${DEGRADE}=1" "$STRIPPED_LOG" 2>/dev/null; then
        BOOT_PASSED=false; BOOT_FAILED=true
        FAIL_REASON="SMOKE_DEGRADE=$DEGRADE: the injection never fired (cmdline fixture not applied?)"
    elif ! grep -qF "${DEG_UP}: init failed -- degraded" "$STRIPPED_LOG" 2>/dev/null; then
        BOOT_PASSED=false; BOOT_FAILED=true
        FAIL_REASON="SMOKE_DEGRADE=$DEGRADE: no '${DEG_UP}: init failed -- degraded' line"
    elif ! grep -aE "Degraded subsystems \([0-9]+\): .*\b${DEG_UP}\b" "$STRIPPED_LOG" >/dev/null 2>&1; then
        BOOT_PASSED=false; BOOT_FAILED=true
        FAIL_REASON="SMOKE_DEGRADE=$DEGRADE: end-of-boot degraded summary does not name ${DEG_UP}"
    elif [ "$DEGRADE" = "fb" ] && ! grep -qF "vpd: display refused -- POST progress display retired" "$STRIPPED_LOG" 2>/dev/null; then
        BOOT_PASSED=false; BOOT_FAILED=true
        FAIL_REASON="SMOKE_DEGRADE=fb: VPD was not retired after the refusal (it still holds the rejected layout)"
    elif [ "$DEGRADE" = "fb" ] && ! grep -qF "test_fail_fb: refused display untouched since refusal" "$STRIPPED_LOG" 2>/dev/null; then
        BOOT_PASSED=false; BOOT_FAILED=true
        FAIL_REASON="SMOKE_DEGRADE=fb: VRAM changed after the refusal, or the driver did not read as an empty surface"
    else
        echo -e "  ${GREEN}✓${NC} forced ${DEG_UP} failure recorded and boot still reached the prompt"
    fi
fi

# Kill QEMU (trap also does this; harmless repeat)
kill "$QEMU_PID" 2>/dev/null || true
wait "$QEMU_PID" 2>/dev/null || true

# Final strip in case the loop exited before the last pass
strip_ansi

# ---- Step 3: Results ----
echo -e "${CYAN}[3/3]${NC} Analyzing results..."
echo ""

LOG_LINES=$(wc -l < "$SERIAL_LOG" 2>/dev/null || echo 0)
echo -e "  ${DIM}Serial log: $SERIAL_LOG ($LOG_LINES lines; ANSI-stripped: $STRIPPED_LOG)${NC}"

# ---- Layer 1 (CORE): POST16 code assertions from manifest ----
# Manifest was already validated non-empty and length-consistent in Step 1;
# this loop just checks serial output.
PATTERN_FAIL=false
POST16_MISSING_COUNT=0
# A UKI-direct boot takes a different, equally exact path: the cmdline comes
# from the signed .cmdline section, so the split-path boot.conf filesystem step
# (BOOT_FS / BOOT_FS_OK) never runs and the UKI probe (UKI_DETECT / _OK) must
# run instead. SELF_MEASURE_OK is required exactly when the self-measure oracle
# below expects a digest (an image within the loader's size cap).
UKI_POST16_SKIP=" POST16_BL_BOOT_FS POST16_BL_BOOT_FS_OK "
if [ -n "$UKI_MODE" ]; then
    for uki_req in POST16_BL_UKI_DETECT:0xB0A0 POST16_BL_UKI_DETECT_OK:0xB0A1; do
        if ! grep -qE -- "^${uki_req%%:*}=${uki_req#*:}\$" "$POST16_MANIFEST"; then
            echo -e "  ${RED}MISSING:${NC} manifest no longer defines ${uki_req%%:*} as ${uki_req#*:} (update this leg)"
            PATTERN_FAIL=true
        elif ! grep -qF -- "[BOOT] POST ${uki_req#*:}" "$STRIPPED_LOG" 2>/dev/null; then
            echo -e "  ${RED}MISSING:${NC} POST16 ${uki_req#*:} (${uki_req%%:*})"
            PATTERN_FAIL=true
            POST16_MISSING_COUNT=$((POST16_MISSING_COUNT + 1))
        fi
    done
    if [ "$(wc -c < "$UKI_FIX_DIR/BOOTX64.UKI.efi" | tr -d ' ')" -gt "$SELF_MEASURE_CAP_BYTES" ]; then
        UKI_POST16_SKIP="$UKI_POST16_SKIP POST16_BL_SELF_MEASURE_OK "
    fi
fi
for i in "${!POST16_REQUIRED[@]}"; do
    name="${POST16_REQUIRED[$i]}"
    code="${POST16_REQUIRED_CODES[$i]}"
    if [ -n "$UKI_MODE" ] && [[ "$UKI_POST16_SKIP" == *" $name "* ]]; then
        continue
    fi
    # Match the bootloader's exact serial format: "[BOOT] POST 0xNNNN"
    # with uppercase hex. Fixed-string match -- the brackets are literal.
    if ! grep -qF -- "[BOOT] POST $code" "$STRIPPED_LOG" 2>/dev/null; then
        echo -e "  ${RED}MISSING:${NC} POST16 $code ($name)"
        PATTERN_FAIL=true
        POST16_MISSING_COUNT=$((POST16_MISSING_COUNT + 1))
    fi
done
echo -e "  ${DIM}POST16 core: ${#POST16_REQUIRED[@]} required, $POST16_MISSING_COUNT missing${NC}"

# ---- Layer 2 (RESIDUAL): user-visible string signals ----
for pattern in "${BOOT_REQUIRED_STRING_SIGNALS[@]}"; do
    if ! grep -qF -- "$pattern" "$STRIPPED_LOG" 2>/dev/null; then
        echo -e "  ${RED}MISSING:${NC} $pattern"
        PATTERN_FAIL=true
    fi
done

# ---- Fallback layer: advisory only, does NOT fail the smoke test ----
# Kept until POST16 core is proven on KVM + TCG + VBox + bare metal.
FALLBACK_MISS_COUNT=0
for pattern in "${BOOT_FALLBACK_STRINGS[@]}"; do
    if ! grep -qF -- "$pattern" "$STRIPPED_LOG" 2>/dev/null; then
        echo -e "  ${YELLOW}fallback-miss:${NC} $pattern (advisory, not failing)"
        FALLBACK_MISS_COUNT=$((FALLBACK_MISS_COUNT + 1))
    fi
done

# ---- Absent patterns: must NOT appear ----
for pattern in "${BOOT_ABSENT_PATTERNS[@]}"; do
    if grep -qF -- "$pattern" "$STRIPPED_LOG" 2>/dev/null; then
        echo -e "  ${RED}UNEXPECTED:${NC} $pattern"
        PATTERN_FAIL=true
    fi
done

if [ "$PATTERN_FAIL" = true ]; then
    BOOT_FAILED=true
    # Keep a reason the poll loop already set: it names the first failure, and
    # the missing markers are usually just its consequence.
    FAIL_REASON="${FAIL_REASON:-Boot pattern check failed (see MISSING/UNEXPECTED above)}"
fi

# ---- UKI signed-payload verdict (SMOKE_UKI=signed) ------------------------
if [ "$UKI_MODE" = "signed" ]; then
    if uki_check_signed_log "$STRIPPED_LOG"; then
        echo -e "  ${GREEN}✓${NC} $UKI_LOADER_LINE"
        echo -e "  ${GREEN}✓${NC} ${UKI_KERNEL_LINE}"
    else
        echo -e "  ${RED}MISSING:${NC} $UKI_FAIL_MSG"
        BOOT_FAILED=true
        FAIL_REASON="${FAIL_REASON:-UKI signed payload: $UKI_FAIL_MSG}"
    fi
fi

# First line of a file containing a fixed string, or nothing. A bare
# `grep | head` inside $(...) exits 1 on no match, which under `set -euo
# pipefail` ends this script with no message -- exactly when a boot omitted the
# line the next branch exists to report as MISSING. awk exits 0 on no match and
# still fails (nonzero) on an unreadable file.
first_line_with() {
    awk -v k="$1" 'index($0, k) { print; exit }' "$2"
}

# ---- Loader self-measurement oracle ---------------------------------------
#
# The loader hashes the ESP file it was launched from and prints the digest.
# Comparing legs against each other would prove only DETERMINISM: four equal
# outputs are equally consistent with hashing the wrong file, a prefix, or a
# constant. So the check here is against an INDEPENDENTLY computed hash of the
# exact loader binary this build staged, plus its byte count -- which a
# prefix-only read or a wrong-file open cannot satisfy.
#
# Which file: the Makefile stages $(UEFI_EFI_SIGNED) when a MOK key exists and
# build/tools/BOOTX64.EFI otherwise, and under the shim chain-load layout that
# same binary is staged as grubx64.efi. In every layout it is the file the
# running loader was launched from, so one variable covers all three.
SELF_MEASURE_LINE="$(first_line_with '[BOOT] self-measure:' "$STRIPPED_LOG")"
if [ -z "$SELF_MEASURE_LINE" ]; then
    echo -e "  ${RED}MISSING:${NC} [BOOT] self-measure: line absent from serial"
    BOOT_FAILED=true
    FAIL_REASON="${FAIL_REASON:-Loader self-measurement did not report}"
else
    # MOK_KEY mirrors the Makefile default and honours the same override, so an
    # externally keyed build does not hash the unsigned loader while the disk
    # staged the signed one.
    SMOKE_MOK_KEY="${MOK_KEY:-keys/MOK.key}"
    LOADER_FILE="$BUILD/tools/BOOTX64.EFI"
    if [ -f "$SMOKE_MOK_KEY" ] && [ -f "$BUILD/tools/BOOTX64.signed.efi" ]; then
        LOADER_FILE="$BUILD/tools/BOOTX64.signed.efi"
    fi
    # A UKI leg launched the fixture UKI, not the staged loader.
    if [ -n "$UKI_MODE" ]; then
        LOADER_FILE="$UKI_FIX_DIR/BOOTX64.UKI.efi"
    fi
    # Validate the WHOLE line against the emitted schema before extracting
    # anything: field order, decimal byte count, 16 uppercase TSC digits and 64
    # uppercase digest digits, in that exact order. Matching fields "anywhere on
    # the line" would let the producer/consumer format drift silently.
    # The digest token is esp-file-sha256, not a bare sha256: the loader hashes
    # the ESP FILE, never the bytes firmware executed (that measurement is the
    # firmware's own Authenticode PE hash in PCR 4). The anchored schema is what
    # makes the rename real -- a producer left on the old token fails here on
    # every leg rather than quietly reverting the claim.
    SELF_MEASURE_SCHEMA='^\[BOOT\] self-measure: status=ok bytes=[0-9]{1,20} tsc=[0-9A-F]{16} esp-file-sha256=[0-9A-F]{64}$'
    SELF_MEASURE_BODY="$(printf '%s' "$SELF_MEASURE_LINE" | sed 's/\r$//')"
    if ! grep -qF -- '#define SELF_MEASURE_SIZE_CAP   (8u * 1024u * 1024u)' "$REPO_ROOT/src/boot/uefi/bootx64.c"; then
        echo -e "  ${RED}MISSING:${NC} SELF_MEASURE_SIZE_CAP changed in bootx64.c; update SELF_MEASURE_CAP_BYTES here"
        BOOT_FAILED=true
        FAIL_REASON="${FAIL_REASON:-self-measure cap copy drifted from the loader}"
    fi
    if [ ! -f "$LOADER_FILE" ]; then
        echo -e "  ${YELLOW}self-measure:${NC} $LOADER_FILE absent -- oracle skipped (advisory)"
    elif [ "$(wc -c < "$LOADER_FILE" | tr -d ' ')" -gt "$SELF_MEASURE_CAP_BYTES" ]; then
        # Above the cap the contract is a refusal, never a digest (a digest of
        # an oversize image would mean the cap was bypassed or a prefix hashed).
        if [ "$SELF_MEASURE_BODY" = "[BOOT] self-measure: status=over-size-cap digest=ABSENT" ]; then
            echo -e "  ${GREEN}✓${NC} self-measure refused the $(wc -c < "$LOADER_FILE" | tr -d ' ')-byte image (over the ${SELF_MEASURE_CAP_BYTES}-byte cap, as contracted)"
        else
            echo -e "  ${RED}MISMATCH:${NC} image is over the self-measure cap but the loader reported: $SELF_MEASURE_BODY"
            BOOT_FAILED=true
            FAIL_REASON="${FAIL_REASON:-Loader self-measurement ignored its size cap}"
        fi
    elif ! printf '%s' "$SELF_MEASURE_BODY" | grep -qE -- "$SELF_MEASURE_SCHEMA"; then
        echo -e "  ${RED}MISSING:${NC} self-measure line is not a schema-valid success: $SELF_MEASURE_BODY"
        BOOT_FAILED=true
        FAIL_REASON="${FAIL_REASON:-Loader self-measurement reported ABSENT or off-schema}"
    else
        REPORTED_SHA="$(printf '%s' "$SELF_MEASURE_BODY" | sed -n 's/.* esp-file-sha256=\([0-9A-F]\{64\}\)$/\1/p' | tr 'A-F' 'a-f')"
        REPORTED_BYTES="$(printf '%s' "$SELF_MEASURE_BODY" | sed -n 's/.* bytes=\([0-9]\{1,20\}\) .*/\1/p')"
        ORACLE_SHA="$(sha256sum "$LOADER_FILE" | cut -d' ' -f1)"
        ORACLE_BYTES="$(wc -c < "$LOADER_FILE" | tr -d ' ')"
        if [ "$REPORTED_SHA" = "$ORACLE_SHA" ] && [ "$REPORTED_BYTES" = "$ORACLE_BYTES" ]; then
            echo -e "  ${GREEN}✓${NC} self-measure matches $LOADER_FILE ($ORACLE_BYTES bytes)"
        else
            echo -e "  ${RED}MISMATCH:${NC} self-measure ${REPORTED_SHA:0:16}.../${REPORTED_BYTES} B"
            echo -e "  ${DIM}  oracle  ${ORACLE_SHA:0:16}.../${ORACLE_BYTES} B ($LOADER_FILE)${NC}"
            BOOT_FAILED=true
            FAIL_REASON="${FAIL_REASON:-Loader self-measurement does not match the staged binary}"
        fi
    fi

    # ---- PCR 4 correlation verdict -----------------------------------------
    # The correlation is the section's actual deliverable and was the one line
    # of the three with no schema behind it -- omission, not intent. Anchor it,
    # because the failure this guards is not a wrong digest but a SILENT one:
    # if correlate_pcr4_measurement stopped running, or started reporting an
    # outcome nobody enumerated, every other assertion here would still pass.
    #
    # The verdict itself is deliberately NOT constrained to a single value. A
    # dev-host boot has no TPM and correctly reports no-event-log; a machine
    # with one reports a match or a named absence. What IS constrained is that
    # the line exists, names an enumerated outcome, and carries its cost. A
    # bare AGREE is rejected on purpose: nothing on this path reads or replays
    # a PCR, so that word must not appear.
    PCR4_LINE="$(first_line_with '[BOOT] pcr4-correlate:' "$STRIPPED_LOG")"
    PCR4_BODY="$(printf '%s' "$PCR4_LINE" | sed 's/\r$//')"
    PCR4_SCHEMA='^\[BOOT\] pcr4-correlate: result=(UNAUTHENTICATED-LOG-MATCH|DISAGREE|no-local-digest|no-image-device-path|no-devpath-utilities|degenerate-device-path|no-event-log|log-unusable|no-comparison)([ ][a-z0-9-]+=[0-9A-Za-z-]+)* tsc=[0-9A-F]{16}$'
    if [ -z "$PCR4_LINE" ]; then
        echo -e "  ${RED}MISSING:${NC} [BOOT] pcr4-correlate: line absent from serial"
        BOOT_FAILED=true
        FAIL_REASON="${FAIL_REASON:-PCR 4 correlation did not report}"
    elif ! printf '%s' "$PCR4_BODY" | grep -qE -- "$PCR4_SCHEMA"; then
        echo -e "  ${RED}MISSING:${NC} pcr4-correlate line is not a schema-valid outcome: $PCR4_BODY"
        BOOT_FAILED=true
        FAIL_REASON="${FAIL_REASON:-PCR 4 correlation reported an unenumerated outcome}"
    else
        echo -e "  ${GREEN}OK${NC} pcr4-correlate: $(printf '%s' "$PCR4_BODY" | sed -n 's/.*result=\([A-Za-z-]*\).*/\1/p')"
    fi

    # ---- Authenticode PE digest oracle -------------------------------------
    # The flat digest above says which FILE was hashed. This one says whether
    # that file could be what firmware executed: it is the Authenticode PE hash,
    # the only digest a PCR 4 EV_EFI_BOOT_SERVICES_APPLICATION measurement can
    # ever equal. Cross-checked against an INDEPENDENT transcription of the
    # spec's numbered steps, so agreement here is the loader agreeing with the
    # spec as read by someone else -- not with itself. A flat hash cannot
    # satisfy this, which is the point.
    SELF_MEASURE_PE_LINE="$(first_line_with '[BOOT] self-measure-pe:' "$STRIPPED_LOG")"
    SELF_MEASURE_PE_BODY="$(printf '%s' "$SELF_MEASURE_PE_LINE" | sed 's/\r$//')"
    SELF_MEASURE_PE_SCHEMA='^\[BOOT\] self-measure-pe: status=ok pe-authenticode-sha256=[0-9A-F]{64}$'
    if [ ! -f "$LOADER_FILE" ]; then
        : # already reported advisory-skipped above
    elif [ "$(wc -c < "$LOADER_FILE" | tr -d ' ')" -gt "$SELF_MEASURE_CAP_BYTES" ]; then
        # The PE line follows a successful flat measurement only, so a refused
        # image must carry none: one would mean the PE hash ran on the image
        # the cap refused.
        if [ -n "$SELF_MEASURE_PE_LINE" ]; then
            echo -e "  ${RED}MISMATCH:${NC} over-cap image still reported: $SELF_MEASURE_PE_BODY"
            BOOT_FAILED=true
            FAIL_REASON="${FAIL_REASON:-Authenticode measurement ran on an image over the cap}"
        else
            echo -e "  ${GREEN}✓${NC} no Authenticode line for the refused image"
        fi
    elif [ -z "$SELF_MEASURE_PE_LINE" ]; then
        echo -e "  ${RED}MISSING:${NC} [BOOT] self-measure-pe: line absent from serial"
        BOOT_FAILED=true
        FAIL_REASON="${FAIL_REASON:-Loader Authenticode measurement did not report}"
    elif ! printf '%s' "$SELF_MEASURE_PE_BODY" | grep -qE -- "$SELF_MEASURE_PE_SCHEMA"; then
        echo -e "  ${RED}MISSING:${NC} self-measure-pe line is not a schema-valid success: $SELF_MEASURE_PE_BODY"
        BOOT_FAILED=true
        FAIL_REASON="${FAIL_REASON:-Loader Authenticode measurement reported ABSENT or off-schema}"
    else
        REPORTED_PE_SHA="$(printf '%s' "$SELF_MEASURE_PE_BODY" \
            | sed -n 's/.* pe-authenticode-sha256=\([0-9A-F]\{64\}\)$/\1/p' | tr 'A-F' 'a-f')"
        ORACLE_PE_SHA="$(python3 "$REPO_ROOT/tools/boot-header-tests/authenticode_oracle.py" \
            "$LOADER_FILE" 2>/dev/null || true)"
        if [ -z "$ORACLE_PE_SHA" ]; then
            echo -e "  ${YELLOW}self-measure-pe:${NC} oracle unavailable -- comparison skipped (advisory)"
        elif [ "$REPORTED_PE_SHA" = "$ORACLE_PE_SHA" ]; then
            echo -e "  ${GREEN}✓${NC} self-measure-pe matches the independent Authenticode oracle"
        else
            echo -e "  ${RED}MISMATCH:${NC} self-measure-pe ${REPORTED_PE_SHA:0:16}..."
            echo -e "  ${DIM}  oracle      ${ORACLE_PE_SHA:0:16}... ($LOADER_FILE)${NC}"
            BOOT_FAILED=true
            FAIL_REASON="${FAIL_REASON:-Loader Authenticode digest does not match the independent oracle}"
        fi
    fi
fi

# ---- Log cleanliness: ANY unexpected [FAIL] fails the run -------------------
#
# FAIL_PATTERNS above is a nine-string allowlist of FATAL signatures. It is an
# early abort, not a verdict: a [FAIL] outside those nine passed straight
# through, so this script would print SMOKE TEST PASSED over a log that
# contained one. Demonstrated 2026-07-27 -- a local run reported PASS with
# "[FAIL] BOOT-BUDGET: EXEC -> DESKTOP_READY took 1139ms" and 32 [WARN] lines in
# its own output. That is the specific reason smoke testing after each TODO
# section did not catch what a full native run later surfaced.
#
# So the verdict is inverted here: every [FAIL] counts UNLESS it is baselined.
# The baseline is shared with the kernel-runner cleanliness gate so the two
# cannot drift, and every entry there carries a reason.
#
# WARN is deliberately NOT gated yet. A clean run still emits ~30 of them, and
# gating before the expected-output declaration lands (the test-side half of
# this work) would mean baselining thirty correct lines -- which is how a
# baseline turns into a rubber stamp. FAIL is the tight, high-signal half.
BASELINE_FILE="$REPO_ROOT/scripts/log-baseline.txt"
UNEXPECTED_FAILS=""
if [ -f "$STRIPPED_LOG" ]; then
    while IFS= read -r fail_line; do
        [ -n "$fail_line" ] || continue
        is_baselined=false
        if [ -f "$BASELINE_FILE" ]; then
            while IFS= read -r entry; do
                case "$entry" in ''|'#'*) continue ;; esac
                case "$fail_line" in *"$entry"*) is_baselined=true; break ;; esac
            done < "$BASELINE_FILE"
        fi
        # A forced-degradation leg expects exactly the refusal it injected;
        # that one line is the leg's evidence, not noise, and is excused for
        # this leg only (never baselined, so a real refusal still fails).
        if [ "$is_baselined" = false ] && [ "$DEGRADE" = "fb" ]; then
            case "$fail_line" in *"gfx: Unsupported pixel format 255"*) is_baselined=true ;; esac
        fi
        if [ "$is_baselined" = false ]; then
            UNEXPECTED_FAILS="${UNEXPECTED_FAILS}${fail_line}"$'\n'
        fi
    done < <(grep -F '[FAIL]' "$STRIPPED_LOG" 2>/dev/null || true)
fi

if [ -n "$UNEXPECTED_FAILS" ]; then
    UNEXPECTED_COUNT=$(printf '%s' "$UNEXPECTED_FAILS" | grep -c . || true)
    echo -e "  ${RED}UNEXPECTED [FAIL] lines: $UNEXPECTED_COUNT${NC}"
    printf '%s' "$UNEXPECTED_FAILS" | head -10 | sed 's/^/    /'
    echo -e "  ${DIM}If one is correct, add it to scripts/log-baseline.txt WITH a reason.${NC}"
    BOOT_FAILED=true
    FAIL_REASON="${FAIL_REASON:-Unexpected [FAIL] in serial log ($UNEXPECTED_COUNT line(s))}"
else
    echo -e "  ${DIM}Log cleanliness: no unexpected [FAIL] lines${NC}"
fi

if [ "$BOOT_FAILED" = true ]; then
    echo ""
    echo -e "${RED}══════════════════════════════════════════════════${NC}"
    echo -e "${RED}  SMOKE TEST FAILED: $FAIL_REASON${NC}"
    echo -e "${RED}══════════════════════════════════════════════════${NC}"
    echo ""
    # Show relevant log lines
    echo -e "${DIM}Last 10 lines of serial output:${NC}"
    tail -10 "$STRIPPED_LOG" 2>/dev/null | sed 's/^/  /'
    echo ""
    exit 1
elif [ "$BOOT_PASSED" = true ]; then
    # Extract boot time
    BOOT_TIME=$(grep -o "Boot complete in [0-9.]*s" "$SERIAL_LOG" 2>/dev/null | head -1 || echo "")

    # ---- Boot Loader Interface variables ---------------------------------
    # Every boot sets or clears each variable it owns (include/boot/loader_vars.h);
    # a firmware refusal is a failure, not noise. The one-line summary comes from
    # the shipped loader image, which the kernel suite cannot reach.
    # boot_policy_invoke() runs on every boot (UKI-direct included), so a leg
    # that reached the prompt without the line skipped publication entirely.
    LV_LINE=$(grep -aoE '\[BOOT\] loader-vars: published [0-9]+, cleared [0-9]+, failed [0-9]+' "$STRIPPED_LOG" 2>/dev/null | head -1)
    if [ -z "$LV_LINE" ]; then
        echo -e "${RED}SMOKE TEST FAILED: the loader never reported its Boot Loader Interface variables${NC}"
        exit 1
    fi
    read -r LV_PUB LV_CLR LV_FAIL <<< "$(echo "$LV_LINE" | sed -E 's/.*published ([0-9]+), cleared ([0-9]+), failed ([0-9]+)/\1 \2 \3/')"
    if [ "$LV_FAIL" != "0" ] || [ $((LV_PUB + LV_CLR)) -ne 11 ]; then
        echo -e "${RED}SMOKE TEST FAILED: every loader variable must be set or cleared, with no firmware refusal (got: $LV_LINE)${NC}"
        exit 1
    fi
    # The two timing variables are set again just before the final memory map,
    # once the TSC is calibrated. Init and Exec are both counted from reset, so a
    # leg with a calibrated TSC sets both; an uncalibrated one clears both.
    LV_TIME=$(grep -aoE '\[BOOT\] loader-vars: timing, published [0-9]+, cleared [0-9]+, failed [0-9]+' "$STRIPPED_LOG" 2>/dev/null | head -1)
    if [ -z "$LV_TIME" ]; then
        echo -e "${RED}SMOKE TEST FAILED: the loader never reported its timing variables${NC}"
        exit 1
    fi
    read -r LT_PUB LT_CLR LT_FAIL <<< "$(echo "$LV_TIME" | sed -E 's/.*published ([0-9]+), cleared ([0-9]+), failed ([0-9]+)/\1 \2 \3/')"
    if [ "$LT_FAIL" != "0" ] || [ $((LT_PUB + LT_CLR)) -ne 2 ]; then
        echo -e "${RED}SMOKE TEST FAILED: both timing variables must be set or cleared, with no firmware refusal (got: $LV_TIME)${NC}"
        exit 1
    fi
    LT_WANT=2
    if grep -aq 'TSC calibration rejected' "$STRIPPED_LOG" 2>/dev/null; then LT_WANT=0; fi
    if [ "$LT_PUB" != "$LT_WANT" ]; then
        echo -e "${RED}SMOKE TEST FAILED: timing must be all set with a calibrated TSC and all cleared without one (want $LT_WANT published, got: $LV_TIME)${NC}"
        exit 1
    fi

    # ---- Valid-store assertions -------------------------------------------
    if [ "$VALID_STORE" = "1" ]; then
        if ! grep -q "policy: bootentries.json parsed reject_code=0 entries=$STORE_ENTRIES" "$STRIPPED_LOG" 2>/dev/null; then
            echo -e "${RED}SMOKE TEST FAILED: a valid boot entry store was not accepted${NC}"
            grep -E "policy:|store-reject:" "$STRIPPED_LOG" 2>/dev/null | head -6
            exit 1
        fi
        # The policy, entry-kind, sticky and history code runs only in the loader
        # image; the kernel suite tests a different build of the same files, so
        # these lines are the only proof the shipped copies work.
        loader_fail() {
            echo -e "${RED}SMOKE TEST FAILED: $1${NC}"
            grep -aE "policy:|boot_sticky|history:|boot_history:" "$STRIPPED_LOG" 2>/dev/null | head -10
            exit 1
        }
        # Every fixture image starts with no last good copy, so an accepted store
        # writes one: staging copy, then publication, then the settled state.
        grep -qF '[BOOT] store-shadow: last good copy saved' "$STRIPPED_LOG" 2>/dev/null \
            || loader_fail "an accepted store did not save its last good copy"
        if [ "$SHADOW_STORE" = "empty" ]; then
            grep -qF '[BOOT] policy: bootentries.lkg.tmp is empty' "$STRIPPED_LOG" 2>/dev/null \
                || loader_fail "the empty staging copy was not read"
            if grep -qF 'store-shadow: a copy could not be read' "$STRIPPED_LOG" 2>/dev/null; then
                loader_fail "an empty staging copy was treated as unreadable and held"
            fi
        fi
        grep -qF '[BOOT] policy: kind validate OK kind=0 kernel="\boot\kernel.exe"' "$STRIPPED_LOG" 2>/dev/null \
            || loader_fail "boot_entry_kind did not decode the split entry's kernel path"
        # A valid store and a GPT ESP make every variable knowable except the two
        # timings (the TSC is calibrated after this point): 9 set, 2 cleared.
        grep -qF '[BOOT] loader-vars: published 9, cleared 2, failed 0' "$STRIPPED_LOG" 2>/dev/null \
            || loader_fail "the loader did not publish the 9 Boot Loader Interface variables a valid store allows"
        # The keys leg overrides the policy default from the menu, and the
        # policy line prints after the menu, so it reports the operator's pick.
        # OVMF booted this image through its own Boot#### option, which carries no
        # entry tag, so the policy must fall through it to the store's default:
        # the firmware-vs-OS layer separation, end to end.
        grep -qaE '\[BOOT\] BootCurrent=0x[0-9A-Fa-f]{4}' "$STRIPPED_LOG" 2>/dev/null \
            || loader_fail "the loader did not read BootCurrent, so the fall-through is unproven"
        sel_entry=smoke-entry; sel_reason='1 (STORE_DEFAULT)'
        case "$MENU" in
        keys|default) sel_entry=smoke-alt; sel_reason='3 (HOTKEY)' ;;
        saved) sel_entry=smoke-alt ;;
        esac
        # rejected=0: a firmware option tagged for an entry the store lacks would be
        # recorded as a reject even when the fall-through still lands on the default.
        grep -qF "[BOOT] policy: selection_reason=$sel_reason selected=\"$sel_entry\" kind=0 rejected=0" \
            "$STRIPPED_LOG" 2>/dev/null \
            || loader_fail "boot_policy did not report selection_reason=$sel_reason for $sel_entry with no reject"
        grep -qF "BOOT: health: entry marked good (id=$sel_entry " "$STRIPPED_LOG" 2>/dev/null \
            || loader_fail "the kernel did not mark the booted entry $sel_entry good"
        # After the handoff the kernel reads LoaderInfo and LoaderEntrySelected
        # back from the firmware; both must agree with what boot_info records.
        grep -qF "loader-vars: readback matches boot_info (selected=$sel_entry)" "$STRIPPED_LOG" 2>/dev/null \
            || loader_fail "the loader variables did not read back as boot_info records them"
        if grep -qaE '\[WARN\] boot_(sticky|history):' "$STRIPPED_LOG" 2>/dev/null; then
            loader_fail "the loader warned while reading BootSticky or the boot history"
        fi
        hist_seq=$(grep -aoE '\[BOOT\] history: append seq=[0-9]+ src=0xFFFE err=0x0000' "$STRIPPED_LOG" 2>/dev/null \
                   | head -1 | sed -E 's/.*seq=([0-9]+).*/\1/')
        [ -n "$hist_seq" ] || loader_fail "the loader did not append a clean boot history record"
        grep -qaE "boot_history: +seq=$hist_seq time=[^ ]+ src=ebs-success err=0x0000" "$STRIPPED_LOG" 2>/dev/null \
            || loader_fail "the kernel did not read back history record seq=$hist_seq"
        # Two writers share the ring: the loader records ebs-success, then the
        # kernel's Phase-3 mark takes the next number.
        grep -qaE "boot_history: Phase-3 mark: append seq=$((hist_seq + 1)) " "$STRIPPED_LOG" 2>/dev/null \
            || loader_fail "the kernel's Phase-3 history mark is not seq=$((hist_seq + 1))"
        hist_n=$(grep -aoE 'boot_history: Recent boot history \([0-9]+ attempts\)' "$STRIPPED_LOG" 2>/dev/null \
                 | head -1 | grep -oE '[0-9]+')
        if [ "$REUSED_VARS" = "0" ]; then
            # Fresh variable store: first boot of a new machine.
            [ "$hist_seq" = "1" ] || loader_fail "a fresh variable store produced history seq=$hist_seq, not 1"
            grep -qF '[BOOT] boot_sticky: absent (first boot)' "$STRIPPED_LOG" 2>/dev/null \
                || loader_fail "boot_sticky did not report a fresh store as absent"
            grep -qF "[BOOT] policy: counter decrement OK -> $sel_entry+2-1 (first-boot bootstrap)" "$STRIPPED_LOG" 2>/dev/null \
                || loader_fail "boot_policy did not bootstrap the try counter on $sel_entry"
        else
            # The previous boots of this machine wrote history the loader must
            # have read back to number this one.
            [ "$hist_seq" -ge 2 ] 2>/dev/null || loader_fail "history seq=$hist_seq on a reused variable store (the loader did not read the earlier records)"
            [ "${hist_n:-0}" -ge 2 ] || loader_fail "the kernel saw ${hist_n:-0} history records on a reused variable store"
            grep -qaE "boot_history: +seq=$((hist_seq - 1)) time=[^ ]+ src=kernel-Phase3 " "$STRIPPED_LOG" 2>/dev/null \
                || loader_fail "the previous boot's kernel Phase-3 record (seq=$((hist_seq - 1))) is missing"
        fi
    fi

    # ---- Boot menu assertions ---------------------------------------------
    # The menu draws once at 2 s, repaints its countdown at 1 s, and the
    # countdown expiry boots the default. Each repaint mirrors a serial line.
    if [ "$MENU" = "1" ]; then
        for want in '[BOOT] menu: 2 entries, selected="smoke-entry" timeout=2s' \
                    '[BOOT] menu: 2 entries, selected="smoke-entry" timeout=1s' \
                    '[BOOT] menu: countdown expired -- auto-selecting default'; do
            if ! grep -qF "$want" "$STRIPPED_LOG" 2>/dev/null; then
                echo -e "${RED}SMOKE TEST FAILED: boot menu line missing: $want${NC}"
                grep -E "\[BOOT\] menu" "$STRIPPED_LOG" 2>/dev/null | head -6
                exit 1
            fi
        done
        # Rounded up, the countdown never shows 0 before it expires.
        if grep -qF 'selected="smoke-entry" timeout=0s' "$STRIPPED_LOG" 2>/dev/null; then
            echo -e "${RED}SMOKE TEST FAILED: the menu countdown showed 0 s before expiring${NC}"
            exit 1
        fi
    fi
    if [ "$MENU" = "keys" ]; then
        if [ "$KEYS_STATE" != "done" ]; then
            echo -e "${RED}SMOKE TEST FAILED: the keys leg stopped at step $KEYS_STATE (Enter was never sent)${NC}"
            grep -E "\[BOOT\] menu" "$STRIPPED_LOG" 2>/dev/null | head -8
            exit 1
        fi
        for want in '[BOOT] menu: countdown cancelled by keypress' \
                    '[BOOT] menu: Enter -- boot selected' \
                    '[BOOT] menu: user override -- chosen="smoke-alt" reason=HOTKEY'; do
            if ! grep -qF "$want" "$STRIPPED_LOG" 2>/dev/null; then
                echo -e "${RED}SMOKE TEST FAILED: boot menu line missing: $want${NC}"
                grep -E "\[BOOT\] menu" "$STRIPPED_LOG" 2>/dev/null | head -8
                exit 1
            fi
        done
        if grep -qF '[BOOT] menu: countdown expired' "$STRIPPED_LOG" 2>/dev/null; then
            echo -e "${RED}SMOKE TEST FAILED: the countdown expired although a key stopped it${NC}"
            exit 1
        fi
    fi
    if [ "$MENU" = "default" ]; then
        if [ "$KEYS_STATE" != "done" ]; then
            echo -e "${RED}SMOKE TEST FAILED: the default leg stopped at step $KEYS_STATE${NC}"
            grep -E "\[BOOT\] menu" "$STRIPPED_LOG" 2>/dev/null | head -10
            exit 1
        fi
        for want in '[BOOT] menu: e -- options for smoke-alt accepted: "test=0"' \
                    '[BOOT] policy: cmdline edited at the menu: "test=0"' \
                    'CONF: cmdline source=edited: "test=0"' \
                    'entry="smoke-alt" cmdline=edited'; do
            if ! grep -qF -- "$want" "$STRIPPED_LOG" 2>/dev/null; then
                echo -e "${RED}SMOKE TEST FAILED: the edited options did not reach the kernel: $want${NC}"
                grep -E "menu: e --|cmdline" "$STRIPPED_LOG" 2>/dev/null | head -6
                exit 1
            fi
        done
        if ! grep -qF '[BOOT] menu: timeout 4s from timeout_override' "$STRIPPED_LOG" 2>/dev/null; then
            echo -e "${RED}SMOKE TEST FAILED: the default leg's countdown did not come from the entry${NC}"
            grep -F "[BOOT] menu: timeout" "$STRIPPED_LOG" 2>/dev/null | head -2
            exit 1
        fi
    fi
    # The saved leg's menu must open on the saved entry, from the variable,
    # with the saved timeout and without the previous boot's one-off edit.
    if [ "$MENU" = "saved" ]; then
        # The first boot's kernel marked smoke-alt good on its try counter; this
        # loader must spend that mark on exactly that counter before it decides.
        # The count alone is not proof: the counter must really be gone, so the
        # entry restarts at its first-boot bootstrap instead of decrementing on.
        for want in '[BOOT] policy: MarkGood consumed (id=smoke-alt, deleted 1 counter file(s))' \
                    '[BOOT] policy: counter decrement OK -> smoke-alt+2-1 (first-boot bootstrap)' \
                    '[BOOT] loader-vars: saved default smoke-alt' \
                    'selected="smoke-alt" kind=0 rejected=0 via=saved-default' \
                    '[BOOT] menu: 2 entries, selected="smoke-alt"' \
                    '[BOOT] menu: timeout 5s from LoaderConfigTimeout' \
                    'entry="smoke-alt" cmdline=boot.conf' \
                    '[BOOT] menu: countdown expired -- auto-selecting default'; do
            if ! grep -qF "$want" "$STRIPPED_LOG" 2>/dev/null; then
                echo -e "${RED}SMOKE TEST FAILED: saved-default line missing: $want${NC}"
                grep -E "\[BOOT\] (menu|policy: selection|policy: MarkGood|loader-vars: saved)" "$STRIPPED_LOG" 2>/dev/null | head -6
                exit 1
            fi
        done
    fi
    # The kernel must receive the entry the menu chose, not only the loader log it.
    if [ "$MENU" != "0" ]; then
        want_entry=smoke-entry
        case "$MENU" in keys|default|saved) want_entry=smoke-alt ;; esac
        if ! grep -qF "entry=\"$want_entry\"" <(grep -F "boot_decision: path=" "$STRIPPED_LOG" 2>/dev/null); then
            echo -e "${RED}SMOKE TEST FAILED: the kernel did not receive entry $want_entry${NC}"
            grep -F "boot_decision:" "$STRIPPED_LOG" 2>/dev/null | head -3
            exit 1
        fi
    fi

    # ---- BootStoreReject record assertion ---------------------------------
    if [ -n "$STORE_RECORD" ]; then
        if ! grep -q "\[BOOT\] store-record: $STORE_RECORD" "$STRIPPED_LOG" 2>/dev/null; then
            echo -e "${RED}SMOKE TEST FAILED: BootStoreReject record was not $STORE_RECORD${NC}"
            grep -E "store-record:|BootStoreReject" "$STRIPPED_LOG" 2>/dev/null | head -4
            exit 1
        fi
        # A missing variable is the normal case and must not read as malformed.
        if grep -q "BootStoreReject record ignored" "$STRIPPED_LOG" 2>/dev/null; then
            echo -e "${RED}SMOKE TEST FAILED: the kernel misread the BootStoreReject record${NC}"
            exit 1
        fi
        # The record exists to reach the running OS, including on a later boot.
        if [ "$STORE_RECORD" = "cleared" ]; then
            if grep -q "Boot entry store unusable:" "$STRIPPED_LOG" 2>/dev/null; then
                echo -e "${RED}SMOKE TEST FAILED: the kernel still reports a repaired store${NC}"
                exit 1
            fi
        elif ! grep -qE "Boot entry store unusable: [A-Z_0-9]+ at line [0-9]+, col [0-9]+: " "$STRIPPED_LOG" 2>/dev/null; then
            echo -e "${RED}SMOKE TEST FAILED: the kernel did not report the rejected store${NC}"
            exit 1
        fi
    fi

    # ---- Corrupt-store assertions -----------------------------------------
    # Reaching here means the machine still booted, which is the SAFE half of
    # the contract. The other half is that it did not do so silently.
    if [ "$CORRUPT_STORE" = "1" ]; then
        if ! grep -q "\[BOOT\] store-reject:" "$STRIPPED_LOG" 2>/dev/null; then
            echo -e "${RED}SMOKE TEST FAILED: corrupt store produced no rejection notice${NC}"
            grep -c "policy:" "$STRIPPED_LOG" 2>/dev/null | sed 's/^/  policy lines seen: /'
            exit 1
        fi
        # The position travels on the token line, the one line of the banner
        # with room for it. Parser unit tests cannot see that routing.
        if ! grep -qE "\[BOOT\] store-reject: [A-Z_0-9]+ at line [0-9]+, col [0-9]+ -- " "$STRIPPED_LOG" 2>/dev/null; then
            echo -e "${RED}SMOKE TEST FAILED: rejection notice names no position in the store${NC}"
            grep "store-reject:" "$STRIPPED_LOG" 2>/dev/null | head -2
            exit 1
        fi
        # The channel is the part that proves a SCREEN render happened. QEMU
        # runs with -display none and the harness captures serial only, so a
        # bare "the line was printed" assertion would still pass with both
        # render branches deleted -- it is the notice's own report of which
        # branch it took that carries the information.
        NOTICE_CHANNEL=$(grep -o "store-reject: notice channel=[a-z-]*" "$STRIPPED_LOG" \
                         2>/dev/null | head -1 | sed 's/.*channel=//')
        if [ -z "$NOTICE_CHANNEL" ]; then
            echo -e "${RED}SMOKE TEST FAILED: rejection notice reported no render channel${NC}"
            exit 1
        fi
        if [ "$NOTICE_CHANNEL" = "serial-only" ]; then
            echo -e "${RED}SMOKE TEST FAILED: notice fell back to serial-only${NC}"
            echo -e "${DIM}  This QEMU boot has a GOP framebuffer, so the screen route${NC}"
            echo -e "${DIM}  should have been taken. serial-only here means the on-screen${NC}"
            echo -e "${DIM}  notice regressed -- which is the exact defect s22 removed.${NC}"
            exit 1
        fi
        # S23: below the full layout floor the notice must name the COMPACT
        # route. Accepting any non-serial channel here would pass on a run that
        # silently kept reporting "gop" while rendering nothing, which is the
        # class of dishonesty the channel field exists to make impossible.
        if [ "$LOWRES" = "1" ] && [ "$NOTICE_CHANNEL" != "gop-compact" ]; then
            echo -e "${RED}SMOKE TEST FAILED: at $LOWRES_MODE the notice reported${NC}"
            echo -e "${RED}  channel=$NOTICE_CHANNEL, expected gop-compact${NC}"
            grep -E "error-screen: tier=" "$STRIPPED_LOG" 2>/dev/null | sed 's/^/    /' | head -2
            exit 1
        fi
        # The notice reports truncation on its own line rather than inline, so
        # the assertion is its ABSENCE: a field edited past its budget makes
        # this fire instead of silently shortening the message on screen.
        if grep -qF -- "[BOOT] store-reject: notice fit=truncated" \
           "$STRIPPED_LOG" 2>/dev/null; then
            echo -e "${RED}SMOKE TEST FAILED: the notice truncated a field${NC}"
            exit 1
        fi
        # Store shadow: with a last good copy the corrupt store boots the
        # copy, and the notice says so; without one it boots the fallback.
        if [ "$SHADOW_STORE" != "0" ]; then
            grep -qF "[BOOT] store-shadow: booting $SHADOW_NAME entries=1" "$STRIPPED_LOG" 2>/dev/null \
                || { echo -e "${RED}SMOKE TEST FAILED: a corrupt store with a good copy did not boot the copy${NC}"
                     grep -aE "store-shadow:|store-reject:" "$STRIPPED_LOG" 2>/dev/null | head -4; exit 1; }
            grep -qE "store-reject: notice channel=[a-z-]+ -- booting the last good copy" "$STRIPPED_LOG" 2>/dev/null \
                || { echo -e "${RED}SMOKE TEST FAILED: the notice did not say the last good copy was booted${NC}"; exit 1; }
            grep -qF '[BOOT] policy: selection_reason=1 (STORE_DEFAULT) selected="smoke-entry"' "$STRIPPED_LOG" 2>/dev/null \
                || { echo -e "${RED}SMOKE TEST FAILED: the recovered store's default entry was not selected${NC}"; exit 1; }
            # A store this boot did not accept never refreshes the copy.
            if grep -qE 'store-shadow: (last good copy (saved|is current)|writing|publishing)' "$STRIPPED_LOG" 2>/dev/null; then
                echo -e "${RED}SMOKE TEST FAILED: a rejected store touched the last good copy${NC}"; exit 1
            fi
            echo -e "  ${GREEN}✓${NC} rejected store booted its last good copy"
        else
            grep -qF '[BOOT] store-shadow: no last good copy -- fallback' "$STRIPPED_LOG" 2>/dev/null \
                || { echo -e "${RED}SMOKE TEST FAILED: a corrupt store with no copy did not report the fallback${NC}"; exit 1; }
        fi
        echo -e "  ${GREEN}✓${NC} rejected store announced on screen (channel=$NOTICE_CHANNEL)"
    fi

    echo ""
    echo -e "${GREEN}══════════════════════════════════════════════════${NC}"
    echo -e "${GREEN}  SMOKE TEST PASSED${NC}"
    if [ -n "$BOOT_TIME" ]; then
        echo -e "  ${DIM}$BOOT_TIME${NC}"
    fi
    echo -e "${GREEN}══════════════════════════════════════════════════${NC}"
    echo ""
    # Content-bound smoke receipt (image + build inputs + toolchain + markers).
    # A rollover / verification over an unchanged image + inputs is then free;
    # any change to the image or a build input invalidates it. Best-effort.
    #
    # NOT recorded in corrupt-store mode: receipts.py fingerprints the
    # CANONICAL build image regardless of which image this run actually
    # booted, so recording here would certify the untouched image (and the
    # default markers) using evidence from a different boot scenario, and a
    # later rollover would reuse it as though the real image had been smoked.
    if [ "$IMAGE_MODIFIED" = "1" ]; then
        echo -e "${DIM}  smoke receipt not recorded (fixture image, not the canonical build)${NC}"
    else
        python3 "$(dirname "$0")/overnight/receipts.py" record-smoke . >/dev/null 2>&1 || true
    fi
    exit 0
else
    echo ""
    echo -e "${RED}══════════════════════════════════════════════════${NC}"
    echo -e "${RED}  SMOKE TEST FAILED: Timeout (${TIMEOUT_SEC}s)${NC}"
    echo -e "${RED}  Boot did not complete within the time limit.${NC}"
    echo -e "${RED}══════════════════════════════════════════════════${NC}"
    echo ""
    echo -e "${DIM}Last 10 lines of serial output:${NC}"
    tail -10 "$STRIPPED_LOG" 2>/dev/null | sed 's/^/  /'
    echo ""
    exit 1
fi
