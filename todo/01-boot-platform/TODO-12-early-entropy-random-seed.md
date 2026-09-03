---
schema_version: 1
id: early-entropy-random-seed
domain: 01-boot-platform
status: active
title: "TODO-12 -- Early Entropy & Random Seed Handoff"
---

# TODO-12 -- Early Entropy & Random Seed Handoff

> **Validated:** 2026-06-21 | backfill -- todo-graph structural validate clean; all sections shipped + reviewed
> **Gap-audited:** 2026-06-21 | backfill -- triage DONE (sections shipped + quality-reviewed / deferred); Stages 1-2 predate this marker

> **Goal:** Provide trustworthy randomness as early as possible. Secure Boot, TPM, code integrity, log integrity, ASLR, stack canaries, and cryptographic services all need entropy, but the boot platform currently has no owned plan for firmware RNG, CPU RNG, TPM RNG, seed carryover, or early-kernel CSPRNG seeding.

> [!IMPORTANT]
> **Current state (2026-06-12):** §1 shipped the kernel entropy model (`include/kernel/entropy.h` + `entropy.c`: source classes, packed quality, classify/policy/framing/report). §2 shipped bootloader collection: `collect_boot_entropy()` gathers EFI_RNG_PROTOCOL + ACPI OEM0 bytes pre-EBS and publishes a `BOOT_PAYLOAD_RANDOM_SEED` descriptor (FLAG_RESERVED). §3 shipped CPU RDSEED/RDRAND collection into the same payload. §5 shipped the src-7 TIME record, the post-`sti` jitter sampler, and the kernel staged transcript (`entropy_stage_source()`/`entropy_staged_drain()`). §4 shipped TPM RNG collection (`entropy_collect_tpm()` over the TODO-13 §2 transport, budgeted + floor-gated). §6 shipped the seed-file carryover lifecycle (`seed_file.c`: MAC'd format, NVRAM anti-clone token, Phase-3 reseed + crash-tolerant rotation). §7 shipped the boot_info seed handoff (digest-chained first-seed consumption + zeroing + pre-EBS carryover read). §8 shipped the named `early_entropy_init()` point, the CSPRNG-owned credited class, the unconditional key-generation gate `csprng_crypto_ok()`, and `csprng_fill_classified()`. §9 shipped the diagnostics surfaces (HKLM\SYSTEM\Boot\Entropy mirror, X:\Diag\entropy.json, splash degraded warning, external-entropy one-shot -- cross-reboot activation blocked on D02 TODO-14 hive-load wiring). §10 shipped the test-sweep closure (15 entropy + 1 boot_seed + 2 csprng security-category suites; 3 added: clone-fail-closed-to-degraded composition, source-framed golden blake2b-256 KAT, and the `boot_seed_record_sources` credit gate). §11 shipped 2026-08-29 the `BOOT_CAP_PAYLOAD_DESCRIPTORS` capability gate on both seed-payload consumers (`boot_seed_consume` and the pre-IDT `canary_seed_desc_ok` peek) plus the pure `boot_seed_length_reservable()` length contract that the Phase-0 reservation pass and every Phase-1 consumer now read as the single rule -- so `BOOT_PAYLOAD_FLAG_RESERVED` is no longer treated as proof the pass pinned the range. Remaining: §9 cross-reboot ExternalEntropy (blocked on D02 TODO-14 §8) and the bare-metal Verification rows (RDRAND/TPM sign-off on the rig); the QEMU degrade-visible + EFI-RNG `fw=ok` rows were verified 2026-06-13.

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

| ⭐  | Order | Deliverable                                  | Depends On    | Status |
| --- | :---: | -------------------------------------------- | ------------- | :----: |
| 💎  |   1   | Entropy source inventory and quality model   | T04 §2        |  [x]   |
| 💎  |   2   | EFI_RNG_PROTOCOL collection                  | §1            |  [x]   |
| 💎  |   3   | CPU RDRAND/RDSEED collection                 | §1, T09 §1    |  [x]   |
| 💎  |   4   | TPM RNG collection                           | §1, T13 §2    |  [x]   |
| 💎  |   5   | Boot timing and interrupt jitter mix-in      | §1            |  [x]   |
| 💎  |   6   | Seed file carryover lifecycle                | §1, T24 §3,§4 |  [x]   |
| 💎  |   7   | boot_info seed handoff                       | T01 §4        |  [x]   |
| 💎  |   8   | Kernel early CSPRNG seeding                  | §7, D02T03 §5 |  [x]   |
| ⭐  |   9   | Entropy diagnostics and policy gates         | §1-§8         |  [/]   |
| 💎  |  10   | Entropy tests                                | §1-§9         |  [x]   |
| 💎  |  11   | Capability gate on the seed payload consumer | §3, §10       |  [x]   |

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
> - Downstream: §8 hashes boot payload + staged transcript then `consume_zero()`; §6 (shipped) and D04T09 §12 virtio-rng reseed later via `csprng_add_entropy()` instead (post-drain sources never stage).
> - Canonical contract doc: `include/kernel/entropy.h` (staged transcript block).
> - Scope boundary: ongoing runtime reseed cadence is D02T03 §5 (`csprng_add_entropy`); this section only feeds the first seed.

> **Verified:** 2026-06-12 | commit `dd19d586` (+`c9f2ca5d` sweep) | 5/5 items | build OK | smoke PASS (KVM 2.570s)
> **Quality reviewed:** 2026-06-12 | Codex 4x (adversarial, consistency, perf, re-adversarial) | 3M+1L fixed, 0 open | scope: kernel-code-quality + boot-code-quality

---

## 6. Seed File Carryover Lifecycle

> [!IMPORTANT]
> **Design decisions (Codex design review 2026-06-12) -- pinned before coding:**
> - **§6/§7 first-seed boundary (explicit).** X:\ (BlackBox FAT32) mounts in Phase 2/3, but `csprng_init()` runs in Phase 1 -- so the kernel-side seed FILE cannot feed the FIRST seed. §6 owns the seed-file FORMAT + per-machine NVRAM token + crash-tolerant rotation + a Phase-3 `csprng_add_entropy()` RESEED (defense-in-depth). The EARLY first-seed contribution (reading the same file format pre-ExitBootServices into the boot_info payload) is owned by §7. §6 is a runtime reseed/rotation/anti-clone feature, NOT the early entropy path.
> - **Crash safety.** FAT32 LFN rename is NOT power-fail-atomic (`docs/boot/boot-policy.md`, `docs/boot/boot-history-schema.md`); a `.tmp`+rename protocol is unsafe for the LFN name `random-seed.bin`. Use the established BlackBox dual-file protocol (write-new + flush + close-success THEN delete-old; highest-valid-counter-on-read). The future FAT32-safe atomic-replace primitive is `05-storage-filesystems/TODO-04 §16`.
> - **Anti-replay.** MAC over `version||counter||payload` stops field tampering but NOT replay of an old valid file. Persist the highest accepted counter in the NVRAM token record; reject `counter <= last_seen` (regenerate). One small NVRAM write per boot rotation (systemd-boot parity).
> - **Mismatch recovery.** On absent/changed NVRAM token (firmware reset, board swap, clone), do NOT mix the old payload, but once CSPRNG is seeded WRITE a fresh token-MACed seed file + log the clone/reset event -- never leave a permanently-degraded invalid file.
> - **TPM NV fallback NOT available.** Only `TPM2_GetRandom` exists today (no `TPM2_NV_Read/Write`); the token is UEFI-NVRAM-only. TPM NV fallback is a tracked follow-up (`04-drivers-hardware/TODO-13` TPM stack, or a new item when TPM NV ops land).

- [x] Seed-file format + pure helpers (`src/kernel/seed_file.c` + `.h`): 80-byte blob (magic/version/counter/payload/keyed-Blake2b MAC), `seed_file_encode()`/`seed_file_accept()` in fixed check order; rejections write no outputs.
- [x] Per-machine anti-clone token: `IPOSSeedToken` NVRAM record (secret[32] + last_seen u64) under `IMPOSSIBLE_OS_VENDOR_GUID`, minted from `csprng_fill()` only with `seed_file_hw_provenance()` (degraded boots defer); copied seeds fail closed.
- [x] Phase-3 read lifecycle (`seed_file_phase3()` in `boot_desktop.c`): both names read, highest fresh counter wins; NVRAM last_seen persist is the commit point -- HIGH credit only after persist, persist failure mixes uncredited (LOW).
- [x] Provenance-gated rotation (HIGH hardware source this boot OR freshly credited carryover -- degraded CSPRNG output is never persisted): rename-free double write; durable `vfs_flush()` boundary fixed at root (FAT32 `blkdev_sync`); wrap guard.
- [x] Degrade without blocking boot: unmounted X: / absent file / NVRAM-less firmware / write failure all WARN + continue; `boot.conf seed_file=off` knob (mirror + manifest + docs synced).
- [x] Commit: `"boot: random seed carryover file"` (08340720)

**Test checkpoint:** After two consecutive boots `X:\Boot\random-seed.bin` exists with an incremented counter and valid MAC; deleting it degrades to `seed=none` without blocking boot; a copied file with a mismatched NVRAM token fails closed AND the next boot writes a fresh valid file; an old-counter file is rejected (anti-replay); the rotate happens only after `csprng_is_seeded()`. Pure format helpers: encode/decode round-trip + bad-MAC + bad-counter rejection. QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\kernel\run-security-tests.bat` (SUITE=security) | 8 entropy suites incl. seed file format + clone rejection, 0 failures

> **Notes:**
> - Shipped `src/kernel/seed_file.c` + `include/kernel/seed_file.h` (80-byte MAC'd format, NVRAM token, Phase-3 lifecycle) plus five FAT32/VFS root-cause fixes: durable `vfs_flush()` (scache error propagation + `blkdev_sync` outside `vol->lock`), sector-cache coherence for multi-sector I/O, LFN-aware delete, unlink parent re-resolution, partition blkdev zero-init + flush forwarding.
> - Runs once from the Phase-3 boot path (`boot_desktop.c`, before `entropy_report()` so the summary reflects the seed source); one NVRAM write per boot (token mint or consumed-counter commit).
> - Downstream: section 7 reads the same file format pre-EBS into the boot_info payload (early first-seed); `vfs_flush()` is the new durability boundary for any write-then-flush protocol; Codex design + test-coverage adoptions in the section commit.
> - Canonical contract doc: `include/kernel/seed_file.h` header comment (format, threat model, crash-tolerance argument).
> - Scope boundary: section 7 owns the early read + descriptor consumption; FAT32-safe atomic-replace primitive stays `05-storage-filesystems/TODO-04 section 16`; TPM NV token fallback tracked in `04-drivers-hardware/TODO-13`.

> **Verified:** 2026-06-12 | commit `08340720` | 5/5 items | build OK | smoke PASS (KVM 2.690s), tests 4565+16 PASS, live 2-boot carryover (seed=ok class=good)
> **Accepted:** [H] FAT32 vol->lock cli-spinlock spans device I/O on writes while reads scan the cache unlocked -> XREF: 05-storage-filesystems/TODO-04 §15 (item: "FAT32 vol->lock concurrency overhaul" at line 454)
> **Accepted:** [H] dir-cache rebuilds can reassign a slot a live open handle aims at -> XREF: 05-storage-filesystems/TODO-04 §15 (item: "FAT32 cache-slot stability for live handles" at line 452)
> **Accepted:** [H] scache_evict discards failed writes + void FAT2 repair reports repaired (false durability) -> XREF: 05-storage-filesystems/TODO-04 §6 (item: "Repair-write remainder" at line 206)
> **Accepted:** [H] boot_audit/health writers ack NVRAM state on write+close alone -> XREF: 01-boot-platform/TODO-24 §5 (item: "Durable-write + write-success honesty retrofit for X:\ diagnostic writers" at line 188; renamed from "Durable-write retrofit for boot-state X:\ writers" by the TODO-24 drain `3ff5fe23c`)
> **Accepted:** [M] vfs_unmount never clean-marks FAT32 -> XREF: 05-storage-filesystems/TODO-04 §15 (item: "vfs_unmount filesystem hook" at line 455)
> **Accepted:** [M] LFN lookups (finddir/stat/read_dir) lack checksum/sequence run validation (delete validates) -> XREF: 05-storage-filesystems/TODO-04 §4 (item: "LFN run validation in lookups" at line 157)
> **Quality reviewed:** 2026-06-12 | Codex 16x (design, test-coverage, adversarial x2, adversarial-impl x5, re-adversarial x5, consistency, perf) | 1C+10H+9M+2L fixed, 4H+2M accepted-XREF, 2 rejected, 0 open | scope: kernel-code-quality + boot-code-quality

---

## 7. boot_info Seed Handoff

- [x] Typed seed payload: 32-byte versioned `entropy_seed_header` (twin mirrors) + whole-payload CRC-32C, `FLAG_CHECKSUMMED`. -> XREF: [`01-boot-platform/TODO-01 §4`](TODO-01-boot-protocol-abi-handoff.md#4-optional-payload-descriptor-array)
- [x] Earlier-stage mixing (Linux EFI config-table pattern): `boot_seed_consume()` walks EVERY `RANDOM_SEED` descriptor and digest-chains each accepted transcript (Blake2b-256), so any chain depth fits; ours appends, never overwrites.
- [x] EARLY first-seed boundary (per §6 design pin): `bl_collect_seed_carryover()` reads both rotation names pre-EBS as src-4 records; `seed_file_early_verify()` fails closed; `csprng_core_seed2()` folds into the FIRST key.
- [x] PMM reservation: `BOOT_PAYLOAD_FLAG_RESERVED` descriptor (reserved by `boot_reserved.c`); `boot_seed_desc_classify()` gates consumption; frames freed only for our producer's certified exclusive pages.
- [x] Zeroization: `boot_seed_release_payload()` (pure) wipes accepted AND rejected payloads; `FLAG_VALID` cleared post-consume; per-payload scratch + 32-byte digest wiped after `csprng_init()`.
- [x] ABI tests: `entropy_seed_parse` suites (header/CRC/framing gates, all-or-nothing NO_FIT, carryover routing, descriptor gate, zeroization matrix) in `test_entropy.c`; `csprng_core_seed2` full-state equivalence in `test_klibs.c`.
- [x] Commit: `"boot: hand off early entropy seed"`

**Test checkpoint:** Kernel logs `entropy: seed payload N bytes (mask=0x.., quality=..)` then `seed payload digest: N transcript bytes over M payload(s)` during Phase 1; the TODO-01 §4 payload validator accepts the descriptor; after CSPRNG consumption a debug read of the seed buffer shows zeros. QEMU WHPX, QEMU TCG, VirtualBox, bare metal. (KVM smoke 2026-06-12: `seed payload 154 bytes (mask=0x82, quality=0x4008): 2 records` -> `digest: 122 transcript bytes over 1 payload(s)` -> `csprng: seeded: transcript 135+32 bytes`.)

> **Test runner:** `scripts\debug\kernel\run-security-tests.bat` (SUITE=security) | entropy + klibs suites, 0 failures

> **Notes:**
> - Shipped: `src/kernel/main/boot_seed.c` consumer + `entropy_seed_parse()` pure parser + `csprng_core_seed2()` first-seed fold + bootloader publish path in `collect_boot_entropy()` (header, mask/quality, carryover read, CRC-32C).
> - Runs once on the BSP in Phase 1 before `csprng_init()`; each validated payload is digest-chained (Blake2b-256), wiped, and page-freed only for our producer's certified pages.
> - Downstream: §8 first-seed ordering partially delivered (payload folds into the initial key); Codex adoption details in the section + review commit messages.
> - Canonical doc: header twin contract in [`include/kernel/entropy.h`](../../include/kernel/entropy.h) (seed payload block).
> - Scope boundary: §6 owns seed-file format/token/rotation/credit; §7 owns the early read + handoff; §8 owns KASLR-consumer ordering + degraded API; §9 owns diagnostics surfaces.

> **Verified:** 2026-06-12 | commit `dff35172` | 6/6 items | build OK | smoke PASS (KVM 2.540s), 4634+16 tests
> **Quality reviewed:** 2026-06-12 | Codex 12x (design, test-coverage, adversarial, re-adversarial, consistency, perf) | 9H+4M+1L fixed, 0 open | scope: boot-code-quality + kernel-code-quality

---

## 8. Kernel Early CSPRNG Seeding

- [x] `early_entropy_init()` (`boot_seed.c`): the single named Phase-1 init point (consume -> first seed -> wipe -> class log), BEFORE every consumer (AT_RANDOM, AP canaries, KUSD cookie, GUIDs, future KASLR).
- [x] Feed Monocypher/ChaCha CSPRNG: `csprng_init()` drains local + staged sources AND the §7 payload digest into the FIRST Blake2b-derived key (shipped D02T03 §5 + §7; named init point here).
- [x] Degraded-flag API: `csprng_fill_classified()` fills (never blocks, BCryptGenRandom parity) and returns the CSPRNG-owned `csprng_credited_class()`; `rdrand_bytes()` stays the documented non-CSPRNG helper.
- [x] Key-generation crypto gate: `csprng_crypto_ok()` = seeded AND credited >= MINIMUM, UNCONDITIONALLY (mutable boot.conf debug never relaxes it); credited class bound to ABSORBED material, never the diagnostic record; `guid_generate()` migrated.
- [x] This section delivers the FIRST seed only; runtime reseeding (`csprng_add_entropy()`, thresholds, interrupt-timing accounting, hwrng hooks) is owned by `../02-kernel-core/TODO-03-kernel-libraries.md` §5.
- [x] Boundary (D02T03 §5 + §7 shipped): `csprng_init()` drains staged jitter/TPM AND folds the §7 payload into the FIRST key; this section owns only KASLR-consumer ordering, the degraded-flag API, and policy gates.
- [x] Commit: `"kernel: seed early CSPRNG from boot entropy"`

**Test checkpoint:** `early_entropy_init()` logs `early entropy init complete: credited class=.., release crypto ..` before any consumer; `csprng_fill()` succeeds post-seed; the credited class is insulated from diagnostic-record mutation (unit-tested) and release-mode `csprng_crypto_ok()` refuses on DEGRADED. QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\kernel\run-security-tests.bat` (SUITE=security) | klibs csprng crypto gate suite, 0 failures

> **Notes:**
> - Shipped: `early_entropy_init()` (`boot_seed.c`) + CSPRNG-owned credited class (`g_crypto_class`, `csprng.c`) + `csprng_crypto_ok()` gate + `csprng_fill_classified()` + `guid_generate()` CSPRNG migration (`gpt.c`).
> - Runs once on the BSP at Phase 1 inside the POST16_CSPRNG markers; credited class snapshotted at the first-seed install and upgraded only on absorbed HIGH material.
> - Downstream: key-generation consumers (future TLS/session keys) gate on `csprng_crypto_ok()`; `seed_file` mint keeps its STRICTER hw-provenance gate; Codex adoptions in the section commit message.
> - Canonical doc: gate + class contract in [`include/kernel/csprng.h`](../../include/kernel/csprng.h).
> - Scope boundary: runtime reseed thresholds/hwrng are D02T03 §5; diagnostics surfaces (registry/VPD/BlackBox) are §9; KASLR itself is future VMM work.

> **Verified:** 2026-06-12 | commit `4ce71522` | 6/6 items | build OK | smoke PASS (KVM 2.680s), 4646+16 tests
> **Quality reviewed:** 2026-06-12 | Codex 8x (design, test-coverage, adversarial, re-adversarial, consistency, perf) | 5H+2M fixed, 0 open | scope: kernel-code-quality

---

## 9. Entropy Diagnostics and Policy Gates

- [x] Mask/class logging without seed bytes: `entropy_report()` summary + the `registry: HKLM\SYSTEM\Boot\Entropy\Diagnostics mask=.. class=..` checkpoint line (`entropy_registry.c`).
- [x] Degraded indication, TWO call sites: `early_entropy_init()` posts the `boot_splash_diag()` WARNING at Phase 1 AND `boot_desktop.c` re-posts the identical string before `boot_splash_finish()` (Phase-2 storage diagnostics reuse + clear the line); registry + JSON carry the durable record.
- [x] `HKLM\SYSTEM\Boot\Entropy\Diagnostics` mirror sub-key (separate from the `ExternalEntropy` input): SourceMask, QualityPacked, Class, CreditedClass, CryptoGateOk, SeedFileHwProvenance, ExternalEntropySeen/Bytes (`entropy_populate_registry()`).
- [/] Admin external-entropy one-shot (Win11 ExternalEntropy parity): `entropy_external_consume()` absorbs (Q_LOW) only when `registry_persistence_active()` is false; on-disk/cross-reboot offerings DEFERRED -> XREF: `02-kernel-core/TODO-14 §8`.
- [x] BlackBox report: `X:\Diag\entropy.json` (mask/quality/class/gate/provenance/external counters -- never bytes) via `entropy_publish_json()`.
- [x] Commit: `"boot: entropy diagnostics and policy"`

**Test checkpoint:** `HKLM\SYSTEM\Boot\Entropy\Diagnostics` holds source mask + quality class (no seed bytes anywhere in registry, logs, or BlackBox); a degraded boot shows the splash WARNING line; serial shows `registry: HKLM\SYSTEM\Boot\Entropy\Diagnostics mask=..` + `JSON: wrote X:\Diag\entropy.json`. QEMU WHPX, QEMU TCG, VirtualBox, bare metal. (KVM smoke 2026-06-12: `mask=0xc2 class=minimum credited=minimum gate=1` + entropy.json 202 bytes.)

> **Test runner:** `scripts\debug\kernel\run-security-tests.bat` (SUITE=security) | entropy external one-shot suite, 0 failures

> **Notes:**
> - Shipped: `entropy_registry.c` (registry mirror + entropy.json + persistence-gated external one-shot consume) + `registry_persistence_active()` predicate + pure `entropy_external_oneshot()`/`entropy_class_str()` in `entropy.c` + degraded splash warning at TWO sites (`early_entropy_init()` Phase 1, `boot_desktop.c` Phase 3).
> - Runs once on the Phase-3 boot path after `seed_file_phase3()` (consume -> report -> registry -> JSON) so every surface reflects the final boot entropy state.
> - Downstream: cross-reboot ExternalEntropy + secure one-shot deletion deferred to D02 TODO-14 §8 (load wiring + scrub `.bak`/journal + per-hive flush); Codex review adoptions in the commit messages.
> - Canonical doc: surface contract in [`include/kernel/entropy.h`](../../include/kernel/entropy.h) (diagnostics surfaces block).
> - Scope boundary: §1 owns the model/report line; §8 owns the crypto gate the surfaces mirror; the desktop-visible post-boot notification is desktop-domain work, not §9.

> **Verified:** 2026-06-13 | commit `47502807` | 4/5 items | build OK | smoke PASS (KVM 2.43s)
> **Quality reviewed:** 2026-06-13 | Codex 8x (adversarial, re-adversarial, consistency, perf) | 3H+5M fixed | scope: kernel-code-quality

---

## 10. Entropy Tests

- [x] Source mixing determinism: `test_csprng_source_framed_vector` (`test_klibs.c`) -- golden framed transcript -> known blake2b-256 KAT + order-sensitivity; `test_csprng_core_vectors`/`test_csprng_core` cover raw-transcript determinism.
- [x] No source -> degraded but safe: `test_entropy_classify` (`test_entropy.c`) -- `entropy_classify(0,0)` is DEGRADED, and quality-without-mask / descriptor-HIGH-timing never credit (never a fake high-quality claim).
- [x] Seed file format: `test_random_seed_format` (`test_entropy.c`) -- `seed_file_encode`/`seed_file_accept` round-trip + every rejection class (len/magic/version/MAC/replay), pure helpers, no live VFS.
- [x] Zeroization after consumption: `test_entropy_seed_zeroized` (`test_entropy.c`) -- `boot_seed_release_payload` wipes accepted, rejected, foreign-producer, and sub-page shapes to all-zero.
- [x] Cloned seed file fails closed to degraded: `test_random_seed_clone_rejected` (wrong-token -> `SEED_FILE_BAD_MAC`) + `test_random_seed_clone_degraded` (cloned-only payload, all-HIGH advisory -> `records_mask=0`, classify DEGRADED).
- [x] External-entropy one-shot: `test_external_entropy_oneshot` (`test_entropy.c`) -- source destroyed on every path; `entropy_external_consume()` ordering validated by boot checkpoint (live registry/CSPRNG forbidden -- see Unit Tests Note).
- [x] Commit: `"test: early entropy handoff"`

**Test checkpoint:** `bash scripts/test.sh SUITE=security` runs the entropy suites with 0 failures; suites use fixtures and pure helpers only (no live boot infrastructure, per test policy). QEMU WHPX, QEMU TCG.

> **Test runner:** `scripts\debug\kernel\run-security-tests.bat` (SUITE=security) | 15 entropy (`test_entropy.c`) + 1 boot_seed (`test_boot_seed.c`) + 2 csprng (`test_klibs.c`) security suites, 0 failures

> **Notes:**
> - What shipped: section 10 test-sweep closure -- suites shipped with sections 1-9; 3 new: clone-fail-closed-to-degraded, source-framed golden blake2b-256 KAT, and the boot_seed_record_sources credit-gate (`test_boot_seed.c`).
> - How it runs: `bash scripts/test.sh SUITE=security` -- 367 kernel + 16 user-mode pass, 0 failures; all three new suites confirmed `[ OK ]` in `build/test.log`.
> - Downstream: completes sections 1-9 coverage; Codex step-13 adversarial adoptions (F2 clone-degraded, F3 source-framed) in the commit message; F1 (external-consume ordering) rejected per test-policy (see Unit Tests Note).
> - Canonical doc: test policy in [`docs/infrastructure/test-policy.md`](../../docs/infrastructure/test-policy.md); entropy contract in [`include/kernel/entropy.h`](../../include/kernel/entropy.h).
> - Scope boundary: live EFI/TPM/VFS/registry collection paths are validated by boot checkpoints, not unit tests; section 9 owns the diagnostics surfaces.

> **Verified:** 2026-06-13 | commit `51d9eb90` | 7/7 items | build OK | tests 367+16 PASS, smoke PASS (KVM 2.520s)
> **Quality reviewed:** 2026-06-13 | Codex 7x (adversarial x2, adversarial-impl x2, re-adversarial, consistency, perf) | 1H+3M+1L fixed, 0 open | scope: kernel-code-quality

---

## 11. Capability Gate on the Seed Payload Consumer

> **Spawned-by:** root
> **User impact:** on a boot where the loader reports the payload-descriptor capability as absent or degraded, the seed consumer reads, WIPES and can FREE physical frames the PMM never reserved and may already have handed to another owner. The symptom is early-kernel memory corruption with no line naming the seed path, on exactly the boots where the handoff was already known to be unhealthy.

`boot_reserved.c` gates its ENTIRE payload reservation loop on `BOOT_CAP_PAYLOAD_DESCRIPTORS` (`src/kernel/mm/boot_reserved.c`, "a producer that reports the capability as degraded is saying do not consume the descriptors"). So `BOOT_PAYLOAD_FLAG_RESERVED` on a descriptor is only evidence that PMM pinned the range WHEN that capability was negotiated. Without it the flag is a claim nobody acted on.

The warm-update consumer already carries the matching gate and says why: `src/kernel/main/boot_hw.c` requires `caps_ok` before it will treat a type-9 descriptor as anything, precisely because "skipping the caps_present gate lets a producer with caps_present clear ship a descriptor that the reservation pass would skip, returning preserved memory to PMM".

VERIFIED 2026-08-19 while adding the same gate to the TPM headless-authorization consumer: `grep -n "caps_present\|BOOT_CAP_" src/kernel/main/boot_seed.c src/kernel/security/stack_canary.c` returns nothing. `boot_seed_desc_classify` checks `FLAG_RESERVED`, the identity-map bound and the length contract, and never the capability. The seed path is the WORSE of the two cases, because `boot_seed_release_payload` does not merely read the range: it wipes it and, on an ownership match, calls `pmm_free_frame` on every page.

- [x] Gated the seed consumer on `BOOT_CAP_PAYLOAD_DESCRIPTORS` before any descriptor is read, wiped or released, via a new `BOOT_SEED_DESC_NO_CAPABILITY` class.
  - The function is `boot_seed_consume` (`src/kernel/main/boot_seed.c`), not `boot_seed_take_payloads` as this item said until 2026-08-29 -- no such symbol exists, and the section pack flagged it unresolved.
  - The gate is in the classifier as planned: `boot_seed_desc_classify` takes `caps_present` FIRST and checks it FIRST, mirroring `boot_headless_authz_classify` (`src/kernel/main/boot_headless_authz.c:58`).
  - The capability describes the HANDOFF, not one descriptor, so the walk is ABANDONED on NO_CAPABILITY: nothing is read, wiped, freed, or retired. Abandoning is also required for termination, since the loop re-queries occurrence 0 and only retiring advances it.
  - The dispatch is an EXHAUSTIVE `switch` with no `default` (review: adversarial [medium]). The prior if/else-if chain ended in a permissive `else` that ran the parse path, so deleting the capability arm would have made an un-negotiated payload parse, wipe and free with every classifier assertion still green. Under `-Wall -Wextra -Werror` (`Makefile:21`) that deletion is now a BUILD FAILURE: verified by removing the case, which produced `error: enumeration value 'BOOT_SEED_DESC_NO_CAPABILITY' not handled in switch [-Werror,-Wswitch]`.
- [x] Gave `stack_canary.c`'s peek the same gate: `canary_seed_desc_ok` takes `caps_present` first and refuses before every existing bound check.
  - This is the EARLIEST payload dereference in the boot (`canary_init` runs pre-IDT), so it is the one where an unreserved range is least survivable.
- [x] Audited every `boot_payload_find` consumer. Four exist outside the definition and its tests; the two above were the only ungated ones.
  - `src/kernel/main/boot_headless_authz.c:131` gates via `boot_headless_authz_classify(info->caps_present, ...)` -- already correct.
  - `src/kernel/main/boot_hw.c:274` (warm update) walks `payload_descriptors` directly behind an explicit `caps_ok` AND `flag_set` pair -- already correct.
  - `src/kernel/main/boot_payload.c:538` is the definition of `boot_payload_find` itself, not a consumer.
- [x] Gave the guards an observable seam by expressing the decision as DATA: pure `boot_seed_desc_wipe_len(cls, length)` and `boot_seed_desc_may_free(cls)`, asserted directly.
  - A payload consumer dereferences `phys_start` raw, which is the Phase-1 boot identity map contract. A unit test runs at Phase 3, where that alias is no longer live -> so a test that writes a canary into its own buffer, hands the buffer's physical address to the consumer, and then checks the canary survives will see it survive EITHER WAY: if the guard fails open, the read and wipe land at an address that is not the test's buffer.
  - MEASURED 2026-08-19 on the TPM headless-authorization consumer, which has the same shape: deleting its capability branch outright left the whole security suite green, and the case now SKIPs rather than reporting a pass it did not earn -> XREF: 01-boot-platform/TODO-13 §30 (item: "Carry the signed authorization from the ESP to the kernel as `BOOT_PAYLOAD_HEADLESS_AUTHZ`").
  - The originally-sketched shape (a seam the consumer reads its payload THROUGH) was REJECTED twice, on the design review and again on the adversarial round, both of which proposed driving `boot_seed_consume` from a test. It calls `pmm_free_frame` on live frames and mutates `g_boot_info`, which `docs/infrastructure/test-policy.md` bans -- the test-coverage leg of the same wave independently said "Do not call boot_seed_consume() from these tests". `01-boot-platform/TODO-10 §32` deleted `boot_stack_reset_for_test()` the same day for being a hook with no safe caller.
  - Decision-as-data is stronger than an address override for this purpose: an override can fail open and still look green, whereas a value assertion cannot. The consumer half that a pure helper genuinely cannot reach is covered by the compile-time `-Wswitch` exhaustiveness above instead of a runtime test.
  - CONTROLS RUN 2026-08-29, all reverted clean: deleting the capability branch in `boot_seed_desc_classify` failed 3 assertions; deleting it in `canary_seed_desc_ok` failed 2; deleting the `switch` case failed the BUILD. A probe control (a deliberately false assertion) was run FIRST to prove the suite was reached at all -- the first control attempt had been silently blocked by a PreToolUse hook, so an unmodified tree was briefly mistaken for a guard that did not fire.
- [x] Bounded the seed payload by its CONTRACT length at the RESERVATION, which is where the bound actually holds -- and stopped BAD_LENGTH from being wiped or freed at all.
  - `boot_seed_release_payload` is called with `d->length` for the BAD_LENGTH class too (`src/kernel/main/boot_seed.c`), and BAD_LENGTH means the classifier has just declared that field wrong. It is bounded only by the identity-map check, so a descriptor claiming gigabytes is rejected and then wipes gigabytes.
  - Found 2026-08-19 by the round-4 adversarial review of the TPM headless-authorization transport, which had copied this shape and now wipes a fixed `TPM_HEADLESS_BLOB_LEN` after an exact-length acceptance -> XREF: 01-boot-platform/TODO-13 §30 (item: "Carry the signed authorization from the ESP to the kernel as `BOOT_PAYLOAD_HEADLESS_AUTHZ`").
  - The seed payload IS secret one-time key material, so unlike the authorization it must still be wiped when refused -- the fix is to bound the length, not to stop wiping.
  - CLAMPING THE WIPE ALONE WOULD NOT HAVE FIXED IT (design review `[high]`, verified at `src/kernel/main/boot_seed.c:215-219` before the change): the frame loop derived its end from `d->length` independently of the length passed to `boot_seed_release_payload`, so a clamped wipe would still have returned a long UNWIPED suffix to the allocator, and an overlong descriptor could have issued ~1M `pmm_free_frame` calls below the 4 GiB ceiling. `boot_seed_desc_may_free` now admits CONSUMABLE only.
  - REJECTED the reviewer's alternative of making an overlong descriptor a fatal handoff error: this section exists so a degraded handoff DEGRADES, and halting the boot over a malformed seed descriptor inverts that.
  - THE FIRST FIX WAS ALSO WRONG, and the post-commit adversarial round caught it: refusing to FREE an over-cap descriptor merely converted the unwiped-suffix free into a PERMANENT PIN. `boot_reserved.c` reserves the full untrusted `d->length` for any `FLAG_RESERVED` descriptor, so a gigabyte-scale claim starved the PMM and the boot died in `heap_init` instead of degrading. The bound has to be applied where the memory is CLAIMED, not where it is released.
  - Shipped: pure `boot_seed_length_reservable(length)` as the SINGLE rule, read by the Phase-0 reservation pass (which now skips pinning a non-conforming RANDOM_SEED descriptor, mirroring the warm-update skip beside it) and by every Phase-1 consumer. The invariant is now stateable: a consumer dereferences only what the reservation pass actually pinned.
  - CONSEQUENCE, and a deliberate reversal of this item's first draft: BAD_LENGTH is no longer wiped at all. A descriptor the pass declined to pin may already be allocator-owned by Phase 1, so wiping even a clamped prefix would corrupt the new owner -- the same reason NOT_RESERVED has always been untouchable. Leaving a malformed producer's bytes in RAM is the lesser harm.
  - A THIRD round caught the follow-on `[high]`: bounding the reservation leaves `FLAG_RESERVED` SET on a descriptor nothing pinned, and `canary_seed_desc_ok` still used its own `length >= 16`. That peek runs after pmm/vmm/heap init and after the boot-stack guard page is unmapped but BEFORE the IDT exists, so it could have faulted on unpinned memory with no handler. It now calls the same predicate; control: restoring `length >= 16` fails 3 assertions.
  - The class extends past the seed type -- eight other payload types still pin whatever length they declare. Filed with its own Implementation Order row so the owner file reopens: -> XREF: `01-boot-platform/TODO-01 §28` (item: "Give each payload type its own declared maximum, in ONE place both the reservation pass and that type's consumers read").
- [x] Unit-tested the degraded-capability handoff against the seam: not read, not wiped, not freed, boot continues, with a negotiated-capability control beside it.
  - `test_boot_seed_desc_classify` (`src/kernel/test/test_entropy.c`) asserts the refusal, that every OTHER capability bit set is still a refusal, that the bit ALONGSIDE others is still consumable (a mask test, not equality), that caps outranks NOT_RESERVED and OUT_OF_MAP, that a maximal length is OUT_OF_MAP rather than BAD_LENGTH, and that `wipe_len`/`may_free` are 0 for every refused class.
  - `test_canary_seed_desc_bounds` (`src/kernel/test/test_security.c`) carries the matching pair for the pre-IDT peek plus the exact boundaries: the 16-byte minimum, a range ending exactly at the 4 GiB map end, a one-byte overrun, and a maximal length that must not wrap into an accept.
- [x] Commit: `"boot: capability gate on the seed payload consumer"`

**Test checkpoint:** with `caps_present` clear and a RESERVED-flagged seed descriptor present, the classifier reports the capability refusal and both disposition helpers return "touch nothing", so `boot_seed_release_payload` is never reached; with the capability set the same descriptor classifies CONSUMABLE. The originally-planned canary-byte fixture was NOT used: it would have required driving `boot_seed_consume`, which the test policy forbids, and at Phase 3 the boot identity alias is dead so the canary would have survived either way -- the assertion would have passed whether or not the guard fired. Scope: this section owns the capability gate on the seed consumers only; the reservation pass itself is correct and unchanged, and the descriptor ABI is `01-boot-platform/TODO-01`. Platforms: kernel unit suites; no hardware.

> **Test runner:** `scripts\debug\kernel\run-security-tests.bat` (SUITE=security) -- 24 new assertions across `test_entropy.c` (capability refusal, other-bits-set refusal, mask-not-equality accept, precedence over the other classes, maximal-length OUT_OF_MAP, and the wipe-length / may-free matrix per class) and `test_security.c` (canary caps refusal + accept, 16-byte minimum, exact 4 GiB end, one-byte overrun, maximal length). SUITE=security 3611 kernel + 17 user-mode PASS.

> **Notes:**
> - Shipped: `BOOT_SEED_DESC_NO_CAPABILITY` + a caps-first `boot_seed_desc_classify`, the pure `boot_seed_desc_wipe_len` / `boot_seed_desc_may_free` / `boot_seed_length_reservable` helpers (`include/kernel/entropy.h`), the exhaustive `switch` dispatch in `boot_seed_consume`, the seed length bound in the Phase-0 reservation pass, and the matching gates in `canary_seed_desc_ok`.
> - Integrates at the two previously ungated `boot_payload_find` consumers; the warm-update and TPM headless-authorization consumers already carried the gate and are unchanged.
> - Downstream: a boot without `BOOT_CAP_PAYLOAD_DESCRIPTORS` now yields no seed entropy and says so on serial, instead of wiping and freeing frames the PMM never reserved; a malformed seed length is refused a reservation rather than pinning that span for the life of the machine.
> - Guard observability is enforced at COMPILE time, not by a test: the dispatch has no `default`, so deleting the capability arm fails the build under `-Werror,-Wswitch`. A test cannot cover it because `boot_seed_consume` may not be called from one.
> - Trust boundary, stated because the capability is PRODUCER-asserted: the same loader that publishes the descriptors also sets the bit, so §11 is not itself independent verification. The independent layer is the `boot_info` header magic/version/size validation that Phase 0 runs before any of this is read; Win11's equivalent structural check lives in the loader block instead. A compromised loader can assert the capability and still hand a garbage-but-in-contract descriptor.
> - Scope boundary: this section bounds the RANDOM_SEED type only. The same unbounded-reservation class across the other eight payload types is filed at `01-boot-platform/TODO-01 §28`; the descriptor ABI itself belongs to that TODO.
> **Verified:** 2026-08-29 | commit `ed0b989e6` | 7/7 items | build OK | 32,713 kernel + 17 user tests, 0 failed | smoke matrix 4/4 legs (TCG+KVM x 1+2 CPUs, 294s) | lint 0 errors | todo-graph 10/10 | guard controls: deleting the classifier arm fails 3 assertions, the canary arm 2, the switch case fails the BUILD
> **Quality reviewed:** 2026-08-29 | Codex 9x (design, adversarial x2, test-coverage, consistency, perf, re-adversarial x3) + kernel-quality-auditor + concurrency-evidence-mapper + parity-research-analyst | 3H+5M+4L fixed, 0 open | scope: kernel-code-quality

---

## OS Comparison

| ⭐  | Feature                        | 🪟 Win11                                 | 🐧 Linux                        | 🚀 Impossible OS                   |
| --- | ------------------------------ | ---------------------------------------- | ------------------------------- | ---------------------------------- |
| 💎  | Entropy quality/credit model   | ✅ internal pool credit                  | ✅ credit accounting            | ✅ §1 mask+class+policy            |
| 💎  | Firmware RNG collection        | ✅ boot entropy via CNG                  | ✅ EFI RNG seeds random         | ✅ §2 + OEM0, framed seed          |
| 💎  | CPU RDRAND/RDSEED              | ✅ CNG source                            | ✅ arch_random + mix            | ✅ §3 RDSEED-first, gated          |
| 💎  | TPM RNG mix                    | ✅ TPM-backed entropy                    | ✅ tpm-rng driver               | ✅ §4 budgeted + floor-gated       |
| 💎  | Jitter/timing mix-in           | ✅ interrupt timing                      | ✅ jitterentropy                | ✅ §5 staged, LOW-clamped          |
| 💎  | Seed carryover file            | ✅ registry system secrets               | ✅ /var/lib random-seed         | ✅ §6 MAC+NVRAM anti-clone         |
| 💎  | Bootloader-kernel seed handoff | ✅ winload loader block                  | ✅ EFI config-table seed        | ✅ §7 typed+CRC, first-seed        |
| 💎  | Early kernel CSPRNG seeding    | ✅ before ASLR consumers                 | ✅ random_init early            | ✅ §8 named init + gate            |
| ⭐  | Visible entropy quality report | ❌ hidden                                | ⚠️ dmesg only                   | ✅ §9 registry+JSON+splash         |
| ⭐  | Seed handoff length+caps gate  | ⚠️ version check, fatal, no length bound | ❌ type dispatch, len unbounded | ✅ §11 caps + [32,16384] both ends |

> **Parity:** rows track Win11 CNG boot entropy and Linux random.c. The §11 row is ⭐, not 💎, and the research is why: Win11's `LOADER_BLOCK_MISMATCH` (bug check 0x100) validates the loader block's size/version and is FATAL on mismatch, but `BOOT_ENTROPY_LDR_RESULT` is a fixed-size struct so there is no producer-declared length to bound; Linux's `parse_setup_data()` dispatches on `SETUP_RNG_SEED` and then passes `data->len` to `add_bootloader_randomness()` with no min or max check at all, and negotiates no capability. Neither baseline has the thing this row names, so claiming parity here would have flattered the section; §1-§8 shipped (model, firmware+OEM0, CPU RNG, TPM RNG, jitter/timing, MAC'd seed carryover with NVRAM anti-clone/anti-replay -- stronger than the unauthenticated Linux seed file -- the §7 checksummed typed handoff folding verified carryover into the FIRST kernel seed, and the §8 named init point with a credited-class release crypto gate -- stronger than Linux, which has no boot-time crypto-readiness gate), and the §9 visible quality surfaces. The ⭐ §9 quality report (HKLM registry mirror + X:\Diag\entropy.json + splash degraded warning) goes beyond both; the §9 admin ExternalEntropy one-shot awaits the D02 TODO-14 hive-load wiring for cross-reboot use.

