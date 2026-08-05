---
schema_version: 1
id: secureboot-tpm
domain: 18-future-research
status: active
title: "TODO-04 -- Secure Boot, TPM 2.0 & Measured Boot"
---

# TODO-04 -- Secure Boot, TPM 2.0 & Measured Boot

> **Goal:** Research spike for the enterprise-grade security chain: UEFI Secure Boot
> PK/KEK/db key hierarchy, TPM 2.0 measured boot chain (PCR 8–10), AES-256-XTS full
> disk encryption sealed to TPM PCR policy, vTPM emulation for ImpossibleHV guests,
> and a research document covering the full attestation story.

> [!IMPORTANT]
> **Secure Boot shim** (rhboot/shim, MOK key pair, `HKLM\SYSTEM\SecureBoot`) is
> owned by `01-boot-platform/TODO-02`; §1 here adds the **PK/KEK/db enrollment
> hierarchy** for enterprise deployment (self-generated Platform Key, Key Exchange Key,
> and db signature) on top of the shim foundation.
>
> **TPM 2.0 command driver** (CRB/FIFO MMIO, `tpm2_startup`, `PCR_Read/Extend`,
> `GetRandom`) is owned by `04-drivers-hardware/TODO-04 §6`. An existing
> `src/kernel/tpm.c` (312 lines) already parses UEFI PCR event logs. §2 here
> cross-references `TODO-04 §6` and adds the **CSPRNG entropy integration** and
> QEMU `swtpm` development setup -- do not re-specify TPM driver internals.
>
> **AES-256-GCM, SHA-256, `cng_sha256`** are owned by `09-desktop-shell/TODO-07 §1`;
> §4 FDE uses `cng_sha256` for PCR sealing policy and relies on AES-256-XTS
> (add to `TODO-07 §1` if not already present -- XTS mode for disk encryption is
> adjacent to existing GCM mode). **Argon2i** for password-based key derivation is
> already in monocypher (`crypto_argon2i()`).
>
> **vTPM for ImpossibleHV** (§5 here) complements
> `13-future-research/TODO-02 §7` (hypervisor deliverables); the two TODOs are
> designed together. This is research-level only -- no production vTPM code until
> ImpossibleHV Phase 2 lands.

---

## Inputs

- `01-boot-platform/TODO-02-uefi-hardening-secureboot.md` (→ XREF) -- shim chain-loading; MOK key pair; `boot_info.secure_boot_enabled`; §1 here adds PK/KEK/db layer above shim
- `04-drivers-hardware/TODO-04-security-hardware.md §6` (→ XREF) -- TPM 2.0 command driver (CRB/FIFO, STARTUP, PCR_Read/Extend, GetRandom, PCR[10] kernel integrity); §2 here adds CSPRNG feed + QEMU swtpm
- `09-desktop-shell/TODO-07-cng-crypto.md §1` (→ XREF) -- `cng_sha256()`, AES-256-GCM, `csprng_read()`; §4 FDE adds AES-256-XTS (XTS mode extension to §1) + Argon2i KDF
- `11-user-platform-sdk/TODO-01` (→ XREF, if CSPRNG integration) -- entropy pool that `tpm2_get_random()` feeds into -- §2 CSPRNG integration
- `13-future-research/TODO-02-hypervisor.md` (→ XREF) -- ImpossibleHV Phase 2 multi-vCPU; §3 vTPM is a companion feature for that phase
- `src/kernel/tpm.c` -- existing 312-line PCR event log parser; §2 extends it with command driver (cross-ref `TODO-11 §8`)
- `include/libs/monocypher.h` -- `crypto_argon2i()` for password-based key derivation in §4 FDE recovery key
- `src/boot/uefi/bootx64.c` -- bootloader; §1 and §3 add signature verification + PCR extension calls here

---

## Outcome

A `docs/architecture/secure-boot-tpm-plan.md` document defining the full security chain
from UEFI firmware through kernel to encrypted disk: PK/KEK/db key hierarchy, PCR
assignment table (PCR 0–10), FDE key derivation and sealing, remote attestation quote
format, and vTPM per-VM architecture. A QEMU `swtpm`-based prototype validates that
`tpm2_get_random()`, `tpm2_pcr_extend()`, and `tpm2_pcr_read()` work before any
bare-metal deployment.

