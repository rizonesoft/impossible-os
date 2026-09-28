<!-- docs: covers=todo/01-boot-platform/TODO-12-early-entropy-random-seed.md sources=include/kernel/entropy.h,src/kernel/entropy.c,src/boot/uefi/bootx64.c,src/kernel/main/boot_seed.c,include/kernel/seed_file.h,src/kernel/seed_file.c,include/kernel/csprng.h,src/kernel/entropy_registry.c,src/kernel/main/boot_interrupts.c,src/kernel/security/stack_canary.c,src/kernel/main.c reviewed=2026-09-28 order=12 -->
# Early Entropy and Random Seed Handoff

## What is it?

This is the subsystem that gets trustworthy randomness into the kernel's CSPRNG before any CSPRNG consumer runs, so GUIDs and keys are never drawn from an unseeded generator. It collects from independent sources during boot (firmware RNG, CPU RDSEED/RDRAND, TPM RNG and boot timing jitter), carries a seed file across reboots, and hands everything to the kernel in one typed, checksummed payload that seeds the CSPRNG before any consumer can draw from it.

Every source is graded rather than blindly trusted: a boot with no hardware entropy source is reported as degraded, not silently accepted as good. The model, the collectors, the handoff and the diagnostics are owned here; the CSPRNG itself (Monocypher-based mixing, runtime reseed) is owned by the [kernel libraries roadmap](../../todo/02-kernel-core/TODO-03-kernel-libraries.md).

## How does it work?

The model lives in `include/kernel/entropy.h`: eight source classes (`ENTROPY_SRC_FW_RNG` through `ENTROPY_SRC_TIME`), a packed 2-bit-per-source quality (`ENTROPY_Q_NONE`, `LOW`, `HIGH`), and an overall classification from `entropy_classify()`: DEGRADED with no HIGH source, MINIMUM with one, GOOD with two or more independent ones. The kernel derives which sources are present from the records it actually accepted, and clamps quality for the timing and seed-file sources (the seed file is always LOW). For hardware-source records it keeps the quality the producer declared, after rejecting invalid values, so a producer that overstates a firmware RNG record is believed.

Collection starts in the bootloader. `collect_boot_entropy()` in `bootx64.c` runs before `ExitBootServices` and gathers, in order: `EFI_RNG_PROTOCOL` output (64 bytes from one `GetRNG` call), an ACPI OEM0 table when present, CPU output via `bl_collect_cpu_rng()` (RDSEED preferred, RDRAND fallback, CPUID-gated so a missing instruction degrades instead of faulting, with bounded retries and rejection of all-zero or all-identical samples), the seed-file bytes read from BlackBox media, and a boot-timing record. Every contribution is framed as a source-tagged, length-prefixed record (`entropy_frame_source()`) so a weak source can never cancel a strong one by XOR, and the whole framed transcript is published as one reserved `BOOT_PAYLOAD_RANDOM_SEED` descriptor that the PMM cannot reclaim.

Inside the kernel, `entropy_collect_tpm()` mixes in `TPM2_GetRandom` output over the TPM transport, and `entropy_collect_jitter()` samples RDTSC deltas at the earliest point after interrupts are enabled. Both stage into a fixed-capacity transcript (`entropy_stage_source()`, `entropy_staged_drain()`) rather than mixing directly.

`early_entropy_init()` in `boot_seed.c` ties it together. It is called once on the BSP in Phase 1 before any CSPRNG consumer: `boot_seed_consume()` walks every validated `RANDOM_SEED` descriptor, digest-chains their transcripts with Blake2b-256 and wipes each payload after use; the digest folds into the CSPRNG's first key via `csprng_init()`; the credited class is then logged and, when degraded, posted to the boot splash as a visible warning.

A carryover seed file (`seed_file.c`) persists a MAC'd blob with an NVRAM anti-clone token at `X:\Boot\random-seed.bin`. The bootloader's early read feeds the first seed as a low-quality personalization source, and a Phase 3 read-and-rotate cycle (`seed_file_phase3()`) reseeds the running CSPRNG and writes a fresh file, but only once the CSPRNG has hardware-backed credit for that boot.

The stack-protector cookie is the one consumer that runs earlier: `canary_init()` seeds it right after Phase 0, before `boot_phase1()`, from its own reading of the seed descriptor and the CPU RNG, and logs a degraded warning when the cookie would be predictable. A healthy CSPRNG later in Phase 1 does not change the cookie's quality.

The handoff is capability-gated: `boot_seed_desc_classify()` checks `BOOT_CAP_PAYLOAD_DESCRIPTORS` before `FLAG_RESERVED`, so a producer that never negotiated the capability cannot have its seed descriptor read, wiped or freed. A descriptor whose declared length falls outside `[header size, 16384]` bytes is refused a reservation in the first place (`boot_seed_length_reservable()`).

```mermaid
sequenceDiagram
  participant FW as Bootloader (collect_boot_entropy)
  participant BI as boot_info payload
  participant K as Kernel (early_entropy_init)
  participant ST as TPM and jitter (staged)
  FW->>FW: EFI RNG, ACPI OEM0, RDSEED/RDRAND, seed file, timing
  FW->>BI: framed transcript as BOOT_PAYLOAD_RANDOM_SEED (reserved)
  K->>BI: boot_seed_consume(): classify, digest-chain, wipe
  ST->>K: entropy_collect_tpm(), entropy_collect_jitter()
  K->>K: csprng_init() folds boot digest and staged sources
  K->>K: log credited class; splash warning if DEGRADED
```

## What are its interfaces?

