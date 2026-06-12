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
> **Current state (2026-06-12):** §1 shipped the kernel entropy model (`include/kernel/entropy.h` + `entropy.c`: source classes, packed quality, classify/policy/framing/report). §2 shipped bootloader collection: `collect_boot_entropy()` gathers EFI_RNG_PROTOCOL + ACPI OEM0 bytes pre-EBS and publishes a `BOOT_PAYLOAD_RANDOM_SEED` descriptor (FLAG_RESERVED). §3 shipped CPU RDSEED/RDRAND collection into the same payload. §5 shipped the src-7 TIME record, the post-`sti` jitter sampler, and the kernel staged transcript (`entropy_stage_source()`/`entropy_staged_drain()`). §4 shipped TPM RNG collection (`entropy_collect_tpm()` over the TODO-13 §2 transport, budgeted + floor-gated). Still missing: seed file lifecycle (§6), kernel-side descriptor consumption + zeroing (§7), CSPRNG seeding (§8 -- `csprng_fill()` itself is owned by `02-kernel-core/TODO-03` §5). `TODO-10` §11 canary + §14 KASLR still use RDRAND directly until §8 lands.

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
| 💎  |   1   | Entropy source inventory and quality model | T04 §2             |  [x]   |
| 💎  |   2   | EFI_RNG_PROTOCOL collection                | §1                 |  [x]   |
| 💎  |   3   | CPU RDRAND/RDSEED collection               | §1, T09 §1         |  [x]   |
| 💎  |   4   | TPM RNG collection                         | §1, T13 §2         |  [x]   |
| 💎  |   5   | Boot timing and interrupt jitter mix-in    | §1                 |  [x]   |
| 💎  |   6   | Seed file carryover lifecycle              | §1, T24 §3,§4      |  [ ]   |
| 💎  |   7   | boot_info seed handoff                     | T01 §4             |  [ ]   |
| 💎  |   8   | Kernel early CSPRNG seeding                | §7, D02T03 §5      |  [ ]   |
| ⭐  |   9   | Entropy diagnostics and policy gates       | §1-§8              |  [ ]   |
| 💎  |  10   | Entropy tests                              | §1-§9              |  [ ]   |

## 1. Entropy Source Inventory and Quality Model

- [x] 8 source classes in `include/kernel/entropy.h` as u32 mask + u32 packed quality, carried in the §7 seed-payload header (not the generic descriptor); guest hwrng owned by `04-drivers-hardware/TODO-09` §12.
- [x] Conditioner contract pinned + `entropy_frame_source()` shipped: source-tagged length-framed transcript with wrap guard, hard-fail on no-fit; Blake2b hash lands with D02T03 §5.
- [x] Bootloader-seed trust policy: descriptor quality is advisory -- `entropy_classify()` re-derives the class and never credits JITTER/TIME even when marked HIGH.
- [x] Conservative quality: `entropy_record_source()` clamps JITTER/TIME to LOW, reserved value 3 records as NONE, Q_NONE retracts a source.
- [x] Policy minimums: `entropy_policy_ok()` -- release requires >= MINIMUM (one HIGH hardware source); debug proceeds with WARN.
- [x] Source mask in boot diagnostics: `entropy_report()` one-line summary from the Phase 3 path (`boot_desktop.c`), WARN when degraded.
- [x] Commit: `"boot: early entropy source model"`