---

## Implementation Order

| Step | Section                                  | 💎/⭐ | Dependency                               |
| ---- | ---------------------------------------- | --- | ---------------------------------------- |
| 1    | TPM 2.0 CSPRNG integration + `swtpm` dev setup | ⭐   | `D01T13 §8` TPM driver; entropy pool     |
| 2    | Measured boot chain (PCR 8–10 extensions) | ⭐   | §1 TPM commands; bootloader PCR extend   |
| 3    | UEFI Secure Boot PK/KEK/db hierarchy     | ⭐   | `TODO-01` shim; `D09T07 §7` code signing |
| 4    | AES-256-XTS full disk encryption (FDE)   | ⭐   | §1 TPM sealing; §2 PCR policy; `cng_sha256`; `crypto_argon2i` |
| 5    | vTPM for ImpossibleHV guests             | ⭐   | `TODO-02` ImpossibleHV Phase 2; §1–§5 TPM architecture |
| 6    | Research deliverables (`secure-boot-tpm-plan.md`) | ⭐   | §1–§5 complete                           |

---

## 1. TPM 2.0 CSPRNG Integration + `swtpm` Dev Setup `[Opus]`

> Security-critical: feeds hardware entropy from TPM RNG into the kernel CSPRNG pool.
> QEMU `swtpm` setup validates the driver before bare-metal. Extends `TODO-11 §8` --
> this section documents the integration design without duplicating driver internals.

- [ ] **Dependency on `TODO-11 §8`**: the TPM command driver (`tpm2_startup`, `tpm2_get_random`, `tpm2_pcr_read/extend`) is implemented there; this section adds the CSPRNG entropy feed and the development environment
- [ ] **CSPRNG entropy feed** (`src/kernel/tpm.c` extension):
  - On TPM init (after `tpm2_startup(TPM2_SU_CLEAR)`): call `tpm2_get_random(rng_buf, 32)` → feed 32 bytes into kernel entropy pool via `csprng_add_entropy(rng_buf, 32)` (function in entropy pool subsystem, cross-ref `TODO-11 §6`)
  - Repeat entropy injection every 30 minutes from a background `sched_task` (guards against entropy exhaustion after long uptime)
  - Log: `"[TPM] 32 B hardware entropy injected into CSPRNG"`
  - Fallback: if `tpm2_get_random()` fails (no TPM, or TPM not ready): `csprng_read` continues with RDRAND-only path (already defined in `TODO-11 §6`) -- no boot failure
- [ ] **QEMU `swtpm` development setup**:
  - QEMU supports software TPM: `qemu-system-x86_64 ... -chardev socket,id=chrtpm,path=/tmp/swtpm-sock -tpmdev emulator,id=tpm0,chardev=chrtpm -device tpm-tis,tpmdev=tpm0`
  - Host: `swtpm socket --tpmstate dir=/tmp/tpm-state --ctrl type=unixio,path=/tmp/swtpm-sock --tpm2 &`
  - Add `scripts/start-swtpm.sh`: start `swtpm` daemon, patch `scripts/build.sh run` to include TPM arguments
  - Verify: ACPI SSDT has `\_SB.TPM` device; `tpm2_startup()` returns `TPM2_RC_SUCCESS`
- [ ] **TPM CRB vs FIFO transport** (research note for `TODO-11 §8` coordination):
  - CRB (Command Response Buffer): MMIO at `0xFED40000`; 4 KB size; registers: `TPM_LOC_CTRL` (request/release locality), `TPM_CRB_CTRL_CMD_ADDR`, `TPM_CRB_CTRL_CMD_SIZE`, `TPM_CRB_CTRL_RSP_ADDR`, `TPM_CRB_CTRL_RSP_SIZE`, `TPM_CRB_CTRL_START`
  - FIFO: older TIS (TPM Interface Specification); separate register for each byte of command/response
  - QEMU `swtpm` exposes CRB by default with `tpm-tis-device`; detect via ACPI `PNP0C31` (CRB) vs `PNP0C01` (FIFO) HID

---

