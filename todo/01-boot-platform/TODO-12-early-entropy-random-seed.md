# TODO-12 -- Early Entropy & Random Seed Handoff

> **Goal:** Provide trustworthy randomness as early as possible. Secure Boot, TPM, code integrity, log integrity, ASLR, stack canaries, and cryptographic services all need entropy, but the boot platform currently has no owned plan for firmware RNG, CPU RNG, TPM RNG, seed carryover, or early-kernel CSPRNG seeding.
> **Current state:** Later TODOs mention CSPRNG and Monocypher. Boot code uses RDRAND in some contexts and TPM presence is parsed, but there is no early entropy pool, no seed file lifecycle, no EFI_RNG_PROTOCOL usage, no TPM RNG read, and no boot_info seed handoff.

## Inputs

- [`src/boot/uefi/bootx64.c`](../../src/boot/uefi/bootx64.c)
- [`include/kernel/boot_info.h`](../../include/kernel/boot_info.h)
- [`include/kernel/tpm.h`](../../include/kernel/tpm.h)
- -> XREF: `TODO-13-tpm-measured-boot-attestation.md §2` -- TPM command transport
- -> XREF: `TODO-01-boot-protocol-abi-handoff.md §4` -- random seed payload descriptor
- -> XREF: `../02-kernel-core/TODO-03-kernel-libraries.md §3` -- kernel CSPRNG
- -> XREF: `../02-kernel-core/TODO-10-kernel-security-hardening.md §14` -- KASLR consumes early entropy

## Outcome

- Bootloader gathers firmware, CPU, TPM, timer, and carryover seed material.
- Kernel receives a bounded seed descriptor and destroys one-time seed material after use.
- CSPRNG can initialize before user-mode or network services.
- Entropy quality is reported honestly; degraded randomness is visible.

## Implementation Order

| ⭐ | Order | Deliverable | Depends On | Status |
| --- | :---: | --- | --- | :---: |
| 💎 | 1 | Entropy source inventory and quality model | TODO-04 §2 | [ ] |
| 💎 | 2 | EFI_RNG_PROTOCOL collection | §1 | [ ] |
| 💎 | 3 | CPU RDRAND/RDSEED collection | §1, TODO-09 §1 | [ ] |
| 💎 | 4 | TPM RNG collection | §1, TODO-13 §2 | [ ] |
| 💎 | 5 | Boot timing and interrupt jitter mix-in | §1 | [ ] |
| 💎 | 6 | Seed file carryover lifecycle | §1, TODO-24 | [ ] |
| 💎 | 7 | boot_info seed handoff | TODO-01 §4 | [ ] |
| 💎 | 8 | Kernel early CSPRNG seeding | §7, ../02-kernel-core/TODO-03 §3 | [ ] |
| ⭐ | 9 | Entropy diagnostics and policy gates | §1-§8 | [ ] |
| 💎 | 10 | Entropy tests | §1-§9 | [ ] |

## 1. Entropy Source Inventory and Quality Model

- [ ] Define source classes: firmware RNG, CPU RNG, TPM RNG, seed file, jitter, time.
- [ ] Assign quality bits conservatively and log degraded states.
- [ ] Define minimum quality for Secure Boot release mode and debug mode.
- [ ] Add source mask to boot diagnostics.
- [ ] Commit: `"boot: early entropy source model"`

## 2. EFI_RNG_PROTOCOL Collection

- [ ] Locate EFI_RNG_PROTOCOL before ExitBootServices.
- [ ] Query supported algorithms when available.
- [ ] Collect at least 64 bytes and mix into seed buffer.
- [ ] Time out and degrade cleanly on broken firmware.
- [ ] Commit: `"boot: collect firmware RNG entropy"`

## 3. CPU RDRAND/RDSEED Collection

- [ ] Use CPUID-gated RDSEED first, RDRAND second.
- [ ] Retry bounded times and reject all-zero/repeated output.
- [ ] Mix CPU vendor/family/model only as non-secret personalization.
- [ ] Record source health in boot_info.
- [ ] Commit: `"boot: collect CPU RNG entropy"`

## 4. TPM RNG Collection

- [ ] Use TPM2_GetRandom when TPM transport is available.
- [ ] Mix TPM output with other sources rather than trusting it alone.
- [ ] Degrade cleanly on no TPM, timeout, or error.
- [ ] Include TPM RNG availability in measured boot report.
- [ ] Commit: `"boot: collect TPM RNG entropy"`

## 5. Boot Timing and Interrupt Jitter Mix-In

- [ ] Mix FPDT/TSC boot timings as low-quality personalization.
- [ ] Add optional PIT/LAPIC jitter sampling after interrupts are available.
- [ ] Never count deterministic VM timing as high-quality entropy.
- [ ] Add VM/hypervisor detection caveats to diagnostics.
- [ ] Commit: `"boot: mix timing jitter into entropy pool"`

## 6. Seed File Carryover Lifecycle

- [ ] Store `X:\Boot\random-seed.bin` with version, counter, and MAC.
- [ ] Read seed during boot, mix once, then rotate after kernel CSPRNG is ready.
- [ ] Prevent seed reuse after crash/power loss where possible.
- [ ] Handle read-only/recovery media without blocking boot.
- [ ] Commit: `"boot: random seed carryover file"`

## 7. boot_info Seed Handoff

- [ ] Add typed seed payload via TODO-01 with address, length, source mask, quality bits, and checksum.
- [ ] Ensure PMM reserves seed memory until CSPRNG consumes it.
- [ ] Zero seed memory after consumption.
- [ ] Add ABI tests for seed descriptor.
- [ ] Commit: `"boot: hand off early entropy seed"`

## 8. Kernel Early CSPRNG Seeding

- [ ] Add `early_entropy_init()` before KASLR/ASLR consumers where possible.
- [ ] Feed Monocypher/ChaCha CSPRNG once available.
- [ ] Provide temporary bounded random API with explicit degraded flag.
- [ ] Block release-mode cryptographic operations until seeded.
- [ ] Commit: `"kernel: seed early CSPRNG from boot entropy"`

## 9. Entropy Diagnostics and Policy Gates

- [ ] Log source mask and quality class without exposing seed bytes.
- [ ] Show degraded randomness in VPD/recovery if release policy fails.
- [ ] Add `HKLM\SYSTEM\Boot\Entropy`.
- [ ] Add BlackBox report.
- [ ] Commit: `"boot: entropy diagnostics and policy"`

## 10. Entropy Tests

- [ ] Unit tests for source mixing determinism with fixtures.
- [ ] Test no source -> degraded but safe.
- [ ] Test seed file rotate.
- [ ] Test zeroization after consumption.
- [ ] Commit: `"test: early entropy handoff"`

## OS Comparison

| ⭐ | Feature | Windows | Linux | Impossible OS |
| --- | --- | --- | --- | --- |
| 💎 | firmware RNG | CNG/boot entropy | EFI RNG to random | TODO-12 |
| 💎 | seed carryover | system secrets | random-seed | TODO-12 §8 |
| 💎 | TPM RNG mix | TPM-backed paths | tpm-rng | TODO-12 §5 |
| ⭐ | visible entropy quality report | hidden | dmesg only | TODO-12 §9 |

## Unit Tests

- [ ] `test_entropy_source_mask`
- [ ] `test_entropy_mix_fixture`
- [ ] `test_random_seed_rotation`
- [ ] `test_entropy_seed_zeroized`

## Verification

- [ ] QEMU OVMF without RNG degrades visibly
- [ ] QEMU with virtio/EFI RNG
- [ ] Bare metal with RDRAND
- [ ] Bare metal with TPM RNG

