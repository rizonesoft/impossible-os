---
schema_version: 1
id: early-entropy-random-seed
domain: 01-boot-platform
status: active
title: "TODO-12 -- Early Entropy & Random Seed Handoff"
---

# TODO-12 -- Early Entropy & Random Seed Handoff

> **Goal:** Provide trustworthy randomness as early as possible. Secure Boot, TPM, code integrity, log integrity, ASLR, stack canaries, and cryptographic services all need entropy, but the boot platform currently has no owned plan for firmware RNG, CPU RNG, TPM RNG, seed carryover, or early-kernel CSPRNG seeding.

> [!IMPORTANT]
> **Current state (2026-06-12):** Later TODOs mention CSPRNG and Monocypher (`02-kernel-core/TODO-03` §5 ships `csprng_fill()`; `TODO-10` §11 seeds `__stack_chk_guard` with RDRAND; §14 KASLR uses RDRAND directly). Boot code uses RDRAND in some contexts and TPM presence is parsed, but there is no early entropy pool, no seed file lifecycle, no EFI_RNG_PROTOCOL usage, no TPM RNG read, and no boot_info seed handoff. `BOOT_PAYLOAD_RANDOM_SEED = 7` is already reserved in `include/kernel/boot_info.h` (owner: this TODO).

## Inputs

- [`src/boot/uefi/bootx64.c`](../../src/boot/uefi/bootx64.c)
- [`include/kernel/boot_info.h`](../../include/kernel/boot_info.h)
- [`include/kernel/tpm.h`](../../include/kernel/tpm.h)
- -> XREF: `TODO-13-tpm-measured-boot-attestation.md §2` -- TPM command transport
- -> XREF: `TODO-01-boot-protocol-abi-handoff.md §4` -- random seed payload descriptor
- -> XREF: `../02-kernel-core/TODO-03-kernel-libraries.md §5` -- Monocypher + kernel CSPRNG (`csprng_fill()`)
- -> XREF: `../02-kernel-core/TODO-10-kernel-security-hardening.md §14` -- KASLR consumes early entropy
- -> XREF: `../04-drivers-hardware/TODO-09-hypervisor-abstraction.md §12` -- virtio-rng guest hwrng source (post-PCI, feeds runtime reseed)

## Outcome

- Bootloader gathers firmware, CPU, TPM, timer, and carryover seed material.
- Kernel receives a bounded seed descriptor and destroys one-time seed material after use.
- CSPRNG can initialize before user-mode or network services.
- Entropy quality is reported honestly; degraded randomness is visible.

## Implementation Order

| ⭐  | Order | Deliverable                                | Depends On         | Status |
| --- | :---: | ------------------------------------------ | ------------------ | :----: |
| 💎  |   1   | Entropy source inventory and quality model | T04 §2             |  [ ]   |
| 💎  |   2   | EFI_RNG_PROTOCOL collection                | §1                 |  [ ]   |
| 💎  |   3   | CPU RDRAND/RDSEED collection               | §1, T09 §1         |  [ ]   |
| 💎  |   4   | TPM RNG collection                         | §1, T13 §2         |  [ ]   |
| 💎  |   5   | Boot timing and interrupt jitter mix-in    | §1                 |  [ ]   |
| 💎  |   6   | Seed file carryover lifecycle              | §1, T24 §3,§4      |  [ ]   |
| 💎  |   7   | boot_info seed handoff                     | T01 §4             |  [ ]   |
| 💎  |   8   | Kernel early CSPRNG seeding                | §7, D02T03 §5      |  [ ]   |
| ⭐  |   9   | Entropy diagnostics and policy gates       | §1-§8              |  [ ]   |
| 💎  |  10   | Entropy tests                              | §1-§9              |  [ ]   |

## 1. Entropy Source Inventory and Quality Model

