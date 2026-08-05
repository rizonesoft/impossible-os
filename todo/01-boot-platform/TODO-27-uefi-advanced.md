---
schema_version: 1
id: uefi-advanced
domain: 01-boot-platform
status: active
title: "TODO-27 -- UEFI Advanced Features"
---

# TODO-27 -- UEFI Advanced Features

> **Validated:** 2026-06-21 | backfill -- todo-graph structural validate clean; all sections shipped + reviewed
> **Gap-audited:** 2026-06-21 | backfill -- triage DONE (sections shipped + quality-reviewed / deferred); Stages 1-2 predate this marker

> **Goal:** Advanced UEFI boot features beyond the core boot path: UEFI capsule firmware updates, W^X memory enforcement on UEFI runtime regions, multi-GPU GOP enumeration, extended Secure Boot state variables with enforcement policy, extended SMBIOS type parsing, DBX revocation list synchronization, and advanced multi-OS menu integration. These are production polish features -- the OS boots and runs correctly without them.

> [!NOTE]
> Split from TODO-02 (UEFI Hardening & Secure Boot). TODO-02 covers §1-§8 (runtime, variables, GOP, SMBIOS, Secure Boot state, shim, boot UX polish, serial klog) plus open §9 ops backlog (SBAT doc, DB registry mirror, EBS retry). This TODO covers deferred advanced UEFI features. The general boot entry store/menu policy is owned by TODO-07; this file's §1 is only the UEFI-specific multi-OS discovery and chainload extension.

## Inputs

- [`src/boot/uefi/bootx64.c`](../../src/boot/uefi/bootx64.c) -- UEFI bootloader
- [`include/kernel/boot_info.h`](../../include/kernel/boot_info.h) -- boot info struct
- [`include/kernel/uefi_runtime.h`](../../include/kernel/uefi_runtime.h) -- UEFI runtime wrappers
- [`include/kernel/smbios.h`](../../include/kernel/smbios.h) -- SMBIOS parser
- -> XREF: `TODO-02-uefi-hardening-secureboot.md` -- core §1-§8 + §9 ops parity backlog; this TODO extends it
- -> XREF: `TODO-02-uefi-hardening-secureboot.md §9` -- SBAT/DB registry/EBS retry must not duplicate capsule write path owned here §2
- -> XREF: `TODO-03-bootloader-error-recovery.md §3` -- GOP timeout and headless fallback wrap enumeration from this file §6
- -> XREF: `TODO-04-firmware-table-platform-inventory.md §8` -- ESRT inventory mirror feeds capsule policy in this file §4
- -> XREF: `TODO-07-boot-entry-store-menu-policy.md §4` -- boot menu framework; this file §1 adds UEFI multi-OS discovery and chainload entries
- -> XREF: `04-drivers-hardware/TODO-17-gpu-display-drivers.md §6` -- multi-head display consumes `boot_info.gop_handles[]` from §4
- -> XREF: `02-kernel-core/TODO-10-kernel-security-hardening.md` -- kernel lockdown triggered by §5 enforcement policy
- -> XREF: `04-drivers-hardware/TODO-03-acpi-power-management.md` -- chassis type from §4 distinguishes desktop vs laptop
- -> XREF: `18-future-research/TODO-04-secureboot-tpm.md §3` -- PK/KEK/db key hierarchy builds on §5 and §1

## Outcome

- UEFI multi-OS discovery and chainload entries feed the TODO-07 boot menu.
- UEFI capsule delivery path for firmware updates via ESRT.
- W^X enforcement on UEFI runtime memory regions.
- Multi-GPU GOP enumeration with ConOut-path primary selection.
- Extended Secure Boot state (SetupMode, AuditMode, DeployedMode) exposed to kernel and registry.
- SMBIOS Type 2/3/16/19 parsed into registry for Device Manager and System Properties.
- DBX revocation list freshness check with proactive stale warning.

## Implementation Order

| ⭐  | Order | Deliverable                                   | Depends On     | Status |
| --- | :---: | --------------------------------------------- | -------------- | :----: |
| ⭐  |   1   | UEFI multi-OS detection and chainload entries | T07 §1-§4      |  [x]   |
| 💎  |   2   | Firmware update advisor (read-only LVFS)      | T04 §6         |  [x]   |
| 💎  |   3   | UEFI memory attributes (W^X)                  | T02 §1, T24 §1 |  [/]   |
| 💎  |   4   | Multi-GPU GOP enumeration                     | T02 §3         |  [x]   |
| 💎  |   5   | Secure Boot extended state + enforcement      | T02 §5         |  [/]   |
| 💎  |   6   | SMBIOS extended type parsing                  | T02 §4         |  [x]   |
| 💎  |   7   | DBX revocation list sync                      | T02 §2, §5     |  [/]   |

---

## 1. UEFI Multi-OS Detection and Chainload Entries

Detect other OS partitions from GPT and contribute chainload entries to the TODO-07 boot menu when the user has multiple OSes installed.

**Files:** `src/boot/uefi/bootx64.c`, `include/kernel/boot_info.h`