---

## Unit Tests

> Wire into `src/kernel/test/test_runner.c` / `test_runner_init()`. Suites span `test_register_entropy()` (`test_entropy.c`), `test_register_boot_seed()` (`test_boot_seed.c`), `test_register_klibs()` (`test_klibs.c` -- csprng mixer), and `test_register_tpm_transport()` (`test_tpm_transport.c` -- §4 TPM RNG), all TEST_CAT_SECURITY. Tests use fixtures + pure helpers only; live EFI/TPM/VFS/registry collection paths are validated by boot checkpoints.

- [x] Shipped with §1 (5 suites): quality slot packing, classification (degraded/minimum/good + descriptor-bypass guard), policy gate, transcript framing (exact-fit/wrap/no-write-on-refusal), record + clamp + Q_NONE retraction
- [x] Shipped with §5 (6th suite): staged transcript -- stage/credit-visibility, NULL + too-small drain guards, full drain + empty re-drain, oversized refusal leaves record intact + overflow flag, consume_zero reset
- [x] Shipped with §4 (in `test_tpm_transport.c`, TEST_CAT_SECURITY): GetRandom marshal/parse negatives, bounded-loop request-cap protocol, RNG collection via fake TIS (floor boundary both sides, stuck heuristics, report flag, no-transport no-op)
- [x] Register in `test_runner_init()`: `test_register_entropy()` (`test_runner.c`)
- [x] Mix determinism (shipped with D02T03 §5 + §7/§8, `test_klibs.c`): csprng core seed determinism + `seed2` concat equivalence + crypto-gate suite (upgrade matrix, oracle consistency, diagnostic-record insulation)
- [x] `test_random_seed_format` (shipped with §6, `test_entropy.c`): round-trip + every rejection class (len/magic/version/MAC/replay incl. mac-field tamper + counter extremes) + no-output-poisoning sentinels
- [x] `test_random_seed_clone_rejected` (shipped with §6, `test_entropy.c`): blob MAC'd under secret A fails closed under secret B
- [x] Shipped with §7 (3 suites, `test_entropy.c`): payload parse gates + NO_FIT, carryover routing, `test_entropy_seed_zeroized` via pure `boot_seed_release_payload`; `csprng_core_seed2` equivalence in `test_klibs.c`
- [x] `test_external_entropy_oneshot` (shipped with §9, `test_entropy.c`): copy/clamp/reject matrix, source destroyed on every path, class-string vocabulary
- [x] Shipped with §11 (2 suites, TEST_CAT_SECURITY): the capability gate on both seed-payload consumers
  - `test_boot_seed_desc_classify` (`test_entropy.c`): capability refusal, other-bits-set refusal, mask-not-equality accept, precedence over NOT_RESERVED and OUT_OF_MAP, maximal length classifying OUT_OF_MAP rather than BAD_LENGTH, and the wipe-length / may-free matrix per class.
  - `test_canary_seed_desc_bounds` (`test_security.c`): the matching pair for the pre-IDT peek -- caps refusal and accept, the 16-byte minimum, a range ending exactly at the 4 GiB map end, a one-byte overrun, and a maximal length that must not wrap into an accept.