**Test checkpoint:** Serial log shows one `entropy:` summary line listing each source class with its quality class (e.g. `entropy: fw=none cpu=none ... (class=degraded)` until collectors land); degraded class logs at WARN. QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\kernel\run-security-tests.bat` (SUITE=security) | 5 entropy suites in `test_entropy.c`, 0 failures

> **Notes:**
> - Shipped `include/kernel/entropy.h` + `src/kernel/entropy.c`: 8 source classes, packed quality, classify/policy/framing/record/report functions.
> - Runs from the Phase 3 boot path: collectors (§2-§6) record sources as they land; `entropy_report()` prints the honest all-none line until then.
> - Downstream: §7 descriptor mirrors the mask+quality pair; D02T03 §5 CSPRNG consumes the transcript framing; Codex test-coverage adoptions in the section commit.
> - Canonical contract doc: `include/kernel/entropy.h` header comment (conditioner transcript + advisory-quality policy).
> - Scope boundary: no collection here -- EFI RNG (§2), CPU (§3), TPM (§4), jitter (§5), seed file (§6), virtio-rng (D04T09 §12).
> - re-adversarial skipped in review: fix diff was header-comment + TODO wording only.

> **Verified:** 2026-06-12 | commit `6ad5d2ac` | 6/6 items | build OK | smoke PASS (KVM 2.620s)
> **Quality reviewed:** 2026-06-12 | Codex 7x (design, test-coverage, adversarial x2, re-adversarial, consistency, perf) | 3H+5M+1L fixed, 0 open | scope: kernel-code-quality

---

## 2. EFI_RNG_PROTOCOL Collection

- [x] `collect_boot_entropy()` (bootx64.c, step 4d2 pre-EBS): locates EFI_RNG_PROTOCOL (GUID + struct added to `efi.h` per UEFI 2.10 37.5), collects 64 bytes via one `GetRNG(NULL)` call into a framed seed transcript.
- [x] GetInfo deliberately SKIPPED (nonessential firmware call that could hang; the default algorithm is consumed either way) -- design review adoption, see commit.
- [x] ACPI OEM0 table bytes collected when present (Win11 parity): `bl_find_acpi_table()` RSDP->XSDT/RSDT walk with signature/length/checksum gates; payload capped 512 bytes.
- [x] Seed page published as `BOOT_PAYLOAD_RANDOM_SEED` descriptor (FLAG_VALID|FLAG_RESERVED) so PMM cannot reclaim raw seed bytes; empty collection zeroes + frees the page.
- [x] Degrade contract: every absent/failed source logs `[BOOT] RNG:` and boot continues; `firmware_rng=off` boot.conf escape hatch (boot_config `_reserved` byte, both headers + manifest + docs) for firmware whose GetRNG never returns.
- [x] Commit: `"boot: collect firmware RNG entropy"`

**Test checkpoint:** On firmware with RNG support the bootloader logs `[BOOT] RNG: EFI_RNG_PROTOCOL 64 bytes` + `RNG: seed payload N bytes` before ExitBootServices; on firmware without the protocol it logs the degraded path and boot continues (verified: stock OVMF logs absent + degraded, smoke PASS). QEMU WHPX, QEMU TCG (OVMF), VirtualBox, bare metal.

> **Test runner:** N/A (pure UEFI bootloader code) | validation: smoke PASS + `[BOOT] RNG:` serial lines; kernel-side parsing tests land with §7

> **Notes:**
> - Shipped `collect_boot_entropy()` + `bl_find_acpi_table()` + `bl_entropy_frame()` in bootx64.c, EFI_RNG_PROTOCOL defs in efi.h, `firmware_rng` boot_config knob (mirror + kernel header + ABI manifest + docs row + POST16 0xB034/0xB035).
> - Runs at boot step 4d2 (after FPDT, pre-EBS); transcript mirrors the `entropy.h` framing so the §7/§8 kernel consumer parses one format.
> - Downstream: §7 consumes the BOOT_PAYLOAD_RANDOM_SEED descriptor (reserve, parse, zero); Codex design adoptions in the section commit.
> - Canonical contract doc: `include/kernel/entropy.h` (framing) + `docs/boot/boot-info-fields.md` (`firmware_rng` row).
> - Scope boundary: kernel-side consumption/zeroing is §7; TPM RNG is §4; the hang limitation is an escape hatch, not a timeout (no pre-EBS preemption).
> - Review fixes: firmware_rng gate scoped to the EFI RNG block only (OEM0 unaffected); bl_entropy_frame mirrors the kernel twin refusals; re-adversarial skipped (20-line control-flow + doc fix).

> **Verified:** 2026-06-12 | commit `65828c7e` | 5/5 items | build OK | smoke PASS (KVM, RNG-absent degrade path)
> **Quality reviewed:** 2026-06-12 | Codex 7x (design, adversarial x2, adversarial-impl, re-adversarial, consistency, perf) | 2H+3M+1L fixed, 0 open | scope: boot-code-quality

---

## 3. CPU RDRAND/RDSEED Collection

- [x] `bl_collect_cpu_rng()` (bootx64.c): RDSEED preferred, RDRAND fallback, both behind max-leaf-gated CPUID probes (leaf 0 first, leaf 7.0 EBX bit 18 / leaf 1 ECX bit 30 -- kernel `cpuid.c` discipline) so missing leaves degrade instead of #UD.
- [x] Bounded retries (`BL_RDSEED_RETRIES` 1024 with pause per qword, `BL_RDRAND_RETRIES` 10); whole sample rejected on all-zero or all-identical qwords (stuck-DRNG heuristic, covers 0xFF fill).
- [x] CPU vendor string + family/model/stepping appended to the ACCEPTED RNG payload as non-secret personalization -- never a standalone record, so a framed src-1 record always implies real RNG output (design review adoption).
- [x] Source health encoded by the framed records in the published seed payload (verified live: `RNG: RDSEED 64 bytes` + 85-byte payload on KVM); the in-payload mask/quality header is §7.
- [x] Commit: `"boot: collect CPU RNG entropy"`

**Test checkpoint:** Platforms with RDSEED log `[BOOT] RNG: RDSEED 64 bytes` (verified on KVM, seed payload published); TCG `qemu64` (no RDSEED) falls back to RDRAND or degrades without crashing (CPUID-gated, never #UD). QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Test runner:** N/A (pure UEFI bootloader code) | validation: smoke PASS + `[BOOT] RNG: RDSEED 64 bytes` + seed payload line on KVM

> **Notes:**
> - Shipped `bl_cpuid()`/`bl_rdseed64()`/`bl_rdrand64()`/`bl_collect_cpu_rng()` in bootx64.c; runs inside `collect_boot_entropy()` between the EFI RNG and OEM0 sources.
> - Sample = 8 qwords RDSEED (RDRAND-grade when any qword fell back) + 16 identity bytes, framed as one src-1 record; intermediates zeroed after framing.
> - Downstream: first live producer exercising the §2 descriptor publication end-to-end (85-byte payload verified on KVM); §7 parses the record.
> - Canonical contract doc: `include/kernel/entropy.h` framing + source classes.
> - Scope boundary: kernel-side AT_RANDOM/canary RDRAND users stay on `src/kernel/random.c` until §8 seeds the CSPRNG.
> - re-adversarial skipped in review: fix was a 6-line contract comment update.

> **Verified:** 2026-06-12 | commit `8c0bb636` | 4/4 items | build OK | smoke PASS (KVM, RDSEED 64 bytes live)
> **Quality reviewed:** 2026-06-12 | Codex 6x (design, adversarial x2, adversarial-impl, consistency, perf) | 2H+1M+1L fixed, 0 open | scope: boot-code-quality

---

## 4. TPM RNG Collection

- [x] TPM2_GetRandom over the TODO-13 §2 transport: `entropy_collect_tpm()` (`entropy.c`) via `tpm2_get_random_bounded()` (`tpm_transport.c`; 2000ms cumulative budget, 48-byte per-call cap, 64-byte target).
- [x] Mix, never trust alone: staged as one src-2 record in the framed transcript (hashed with all sources at §8); 32-byte floor + periodicity scan (rejects period p <= n/2: constant fill, replayed chunks) gate the HIGH credit.
- [x] Degrade cleanly: absent/wedged transport skips silently (report shows `tpm=none`); short yield or protocol failure logs WARN with no credit; boot continues.
- [x] TPM RNG availability in the measured boot report: `tpm_rng_available` field in `boot_integrity_report` + `tpm_integrity_set_rng_available()` (set only on credited collection).
- [x] Commit: `"boot: collect TPM RNG entropy"`

**Test checkpoint:** With QEMU `-tpm` (swtpm) the log shows `RNG: TPM2_GetRandom 64 bytes` and the report line `tpm=ok`; without a TPM the source reads `tpm=none` and boot continues (verified live on KVM smoke). Known TCG TPM-init freeze (`project_tcg_tpm_freeze`) must not regress. QEMU WHPX, QEMU TCG, VirtualBox, bare metal (test laptop has fTPM).

> **Test runner:** `scripts\debug\kernel\run-security-tests.bat` (SUITE=security) | 4 TPM suites in `test_tpm_transport.c` (2 added with this section: GetRandom marshal+parse, RNG collection via fake TIS), 0 failures

> **Notes:**
> - Shipped `entropy_collect_tpm()` in `entropy.c` + `tpm2_build_get_random()`/`tpm2_parse_get_random()`/`tpm2_get_random_bounded()` in `tpm_transport.c` + `tpm_rng_available` report field in `tpm.h`/`tpm.c`.
> - Runs once in Phase 1 right after `tpm_transport_init()` (boot_interrupts.c); the whole GetRandom sequence shares one 2000ms cumulative budget so a slow TPM cannot stall boot.
> - Downstream: §8 first seed consumes the staged src-2 record; D02T03 §5 runtime reseed and the TODO-04 (drivers) command layer reuse `tpm2_get_random_bounded()`; Codex design + test-coverage adoptions in the section commit.
> - Canonical contract doc: `include/kernel/tpm_transport.h` (GetRandom helpers) + `include/kernel/entropy.h` (collector contract).
> - Scope boundary: first-seed collection only -- runtime reseed cadence is D02T03 §5; attestation/PCR use of the report flag is `01-boot-platform/TODO-13` §9.

> **Verified:** 2026-06-12 | commit `ee546157` | 5/5 items | build OK | smoke PASS (KVM 2.560s)
> **Quality reviewed:** 2026-06-12 | Codex 10x (design, adversarial x2, test-coverage, consistency, perf, re-adversarial x4) | 4H+6M fixed, 0 open | scope: kernel-code-quality

---

## 5. Boot Timing and Interrupt Jitter Mix-In

- [x] Boot timing personalization: bootloader src-7 TIME record (bl_entry, reset_end, tsc_freq, rdtsc; 32 bytes); `collect_boot_entropy()` moved after the TSC measurement, record refused when tsc_freq is 0.
- [x] Jitter sampling: `entropy_collect_jitter()` in `boot_interrupts.c` at the earliest post-`sti` point (so the §8 first seed sees it); 64 rdtsc deltas across variable pause loops, staged.
- [x] Staged transcript in `entropy.c`: `entropy_stage_source()` (success-return, sticky overflow flag, quality only on success), `entropy_staged_drain()` (atomic copy+zero+reset, all-or-nothing), `entropy_staged_consume_zero()`.
- [x] VM timing never high: §1 model clamps JITTER/TIME to LOW; live report shows `jitter=low` and the sampler logs the vm caveat (verified on KVM).
- [x] Commit: swept into `c9f2ca5d` (run-2 died mid-section) + `dd19d586` (drain refactor) in place of the planned `"boot: mix timing jitter into entropy pool"`.

**Test checkpoint:** Serial shows `[BOOT] RNG: boot timing 32 bytes` + `entropy: jitter: 64 samples staged (LOW...)` and the report line shows `jitter=low` never high (verified on KVM, 122-byte seed payload). QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\kernel\run-security-tests.bat` (SUITE=security) | 6 entropy suites incl. staged transcript, 0 failures

