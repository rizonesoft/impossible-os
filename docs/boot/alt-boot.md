# Alternate Boot Protocol Policy

> **Owner:** [Alternate Boot Protocols & Compatibility Boundary](../../todo/01-boot-platform/TODO-08-alternate-boot-protocols.md#1-alternate-boot-protocol-policy).
>
> **Canonical policy statement:** UEFI/GPT/ESP is the only supported boot path for Impossible OS. Multiboot2 / GRUB / Limine / stivale / legacy BIOS / Linux x86 boot protocol / EFI stub direct boot / kexec are NOT supported entry points.
>
> **Current build state:** `diagnostic` by inertia -- the Multiboot2 parser at [`src/kernel/multiboot2_parse.c`](../../src/kernel/multiboot2_parse.c) compiles into the kernel image and [`src/boot/entry.asm`](../../src/boot/entry.asm) carries the magic-check dispatch, but the production UEFI boot path never invokes the parser. No build today actually runs the alternate path.

This doc pins the policy contract so adjacent contributors don't accidentally re-introduce alternate-protocol code without a deliberate policy decision.

## The three policy modes

| Mode | Meaning | Default for | Status |
|---|---|---|---|
| `off` | Parser excluded from kernel image; entry.asm magic check compiled out; no GRUB artifact ships. | Release builds (recommended; not yet enforced) | Enforcement gating lands in the [deprecation-or-promotion gate](../../todo/01-boot-platform/TODO-08-alternate-boot-protocols.md#7-deprecation-or-promotion-gate) |
| `diagnostic` | Parser code included but never reached on UEFI path. Useful for development / spec research. | Developer builds | Today's actual state |
| `compatible` | Full Multiboot2 parity per the [Multiboot2-to-boot_info adapter](../../todo/01-boot-platform/TODO-08-alternate-boot-protocols.md#3-multiboot2-to-boot_info-adapter); GRUB ISO ships. | Not selected | Implementation in the [later compatibility sections](../../todo/01-boot-platform/TODO-08-alternate-boot-protocols.md#2-multiboot2-feature-parity-audit) |

### Build-mode plumbing

The `BUILD_ALT_BOOT` Makefile variable selects the mode. Three call sites:

```
# Makefile:
BUILD_ALT_BOOT ?= diagnostic
# propagates -DBUILD_ALT_BOOT_{OFF,DIAGNOSTIC,COMPATIBLE}=1 to CFLAGS + ASFLAGS

# scripts/build.sh:
BUILD_ALT_BOOT=off bash scripts/build.sh    # explicit override

# scripts/release/build-image.sh:
# Release builds SHOULD set BUILD_ALT_BOOT=off before invoking make.
# The release script does NOT yet enforce this; it copies the prebuilt
# build/kernel.exe without checking which mode produced it. Release-
# time enforcement (assertion that kernel image contains no multiboot2
# symbols) lands together with the deprecation gate's symbol gating
# in entry.asm and multiboot2_parse.c. Until then, the release default
# is unenforced -- caller responsibility.
```

The define propagates to both C and NASM toolchains so the eventual gating can use the same `-D` macro from `entry.asm` (NASM `%ifdef`), `multiboot2_header.asm`, `boot_hw.c`, and `multiboot2_parse.c`. Today the defines exist but no code consumes them; the value is informational until the [deprecation-or-promotion gate](../../todo/01-boot-platform/TODO-08-alternate-boot-protocols.md#7-deprecation-or-promotion-gate) lands.

### Today vs the future gating

| Item | Today (policy section) | After deprecation gate |
|---|---|---|
| `BUILD_ALT_BOOT` Makefile variable | OK defined; default `diagnostic` | unchanged |
| `-D` define propagated to CFLAGS + ASFLAGS | OK | unchanged |
| `scripts/build.sh` help text | OK documents env var | unchanged |
| `entry.asm` magic check gated by `BUILD_ALT_BOOT_OFF` | not gated; always present | excluded when off |
| `multiboot2_parse.c` in kernel image when `off` | always present | excluded |
| `nm build/kernel.exe \| grep multiboot2` returns empty with `BUILD_ALT_BOOT=off` | no (no gating yet) | yes |
| Release script asserts no multiboot2 symbols | no | yes |

The policy section ships the plumbing; the deprecation-or-promotion gate enforces the policy by adding the actual gates.

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

## Decision-flip criteria

The default `diagnostic` is not a positive choice; it's the codified current state. To move to `off` or `compatible`, document the trigger in a future policy-section update:

**Flip to `off` (recommended):**

- Zero concrete users requesting Multiboot2 / GRUB compatibility.
- [Deprecation-or-promotion gate](../../todo/01-boot-platform/TODO-08-alternate-boot-protocols.md#7-deprecation-or-promotion-gate) work scheduled to delete the parser, gate entry.asm, and update INDEX.md.
- Analysis in [TODO-08](../../todo/01-boot-platform/TODO-08-alternate-boot-protocols.md) gap-audit suggests this is the right long-term answer.

**Flip to `compatible`:**

- Concrete user / deployment scenario requiring GRUB boot (e.g., chainload from existing dual-boot install, kiosk / embedded with locked-down firmware).
- Team capacity to track upstream GRUB + shim CVE backlog quarterly.
- The [parity audit](../../todo/01-boot-platform/TODO-08-alternate-boot-protocols.md#2-multiboot2-feature-parity-audit) + [adapter](../../todo/01-boot-platform/TODO-08-alternate-boot-protocols.md#3-multiboot2-to-boot_info-adapter) + [degradation matrix](../../todo/01-boot-platform/TODO-08-alternate-boot-protocols.md#4-unsupported-feature-degradation-matrix) + [test images](../../todo/01-boot-platform/TODO-08-alternate-boot-protocols.md#6-compatibility-test-images) scope accepted.
- Promotion-gate criteria satisfied.

**Stay at `diagnostic` (current state):**

- Policy decision postponed.
- Maintenance cost: every `boot_info` refactor must keep `multiboot2_parse.c` in sync; this is the cost of leaving dead code in the boot path.

## References

- [TODO-08 Alternate Boot Protocols & Compatibility Boundary](../../todo/01-boot-platform/TODO-08-alternate-boot-protocols.md)
- [Warm-Kernel-Update Handoff ABI](../../todo/01-boot-platform/TODO-01-boot-protocol-abi-handoff.md#14-warm-kernel-update-handoff-abi) (owner of kexec / KHO non-goal pointer)
- [OS-Visible Loader UEFI Variables](../../todo/01-boot-platform/TODO-07-boot-entry-store-menu-policy.md#15-os-visible-loader-uefi-variables) (`loader_vars_degraded` ABI)
- [UKI signed-payload chain](../../todo/01-boot-platform/TODO-02-uefi-hardening-secureboot.md#16-signed-initrd--recovery--module-pe-sections-in-uki)
- [GNU Multiboot2 Specification](https://www.gnu.org/software/grub/manual/multiboot2/multiboot.html)
- [Eclypsium BootHole writeup](https://eclypsium.com/blog/theres-a-hole-in-the-boot/)