## 2. Measured Boot Chain (PCR 8–10 Extensions) `[Opus]`

> Novel: extending the TPM PCR chain beyond what UEFI firmware measures (PCR 0–7).
> Bootloader and kernel must extend PCRs before loading each component. Security-critical:
> any implementation error silently breaks the attestation chain.

- [ ] **PCR assignment policy** (document for §6 deliverable):

| PCR | Owner                    | Content hashed                        | When extended                          |
| --- | ------------------------ | ------------------------------------- | -------------------------------------- |
| 0–7 | UEFI firmware            | Firmware + boot config                | By firmware automatically              |
| 8   | Bootloader (`bootx64.c`) | SHA-256 of `kernel.exe` file contents | Before `ExitBootServices()`            |
| 9   | Kernel                   | SHA-256 of each `.kmod` loaded        | In `kmod_load()` before `init_fn()`    |
| 10  | Kernel                   | SHA-256 of first user-process image   | On first `task_create()` with user ELF |

- [ ] **PCR 8 -- kernel hash extension** (in `src/boot/uefi/bootx64.c`):
  - After loading `kernel.exe` ELF into memory, before `ExitBootServices()`:
  - `cng_sha256(kernel_elf_buf, kernel_elf_size, digest32)` -- compute SHA-256 over loaded ELF bytes
  - `tpm2_pcr_extend(8, digest32)` -- extend PCR 8 with kernel hash
  - `tpm2_pcr_read(8, pcr8_out)` -- read back PCR 8; log `"[TPM] PCR[8] = {hex}"` to boot serial
  - If Secure Boot enforcement is on (`HKLM\SYSTEM\SecureBoot\Enforce = 1`) and TPM PCR 8 doesn't match stored expected value: refuse to boot
- [ ] **PCR 9 -- kmod hash extension** (in `src/kernel/kmod.c`, cross-ref `11-user-platform-sdk/TODO-03 §5`):
  - In `kmod_load()`, after ELF validation but before `init_fn()`:
  - `cng_sha256(kmod_buf, kmod_size, digest32)` + `tpm2_pcr_extend(9, digest32)`
  - Log `"[TPM] PCR[9] extended with kmod={name}, digest={hex[:8]}..."`
- [ ] **PCR 10 -- first user process hash** (in `src/kernel/sched/task.c`):
  - On first `task_create()` with `is_user = 1`: hash the process image; `tpm2_pcr_extend(10, digest32)`
  - Log `"[TPM] PCR[10] = first user process measured"`
> [!NOTE]
> **TPM Quote SUPERSEDED for implementation** by the active TODO -> XREF: [`01-boot-platform/TODO-13 §13`](../01-boot-platform/TODO-13-tpm-measured-boot-attestation.md) (Attestation Key Provisioning and TPM2 Quote: EK->AK provisioning, credential-activation, `TPM2_CC_Quote`, anti-replay nonce). This research-spike item informed §13; the implementation work is tracked there.

- [ ] **TPM Quote (remote attestation)** (`tpm2_quote(pcr_mask, nonce, sig_out, sig_len)`):
  - `TPM2_CC_Quote` command: selects PCRs by bitmask (`pcr_mask`); signs PCR digest with TPM's AIK (Attestation Identity Key) using ECDSA or RSASSA
  - Returns signed quote blob → can be verified by remote verifier that holds the TPM's public AIK
  - `HKLM\SYSTEM\SecureBoot\ExpectedPCRs\PCR{N}` -- store expected PCR values at time of "known-good" installation; `tpm2_local_attest()` compares live PCR reads against stored values; return `ATTEST_OK` / `ATTEST_DEGRADED` / `ATTEST_COMPROMISED`
  - `attest.exe` shell command: `attest status` → print all PCR[0–10] values + comparison; `attest quote <nonce_hex>` → print base64 quote blob

---

## 3. UEFI Secure Boot PK/KEK/db Key Hierarchy `[Opus]`

> Security-critical: establishing the full UEFI Secure Boot chain-of-trust for
> enterprise deployment. Extends `TODO-01` shim foundation with self-generated
> PK/KEK/db and UEFI NVRAM enrollment.

