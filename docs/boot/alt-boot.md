<!-- docs: covers=todo/01-boot-platform/TODO-08-alternate-boot-protocols.md sources=Makefile,scripts/build.sh reviewed=2026-09-28 -->
# Alternate Boot Protocol Policy

> **Owner:** [Alternate Boot Protocols & Compatibility Boundary](../../todo/01-boot-platform/TODO-08-alternate-boot-protocols.md#1-alternate-boot-protocol-policy).
>
> **Canonical policy statement:** UEFI/GPT/ESP is the only supported boot path for Impossible OS. Multiboot2 / GRUB / Limine / stivale / legacy BIOS / Linux x86 boot protocol / EFI stub direct boot / kexec are NOT supported entry points.
>
> **Current build state:** `unsupported` (policy committed). The Multiboot2 parser, multiboot2_header.asm, entry.asm magic-check stub, and grub.cfg have been deleted from the tree. UEFI/GPT/ESP is the only supported boot path. `BUILD_ALT_BOOT` defaults to `off`; the variable is retained as a knob for a hypothetical future flip back to `diagnostic` / `compatible` (no source code consumes the macro now that the parser is gone).

This doc pins the policy contract so adjacent contributors don't accidentally re-introduce alternate-protocol code without a deliberate policy decision.

## The three policy modes

| Mode | Meaning | Default for | Status |
|---|---|---|---|
| `off` | Parser excluded from kernel image; no GRUB artifact ships. | All builds (default) | **Current state** -- parser + entry stub + grub.cfg + multiboot2.h deleted in the [deprecation-or-promotion gate](../../todo/01-boot-platform/TODO-08-alternate-boot-protocols.md#7-deprecation-or-promotion-gate) |
| `diagnostic` | (Retired) Parser code included but never reached on UEFI path. | Not selected | Was today's state pre-policy commit; parser since deleted |
| `compatible` | Full Multiboot2 parity; GRUB ISO ships. | Not selected | Would require re-implementing the parser from the GNU Multiboot2 spec |

### Build-mode plumbing

The `BUILD_ALT_BOOT` Makefile variable is retained for documentation continuity and to make a hypothetical future flip back to `diagnostic` / `compatible` a single-knob change. Today the parser, header, entry stub, and `grub.cfg` are deleted from the tree; no source code reads the macro.

```
# Makefile:
BUILD_ALT_BOOT ?= off
# propagates -DBUILD_ALT_BOOT_{OFF,DIAGNOSTIC,COMPATIBLE}=1 to CFLAGS + ASFLAGS
# (informational; no source code reads the macro post-deletion)

# scripts/build.sh:
BUILD_ALT_BOOT=off bash scripts/build.sh    # default; override is a no-op today
```

If the policy ever flips back, the deleted files are restored from git history AND gated with `%ifndef BUILD_ALT_BOOT_OFF` (NASM) / `#ifndef BUILD_ALT_BOOT_OFF` (C) so the `off` mode keeps producing a clean kernel image. The `-D` macro propagation to both CFLAGS and ASFLAGS is already in place to support that gating from either toolchain.

### Today vs the retired state

| Item | Today (unsupported) | If policy ever flips back |
|---|---|---|
| `BUILD_ALT_BOOT` Makefile variable | defined; default `off`; informational only | gates restored parser via `-D` |
| `-D` define propagated to CFLAGS + ASFLAGS | yes (informational) | actively consumed by restored gates |
| `scripts/build.sh` help text | documents env var | unchanged |
| `entry.asm` / `multiboot2_*` files | deleted (git log carries history) | restored from git; `%ifndef BUILD_ALT_BOOT_OFF` gated |
| `nm build/kernel.exe \| grep multiboot2` | empty (no symbols emit) | empty when `off`; populated when `diagnostic`/`compatible` |
| Release script asserts no multiboot2 symbols | not needed -- symbols structurally absent | required gate to prevent leakage |

The deletion is the enforcement. There is no separate gating layer to maintain.

## Explicit non-goals (decided regardless of Multiboot2 outcome)

These boot entry points are NOT supported at any `BUILD_ALT_BOOT` setting. Adjacent contributors must not add code to support them without a new policy decision.

| Non-goal | Why | Owner |
|---|---|---|
| **Multiboot1** (pre-GRUB-2.02) | Superseded by Multiboot2 in 2014. Missing tag taxonomy. | [Documentation section](../../todo/01-boot-platform/TODO-08-alternate-boot-protocols.md#5-grub--limine--legacy-bios-documentation) rejects. |
| **Linux x86 boot protocol** / bzImage / zboot entry | Linux-specific entry point. Impossible OS uses its own `boot_info` ABI. | Documentation section rejects. |
| **EFI stub direct boot** (CONFIG_EFI_STUB equivalent) | Bootloader owns boot-policy ladder + NVRAM read + capability negotiation; kernel should not be touching UEFI. | Documentation section rejects. |
| **kexec kernel-to-kernel** | Warm-kernel-update is owned by the [boot-protocol warm-update ABI](../../todo/01-boot-platform/TODO-01-boot-protocol-abi-handoff.md#14-warm-kernel-update-handoff-abi) + [warm-kernel-update runtime](../../todo/03-memory-concurrency/TODO-11-warm-kernel-update-runtime.md). kexec is kernel-internal, not bootloader handoff. | Documentation section + warm-update ABI XREF. |
| **Legacy BIOS / CSM** | Win11 24H2 ships UEFI-only. Modern hardware converged. | Documentation section rejects; `Legacy BIOS / CSM` row in [OS Comparison](../../todo/01-boot-platform/TODO-08-alternate-boot-protocols.md#os-comparison). |
| **Limine / stivale** | UEFI + Multiboot2 (if compatible) cover dual-boot. Limine adds maintenance burden without concrete user need. | Default rejection in the policy section. |

## UEFI-only features (lost on any alt-boot path that ever ships)

If `BUILD_ALT_BOOT=compatible` ever ships, these features become unavailable / degraded on the alt-boot path. The [unsupported-feature degradation matrix](../../todo/01-boot-platform/TODO-08-alternate-boot-protocols.md#4-unsupported-feature-degradation-matrix) owns the per-feature behavior contract.

- **UEFI Runtime Services** -- no `gRT->GetVariable` / `gRT->SetVariable` / `gRT->ResetSystem`. Kernel `uefi_runtime_init()` fails closed.
- **Secure Boot** -- Microsoft's UEFI CA + 3rd-party CA chain only validates UEFI-launched payloads. GRUB Multiboot2 path inherits upstream GRUB/shim CVE surface instead (see Security trade-offs below).
- **TPM measured boot** -- no PCR extension from UEFI; Multiboot2 has no equivalent of the TCG event log.
- **BootOrder / BootCurrent / BootNext** UEFI variables -- the [OS-visible LoaderXxx UEFI variables](../../todo/01-boot-platform/TODO-07-boot-entry-store-menu-policy.md#15-os-visible-loader-uefi-variables) become unusable; `boot_info.loader_vars_degraded = 1`.
- **GOP framebuffer** -- Multiboot2 framebuffer tag is a single-mode subset.
- **USB pre-EBS handoff** -- xHCI takeover happens in the UEFI bootloader; alt-boot misses it.
- **ESP integrity gate** -- the UEFI bootloader validates the ESP file structure before kernel handoff.
- **UKI signed-payload chain** ([signed `.initrd` / Recovery / Module PE sections in UKI](../../todo/01-boot-platform/TODO-02-uefi-hardening-secureboot.md#16-signed-initrd--recovery--module-pe-sections-in-uki)) -- alt-boot loads unsigned modules.
- **A/B rollback** ([A/B boot rollback](../../todo/01-boot-platform/TODO-21-ab-boot-rollback.md)) -- defaults to slot A on alt-boot.
- **Recovery partition trigger** ([Recovery partition](../../todo/01-boot-platform/TODO-22-recovery-partition.md)) -- `boot.conf`-only path on alt-boot.
- **Hibernation resume** ([Hibernation resume](../../todo/01-boot-platform/TODO-26-hibernation-resume-fast-startup-handoff.md)) -- refused on alt-boot (no UEFI variable for snapshot state).
- **UEFI watchdog** ([Boot watchdog](../../todo/01-boot-platform/TODO-23-boot-watchdog.md)) -- replaced or disabled on alt-boot.

## Security trade-offs (compatible mode only)

Shipping a GRUB Multiboot2 path means owning the upstream GRUB + shim CVE pipeline. The 2020-2025 incident list is illustrative, not exhaustive:

- **CVE-2020-10713 "BootHole"** -- GRUB2 grub.cfg parser buffer overflow; Secure Boot bypass. Fix required coordinated updates to shim, GRUB, kernel, fwupd, and the UEFI Secure Boot signing process.
- **CVE-2023-40547 + family (Feb 2024)** -- multiple shim vulnerabilities including HTTP-protocol handling out-of-bounds write. Affected nearly all Linux distros.
- **CVE-2025-3052** -- Secure Boot bypass affecting most UEFI devices via a module signed with Microsoft's third-party UEFI cert.
- **CVE-2025-427 "Hydroph0bia"**, **CVE-2025-47827** -- 2025 firmware-level Secure Boot validation flaws.

If `compatible` mode ships, the [deprecation-or-promotion gate](../../todo/01-boot-platform/TODO-08-alternate-boot-protocols.md#7-deprecation-or-promotion-gate) requires:

- Upstream GRUB CVE backlog scan vs pinned version.
- Upstream shim CVE backlog scan.
- SBAT / dbx / revocation status check (Microsoft UEFI CA + 3rd-party CA).
- Reciprocal XREF to the [UKI Secure Boot owner](../../todo/01-boot-platform/TODO-02-uefi-hardening-secureboot.md#16-signed-initrd--recovery--module-pe-sections-in-uki) confirming the alt-boot variant is documented as a weaker trust-boundary.
- Quarterly re-check while `compatible` ships.

Native UEFI/UKI chain remains the canonical secure path. `compatible` is the explicit trade-off path.

## Reopen criteria

The committed policy is `unsupported`. A future re-evaluation back to `diagnostic` or `compatible` requires both a new policy decision AND code restoration -- not a single flag flip.

**To reopen toward `diagnostic` (parser compiled but not reached):**

- File a new TODO section under TODO-08 (or successor TODO) explaining the deployment scenario that needs the parser available for inspection / spec research without shipping it on a live boot path.
- `git restore` the deleted files at their pre-§7 revision: `src/kernel/multiboot2_parse.c`, `include/kernel/multiboot2.h`, `src/boot/multiboot2_header.asm`, `src/boot/entry.asm`, `src/boot/grub.cfg`.
- Restore `g_boot_info` storage to the canonical location (do NOT duplicate -- keep the `src/kernel/main/boot_hw.c` definition; delete the multiboot2_parse.c copy if `git restore` brings it back).
- Add `%ifndef BUILD_ALT_BOOT_OFF` / `#ifndef BUILD_ALT_BOOT_OFF` gates so the default `off` mode keeps producing a clean kernel image.

**To reopen toward `compatible` (full Multiboot2 parity + GRUB artifact):**

- All of the above PLUS the §7 promotion gate: upstream GRUB CVE backlog scan, upstream shim CVE backlog scan, SBAT / dbx / revocation status check, reciprocal XREF to the Secure Boot owner confirming the alt-boot variant is a documented weaker trust-boundary, quarterly re-check cadence.
- Concrete deployment scenario justifying the ongoing CVE / SBAT maintenance burden.
- Reimplement the full §2-§6 work that was retired under closure: parity audit, boot_info adapter, degradation matrix, GRUB documentation, compat test images.

**Status today:**

`unsupported`. Multiboot2 parser, entry stub, header asm, and `grub.cfg` are deleted. `BUILD_ALT_BOOT` defaults to `off`. UEFI/GPT/ESP is the only supported boot path. The deletion itself is the enforcement; there is no flag check at boot time because there is no parser to enable.

## References

- [TODO-08 Alternate Boot Protocols & Compatibility Boundary](../../todo/01-boot-platform/TODO-08-alternate-boot-protocols.md)
- [Warm-Kernel-Update Handoff ABI](../../todo/01-boot-platform/TODO-01-boot-protocol-abi-handoff.md#14-warm-kernel-update-handoff-abi) (owner of kexec / KHO non-goal pointer)
- [OS-Visible Loader UEFI Variables](../../todo/01-boot-platform/TODO-07-boot-entry-store-menu-policy.md#15-os-visible-loader-uefi-variables) (`loader_vars_degraded` ABI)
- [UKI signed-payload chain](../../todo/01-boot-platform/TODO-02-uefi-hardening-secureboot.md#16-signed-initrd--recovery--module-pe-sections-in-uki)
- [GNU Multiboot2 Specification](https://www.gnu.org/software/grub/manual/multiboot2/multiboot.html)
- [Eclypsium BootHole writeup](https://eclypsium.com/blog/theres-a-hole-in-the-boot/)