- [x] `chainload_detect()` (`bootx64.c`, Step 1a' pre-EBS): `LocateHandleBuffer(SFS)` enumerates non-boot volumes and read-only-probes the well-known foreign bootloader paths into `g_chainload_targets[8]`.
- [x] Direct bootloader-file detection supersedes the GPT-type-GUID scan: the chainload target is the loader file on the ESP, not a data-partition type.
- [x] `chainload_synthesize(parse)` appends up to 8 `BOOT_ENTRY_KIND_CHAINLOAD` envelopes (id `chainload-N`, title=OS, sort_key `zzz-` so they sort after IPOS) to the TODO-07 parse result, bounded by `BOOT_ENTRIES_MAX_ENTRIES`.
- [x] TODO-07 §6 renders the countdown menu over the combined entries; IPOS stays the policy default (foreign entries carry no policy tags).
- [x] `chainload_exec()` builds the volume device path + MEDIA/FILEPATH node, `LoadImage(BootPolicy=FALSE)` (firmware Secure-Boot-verifies; `EFI_SECURITY_VIOLATION` refused) + `StartImage`; a failed chainload demotes to the IPOS fallback.
- [x] `efi.h` types `LoadImage`/`StartImage`/`UnloadImage` (`EFI_IMAGE_LOAD`/`_START`/`_UNLOAD`) for the chainload path.
- [x] Commit: `"boot: multi-OS GPT detection and countdown text-mode boot menu"`

**Test checkpoint:** On QEMU/bare-metal with a foreign UEFI bootloader on another volume, serial logs `[MULTIBOOT] detected <OS>`, the TODO-07 menu shows the foreign entry after the IPOS default, the countdown auto-selects IPOS, and selecting the foreign entry chainloads it (`[MULTIBOOT] chainloading <OS>`); with no foreign loader, serial logs `[MULTIBOOT] no foreign OS bootloaders found` and boots IPOS. Verify on QEMU WHPX/TCG + bare-metal dual-boot.

> **Test runner:** N/A (UEFI bootloader-only; no kernel test surface) | validation: serial `[MULTIBOOT]` lines on QEMU + bare-metal dual-boot

> **Notes:**
> - Shipped `chainload_detect`/`_probe_volume`/`_build_devpath`/`_exec`/`_synthesize` (`bootx64.c`) + typed `LoadImage`/`StartImage`/`UnloadImage` (`efi.h`); detect at Step 1a', synthesize after the parse, dispatch after the menu.
> - Foreign loaders found by read-only ESP bootloader-path probe (not GPT type); trust = firmware Secure Boot at `LoadImage` (dbx = `EFI_SECURITY_VIOLATION` refused); a failed chainload demotes to the IPOS fallback.
> - Scope boundary: the JSON-store chainload entry-kind validator stays TODO-07 §13 (synthesized entries built trusted, bypass it); W^X/multi-GPU/SecureBoot/SMBIOS/dbx are §3-§7.

> **Verified:** 2026-06-17 | commit `f259f12c` | 6/6 items | build OK | smoke PASS (TCG 2.70s)
> **Quality reviewed:** 2026-06-17 | Codex 9x (design, adversarial, consistency, perf, re-adversarial) | 8H fixed | scope: boot-code-quality

---

## 2. Firmware Update Advisor (read-only LVFS-style)

Surface what firmware updates exist for the host and tell the operator how to apply them via the **vendor's** update path. Impossible OS does NOT call `UpdateCapsule()`, does NOT write the `OsIndications` capsule bit, does NOT stage capsule images on the ESP, and does NOT trigger reboot-and-flash. Firmware writes are the single failure mode that turns a laptop into a paperweight; the OS-side risk surface for an actual capsule path is wider than its hobby-OS value, so this section is deliberately scoped as advisory only.

**Files:** `src/kernel/firmware_advisor.c` (new), `include/kernel/firmware_advisor.h` (new), `user/sysinfo/firmware_cmd.c` (new)

> [!NOTE]
> **Option B scope decision (2026-05-02):** the original §2 plan covered actual capsule delivery (UpdateCapsule + OsIndications + ESP staging + submission journal + torn-write recovery). That work is **out of scope and intentionally unowned**. Operators update firmware via the vendor's tool (BIOS Setup, Lenovo Vantage, Dell Command Update, fwupd from a Linux live USB, etc.); Impossible OS only tells them *what* to update and *why*. Removed deliverables: `capsule_update_request`, `capsule_check_result`, `OsIndications` write, ESP `\EFI\UpdateCapsule\` staging, capsule submission journal, torn-write recovery. **Stance change condition:** revisit only if (a) Impossible OS becomes the user's primary daily-driver OS AND (b) a vendor-signing path with brick-test coverage on real hardware is in place.

- [x] **ESRT consumer** (no re-parse): `firmware_advisor_init` consumes the `uefi_config.h` ESRT API (count, entry, decode_status, decode_type, rollback_floor_ok). Read-only.
- [x] **Offline LVFS cache reader** (`X:\Diag\lvfs-metadata.json`, schema_version=1) via `kernel/json.h`. Live HTTPS fetch + GPG/PKCS7 verify + gzip + XML parse owned by §8 (needs TCP/TLS/crypto/XML).
- [x] **Version-comparison advisor** (`fa_classify`): emits `up_to_date` / `update_available` / `unknown` per FwClass; severity `critical` iff cache row carries non-empty CVE; `LastAttemptStatusName` propagated to registry + JSON for operator UX.
- [x] **Rollback gate (advisory only)**: `RollbackFloorOk` mirrored to registry + JSON; sysinfo prints downgrade-blocked note when clear. Never enforced.
- [x] **`sysinfo.exe firmware-updates` CLI** (`user/sysinfo/sysinfo.c` + cmd.exe `sysinfo` builtin): reads `X:\Diag\firmware-advisor.json` (kernel-published), renders per-component table, closes with canonical refusal line.
- [x] **Registry mirror** at `HKLM\SOFTWARE\Impossible\FirmwareAdvisor\<GUID>\` (12 per-component values + `_Header` sibling). Idempotent via `RegDeleteTree`.
- [x] **Refusal sentinel** (`firmware_capsule_refused.c`): compile-time `_Static_assert` (gated `CAPSULE_REFUSAL_ENABLED`) + 4 linker symbols in `.firmware_capsule_refused` section.
- [x] **JSON publish to `X:\Diag\firmware-advisor.json`** (`fa_publish_json`): denormalized verdict, schema_version=1 pinned, fail-closed on truncation. Avoids user-mode registry SSDT plumbing.
- [x] Commit: `"kernel: firmware-update advisor (read-only LVFS metadata + ESRT join)"`

**Test checkpoint:** `sysinfo firmware-updates` at the C:\> prompt on QEMU OVMF (no ESRT) prints `No firmware components reported by ESRT...` followed by the canonical refusal line. Smoke confirms `[OK] advisor: cache: missing` + `[OK] advisor: JSON: wrote X:\Diag\firmware-advisor.json (187 bytes)`. Synthetic-ESRT + synthetic-cache fixture coverage filed as the §8 follow-up. **No `OsIndications` variable is touched on any boot.** Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | 5 advisor sub-tests / 16 assertions, 0 failures
>
> **Notes:**
> - **What shipped** -- `firmware_advisor.[ch]` + `firmware_capsule_refused.c` + `user/sysinfo/sysinfo.c` + cmd.exe `sysinfo` builtin + 5 unit tests in `test_firmware_advisor.c`.
> - **How it runs** -- BSP-only Phase 3 init in `boot_desktop.c` after VFS / ESRT / registry. Idempotent (`RegDeleteTree`); cache absent / malformed degrades to `status=unknown` with one `LOG_INFO`.
> - **Downstream effects** -- closes the TODO-04 §6 deferral; `HKLM\SOFTWARE\Impossible\FirmwareAdvisor\` is the canonical desktop-notification source; `X:\Diag\firmware-advisor.json` is the canonical user-mode source.
> - **Canonical doc** -- [`include/kernel/firmware_advisor.h`](../../include/kernel/firmware_advisor.h).
> - **Scope boundary** -- §2 owns offline cache reader + ESRT join + registry + JSON publish + sysinfo CLI + refusal sentinel; live HTTPS fetch + GPG/PKCS7 verify + gzip + XML parse owned by §8.

> **Verified:** 2026-05-03 | this commit | 9/9 items | build OK | smoke PASS (KVM 2.55s) + 187-byte advisor JSON + sentinel symbols present
> **Quality reviewed:** 2026-05-03 | Codex 7x (design + adversarial x2 + consistency x2 + perf + re-adversarial) | 1H+5M+1L fixed | scope: kernel-code-quality + userland-code-quality

---

## 3. UEFI Memory Attributes (W^X)

Enforce write-XOR-execute on UEFI runtime memory regions by walking the `EFI_MEMORY_ATTRIBUTES_TABLE`.

**Files:** `src/kernel/uefi_runtime.c`, `src/kernel/mm/vmm.c`

> [!NOTE]
> `01-boot-platform/TODO-04 §5 UEFI Memory Attributes and Runtime Properties Inventory` shipped the consumer API: `mat_get_count()` / `mat_get_entry(idx, *out)` returning `mat_entry_t {phys_addr, num_pages, attribute, cls}` with `cls` ∈ `{GUARD, CODE, DATA, RODATA, WX_VIOLATION}` from `include/kernel/uefi_config.h`. Iterate that instead of re-walking firmware tables. `mat_classify_attr()` is a pure test helper.

- [x] Locate `EFI_MEMORY_ATTRIBUTES_TABLE` in UEFI config tables -- shipped by TODO-04 §1 catalog + §5 inventory; consume via `mat_get_count()`/`mat_get_entry()`.
- [x] `uefi_runtime_enforce_wx()` (`uefi_runtime.c`) walks `mat_get_entry()`: DATA/RODATA -> `vmm_set_nx`, CODE -> `vmm_set_ro` (identity-mapped). Fail-closed: success log fires only when nothing failed and no W^X violation remains.
- [x] `vmm_set_nx()`/`vmm_set_ro()` + `vmm_query_flags()` (`vmm.c`): split-aware read-modify-write of one PTE bit (NX set / WRITABLE clear), preserving other flags; local TLB flush.
- [x] Graceful degradation: MAT absent -> warn + skip; `MAT_CLASS_WX_VIOLATION` left intact + counted unsafe; a boot-order guard refuses enforcement once SMP is up. Pinned in Phase 1 after `mat_init`.
- [ ] **EFI_MEMORY_ATTRIBUTE_PROTOCOL runtime sync** (gap-audit 2026-05-01 H2, surfaced from TODO-02 review pipeline): the static `EFI_MEMORY_ATTRIBUTES_TABLE` (UEFI 2.6) freezes attributes at boot; UEFI 2.10 adds the runtime-callable `EFI_MEMORY_ATTRIBUTE_PROTOCOL` with `GetMemoryAttributes` / `SetMemoryAttributes` / `ClearMemoryAttributes` so firmware-backed permission flips can stay in sync with OS page-table flips. Linux 6.7+ uses this for the EFI stub. Add: (a) protocol discovery via `LocateProtocol(EFI_MEMORY_ATTRIBUTE_PROTOCOL_GUID, ...)` BEFORE ExitBootServices; cache the function pointers via `boot_info.uefi_runtime` mirror (UEFI 2.10 protocol survives EBS like the rest of RT). (b) When `vmm_set_nx`/`vmm_set_ro` modify a UEFI runtime page, ALSO call the protocol's `SetMemoryAttributes(EFI_MEMORY_XP)` / `EFI_MEMORY_RO` so firmware-internal page-table state matches the OS view -- avoids drift on systems where firmware re-asserts permissions after `SetVirtualAddressMap`. (c) Graceful degradation: when the protocol is absent (UEFI < 2.10 or stripped firmware), log `[UEFI] memory-attribute protocol absent; falling back to static MAT enforcement only` and continue. Tests: synthetic UEFI 2.10 fixture (protocol present) asserts both paths fire; synthetic UEFI 2.6 fixture (protocol absent) asserts the absence is logged and the static path still works. Owner: this section.
- [x] Commit: `"kernel: UEFI runtime W^X enforcement via EFI_MEMORY_ATTRIBUTES_TABLE"`

**Regression risk:** Modifies page table permissions on UEFI runtime regions. Rollback: skip enforcement (BOOT_DEGRADED path).

**Test checkpoint:** Serial shows `[UEFI] W^X enforced on N runtime memory regions (static MAT)` (or the `W^X DEGRADED: ...` accounting line when a region failed / a W^X violation remains); `uefi_get_time()` still works after enforcement. Verify on QEMU WHPX/TCG + bare metal.

> **Test runner:** `scripts\debug\kernel\run-mm-tests.bat` (SUITE=mm) | `test_vmm_set_nx` + `test_vmm_set_ro` | enforce_wx validated via serial `[UEFI] W^X` line (calls live boot infra)

> **Notes:**
> - Shipped `uefi_runtime_enforce_wx()` (`uefi_runtime.c`) + `vmm_set_nx`/`vmm_set_ro`/`vmm_query_flags` (`vmm.c`); walks the MAT (TODO-04 §5 API), DATA/RODATA->NX, CODE->RO, fail-closed accounting; wired Phase 1 after `mat_init`.
> - `vmm_set_nx`/`vmm_set_ro` are split-aware PTE read-modify-write (preserve other flags, local TLB flush, single-CPU); a boot-order guard refuses enforcement once SMP is up (Codex design D1+D2 adoptions in commit).
> - Scope boundary: the runtime `EFI_MEMORY_ATTRIBUTE_PROTOCOL` sync (item below, DEFERRED -- needs a boot_info ABI add, UEFI 2.10-only) stays this section; static MAT enforcement is the shipped core.

> **Verified:** 2026-06-17 | commit `031c5a9b` | 4/5 items | build OK | tests 112/112 PASS (mm) + smoke PASS (no-MAT graceful-skip)
> **Deferred:** [M] `EFI_MEMORY_ATTRIBUTE_PROTOCOL` runtime sync (firmware-page-table mirror of OS NX/RO flips) needs a boot_info ABI add + bootloader LocateProtocol pre-EBS; UEFI 2.10-only, absent on older firmware (Codex design review recommended defer) -> XREF: 01-boot-platform/TODO-27 §3 (item: "EFI_MEMORY_ATTRIBUTE_PROTOCOL runtime sync" at line 128)
> **Quality reviewed:** 2026-06-17 | Codex 5x (design, adversarial, consistency, perf, re-adversarial) | 3H+1M fixed | scope: kernel-code-quality

---

## 4. Multi-GPU GOP Handle Enumeration

Enumerate all GOP handles via `LocateHandleBuffer` and select the active display using ConOut device path.

**Files:** `src/boot/uefi/bootx64.c`, `include/kernel/boot_info.h`

> [!NOTE]
> -> XREF: `04-drivers-hardware/TODO-17-gpu-display-drivers.md §6` -- kernel multi-head `display_register_head()` will consume `boot_info.gop_handles[]`.

- [x] `struct boot_gop_handle` (32B) + `gop_handles[4]` + `gop_handle_count` appended at `boot_info` tail in `boot_info.h` + mirror; `BOOT_INFO_VERSION` 22->23; manifest + doc-coverage + `boot-info-fields.md` registered.
- [x] `gop_enumerate_and_select()` (`bootx64.c`) replaces `LocateProtocol` with `LocateHandleBuffer(ByProtocol, GOP)`; records up to 4 handles, frees the buffer on every path; `init_gop()` tries primary then valid candidates before headless.
- [x] Primary selection (Codex design D2): `gST->ConsoleOutHandle` device-path prefix match first (`gop_handle_is_conout`, `GetDevicePathSize`-bounded), then `EFI_CONSOLE_OUT_DEVICE_GUID` tie-breaker, then largest `width*height`.
- [x] `gop_record_handle()` per-head validation gauntlet (Codex design D1): `fb_addr`/`fb_valid` set only on full validation (base!=0, Info!=NULL, supported fmt, `pitch>=width`, no overflow, size covers); failing secondaries record geometry only.
- [x] Commit: `"boot: enumerate all GOP handles -- LocateHandleBuffer, ConOut-path primary selection"`

**Test checkpoint:** QEMU (single GPU): `[BOOT] GOP: 1 handle found`, `boot_info.fb` byte-identical to pre-change. Bare metal iGPU+dGPU: `[BOOT] GOP: 2 handles found`, primary = ConsoleOut-attached display.

> **Test runner:** N/A (UEFI bootloader-only; no kernel test surface -- `init_gop` enumeration runs on live Boot Services protocols) | validation: serial `[BOOT] GOP: N handle(s)` on QEMU/WHPX + bare-metal multi-GPU; `boot_gop_handle` 32B layout is `_Static_assert`-pinned kernel+mirror.

> **Notes:**
> - Shipped `gop_enumerate_and_select()`/`gop_handle_is_conout()`/`gop_record_handle()` in `bootx64.c`; `init_gop()` enumerates all GOP handles, publishes `boot_info.gop_handles[4]` (v23 ABI), keeps the primary's `boot_info.fb` unchanged for single-GPU.
> - Primary = ConsoleOut device-path match, then `EFI_CONSOLE_OUT_DEVICE_GUID` tie-breaker, then largest resolution; secondaries carry validated geometry only. Codex design D1+D2 adopted in commit.
> - Downstream: `04-drivers-hardware/TODO-17 sec6` `display_register_head()` reads `gop_handles[]`; `gop_handle_count==0` falls back to single-head `boot_info.fb`.
> - Canonical doc: [docs/boot/boot-info-fields.md](../../docs/boot/boot-info-fields.md) "Multi-GPU GOP handles (v23)" + nested `boot_gop_handle`.
> - Scope boundary: this section owns bootloader enumeration + per-head ABI publish; kernel multi-head registration/compositing is TODO-17 sec6.

> **Verified:** 2026-06-17 | commit `3662b910` | 4/4 items | build OK | smoke PASS (TCG 2.64s, 2 handles found, 1280x800 BGRX to C:\)
> **Quality reviewed:** 2026-06-17 | Codex 8x (design, adversarial, consistency, perf, re-adversarial) | 5H+2M+1L fixed | scope: boot-code-quality

---

## 5. Secure Boot Enforcement Policy

Provide a kernel-lockdown enforcement policy gated on the canonical Secure Boot state. The state itself (SetupMode/AuditMode/DeployedMode + drift detection) is owned by [TODO-02 §5](TODO-02-uefi-hardening-secureboot.md#5-secure-boot-state-detection); this section is enforcement policy that CONSUMES the canonical state via the `uefi_secureboot_*` API, not duplicate variable reads.

**Files:** `src/kernel/uefi_runtime.c`, `include/kernel/uefi_runtime.h`, `include/kernel/boot_info.h`

> [!NOTE]
> Gap-audit 2026-05-01 narrowed this section's scope: the original "Read SetupMode/AuditMode/DeployedMode" item was duplicated work -- TODO-02 §5 already publishes those values via `uefi_secureboot_init()`. Removed to avoid drift between two readers; this section now consumes the canonical state.

- [ ] `boot.conf` key `SecureBootEnforce=0`: when 1, read `g_system_state.secure_boot_enforced` (canonical state from TODO-02 §5) and trigger kernel lockdown (-> XREF: `02-kernel-core/TODO-10-kernel-security-hardening.md`)
- [ ] Audit-mode trap-and-log: when `g_system_state.audit_mode == 1`, install a kernel hook that logs every Secure Boot policy violation (failed signature verify, MOK miss, etc.) to `HKLM\SYSTEM\SecureBoot\AuditLog\` without halting the boot. Lets operators dry-run enforcement.
- [ ] DeployedMode lockdown: when `g_system_state.deployed_mode == 1`, refuse to clear PK / KEK / db / dbx via `uefi_var_set()` from kernel space (returns `STATUS_ACCESS_DENIED`). Production fleets stay locked.
- [ ] Drift consumer: poll `uefi_secureboot_drift_detected()` (canonical API shipped in [TODO-02 §15](TODO-02-uefi-hardening-secureboot.md#15-post-boot-securebootrevalidation)) from the same kernel worker that fires `uefi_secureboot_revalidate_tick()`, and trigger immediate lockdown when the sticky flag transitions 0->1 between ticks. Alternatively register a callback via a future `uefi_secureboot_register_drift_listener()` if one is added when the periodic worker pattern lands.
- [ ] Serial log: `[SecureBoot] policy: enforce=%u audit=%u deployed=%u` (no longer logs the raw variable values -- those are TODO-02 §5's surface).
- [ ] Commit: `"kernel: Secure Boot enforcement policy consuming canonical state"`

**Test checkpoint:** QEMU Setup Mode: `SetupMode=1`. Enrolled PK: `SetupMode=0`. `SecureBootEnforce=1`: `g_system_state.secure_boot_enforced == 1`.

> [!NOTE]
> **Deferred 2026-06-17 -- ownership moved to `02-kernel-core/TODO-10 §16` (Secure Boot Lockdown Enforcement).** The enforcement-policy core blocks on the kernel lockdown mechanism, which does not exist; TODO-10 reserves the `boot.conf` lockdown knob (its line 38) and now owns it via §16. The `g_system_state.secure_boot_enforced` / `audit_mode` / `deployed_mode` fields referenced in the items above do NOT exist -- TODO-10 §16 consumes the canonical `uefi_secureboot_*` accessor API instead (adding `uefi_secureboot_deployed_mode()` / `uefi_secureboot_audit_mode()` over the existing `s_sb_*` statics). The genuinely-unblocked piece (DeployedMode `uefi_var_set` write-protection) ships there too, alongside the lockdown action it pairs with.

> **Deferred:** [H] Secure Boot enforcement policy core blocks on the kernel lockdown mechanism (does not exist) + the `boot.conf` lockdown knob is reserved by TODO-10 -> XREF: 02-kernel-core/TODO-10 §16 (item: "`kernel_lockdown_engage(reason)`" -- owns SecureBootEnforce knob + lockdown + DeployedMode write-protection + audit-log + drift-triggered lockdown)

---

## 6. SMBIOS Extended Type Parsing

Parse SMBIOS Type 2 (Baseboard), Type 3 (Chassis), Type 16 (Memory Array), Type 19 (Memory Mapped Address).

**Files:** `src/kernel/smbios.c`, `include/kernel/smbios.h`

- [x] Type 2: manufacturer, product, version, serial, asset tag -> `HKLM\HARDWARE\Baseboard\*` (`parse_type2` extended; per-field length guards)
- [x] Type 3: manufacturer, type code (bit7 lock stripped), serial, asset tag -> `HKLM\HARDWARE\Chassis\*`; `smbios_chassis_is_laptop()` over pure `smbios_chassis_type_is_mobile()` (codes 8/9/10/11/14/30/31/32)
- [x] Type 16: location, use, max capacity, device count, error-correction (offset 0x06) -> `HKLM\HARDWARE\MemoryArray\*` incl. `ErrorCorrection` (`parse_type16` via pure `smbios_type16_decode`)
- [x] Type 19: start/end address -> `HKLM\HARDWARE\MemoryArray\StartAddr`/`EndAddr` (`parse_type19` via pure `smbios_type19_decode`, min/max aggregation, end>=start reject)
- [x] Commit: `"kernel: SMBIOS Type 2/3/16/19 extended parsing -> Registry HARDWARE hives"`

**Test checkpoint:** QEMU: serial shows `SMBIOS: Registry populated: ... Chassis, MemoryArray`; `HKLM\HARDWARE\MemoryArray\ErrorCorrection` holds a valid enum (Unknown/None/ECC). Bare metal laptop: `Chassis\Type` = 9 (Laptop) and `smbios_chassis_is_laptop()` returns 1. Verify on QEMU WHPX/TCG + bare metal (firmware SMBIOS tables differ from QEMU synthetic).

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | `test_uefi_advanced.c` 13 sub-tests (chassis codes, KB->byte wrap, Type16 capacity/sentinel/short, Type19 inclusive/extended/mixed/reject + handle) | live registry populate confirmed via serial

> **Notes:**
> - **What shipped** -- `parse_type2/3/16/19` + pure decoders (`smbios_type16_decode`/`smbios_type19_decode`/`smbios_kb_to_bytes`/`smbios_chassis_type_is_mobile`) + accessors; Baseboard/Chassis/MemoryArray registry hives.
> - **How it runs** -- walker cases 3/16/19; Type 16 prefers the System-Memory array (Use==0x03); Type 19 folds into per-handle min/max summaries resolved against the selected array after the walk; `s_info` reset per walk.
> - **Downstream effects** -- `smbios_chassis_is_laptop()` feeds power/UI defaults (XREF `04-drivers-hardware/TODO-03`); `ErrorCorrection` gives Device Manager ECC-vs-non-ECC.
> - **Canonical doc** -- [`include/kernel/smbios.h`](../../include/kernel/smbios.h) (`SMBIOS_ECC_*` + pure-decoder contracts).
> - **Scope boundary** -- §6 owns Type 2/3/16/19 parse + HARDWARE hives; walker + Type 0/1/4/17 stay in `smbios.c` core; §7 owns dbx.
> - **Codex** -- design 4x + test-coverage + review (adversarial/consistency/perf + re-adversarial convergence); adoptions in commit history.

> **Verified:** 2026-06-19 | this commit | 5/5 items | build OK | tests 2836/2836 PASS (boot) + live registry populate
> **Quality reviewed:** 2026-06-19 | Codex 16x (design, test-coverage, adversarial, consistency, perf, re-adversarial) | 1H+9M+2L fixed | scope: kernel-code-quality

---

## 7. DBX Revocation List Sync

Detect when the installed UEFI dbx is missing revocations shipped with OS updates and warn proactively. DEFERRED: the freshness check is a security signal and needs an integrity-bound official revocation-list baseline (see the deferral note below); the authenticated write path is separately deferred.

**Files:** `src/kernel/uefi_runtime.c` (extend DB inventory), `src/kernel/secureboot_dbx.c` (new, freshness consumer), `resources/secureboot/dbx-latest.bin` (new)

> [!TIP]
> **Competitive edge:** Neither Win11 nor Linux proactively validates dbx freshness from within the OS. Impossible OS logs a boot warning when dbx is stale.

> [!NOTE]
> Do NOT add a second dbx parser. `src/kernel/uefi_runtime.c` already owns it: `secureboot_keys_init()` -> `read_security_db()` -> `count_sig_entries()` parse db/dbx/dbt and `secureboot_get_db_info()` exposes counts (mirrored to `HKLM\SYSTEM\SecureBoot\DbxEntries`/`DbxSha256`). But `read_security_db()` truncates dbx to an 8192-byte stack buffer and returns only counts, so freshness must EXTEND that owner, not fork it. Freshness uses subset/membership or a dbx2-style version compare -- NOT entry counts (dbx is APPEND_WRITE so installed is a superset; fwupd `org.uefi.dbx2` uses version, not count).

- [ ] Extend the DB inventory in `uefi_runtime.c`: full bounded dbx read (size from `GetVariable`, heap-backed, no 8192 truncation for the freshness path), reusing `count_sig_entries`
- [ ] Export a dbx membership/identity API over the parsed buffer (`secureboot_dbx_contains(const uint8_t sha256[32])` + a version/last-entry checksum accessor); keep the count mirror as a summary
- [ ] Ship `resources/secureboot/dbx-latest.bin` (official UEFI revocation list) per release, integrity-bound to a compiled-in SHA-256 digest (or signed manifest) so on-disk tampering cannot suppress/forge the stale signal
- [ ] `secureboot_dbx_check_freshness()`: compare ALL baseline `EFI_SIGNATURE_LIST` entries by `(type, owner, bytes)` (SHA-256 + X.509 + other) -- subset, never count; `secureboot_esl_contains_sha256` is only a helper
- [ ] Tri-state `current`/`stale`/`indeterminate` (baseline absent or digest-unverifiable -> indeterminate, not current)
- [ ] If stale: `klog(LOG_WARN, ...)` + set `HKLM\SYSTEM\SecureBoot\DbxStale = 1` + store installed/shipped identity
- [ ] Wire `secureboot_dbx_check_freshness()` at Phase 3 (`boot_desktop.c`) after VFS + registry ready; keep the firmware dbx read/cache in `secureboot_keys_init`
- [ ] -> XREF: `TODO-02-uefi-hardening-secureboot.md §17` -- SBAT generation-based revocation complements dbx hash lists
- [ ] Commit: `"kernel: DBX revocation freshness check (verified baseline, full-ESL subset compare)"`

- [ ] `secureboot_dbx_apply(path)` authenticated `APPEND_WRITE` of a signed dbx update -- deferred (see note below): needs `EFI_VARIABLE_AUTHENTICATION_2` write + SBAT-compat preflight + rollback guard (irreversible NVRAM write risks bricking)

**Test checkpoint:** QEMU with OVMF: freshness check runs without crash when dbx is empty (logs `dbx: indeterminate (no baseline)`); with a verified baseline missing from installed dbx, `HKLM\SYSTEM\SecureBoot\DbxStale = 1` and a `LOG_WARN` fires. Verify on bare metal -- firmware dbx population differs from OVMF.

> [!NOTE]
> **Deferred 2026-06-19 (Codex design review).** dbx freshness is a SECURITY signal (`DbxStale`), so it cannot ship as infrastructure-only: an absent or unverified on-disk baseline makes the check either a permanent no-op or tamperable (an attacker deletes the baseline to suppress detection, or injects hashes to force a false alarm). Shipping it that way is paper completion for a security feature. Authoritative operation requires (1) the official UEFI revocation-list baseline (external release data, not autonomously vendorable) integrity-bound to a compiled-in digest or signed release manifest; (2) full-`EFI_SIGNATURE_LIST`-type comparison (SHA-256 + X.509 + other), not SHA-256-only; (3) Phase-3 wiring after VFS + registry. None of the trust primitives (verified baseline + release-signing) exist yet. The authenticated `secureboot_dbx_apply` write additionally needs `EFI_VARIABLE_AUTHENTICATION_2` + a brick-safety preflight. The dbx read/parse/count surface already ships via [TODO-02 §9](TODO-02-uefi-hardening-secureboot.md#9-sbat-ops-secure-boot-db-registry-mirror-and-exitbootservices-retry).

> **Deferred:** [H] dbx freshness check blocked on an integrity-bound official revocation-list baseline (external release data + compiled-in digest / release-signing) + full-ESL comparison; authenticated apply blocked on `EFI_VARIABLE_AUTHENTICATION_2` write + brick-safety preflight -> XREF: 01-boot-platform/TODO-27 §7 (item: "Ship `resources/secureboot/dbx-latest.bin` ... integrity-bound to a compiled-in SHA-256 digest" -- this section owns the verified-baseline + full-ESL freshness once the release-signing primitive lands)

---

## OS Comparison

| ⭐  | Feature                   | 🪟 Win11                   | 🐧 Linux                    | 🚀 Impossible OS                       |
| --- | ------------------------- | -------------------------- | --------------------------- | -------------------------------------- |
| ⭐  | In-bootloader OS menu     | ❌ Separate BCD/bootmgr    | ❌ GRUB is separate         | ✅ §1 detect + chainload menu          |
| ⭐  | Firmware update advisor   | ⚠️ silent WU push only     | ⚠️ fwupd writes flash       | ⬜ §2 read-only LVFS; no UpdateCapsule |
| 💎  | UEFI memory W^X           | ✅ Since Win10 1607        | ✅ EFI_MEMORY_ATTRIBUTES    | ✅ §3 static MAT enforce               |
| 💎  | Multi-GPU GOP             | ✅ LocateHandleBuffer      | ✅ grub handle buffer       | ✅ §4 enum + ConOut primary            |
| 💎  | Secure Boot extended vars | ✅ SetupMode + Deployed    | ✅ efivarfs all SB vars     | ✅ T02 §5 state detection              |
| 💎  | Secure Boot enforcement   | ✅ HVCI lockdown           | ✅ kernel lockdown mode     | ⬜ §5 deferred -> D02 T10 §16          |
| 💎  | SMBIOS extended types     | ✅ WMI BaseBoard/Enclosure | ✅ /sys/firmware/dmi full   | ✅ §6 Type 2/3/16/19 + ECC             |
| 💎  | DBX revocation sync       | ✅ WU silent dbx push      | ✅ fwupd/dbxtool            | ⏸️ §7 deferred (baseline)              |
| ⭐  | Proactive dbx stale alert | ❌ Silent WU push only     | ❌ Requires manual fwupdmgr | ⏸️ §7 deferred (verified base)         |

---

## Unit Tests

> Wire into `test_runner_init()` via `test_register_uefi_advanced()`.

- [/] `src/kernel/test/test_uefi_advanced.c` created (TEST_CAT_BOOT, 13 sub-tests; shipped §6 commit `6f040b09`):
  - [x] §6: `smbios_chassis_type_is_mobile()` codes 8/9/10/11/14/30/31/32 true, desktop/server/unknown false
  - [x] §6: `smbios_kb_to_bytes()` incl. >4 GiB no-wrap; `smbios_type16_decode`/`smbios_type19_decode` sentinel/extended/inclusive-end/reject/handle fixtures
  - [/] §5: `HKLM\SYSTEM\SecureBoot\SetupMode` presence -- deferred with §5 (owner TODO-10 §16)
  - [/] §7: `secureboot_dbx_check_freshness()`/`secureboot_dbx_contains()` -- deferred with §7 (verified-baseline blocker)
- [x] Register in `test_runner_init()`: `test_register_uefi_advanced()` (shipped §6)
- [x] Commit: `"test: add uefi_advanced test suite"` (landed in §6 commit `6f040b09`)

---

## Verification

> **Test runner:** `scripts\debug\kernel\run-boot-tests.bat` (SUITE=boot) | 2836 kernel + 16 user-mode PASS (KVM 2026-06-19), 0 failures

- [ ] Boot menu appears when two GPT partitions present; countdown works (manual -- needs dual-OS GPT setup; QEMU/bare-metal)
- [ ] `[UEFI] W^X enforced on N runtime memory regions (static MAT)`, N > 0 (manual -- needs firmware exposing EFI_MEMORY_ATTRIBUTES_TABLE; this KVM OVMF lacks it so §3 logs the graceful enforcement-skipped path)
- [/] `[SecureBoot] SetupMode=...` in serial log -- deferred with §5 (enforcement-policy owner TODO-10 §16)
- [x] SMBIOS Chassis + MemoryArray populated: smoke 2026-06-19 logged `SMBIOS: Registry populated: BIOS, System, 1 CPU(s), 1 DIMM(s), Chassis, MemoryArray` (Baseboard absent on QEMU which omits Type 2 manufacturer)
- [/] `[SecureBoot] dbx: N entries` in serial log -- deferred with §7 (verified-baseline blocker); dbx counts already surface via TODO-02 §9
- [x] Commit: `"boot: uefi-advanced verified"` (this close-out commit)