- [ ] **Key hierarchy** (three levels):
  - **Platform Key (PK)**: root of trust; 1 key maximum; controls who can update KEK; generated per deployment
  - **Key Exchange Key (KEK)**: controls who can add/remove db entries; typically OEM or OS vendor key
  - **Signature Database (db)**: allowed signatures; `BOOTX64.EFI` must match an entry here
  - **Forbidden Database (dbx)**: revoked hashes; takes precedence over db
- [ ] **Key generation** (host-side, run once per deployment; keys stored securely):
  ```bash
  # Generate 4096-bit RSA PK
  openssl req -newkey rsa:4096 -nodes -keyout PK.key -new -x509 -sha256 -days 3650 -subj "/CN=Impossible OS Platform Key/" -out PK.crt
  # Generate KEK
  openssl req -newkey rsa:4096 -nodes -keyout KEK.key -new -x509 -sha256 -days 3650 -subj "/CN=Impossible OS KEK/" -out KEK.crt
  # Generate db signing key
  openssl req -newkey rsa:4096 -nodes -keyout db.key -new -x509 -sha256 -days 3650 -subj "/CN=Impossible OS Signing Key/" -out db.crt
  # Sign BOOTX64.EFI with db key
  sbsign --key db.key --cert db.crt --output BOOTX64.EFI.signed BOOTX64.EFI
  ```
  **PK.key and db.key MUST NEVER be committed to the repository** (same discipline as MOK.key in `TODO-01`); stored in `CODESIGN_DB_KEY` CI secret
- [ ] **UEFI NVRAM enrollment** (`scripts/enroll-secureboot-keys.sh`):
  - Convert certs to EFI signature list format via `cert-to-efi-sig-list` tool
  - `efi-updatevar -e -f db.esl db` → enroll db
  - `efi-updatevar -e -f KEK.esl KEK` → enroll KEK
  - `efi-updatevar -f PK.auth PK` → set PK (final step -- Secure Boot activates)
  - QEMU test: `qemu-system-x86_64 -drive if=pflash,file=OVMF_CODE.fd,readonly=on -drive if=pflash,file=OVMF_VARS.fd` + enroll keys into OVMF_VARS.fd via script
- [ ] **Bootloader signature chain** (in `src/boot/uefi/bootx64.c`):
  - On boot: call UEFI `SECURITY_PROTOCOL2->FileAuthenticationState()` to verify `kernel.exe` against db
  - If verification fails and `HKLM\SYSTEM\SecureBoot\Enforce = 1`: `Print(L"[SECURITY] Kernel signature invalid"); return EFI_ABORTED`
  - If `Enforce = 0`: log warning but continue (development mode)
  - Store result in `boot_info.secure_boot_enforced` and `boot_info.kernel_sig_valid`
- [ ] **Long-term: shim-review submission** (stretch): submit shim binary to Microsoft shim-review process; enables Impossible OS to boot on retail hardware with Secure Boot on without user enrolling keys -- reference `TODO-01` shim cross-link

---

## 4. AES-256-XTS Full Disk Encryption (FDE) `[Opus]`

> Novel: full disk encryption sealed to TPM PCR policy. No prior Impossible OS FDE.
> Security-critical: volume key exposure = data loss. AES-256-XTS is the NIST-standard
> mode for disk encryption (IEEE P1619).

**Source:** `src/kernel/fde.c` (new, gated `#ifdef ENABLE_FDE`); extends `09-desktop-shell/TODO-07 §1` with XTS mode

- [ ] **AES-256-XTS mode** (extend `TODO-07 §1`):
  - XTS = XEX-based tweaked-codebook mode with ciphertext stealing (IEEE Std 1619-2007)
  - Two 256-bit AES keys: `key1` (data encryption), `key2` (tweak); total 512 bits = `volume_key`
  - Per-sector tweak: `tweak = AES_ECB_encrypt(key2, sector_number_as_128bit_LE)`
  - Per-512B-sector encryption: `for each 16B block: C_i = AES_ECB_encrypt(key1, P_i XOR T_i) XOR T_i`; then `T_i+1 = GF(2^128) multiply T_i by alpha (polynomial 0x87)`
  - `cng_aes256xts_encrypt(key1, key2, sector_no, plaintext, ciphertext, sector_count)`
  - `cng_aes256xts_decrypt(key1, key2, sector_no, ciphertext, plaintext, sector_count)`
