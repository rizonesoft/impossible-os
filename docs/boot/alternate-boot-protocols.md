<!-- docs: covers=todo/01-boot-platform/TODO-08-alternate-boot-protocols.md sources=Makefile,scripts/build.sh,src/boot/linker.ld,src/kernel/main/boot_hw.c reviewed=2026-09-28 order=8 -->
# Alternate Boot Protocols

## What is it?

This is a closed policy decision, not a live subsystem: UEFI/GPT/ESP is the only boot path Impossible OS supports, and Multiboot2, GRUB, Limine, legacy BIOS, the Linux x86 boot protocol, EFI stub direct boot and kexec are all explicit non-goals. The roadmap closed by deleting the code rather than gating it behind a flag, so there is nothing left to accidentally re-enable.

## How does it work?

Enforcement is deletion, not a runtime check. The Multiboot2 parser (`src/kernel/multiboot2_parse.c`), its header, the entry-stub magic check, the Multiboot2 header assembly, and `src/boot/grub.cfg` were all removed from the tree. The global `g_boot_info` definition, which the parser used to own, moved to [`boot_hw.c`](../../src/kernel/main/boot_hw.c). The linker script's entry point changed from the old Multiboot2-compatible stub to the kernel's C entry point directly:

```
ENTRY(kernel_main)
```
(`src/boot/linker.ld:18`)

Because the parser no longer exists, there is no code path a bug or a stray include could reach: `nm build/kernel.exe | grep multiboot2` returns nothing, and `make iso` has no rule to build a GRUB image. A `BUILD_ALT_BOOT` variable is kept in the [`Makefile`](../../Makefile) purely as a documented knob (`off` / `diagnostic` / `compatible`, defined at `Makefile:52`) that propagates a `-D` macro to both `CFLAGS` and `ASFLAGS`; no source file reads that macro today, since there is no gated code left for it to select.

## What are its interfaces?

| Interface | Purpose |
| --------- | ------- |
| `BUILD_ALT_BOOT` in [`Makefile`](../../Makefile) | Documented, currently inert build knob for a hypothetical future reopening |
| `ENTRY(kernel_main)` in [`linker.ld`](../../src/boot/linker.ld) | Kernel image entry point (Multiboot2-compatible stub removed) |
| `g_boot_info` in [`boot_hw.c`](../../src/kernel/main/boot_hw.c) | Sole definition of the boot handoff struct storage, relocated off the deleted parser |
| [`docs/boot/alt-boot.md`](alt-boot.md) | Canonical policy statement, non-goal table, reopen criteria: full deep dive |

## How do I use it?

There is nothing to invoke: the policy is enforced by absence. To confirm it on a checked-out tree:

```bash
find src/boot/ src/kernel/ include/kernel -iname "multiboot2*"   # expect no output
nm build/kernel.exe | grep -i multiboot                          # expect no output
bash scripts/build.sh clean                                      # expect === BUILD OK ===
bash scripts/test-smoke.sh                                       # expect SMOKE TEST PASSED
```

## What is not implemented yet?

Nothing is open on this roadmap file: every section (1 through 7) shows `[x]` in the Implementation Order table, and sections 2 through 6 are marked retired-closed N/A because section 1 chose `unsupported` and section 7 carried out the deletion. [Deprecation or Promotion Gate](../../todo/01-boot-platform/TODO-08-alternate-boot-protocols.md#7-deprecation-or-promotion-gate) is the section that performed the closure.

Reopening this area at all (toward `diagnostic` or `compatible`) needs a new policy decision plus restoring the deleted files from git history; the exact steps are recorded in [Alternate Boot Protocol Policy](alt-boot.md#reopen-criteria).

## How does it compare with Windows 11 and Linux?

Windows 11 has shipped UEFI/GPT-only since 24H2, with no Multiboot equivalent and legacy BIOS/CSM removed. Linux distributions widely ship Multiboot2 through GRUB and still support legacy BIOS on many distros. Impossible OS sits at the Windows 11 end of that range by policy (UEFI/GPT only) but goes further than either incumbent on how the boundary is enforced: deletion-as-enforcement leaves no code to drift out of sync with the policy, and the non-goal table in `alt-boot.md` documents each adjacent rejection (Multiboot1, the Linux x86 boot protocol, EFI stub direct boot, kexec, Limine/stivale) so a later contributor does not have to rediscover why each one is out of scope.

## See also

- [Alternate Boot Protocols & Compatibility Boundary roadmap](../../todo/01-boot-platform/TODO-08-alternate-boot-protocols.md)
- [Alternate Boot Protocol Policy](alt-boot.md)
- [Warm-Kernel-Update Handoff ABI](../../todo/01-boot-platform/TODO-01-boot-protocol-abi-handoff.md#14-warm-kernel-update-handoff-abi) (owner of the kexec/KHO non-goal)
- [OS-Visible Loader UEFI Variables](loader-vars.md) (`loader_vars_degraded`, the ABI field this closure's non-goal table points at)