- [ ] Define source classes: firmware RNG, CPU RNG, TPM RNG, ACPI OEM0 table, seed file, guest hwrng (virtio-rng, post-PCI only -- never pre-KASLR; collection owned by `04-drivers-hardware/TODO-09` §12), jitter, time.
- [ ] Pin the conditioner: source-tagged, length-framed transcript hashed with Blake2b (Monocypher, D02T03 §5) -- never raw XOR, so weak sources cannot cancel strong ones.
- [ ] Define bootloader-seed trust/credit policy (Linux RANDOM_TRUST_BOOTLOADER analog): descriptor quality bits are advisory; the kernel decides credit.
- [ ] Assign quality bits conservatively and log degraded states.
- [ ] Define minimum quality for Secure Boot release mode and debug mode.
- [ ] Add source mask to boot diagnostics.
- [ ] Commit: `"boot: early entropy source model"`

**Test checkpoint:** Serial log shows one `entropy:` summary line listing each source class with its quality class (e.g. `entropy: fw=ok cpu=ok tpm=none seed=none jitter=low time=low`); degraded sources log at WARN. QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## 2. EFI_RNG_PROTOCOL Collection

- [ ] Locate EFI_RNG_PROTOCOL before ExitBootServices.
- [ ] Query supported algorithms when available.
- [ ] Collect at least 64 bytes and mix into seed buffer.
- [ ] Collect ACPI OEM0 entropy table bytes when present (Win11 parity; raw table access via T04 §2 catalog).
- [ ] Time out and degrade cleanly on broken firmware.
- [ ] Commit: `"boot: collect firmware RNG entropy"`

**Test checkpoint:** On OVMF with RNG support the bootloader logs `RNG: EFI_RNG_PROTOCOL N bytes` before ExitBootServices; on firmware without the protocol it logs the degraded path and boot continues. QEMU WHPX, QEMU TCG (OVMF), VirtualBox, bare metal.

---

## 3. CPU RDRAND/RDSEED Collection

- [ ] Use CPUID-gated RDSEED first, RDRAND second.
- [ ] Retry bounded times and reject all-zero/repeated output.
- [ ] Mix CPU vendor/family/model only as non-secret personalization.
- [ ] Record source health in boot_info.
- [ ] Commit: `"boot: collect CPU RNG entropy"`