- [ ] **Volume key management**:
  - `volume_key[64]` = 512-bit random key from `csprng_read(64)` -- never stored in plaintext
  - **TPM-sealed copy** (`tpm2_seal_key`): `TPM2_CC_Create` with PCR policy `{PCR 0,1,2,3,7,8}` (firmware + bootloader + kernel hash); returns sealed blob (encrypted by TPM's Storage Root Key, only unlockable if PCR values match); store blob in IXFS partition header reserved area (512 B at offset 0x200)
  - **Recovery key** (password-based): `recovery_key_blob = AES_256_GCM_encrypt(key=argon2i(user_password, salt), plaintext=volume_key)`; store recovery blob in `HKLM\SYSTEM\FDE\RecoveryKey` and optionally export to USB
  - **Unlock at boot** (in `kmod_load` or kernel init pre-IXFS mount):
    1. `tpm2_unseal(sealed_blob, pcr_policy)` → `volume_key` (succeeds only if PCRs match)
    2. If TPM unsealing fails: prompt for recovery password → `argon2i(password, salt)` → AES-GCM decrypt blob → `volume_key`
    3. Mount IXFS with `volume_key` passed to block layer
- [ ] **Block layer integration** (`src/kernel/fde.c`):
  - Register `fde_blkdev_t` wrapping the underlying IXFS blkdev
  - `fde_read_sectors(dev, lba, count, buf)`: `blkdev_read(dev->underlying, lba, count, cipher_buf)` + `cng_aes256xts_decrypt(key1, key2, lba, cipher_buf, buf, count)`
  - `fde_write_sectors(dev, lba, count, buf)`: `cng_aes256xts_encrypt(...)` then `blkdev_write(dev->underlying, lba, enc_buf, count)`
  - VFS mounts through `fde_blkdev_t` -- all higher-level FS code is unaware of encryption
- [ ] **`bitlocker.cpl`** Control Panel applet:
  - **Enable FDE**: generate `volume_key`, seal to TPM, encrypt with recovery password, reformat IXFS partition with FDE header; require reboot
  - **Suspend FDE**: `HKLM\SYSTEM\FDE\Suspended = 1`; boot skips PCR policy check once; resumes on next boot (used for firmware updates that change PCR values)
  - **Backup recovery key**: export recovery blob to USB or print as 48-character key (Base32 encoded)
  - **Status**: show FDE enabled/disabled, TPM seal status, last PCR values

---

## 5. vTPM for ImpossibleHV Guests `[Sonnet]`

> Each VM guest in ImpossibleHV gets an isolated virtual TPM with independent PCR
> hierarchy and sealed key material. Extends `13-future-research/TODO-02` (ImpossibleHV
> Phase 2); §5 is design-only in this spike.

- [ ] **vTPM architecture**:
  - Each VM gets a `vtpm_t` state object: `{pcr_values[24][32], ek_seed[32], srk_seed[32], nv_storage[4096]}`
  - `vtpm_create_vm(vm_id)` → allocates and initializes `vtpm_t` from `pmm_alloc_contiguous(2)`
  - Guest TPM commands (via virtio-tpm MMIO or direct CRB emulation at guest `0xFED40000`) are intercepted by VM exit handler → forwarded to VMM's `vtpm_handle_command(vtpm, cmd_buf, cmd_len, rsp_buf, rsp_len)`
- [ ] **`libtpms` integration** (software TPM emulator):
  - `libtpms` (BSD-3-Clause): software implementation of TPM 2.0 spec; ~400 K LOC; used by `swtpm`
  - Porting assessment: `libtpms` uses `openssl` and `stdlib` -- needs shims; alternative: use `swtpm` as a co-process (host-side) forwarding commands via socket, same as QEMU's `chardev socket` approach
  - Recommended for research phase: **co-process model** (vTPM requests forwarded from VMM to host `swtpm` via Unix socket) -- avoids porting `libtpms` into the kernel
- [ ] **vTPM isolation**: each VM's `vtpm_t` has independent PCR state; a compromised guest cannot read another guest's sealed material; PCR values persist across VM save/restore via §4 snapshot mechanism (→ XREF `TODO-02 §3`)
- [ ] **Use case**: guest Impossible OS instance runs FDE (§4 here); seals its volume key to guest TPM PCRs; live migration (§5 of `TODO-02`) migrates both guest RAM + `vtpm_t` PCR state → FDE unlocks on destination host without user intervention

---

## 6. Research Deliverables `[Sonnet]`

**Source:** `docs/architecture/secure-boot-tpm-plan.md`

- [ ] **`docs/architecture/secure-boot-tpm-plan.md`** -- sections:
  - **Secure Boot key hierarchy diagram**: PK → KEK → db → `BOOTX64.EFI` → `kernel.exe` chain; MOK shim layer from `TODO-01`; enrollment scripts
  - **TPM 2.0 command table** (which commands needed + byte-level encoding reference):

| Command              | CC      | Used in            | Byte layout reference |
| -------------------- | ------- | ------------------ | --------------------- |
| `TPM2_CC_Startup`    | `0x144` | boot               | Part 3 §12.1          |
| `TPM2_CC_GetRandom`  | `0x17B` | CSPRNG             | Part 3 §16.1          |
| `TPM2_CC_PCR_Extend` | `0x182` | measured boot      | Part 3 §22.2          |
| `TPM2_CC_PCR_Read`   | `0x17E` | attestation        | Part 3 §22.4          |
| `TPM2_CC_Create`     | `0x153` | FDE key seal       | Part 3 §13.2          |
| `TPM2_CC_Unseal`     | `0x15E` | FDE boot unlock    | Part 3 §13.3          |
| `TPM2_CC_Quote`      | `0x158` | remote attestation | Part 3 §18.4          |

  - **Measured boot PCR assignment table**: PCR 0–10, owner, content, when extended (from §2)
  - **FDE architecture diagram**: boot flow (TPM unseal → volume key → FDE blkdev → VFS); recovery key path; `bitlocker.cpl` UI
  - **vTPM per-VM architecture**: `vtpm_t` isolation; co-process vs. in-kernel `libtpms` trade-offs
  - **Blocking dependencies**: ACPI `\_SB.TPM` device enumeration (for real hardware -- `swtpm` bypasses); AES-256-XTS in `TODO-07 §1`; `kmod_load` PCR extend requires `TODO-11 §8` complete
  - **Effort estimate**: TPM + measured boot: `[X person-weeks]`; FDE: `[Y person-weeks]`; vTPM: `[Z person-weeks]` (post-ImpossibleHV Phase 2)
- [ ] **`swtpm` QEMU prototype**: `scripts/start-swtpm.sh` + patched `scripts/build.sh run` with TPM args; serial shows `tpm2_startup() OK`, `tpm2_get_random(32)` returns 32 non-zero bytes, PCR[8] logged with kernel hash; proves driver works before any bare-metal TPM testing
- [ ] **Tracking GitHub Issues**: "Secure Boot PK/KEK/db enrollment" (blocks enterprise deployment); "AES-256-XTS FDE" (major security feature); both link to `secure-boot-tpm-plan.md`

---

## OS Comparison


| ⭐   | Feature                                  | 🪟 Win11                                  | 🐧 Linux                                  | 🚀 Impossible OS                          |
| --- | ---------------------------------------- | ---------------------------------------- | ---------------------------------------- | ---------------------------------------- |
| 💎   | UEFI Secure Boot chain of trust          | ✅ Required for Win11; PK/KEK/db +        | ✅ shim + MOK (distro-signed); grub2      | ⬜ §3 -- PK/KEK/db hierarchy; `sbsign`; `TODO-01` shim |
| 💎   | TPM 2.0 measured boot                    | ✅ Bitlocker PCR policy; Windows VSB      | ✅ IMA (Integrity Measurement Architecture) PCR | ⬜ §2 -- PCR 8–10 extended by bootloader  |
| 💎   | Full disk encryption sealed to TPM PCR   | ✅ BitLocker TPM 2.0 PCR policy           | ✅ `cryptsetup` LUKS2 with `clevis-tpm2` TPM | ⬜ §4 -- AES-256-XTS; TPM2 `CC_Create` PCR policy |
| 💎   | Remote attestation via TPM Quote         | ✅ Windows Health Attestation Service; WDAC | ✅ `tpm2-quote` + Keylime remote attestation | ⬜ §2 -- `tpm2_quote()` + `attest.exe`; `HKLM\SYSTEM\SecureBoot\ExpectedPCRs` |
| ⭐   | Hardware entropy from TPM fed into CSPRNG | ✅ Windows uses TPM RNG in                | ✅ Linux: `hwrng` → `/dev/random`; `tpm_core` | ⬜ §1 -- `tpm2_get_random()` → `csprng_add_entropy()` every 30 |
| 💎   | vTPM per VM guest                        | ✅ Hyper-V vTPM (1.2 + 2.0                | ✅ QEMU `swtpm` + `libtpms` per-VM        | ⬜ §5 -- `vtpm_t` per ImpossibleHV VM; co-process |

Impossible OS's `⭐` advantage: the `tpm2_get_random()` → CSPRNG entropy feed runs
on a background task every 30 minutes -- not just at boot -- so entropy quality improves
throughout uptime even on systems where RDRAND is unavailable or distrusted. The
`bitlocker.cpl` FDE UI deliberately mirrors BitLocker's UX (Enable, Suspend, Backup
recovery key) so enterprise administrators already trained on Windows can manage
Impossible OS encryption with zero retraining.

---

## Verification

- [ ] **swtpm QEMU probe**: `bash scripts/start-swtpm.sh && bash scripts/build.sh run` (with TPM args) → serial shows `"[TPM] TPM 2.0 CRB at 0xFED40000"` + `"[TPM] tpm2_startup() OK"`
- [ ] **CSPRNG entropy feed**: `tpm2_get_random(32)` returns 32 bytes where not all zero; bytes injected into CSPRNG pool; subsequent `csprng_read(16)` returns different bytes than before injection
- [ ] **PCR 8 extension**: serial shows `"[TPM] PCR[8] = {64 hex chars}"` before `ExitBootServices()`; two boots with same `kernel.exe` → identical PCR 8 value; replace `kernel.exe` with modified binary → different PCR 8 value
- [ ] **PCR 9 kmod extension**: load two kmods → PCR 9 extended twice; `attest status` shows PCR 9 value changed from baseline
- [ ] **PCR 10 user process**: first `task_create()` with user=1 → PCR 10 extended; `tpm2_pcr_read(10)` non-zero
- [ ] **AES-256-XTS round-trip**: `cng_aes256xts_encrypt(k1, k2, sector_0, plaintext_512B, cipher)` → `cng_aes256xts_decrypt(k1, k2, sector_0, cipher, recovered)` → `recovered == plaintext_512B`; verify changing `sector_no` produces different ciphertext from same plaintext
- [ ] **TPM key seal/unseal**: `tpm2_seal_key(volume_key, 64, pcr_policy)` → sealed blob; `tpm2_unseal(blob, pcr_policy)` → same `volume_key`; tamper PCR 8 (mock different PCR value) → `tpm2_unseal` returns `TPM2_RC_POLICY_FAIL`
- [ ] **FDE boot flow**: enable FDE via `bitlocker.cpl`; reboot; IXFS mounts through `fde_blkdev_t`; files accessible; TPM sealed key auto-unlocks without password prompt
- [ ] **FDE recovery key**: `bitlocker.cpl` → Backup recovery key; reboot with TPM seal bypassed (simulated PCR mismatch) → password prompt → enter recovery key → IXFS unlocks; verify data integrity
- [ ] **Secure Boot enrollment**: run `scripts/enroll-secureboot-keys.sh` against OVMF_VARS.fd; QEMU boots with Secure Boot on; unsigned kernel copy → QEMU halts with `"[SECURITY] Kernel signature invalid"`; correctly signed kernel → boots normally
- [ ] Commit: `"security: Secure Boot PK/KEK/db hierarchy, measured boot PCR 8-10, AES-256-XTS FDE, TPM CSPRNG feed, vTPM design"`