> **Notes:**
> - Shipped: src-7 TIME record in the bootloader seed payload; `entropy_collect_jitter()` post-`sti` in `boot_interrupts.c`; staged-transcript APIs (768-byte cap) in `entropy.c`.
> - Runs at the earliest post-interrupt point so the staged jitter is available to the §8 first seed; staging failure sets a sticky overflow flag and never records quality.
> - Downstream: §6 seed-file bytes and D04T09 §12 virtio-rng use `entropy_stage_source()`; §8 hashes boot payload + staged transcript then `consume_zero()`.
> - Canonical contract doc: `include/kernel/entropy.h` (staged transcript block).
> - Scope boundary: ongoing runtime reseed cadence is D02T03 §5 (`csprng_add_entropy`); this section only feeds the first seed.

> **Verified:** 2026-06-12 | commit `dd19d586` (+`c9f2ca5d` sweep) | 5/5 items | build OK | smoke PASS (KVM 2.570s)
> **Quality reviewed:** 2026-06-12 | Codex 4x (adversarial, consistency, perf, re-adversarial) | 3M+1L fixed, 0 open | scope: kernel-code-quality + boot-code-quality

---

## 6. Seed File Carryover Lifecycle

> [!NOTE]
> **UNBLOCKED (2026-06-12):** the user authorized Monocypher vendoring and `02-kernel-core/TODO-03` §5 SHIPPED (Monocypher 4.0.2 + kernel CSPRNG + `NtGetRandom`). HMAC-Blake2b for the seed-file MAC/KDF builds on `crypto_blake2b()`; the CSPRNG-ready signal for the rotate step is `csprng_is_seeded()`.

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
- [ ] Boundary (D02T03 §5 shipped): `csprng_init()` already drains the staged jitter/TPM transcript at Phase 1; this section owns the boot_info seed PAYLOAD mix (via §7 + `csprng_add_entropy()`), KASLR-consumer ordering, and policy gates.
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
| 💎  | Entropy quality/credit model   | ✅ internal pool credit    | ✅ credit accounting        | ✅ §1 mask+class+policy     |
| 💎  | Firmware RNG collection        | ✅ boot entropy via CNG    | ✅ EFI RNG seeds random     | ✅ §2 + OEM0, framed seed   |
| 💎  | CPU RDRAND/RDSEED              | ✅ CNG source              | ✅ arch_random + mix        | ✅ §3 RDSEED-first, gated   |
| 💎  | TPM RNG mix                    | ✅ TPM-backed entropy      | ✅ tpm-rng driver           | ✅ §4 budgeted + floor-gated |
| 💎  | Jitter/timing mix-in           | ✅ interrupt timing        | ✅ jitterentropy             | ✅ §5 staged, LOW-clamped   |
| 💎  | Seed carryover file            | ✅ registry system secrets | ✅ /var/lib random-seed     | ⬜ §6 versioned + MAC       |
| 💎  | Early kernel CSPRNG seeding    | ✅ before ASLR consumers   | ✅ random_init early        | ⬜ §8 pre-KASLR             |
| ⭐  | Visible entropy quality report | ❌ hidden                  | ⚠️ dmesg only               | ⬜ §9 registry + BlackBox   |

