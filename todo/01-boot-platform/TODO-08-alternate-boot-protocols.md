# TODO-08 -- Alternate Boot Protocols & Compatibility Boundary

> **Goal:** Decide and enforce how non-primary boot protocols work. The tree still contains Multiboot2 structures and parser code, while the product documentation says UEFI/GPT/ESP is required. This TODO either brings alternate boot protocols to a defined compatibility level or fences them off so they cannot silently rot.
> **Current state:** `src/boot/multiboot2_header.asm`, `include/kernel/multiboot2.h`, and `src/kernel/multiboot2_parse.c` exist. UEFI `BOOTX64.EFI` is the real boot path. There is no parity matrix, no CI boot for Multiboot2, no feature-complete fallback contract, and no explicit deprecation policy.

## Inputs

- [`src/boot/multiboot2_header.asm`](../../src/boot/multiboot2_header.asm)
- [`include/kernel/multiboot2.h`](../../include/kernel/multiboot2.h)
- [`src/kernel/multiboot2_parse.c`](../../src/kernel/multiboot2_parse.c)
- [`src/boot/grub.cfg`](../../src/boot/grub.cfg)
- -> XREF: `TODO-01-boot-protocol-abi-handoff.md` -- UEFI boot_info is the canonical handoff
- -> XREF: `TODO-06-boot-media-image-installer-handoff.md §4` -- ISO may expose only UEFI unless this TODO expands support

## Outcome

- Alternate boot protocols have explicit support level: unsupported, diagnostic-only, or compatible.
- If retained, Multiboot2 maps into canonical `boot_info` semantics with clear missing-feature behavior.
- If deprecated, build/docs/tests make unsupported paths impossible to mistake for product boot.
- Legacy BIOS, Limine, GRUB, and future protocols have a policy.

## Implementation Order

| ⭐ | Order | Deliverable | Depends On | Status |
| --- | :---: | --- | --- | :---: |
| 💎 | 1 | Alternate boot protocol policy | TODO-01 §8 | [ ] |
| 💎 | 2 | Multiboot2 feature parity audit | §1 | [ ] |
| 💎 | 3 | Multiboot2-to-boot_info adapter | §2 | [ ] |
| 💎 | 4 | Unsupported-feature degradation matrix | §2, §3 | [ ] |
| 💎 | 5 | GRUB/Limine/legacy BIOS documentation | §1 | [ ] |
| ⭐ | 6 | Compatibility test images | §3 | [ ] |
| ⭐ | 7 | Deprecation or promotion gate | §1-§6 | [ ] |

## 1. Alternate Boot Protocol Policy

- [ ] Decide supported states: UEFI only, Multiboot2 diagnostic, or Multiboot2 compatible.
- [ ] Document what features require UEFI: runtime services, Secure Boot, TPM event log, BootOrder, GOP mode list, USB handoff.
- [ ] Add build flag to include/exclude alternate boot artifacts.
- [ ] Update getting-started docs.
- [ ] Commit: `"boot: alternate boot protocol policy"`

## 2. Multiboot2 Feature Parity Audit

- [ ] Compare Multiboot2 tags against `boot_info` fields.
- [ ] Identify unavailable fields and whether kernel can degrade.
- [ ] Audit framebuffer, memory map, ACPI, module, command line, and boot device identity.
- [ ] Record parity table in this TODO and docs.
- [ ] Commit: `"boot: audit Multiboot2 parity"`

## 3. Multiboot2-to-boot_info Adapter

- [ ] Populate `boot_info.header` even on Multiboot2 path.
- [ ] Map memory, framebuffer, ACPI, module, and cmdline into canonical fields.
- [ ] Set flags for missing UEFI-only data.
- [ ] Reuse `boot_info_validate_*()` where possible.
- [ ] Commit: `"boot: adapt Multiboot2 into boot_info"`

## 4. Unsupported-Feature Degradation Matrix

- [ ] Define behavior for no UEFI variables, no runtime services, no Secure Boot, no TPM log, no BootOrder, no USB handoff.
- [ ] Mark affected subsystems degraded through readiness oracle.
- [ ] Show warning in VPD/serial.
- [ ] Prevent release mode from booting unsupported protocol unless explicitly allowed.
- [ ] Commit: `"boot: alternate protocol degradation matrix"`

## 5. GRUB/Limine/Legacy BIOS Documentation

- [ ] Document GRUB Multiboot2 usage if retained.
- [ ] Document Limine/stivale policy if future adoption is allowed.
- [ ] Document that legacy BIOS is unsupported unless a future TODO adds it.
- [ ] Update VM boot testing guide.
- [ ] Commit: `"docs: alternate boot protocol boundary"`

## 6. Compatibility Test Images

- [ ] Build GRUB Multiboot2 test ISO.
- [ ] Boot QEMU legacy/UEFI GRUB path depending on policy.
- [ ] Assert missing UEFI-only features degrade cleanly.
- [ ] Add CI skip gate if policy is unsupported.
- [ ] Commit: `"test: alternate boot protocol image"`

## 7. Deprecation or Promotion Gate

- [ ] If unsupported: remove stale parser from release builds and keep docs only.
- [ ] If diagnostic: keep parser but gate with `ALLOW_ALT_BOOT=1`.
- [ ] If compatible: add it to certification matrix in TODO-28.
- [ ] Update `GAP-ANALYSIS.md` closure status.
- [ ] Commit: `"boot: settle alternate boot support level"`

## OS Comparison

| ⭐ | Feature | Windows | Linux | Impossible OS |
| --- | --- | --- | --- | --- |
| 💎 | UEFI primary boot | yes | yes | existing |
| 💎 | Multiboot/GRUB style | no | common | TODO-08 decision |
| ⭐ | Explicit unsupported-path fence | bootmgr-specific | distro-specific | TODO-08 §2 |

## Unit Tests

- [ ] `test_multiboot2_mmap_to_boot_info`
- [ ] `test_multiboot2_missing_uefi_degraded`
- [ ] `test_alt_boot_policy_release_rejects`

## Verification

- [ ] QEMU UEFI remains primary path
- [ ] QEMU GRUB path according to selected policy
- [ ] Documentation clearly states support status