- [x] Shipped with §10: `test_random_seed_clone_degraded` (clone -> classify DEGRADED) + `test_csprng_source_framed_vector` (golden framed blake2b-256 KAT) + `test_boot_seed.c` record-sources credit gate (records_mask not hdr_mask)

> **Note:** `entropy_external_consume()` integration ordering (secure-delete-before-absorb, one-shot deletion, refuse-second-consume) is not unit-tested -- it is entangled with the live registry (`RegOpenKeyEx`/`RegQueryValueEx`/`RegDeleteValue`), `registry_persistence_active()`, and the global `csprng_add_entropy()`, all on the test-policy HARD-BAN list. The pure one-shot core `entropy_external_oneshot` (source-overwrite on every path) IS unit-tested; the ordering is validated by the Phase-3 boot serial checkpoint.

---

## Verification

- [x] `bash scripts/build.sh clean` -> `=== BUILD OK ===` (2026-06-13 clean build)
- [x] `bash scripts/test.sh SUITE=security` -> 3,622 kernel + 17 user-mode PASS, 0 failures (re-measured 2026-08-29 at close-out; the 367 + 16 figure was §10's 2026-06-13 reading, before §11 and the rest of the security category grew)
- [x] `bash scripts/test-smoke.sh` -> Boot complete 2.520s + `C:\>`; source-mask line `entropy: fw=none cpu=ok tpm=none ... (class=minimum)` (KVM)
- [x] QEMU OVMF without RNG protocol degrades visibly (serial shows degraded WARN). Verified 2026-06-13 (TCG `qemu64,-rdrand,-rdseed`, no TPM): `class=degraded`, `gate=0`, release crypto REFUSED, boot continues.
- [x] QEMU with EFI RNG: `entropy: fw=ok` in serial. Verified 2026-06-13 (KVM + `virtio-rng-pci` -> OVMF VirtioRngDxe): `EFI_RNG_PROTOCOL 64 bytes`, `fw=ok ... class=good`, `gate=1`.
- [ ] Bare metal with RDRAND: `cpu=ok`. Blocked: manual -- run on rig (KVM already shows cpu=ok; row requires real hardware)
- [ ] Bare metal with TPM RNG: `tpm=ok`. Blocked: manual -- run on rig (needs fTPM/dTPM)
- [ ] Commit: `"boot: early entropy verified -- sources, seed handoff, CSPRNG"`. Deferred: whole-TODO sign-off gated on §9 cross-reboot (-> `02-kernel-core/TODO-14 §8`) + the bare-metal rows above

**Test runner:** `scripts\debug\kernel\run-security-tests.bat` (SUITE=security) | entropy suites, 0 failures