> **Parity:** rows track Win11 CNG boot entropy and Linux random.c; §1-§5 shipped (model, firmware+OEM0, CPU RNG, TPM RNG, jitter/timing), §6-§9 still planned (⬜). The ⭐ §9 visible quality report (registry + BlackBox + VPD degraded indication) goes beyond both.

---

## Unit Tests

> Wire into `src/kernel/test/test_runner.c` / `test_runner_init()` via `test_register_entropy()` (`src/kernel/test/test_entropy.c`, TEST_CAT_SECURITY). Tests use fixtures + pure helpers only; live EFI/TPM/VFS collection paths are validated by boot checkpoints.

- [x] Shipped with §1 (5 suites): quality slot packing, classification (degraded/minimum/good + descriptor-bypass guard), policy gate, transcript framing (exact-fit/wrap/no-write-on-refusal), record + clamp + Q_NONE retraction
- [x] Shipped with §5 (6th suite): staged transcript -- stage/credit-visibility, NULL + too-small drain guards, full drain + empty re-drain, oversized refusal leaves record intact + overflow flag, consume_zero reset
- [x] Shipped with §4 (in `test_tpm_transport.c`, TEST_CAT_SECURITY): GetRandom marshal/parse negatives, bounded-loop request-cap protocol, RNG collection via fake TIS (floor boundary both sides, stuck heuristics, report flag, no-transport no-op)
- [x] Register in `test_runner_init()`: `test_register_entropy()` (`test_runner.c`)
- [ ] `test_entropy_mix_fixture` -- fixed-input Blake2b transcript determinism (lands with §8 once D02T03 §5 vendors Monocypher)
- [ ] `test_random_seed_format` -- seed file version/counter/MAC round-trip + bad-MAC rejection (§6)
- [ ] `test_random_seed_clone_rejected` -- valid-MAC seed file without the machine's system token classifies degraded (§6)
- [ ] `test_entropy_seed_zeroized` -- buffer all-zero after consumption (§7)
- [ ] `test_external_entropy_oneshot` -- injected entropy consumed once, source overwritten before use (§9)

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

