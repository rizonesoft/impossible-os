<!-- docs: covers=todo/01-boot-platform/TODO-13-tpm-measured-boot-attestation.md sources=include/kernel/tpm_pcr_alloc.h,src/kernel/tpm_pcr_alloc.c,include/boot/pcr_manifest.h reviewed=2026-09-28 -->
# PCR Allocation and Policy Masks

Single source of truth for which boot event extends which TPM PCR, and which PCRs
the sealed-secret / quote / baseline policies use. The machine-readable form is
the event allocation table in [`include/kernel/tpm_pcr_alloc.h`](../../include/kernel/tpm_pcr_alloc.h)
+ [`src/kernel/tpm_pcr_alloc.c`](../../src/kernel/tpm_pcr_alloc.c); the masks are
DERIVED from per-event policy flags (`tpm_pcr_seal_mask()` / `tpm_pcr_quote_mask()`
/ `tpm_pcr_baseline_mask()`), never hard-coded per consumer, so they cannot drift
from this table. This doc is the human-facing companion.

## Why event-centric, not per-PCR

A PCR digest is an aggregate of every event extended into it, so a single
"owner" per PCR cannot tell *which* event on a shared bank changed. PCR 11 in
particular carries two distinct events (UKI kernel-boot and the Impossible OS
kernel-ABI manifest). The table therefore allows multiple ordered entries per
PCR; replay and baseline diagnostics classify from the ordered event list so a
manifest-only change is attributed to the **kernel-ABI layer**, not reported as a
generic PCR-11 / baseline mismatch.

## Ownership table

| PCR | Producer | Layer | Event(s) | Seal | Quote | Baseline | Volatile |
|----|----------|-------|----------|:----:|:-----:|:--------:|:--------:|
| 0 | firmware | firmware code | Firmware code | | x | x | |
| 1 | firmware | firmware config | Firmware configuration | | x | x | |
| 2 | firmware | option ROM | Option ROM code | | x | x | |
| 3 | firmware | option ROM | Option ROM config | | x | x | |
| 4 | bootloader | bootloader/IPL | IPL / boot app | | x | x | |
| 5 | bootloader | bootloader/IPL | IPL configuration | | x | x | |
| 6 | firmware | firmware | State transition | | x | x | |
| 7 | firmware | Secure Boot | Secure Boot policy (db/dbx/KEK/PK) | **x** | x | x | |
| 11 | bootloader | UKI kernel-boot | UKI kernel-boot (ordering 0) | | x | x | x |
| 11 | bootloader | kernel-ABI | `IMPOSSIBLE_OS_KERNEL_ABI_MANIFEST` (ordering 1) | | x | x | x |

PCR 8-10 and 12-23 carry no Impossible OS allocation today (PCR 8-9 are Linux
boot-aggregate conventions; 17-22 are DRTM; the OS-data range 12-15 is reserved
for future runtime measurements).

## Mask membership and rationale

- **Seal default = PCR 7 only.** Sealed secrets (FDE keys) bind to the Secure
  Boot policy, which is stable across kernel updates under the same keys.
  BitLocker's PCR7+PCR11 default re-seals on *any* PCR-11 change, so a kernel
  rebuild (which moves the kernel-ABI manifest and UKI measurements) would brick
  unseal. Impossible OS keeps **PCR 11 out of the seal default** (opt-in for
  callers that explicitly want kernel-image binding). This is enforced by a
  negative unit test (`PCR 11 excluded from the seal default`).
- **Quote + Baseline = PCR 0-7 + PCR 11.** Attestation and the enrolled baseline
  cover firmware + Secure Boot (0-7) plus the kernel-ABI + UKI measurements (11),
  so a manifest-only change is attributable to the kernel-ABI layer rather than a
  blanket mismatch.

## Producer/consumer drift guard

The manifest event index + name (`TPM_PCR_MANIFEST_INDEX` = 11,
`TPM_PCR_MANIFEST_EVENT` = `IMPOSSIBLE_OS_KERNEL_ABI_MANIFEST`) are defined ONCE
in the dependency-free [`include/boot/pcr_manifest.h`](../../include/boot/pcr_manifest.h)
(no `kernel/types.h` or other include), so the freestanding UEFI bootloader
producer and the kernel allocation table both consume the exact same values.
`include/kernel/tpm_pcr_alloc.h` includes it; the bootloader pre-jump manifest
extend MUST include it too (rather than hard-coding 11 / the event string), with
a build-time drift check. That producer-side wire-up is owned by the bootloader
manifest-extend work (TODO-13 attestation export section); this table is the
authoritative definition it must match.

## Conventions referenced

- BitLocker: seals to PCR 7 + PCR 11 by default (re-seal on PCR-11 change).
- systemd-stub / `systemd-pcrlock`: UKI kernel-boot measured into PCR 11.
- Keylime: runtime attestation masks often exclude PCR 11 to avoid churn.