| Interface | Purpose |
| --------- | ------- |
| `entropy_classify()` / `entropy_policy_ok()` | Overall class derivation and the release/debug boot-progression gate ([`entropy.h`](../../include/kernel/entropy.h)) |
| `entropy_frame_source()` / `entropy_record_source()` | Source-tagged length-framed transcript builder and the mask/quality recorder ([`entropy.h`](../../include/kernel/entropy.h)) |
| `entropy_stage_source()` / `entropy_staged_drain()` | Staged transcript (jitter, TPM) held under an irqsave lock until the first seed drains it ([`entropy.h`](../../include/kernel/entropy.h)) |
| `collect_boot_entropy()` | Bootloader collector: EFI RNG, OEM0, CPU RNG, seed file, timing; publishes the descriptor ([`bootx64.c`](../../src/boot/uefi/bootx64.c)) |
| `boot_seed_consume()` | Phase 1 consumer: walk descriptors, classify, digest-chain, wipe ([`boot_seed.c`](../../src/kernel/main/boot_seed.c)) |
| `boot_seed_desc_classify()` / `boot_seed_length_reservable()` | Capability and length gate shared by the Phase 0 reservation pass and the Phase 1 consumer ([`entropy.h`](../../include/kernel/entropy.h)) |
| `early_entropy_init()` | The single named early CSPRNG seeding entry point ([`boot_seed.c`](../../src/kernel/main/boot_seed.c)) |
| `csprng_init()` / `csprng_crypto_ok()` / `csprng_fill_classified()` | First-seed fold, the key-generation readiness gate, and the degraded-flag fill API ([`csprng.h`](../../include/kernel/csprng.h)) |
| `seed_file_encode()` / `seed_file_accept()` / `seed_file_phase3()` | Carryover file format helpers and the Phase 3 read-and-rotate lifecycle ([`seed_file.h`](../../include/kernel/seed_file.h)) |
| `entropy_report()` | One-line boot diagnostics summary, WARN when degraded ([`entropy.c`](../../src/kernel/entropy.c)) |
| `entropy_populate_registry()` / `entropy_publish_json()` | `HKLM\SYSTEM\Boot\Entropy\Diagnostics` mirror and `X:\Diag\entropy.json` (mask, class and gate; never seed bytes) ([`entropy_registry.c`](../../src/kernel/entropy_registry.c)) |
| `firmware_rng` / `seed_file` in `boot.conf` | Escape hatches disabling EFI RNG collection or the carryover lifecycle ([boot_info Field Ownership](boot-info-fields.md)) |

## How do I use it?

```bash
bash scripts/test.sh SUITE=security   # entropy, boot_seed, csprng and TPM RNG suites
make test-security                    # same, as a make target
bash scripts/test-smoke.sh            # boots and prints the entropy summary line
```

On a live boot, serial shows the bootloader's collection lines (`[BOOT] RNG: EFI_RNG_PROTOCOL 64 bytes`, `[BOOT] RNG: RDSEED 64 bytes`, `[BOOT] RNG: seed payload N bytes`), then in the kernel `entropy: seed payload digest: N transcript bytes over M payload(s)` and `early entropy init complete: credited class=...`.

The Phase 3 summary line has the shape `fw=.. cpu=.. tpm=.. oem0=.. seed=.. hwrng=.. jitter=.. time=.. (class=..)`, logged at WARN when the class is degraded and INFO otherwise, followed by the Registry and `X:\Diag\entropy.json` publication lines. A degraded boot also shows a splash warning: `WARNING: degraded randomness -- no hardware entropy source credited`.

To disable a source deliberately for testing, set `firmware_rng=off` or `seed_file=off` in `boot.conf`.

## What is not implemented yet?

- The admin external-entropy one-shot (Windows `ExternalEntropy` parity) absorbs only when no on-disk Registry persistence is active; cross-reboot use needs hive-load wiring and secure delete: [Advanced Hive Features](../../todo/02-kernel-core/TODO-14-registry-completion.md#8-advanced-hive-features).
- The two seed classifiers take the "did the reservation pass actually pin this payload" fact at each call site rather than as a parameter, so a new seed consumer could forget to ask: [Per-Type Payload Length Contract Before Reserving](../../todo/01-boot-platform/TODO-01-boot-protocol-abi-handoff.md#28-per-type-payload-length-contract-before-reserving).
- Bare-metal sign-off for `cpu=ok` (RDRAND on real hardware) and `tpm=ok` (a physical fTPM or dTPM) is still open; KVM exercises both paths: [Verification](../../todo/01-boot-platform/TODO-12-early-entropy-random-seed.md#verification).

## How does it compare with Windows 11 and Linux?

The core model tracks Windows CNG boot entropy and Linux `random.c` feature for feature: firmware RNG, CPU RDRAND/RDSEED, TPM-backed entropy, interrupt-timing jitter, a carryover seed file, and a bootloader-to-kernel seed handoff that seeds the CSPRNG before ASLR-class consumers run. Two rows go past both. The visible entropy quality report (a Registry mirror, a BlackBox JSON file and a splash warning) is something neither Windows (hidden) nor Linux (dmesg only) exposes. The seed-descriptor length and capability gate is stronger than either: Linux's `parse_setup_data()` dispatches on `SETUP_RNG_SEED` with no length bound and no negotiated capability. The seed file's MAC plus NVRAM anti-clone token is also stronger than Linux's unauthenticated seed file.

## See also

- [Early Entropy and Random Seed Handoff roadmap](../../todo/01-boot-platform/TODO-12-early-entropy-random-seed.md)
- [Boot Protocol ABI Handoff](boot-protocol-abi-overview.md)
- [boot_info Field Ownership](boot-info-fields.md)
- [BlackBox Diagnostic Artifacts](black-box-artifacts.md)
- [TPM Measured Boot and Attestation](tpm-measured-boot.md)