**Test checkpoint:** Platforms with RDSEED log `RNG: RDSEED N bytes`; TCG `qemu64` (no RDSEED) falls back to RDRAND or degrades without crashing (CPUID-gated, never #UD). QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## 4. TPM RNG Collection

- [ ] Use TPM2_GetRandom when TPM transport is available.
- [ ] Mix TPM output with other sources rather than trusting it alone.
- [ ] Degrade cleanly on no TPM, timeout, or error.
- [ ] Include TPM RNG availability in measured boot report.
- [ ] Commit: `"boot: collect TPM RNG entropy"`

**Test checkpoint:** With QEMU `-tpm` (swtpm) the log shows `RNG: TPM2_GetRandom N bytes`; without a TPM the source reads `tpm=none` and boot continues. Known TCG TPM-init freeze (`project_tcg_tpm_freeze`) must not regress. QEMU WHPX, QEMU TCG, VirtualBox, bare metal (test laptop has fTPM).

---

## 5. Boot Timing and Interrupt Jitter Mix-In

- [ ] Mix FPDT/TSC boot timings as low-quality personalization.
- [ ] Add optional PIT/LAPIC jitter sampling after interrupts are available.
- [ ] Never count deterministic VM timing as high-quality entropy.
- [ ] Add VM/hypervisor detection caveats to diagnostics.
- [ ] Commit: `"boot: mix timing jitter into entropy pool"`

**Test checkpoint:** Source mask shows `jitter=low time=low` (never high) on every platform; on detected hypervisors the diagnostics line carries the VM caveat. QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## 6. Seed File Carryover Lifecycle

- [ ] Store `X:\Boot\random-seed.bin` with version, counter, and MAC.
- [ ] Anti-clone system token: per-machine secret in a UEFI NVRAM variable (TPM NV fallback) bound into the seed-file MAC/KDF -- a cloned image with a copied seed file fails closed to degraded (systemd-boot parity).
- [ ] Read seed during boot, mix once, then rotate after kernel CSPRNG is ready.
- [ ] Prevent seed reuse after crash/power loss where possible.
- [ ] Handle read-only/recovery media without blocking boot.
- [ ] Commit: `"boot: random seed carryover file"`

**Test checkpoint:** After two consecutive boots `X:\Boot\random-seed.bin` exists with an incremented counter and valid MAC; deleting it degrades to `seed=none` without blocking boot; the rotate happens only after the kernel CSPRNG signals ready. QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## 7. boot_info Seed Handoff

- [ ] Add typed seed payload via TODO-01 with address, length, source mask, quality bits, and checksum. Use `BOOT_PAYLOAD_RANDOM_SEED` from [`include/kernel/boot_info.h`](../../include/kernel/boot_info.h); the TODO-01 §4 validator rejects overlap with boot_info / rt_mmap / USB DMA / framebuffer before CSPRNG seeds against the buffer. -> XREF: [`01-boot-platform/TODO-01 §4`](TODO-01-boot-protocol-abi-handoff.md#4-optional-payload-descriptor-array)
- [ ] Accept and concatenate a seed from an earlier boot stage instead of overwriting it (Linux EFI config-table pattern); the kernel mixes both.
- [ ] Ensure PMM reserves seed memory until CSPRNG consumes it.
- [ ] Zero seed memory after consumption.
- [ ] Add ABI tests for seed descriptor.
- [ ] Commit: `"boot: hand off early entropy seed"`

**Test checkpoint:** Kernel logs `entropy: seed payload N bytes (mask=0x.., quality=..)` during Phase 0/1; the TODO-01 §4 payload validator accepts the descriptor; after CSPRNG consumption a debug read of the seed buffer shows zeros. QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## 8. Kernel Early CSPRNG Seeding

- [ ] Add `early_entropy_init()` before KASLR/ASLR consumers where possible.
- [ ] Feed Monocypher/ChaCha CSPRNG once available.
- [ ] Provide temporary bounded random API with explicit degraded flag.
- [ ] Block release-mode cryptographic operations until seeded.
- [ ] This section delivers the FIRST seed only; runtime reseeding (`csprng_add_entropy()`, thresholds, interrupt-timing accounting, hwrng hooks) is owned by `../02-kernel-core/TODO-03-kernel-libraries.md` §5.
- [ ] Commit: `"kernel: seed early CSPRNG from boot entropy"`

**Test checkpoint:** `early_entropy_init()` logs before KASLR/canary consumers; `csprng_fill()` (D02T03 §5) succeeds post-seed; with all sources forced off the temporary API returns the degraded flag and release-mode crypto refuses. QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## 9. Entropy Diagnostics and Policy Gates

- [ ] Log source mask and quality class without exposing seed bytes.
- [ ] Show degraded randomness in VPD/recovery if release policy fails.
- [ ] Add `HKLM\SYSTEM\Boot\Entropy`.
- [ ] Admin external-entropy injection (Win11 ExternalEntropy parity): one-shot consume with overwrite-before-use, bytes never logged, mask-only diagnostics.
- [ ] Add BlackBox report.
- [ ] Commit: `"boot: entropy diagnostics and policy"`

**Test checkpoint:** `HKLM\SYSTEM\Boot\Entropy` holds source mask + quality class (no seed bytes anywhere in registry, logs, or BlackBox); a forced-degraded boot in release policy shows the VPD/recovery indication. QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

---

## 10. Entropy Tests

- [ ] Unit tests for source mixing determinism with fixtures (pure mixer fed fixed inputs produces the known digest).
- [ ] Test no source -> degraded but safe (mask=0 yields degraded flag, never a fake high-quality claim).
- [ ] Test seed file format: version/counter/MAC encode + decode round-trip, bad-MAC rejection (pure helpers, no live VFS).
- [ ] Test zeroization after consumption (consume fixture buffer, assert all-zero).
- [ ] Test cloned seed file: valid MAC but missing/different system token fails closed to degraded.
- [ ] Test external-entropy one-shot: consumed exactly once, source overwritten before use.
- [ ] Commit: `"test: early entropy handoff"`

**Test checkpoint:** `bash scripts/test.sh SUITE=security` runs the entropy suites with 0 failures; suites use fixtures and pure helpers only (no live boot infrastructure, per test policy). QEMU WHPX, QEMU TCG.

---

## OS Comparison

| ⭐  | Feature                        | 🪟 Win11                   | 🐧 Linux                    | 🚀 Impossible OS            |
| --- | ------------------------------ | -------------------------- | --------------------------- | --------------------------- |
| 💎  | Firmware RNG collection        | ✅ boot entropy via CNG    | ✅ EFI RNG seeds random     | ⬜ §2 EFI_RNG_PROTOCOL      |
| 💎  | CPU RDRAND/RDSEED              | ✅ CNG source              | ✅ arch_random + mix        | ⬜ §3 CPUID-gated           |
| 💎  | TPM RNG mix                    | ✅ TPM-backed entropy      | ✅ tpm-rng driver           | ⬜ §4 TPM2_GetRandom        |
| 💎  | Jitter/timing mix-in           | ✅ interrupt timing        | ✅ jitterentropy             | ⬜ §5 low-quality only      |
| 💎  | Seed carryover file            | ✅ registry system secrets | ✅ /var/lib random-seed     | ⬜ §6 versioned + MAC       |
| 💎  | Early kernel CSPRNG seeding    | ✅ before ASLR consumers   | ✅ random_init early        | ⬜ §8 pre-KASLR             |
| ⭐  | Visible entropy quality report | ❌ hidden                  | ⚠️ dmesg only               | ⬜ §9 registry + BlackBox   |

> **Parity:** rows track Win11 CNG boot entropy and Linux random.c; all sections planned (⬜). The ⭐ §9 visible quality report (registry + BlackBox + VPD degraded indication) goes beyond both.

---

## Unit Tests

> Wire into `src/kernel/test/test_runner.c` / `test_runner_init()` via `test_register_entropy()` (new `src/kernel/test/test_entropy.c`, TEST_CAT_SECURITY). Tests use fixtures + pure helpers only; live EFI/TPM/VFS collection paths are validated by boot checkpoints.

- [ ] `test_entropy_source_mask` -- mask bit per source class; degraded classification for mask=0
- [ ] `test_entropy_mix_fixture` -- fixed-input mixer determinism (known digest)
- [ ] `test_random_seed_format` -- seed file version/counter/MAC round-trip + bad-MAC rejection
- [ ] `test_entropy_seed_zeroized` -- buffer all-zero after consumption
- [ ] `test_random_seed_clone_rejected` -- valid-MAC seed file without the machine's system token classifies degraded
- [ ] `test_external_entropy_oneshot` -- injected entropy consumed once, source overwritten before use
- [ ] Register in `test_runner_init()`: `test_register_entropy()`

---

## Verification

- [ ] `bash scripts/build.sh clean` -> `tail -1 build/build.log` -> `=== BUILD OK ===`
- [ ] `bash scripts/test.sh SUITE=security` -> entropy suites PASS, 0 failures
- [ ] `bash scripts/test-smoke.sh` -> boots to `C:\>`; stripped log shows the `entropy:` source-mask line
- [ ] QEMU OVMF without RNG protocol degrades visibly (serial shows degraded WARN) (manual -- needs RNG-less OVMF build)
- [ ] QEMU with EFI RNG: `entropy: fw=ok` in serial
- [ ] Bare metal with RDRAND: `cpu=ok` (manual -- run on rig)
- [ ] Bare metal with TPM RNG: `tpm=ok` (manual -- run on rig)
- [ ] Commit: `"boot: early entropy verified -- sources, seed handoff, CSPRNG"`

**Test runner:** `scripts\debug\kernel\run-security-tests.bat` (SUITE=security) | entropy suites, 0 failures

