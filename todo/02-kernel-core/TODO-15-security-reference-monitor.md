---
schema_version: 1
id: security-reference-monitor
domain: 02-kernel-core
status: active
title: "TODO-15 -- Security Reference Monitor"
---

# TODO-15 -- Security Reference Monitor

> **Validated:** 2026-07-05 | validate-todo-file clean (structure / IO table / XREF / test wiring). §3/§4 are implemented-but-unstamped (DONE_UNSTAMPED); §4 has 3 open token-marshalling/lock items -> route to review in SECTIONS.
> **Gap-audited:** 2026-07-05 | gap-audit + codex-gap-audit; 7 gaps filed (WRITE_RESTRICTED §5/§9, conditional-ACE fail-closed §5, derived capability SIDs + well-known caps §14, UIPI XREF §6, NtAccessCheckAndAuditAlarm audit-sink fail-closed §12, Landlock/seccomp self-sandbox + circular-PPL + code-signing deferral owners fixed). All Branch A/C; no new sections.

> **Goal:** Implement the Windows Security Reference Monitor (SRM) -- the kernel subsystem that enforces every resource access decision in the OS. The SRM owns three things: the `ACCESS_TOKEN` object (who you are, what groups you belong to, what privileges you hold), the `SECURITY_DESCRIPTOR` + ACL machinery (who is allowed to do what to a named resource), and the `SeAccessCheck` engine that compares the two to produce an allow/deny decision. Without SRM, the OS has no file permissions, no process isolation, no privilege separation, and no UAC -- it is a flat single-user system where every process can touch every resource.

> [!IMPORTANT]
> **Current state:** SID/LUID primitives (§1) and privilege constants (§2) are implemented + reviewed. SECURITY_DESCRIPTOR/ACL/ACE types (§3) and the core ACCESS_TOKEN object (§4) are implemented but partial: §3 reviewed + downgraded to [/] -- its Win32 SD/ACL/ACE types + trusted-path helpers work, but the review found the untrusted-input parsers (self-relative SD unmarshal, ACL/ACE walk, `RtlLengthSid`) need bounded validators (latent OOB, no live caller yet); §4 still has 3 open token-marshalling/lock items (safe `NtSetInformationToken` setters, query marshalling, per-token SMP lock -> TODO-12 §16). The Object Manager (TODO-05 §8) integrates security descriptors. Remaining: SeAccessCheck engine (§5), MIC (§6), token assignment at process spawn (§7), privilege enforcement (§8), UAC/NtFilterToken (§9), Win32 wrappers (§10), SSDT wiring (§12), SD inheritance (§13), AppContainer tokens (§14), and the access denial explainer (§15).

---

## Inputs

- `src/kernel/sched/task.c` -- `struct task`, `task_exec`, `NtCreateProcess` path
- `include/kernel/sched/task.h` -- task struct (token field must be added)
- `src/kernel/mm/vmm.c` -- kernel/user access mode (UserMode / KernelMode)
- → XREF: `TODO-05-object-manager.md §8` -- Object Manager security descriptor integration; ObXxx calls `SeAccessCheck` before granting any handle
- → XREF: `TODO-11-peb-teb-user-abi.md §6` -- TEB carries `ImpersonationInfo` pointer (thread token)
- → XREF: `TODO-12-native-api-ssdt.md §1` -- NTSTATUS return codes used by all token/ACL syscalls
- → XREF: `TODO-12-native-api-ssdt.md §5` -- SSDT indices 0x00B0–0x00C5 reserved for security/token syscalls; §12 of this TODO wires NtOpenProcessToken, NtAccessCheck, NtCreateLowBoxToken, NtQueryAccessDenialReason, etc. into the SSDT
- → XREF: `TODO-21-process-model-extensions.md §2` -- process spawn path (`task_exec()` / future `NtCreateProcess`) is shared territory; §7 adds token duplication, T21 §2 adds standard handle wiring
- → XREF: `05-storage-filesystems/TODO-05-win32-file-io-api.md §4` -- file handle open (`CreateFile`) calls `SeAccessCheck` with `FILE_GENERIC_READ`/`WRITE` desired access
- → XREF: `TODO-14-registry-completion.md §2` -- registry KEY_* access rights enforcement uses `SeAccessCheck` (§5) and `SECURITY_DESCRIPTOR` (§3); also `SeBackupPrivilege` / `SeRestorePrivilege` (§8) for `RegSaveKey` / `RegRestoreKey`
- → XREF: `05-storage-filesystems/TODO-06-ixfs-core-win32-compat.md §5` -- IXFS security descriptors stored as `SECURITY_DESCRIPTOR` on inodes; SRM is the enforcement engine
- → XREF: `09-desktop-shell/TODO-06-security-accounts.md §11` -- UAC consent UI triggers token elevation; this TODO provides `NtFilterToken` + elevation protocol
- → XREF: `TODO-05-object-manager.md §1` -- §13 (SeAssignSecurity) is called by ObCreateObject when a named object is created in a directory
- → XREF: `TODO-02-kernel-configuration-policy.md §8` -- runtime config writes require `SeSystemProfilePrivilege` or administrator policy gates
- → XREF: `TODO-03-kernel-libraries.md §5` -- Monocypher SHA-256 used by §14 (AppContainer SID generation from package name hash)

---

## Outcome

- `ACCESS_TOKEN` kernel object: User SID, group SIDs (with attributes), privileges (enabled/default/removed), integrity level, primary/impersonation type.
- `SECURITY_DESCRIPTOR` type with absolute and self-relative formats, DACL/SACL, and per-object-type `GENERIC_MAPPING`.
- `SeAccessCheck` correctly grants or denies access based on DACL walk + owner bypass + kernel-mode bypass + restricted-token dual check.
- Mandatory Integrity Control (MIC): No-Write-Up enforced; No-Read-Up optional.
- `SeAssignSecurity` propagates inheritable ACEs from parent containers to new objects (files, directories, registry keys) with canonical ordering.
- AppContainer tokens (`NtCreateLowBoxToken`) provide sandboxed process execution with capability SIDs and default-deny access.
- Every kernel object type (file, process, thread, event, mutex, registry key) has a default security descriptor and enforces access via `ObpReferenceObjectByHandle`.
- UAC filtered-token split is structurally in place; consent UI wired in `08-desktop-shell`.
- Win32 token API surface (`OpenProcessToken`, `GetTokenInformation`, `AdjustTokenPrivileges`, `CreateRestrictedToken`, etc.) is complete.
- `NtQueryAccessDenialReason` and `accesswhy.exe` explain exactly which ACE/MIC/privilege caused an access denial -- unique to Impossible OS.

---

## Implementation Order

| ⭐  | Order | Deliverable                                       | Depends On          | Status |
| --- | :---: | ------------------------------------------------- | ------------------- | :----: |
| 💎  |   1   | SID & LUID primitives                             | --                  |  [x]   |
| 💎  |   2   | Privilege constants & PRIVILEGE_SET               | §1                  |  [x]   |
| 💎  |   3   | SECURITY_DESCRIPTOR, ACL, ACE types               | §1                  |  [/]   |
| 💎  |   4   | ACCESS_TOKEN object (primary)                     | §1, §2, §3, T05 §1  |  [/]   |
| 💎  |   5   | SeAccessCheck engine                              | §3, §4              |  [/]   |
| 💎  |   6   | Mandatory Integrity Control (MIC)                 | §4, §5              |  [/]   |
| 💎  |   7   | Process/thread token assignment & impersonation   | §4                  |  [ ]   |
| 💎  |   8   | SePrivilegeCheck & per-privilege enforcement      | §2, §4, §5          |  [ ]   |
| 💎  |   9   | UAC token split & NtFilterToken                   | §4, §6, §7          |  [ ]   |
| 💎  |  10   | Win32 security API wrappers                       | §4–§9, T12 §1       |  [ ]   |
| ⭐  |  11   | Live token inspector (`whoami.exe` + tray popout) | §4–§10              |  [ ]   |
| 💎  |  12   | Security/token syscalls wired to SSDT             | §4, §8, T12 §4      |  [ ]   |
| 💎  |  13   | SD inheritance / SeAssignSecurity                 | §3, §4, §5, T05 §1  |  [ ]   |
| 💎  |  14   | AppContainer / LowBox tokens                                      | §4, §5, §6, §9      |  [ ]   |
| ⭐  |  15   | Access denial explainer                                           | §5, §6, §8, §14     |  [ ]   |
| 💎  |  16   | Security primitive hardening (SHA-1 SID, 64-bit LUID, const-time) | §1, T03 §5          |  [ ]   |

> 💎 = parity work -- matches what Windows 11 and Linux already do.
> ⭐ = exclusive work -- Impossible OS is superior or first.

---

## 1. SID & LUID Primitives

- [x] Define `struct SID` in `include/kernel/security/sid.h`:
  ```c
  typedef struct {
      uint8_t  Revision;           /* always 1 */
      uint8_t  SubAuthorityCount;  /* 0–15 */
      uint8_t  IdentifierAuthority[6];
      uint32_t SubAuthority[];     /* variable-length */
  } SID;
  ```
- [x] Define well-known SID constants as initialised globals in `src/kernel/security/sid.c`:

  | Constant                      | Value          |
  |-------------------------------|----------------|
  | `SeNullSid`                   | `S-1-0-0`      |
  | `SeWorldSid` (Everyone)       | `S-1-1-0`      |
  | `SeCreatorOwnerSid`           | `S-1-3-0`      |
  | `SeNtAuthoritySid`            | `S-1-5`        |
  | `SeInteractiveSid`            | `S-1-5-4`      |
  | `SeServiceSid`                | `S-1-5-6`      |
  | `SeAnonymousLogonSid`         | `S-1-5-7`      |
  | `SeLocalSystemSid`            | `S-1-5-18`     |
  | `SeLocalServiceSid`           | `S-1-5-19`     |
  | `SeNetworkServiceSid`         | `S-1-5-20`     |
  | `SeBuiltinAdministratorsSid`  | `S-1-5-32-544` |
  | `SeBuiltinUsersSid`           | `S-1-5-32-545` |
  | `SeBuiltinGuestsSid`          | `S-1-5-32-546` |

- [x] Implement SID utilities:
  - `RtlLengthSid(sid)` → `8 + 4 * SubAuthorityCount`
  - `RtlEqualSid(a, b)` → `memcmp` after length check
  - `RtlCopySid(buf, sid)` → validated `memcpy`
  - `RtlInitializeSid(sid, authority, count)`
  - `RtlSubAuthoritySid(sid, n)` → pointer into SubAuthority array
  - `RtlConvertSidToString(str, sid, buf_size)` -- formats `S-1-X-Y-...`
  - `RtlCreateServiceSid(name, sid, len)` -- generates `S-1-5-80-<hash>` for service accounts (used by Service Manager)
- [x] Define `LUID` (64-bit opaque identifier):
  ```c
  typedef struct { uint32_t LowPart; int32_t HighPart; } LUID;
  ```
- [x] `RtlEqualLuid`, `RtlIsZeroLuid`
- [x] `NtAllocateLocallyUniqueId` -- monotonically incrementing counter, returned to user mode for dynamic LUID allocation
- [x] Commit: `"kernel/security: SID primitives and well-known SID table"`

> **Test runner:** `scripts\debug\kernel\run-security-tests.bat` (SUITE=security), 11 suites, 0 failures expected

> **Verified** (2026-04-13): SID struct at sid.h:25-30 matches Windows ABI (_Static_assert at sid.h:33-40). 13 well-known SIDs initialized in sid.c:34-50 with correct S-1-X-Y values. 7 SID utility functions implemented (RtlLengthSid, RtlEqualSid, RtlCopySid, RtlInitializeSid, RtlSubAuthoritySid, RtlConvertSidToString, RtlCreateServiceSid). LUID at luid.h:15-18, NtAllocateLocallyUniqueId at luid.c:13-19 (atomic counter, SMP-safe). All consumer functions guard with RtlValidSid. RtlInitializeSid clamps count to SID_MAX_SUB_AUTHORITIES. 48-bit authority formatting handles values above 32 bits. Build clean.
> **Accepted:** FNV-1a service SID derivation (32-bit entropy, not Windows SHA-1 compatible; acceptable for < 100 services) -> XREF: 02-kernel-core/TODO-15 §16, 09-desktop-shell/TODO-07 §1. LUID 32-bit counter (HighPart always 0; wrap at ~4B allocations) -> XREF: 02-kernel-core/TODO-15 §16. RtlEqualSid uses memcmp (timing variable; no user-mode oracle exists) -> XREF: 02-kernel-core/TODO-15 §16, 09-desktop-shell/TODO-07 §1.
> **Quality reviewed** (2026-04-13): dead code clean (all well-known SIDs consumed by token.c, acl.c, default_sds.c). Struct layouts consistent (RtlLengthSid and RtlValidSid agree on 8+4*N). Stack usage bounded (sid_str[80] in RtlAclToCStr). fnv1a_hash_name not in hot path (service registration only). RtlAclToCStr handles invalid SID from RtlConvertSidToString failure (emits "<invalid-sid>"). 3 new tests added: service SID generation, LUID allocator, malformed SID rejection.

---

## 2. Privilege Constants & PRIVILEGE_SET

- [x] Define all standard privilege LUIDs in `include/kernel/security/privileges.h` as `const LUID` values with `HighPart=0`, `LowPart=<n>`:

  | Constant                          | LP | When needed                       |
  |-----------------------------------|----|-----------------------------------|
  | `SeCreateTokenPrivilege`          |  2 | NtCreateToken                     |
  | `SeAssignPrimaryTokenPrivilege`   |  3 | NtAssignProcessToJobObject        |
  | `SeLockMemoryPrivilege`           |  4 | MmLockPages                       |
  | `SeIncreaseQuotaPrivilege`        |  5 | NtSetInformationProcess quota     |
  | `SeTcbPrivilege`                  |  7 | Token ops bypassing checks        |
  | `SeSecurityPrivilege`             |  8 | SACL read/write                   |
  | `SeTakeOwnershipPrivilege`        |  9 | Write owner without DACL          |
  | `SeLoadDriverPrivilege`           | 10 | `module_load()`                   |
  | `SeSystemProfilePrivilege`        | 11 | NtQuerySystemInformation (perf)   |
  | `SeSystemtimePrivilege`           | 12 | NtSetSystemTime                   |
  | `SeProfileSingleProcessPrivilege` | 13 | per-process profiling             |
  | `SeIncreaseBasePriorityPrivilege` | 14 | REALTIME_PRIORITY_CLASS           |
  | `SeCreatePagefilePrivilege`       | 15 | NtCreatePagingFile                |
  | `SeBackupPrivilege`               | 17 | read files ignoring DACL          |
  | `SeRestorePrivilege`              | 18 | write files ignoring DACL         |
  | `SeShutdownPrivilege`             | 19 | NtShutdownSystem                  |
  | `SeDebugPrivilege`                | 20 | NtOpenProcess any PID             |
  | `SeAuditPrivilege`                | 21 | NtAccessCheckAndAuditAlarm        |
  | `SeChangeNotifyPrivilege`         | 23 | bypass traverse-check (always on) |
  | `SeUndockPrivilege`               | 25 | laptop undock                     |
  | `SeManageVolumePrivilege`         | 28 | direct volume I/O                 |
  | `SeImpersonatePrivilege`          | 29 | NtImpersonateThread at higher IL  |
  | `SeCreateGlobalPrivilege`         | 30 | objects in global namespace       |
  | `SeCreateSymbolicLinkPrivilege`   | 35 | NtCreateSymbolicLinkObject        |

- [x] Define types:
  ```c
  typedef struct { LUID Luid; uint32_t Attributes; } LUID_AND_ATTRIBUTES;
  /* Attributes: SE_PRIVILEGE_ENABLED, SE_PRIVILEGE_ENABLED_BY_DEFAULT,
                 SE_PRIVILEGE_REMOVED, SE_PRIVILEGE_USED_FOR_ACCESS */

  typedef struct {
      uint32_t          PrivilegeCount;
      uint32_t          Control; /* PRIVILEGE_SET_ALL_NECESSARY */
      LUID_AND_ATTRIBUTES Privilege[]; /* variable */
  } PRIVILEGE_SET;

  typedef struct {
      uint32_t          PrivilegeCount;
      LUID_AND_ATTRIBUTES Privileges[];
  } TOKEN_PRIVILEGES;
  ```
- [x] `RtlPrivilegeSetToString(ps, buf, len)` -- debug helper
- [x] Commit: `"kernel/security: privilege LUID table and PRIVILEGE_SET types"`

> **Test runner:** `scripts\debug\kernel\run-security-tests.bat` (SUITE=security), 12 suites, 57 assertions, 0 failures
> **Verified on WHPX** (2026-04-14): 57 tests passed, 0 failed (0.3s)

> **Verified** (2026-04-14): 24 privilege LUID constants at privileges.h:46-69, all with HighPart=0 via SE_PRIVILEGE_LUID macro. LowPart values match Windows SDK exactly (gaps at 6,16,22,24,26,27 intentional). LUID_AND_ATTRIBUTES at privileges.h:79-82 (12 bytes, _Static_assert). PRIVILEGE_SET at privileges.h:89-93 (base 8 bytes, Control at offset 4, _Static_assert). TOKEN_PRIVILEGES at privileges.h:97-100 (base 4 bytes, _Static_assert). SE_PRIVILEGE_* attribute values match Windows SDK (ENABLED_BY_DEFAULT=1, ENABLED=2, REMOVED=4, USED_FOR_ACCESS=0x80000000). RtlPrivilegeLuidToName at privileges.c:71-84 (bounded scan, 24 entries). RtlPrivilegeSetToString at privileges.c:88-137 (remaining-length safe, len < 8 guard). All 24 privileges consumed in token.c SeCreateSystemToken/SeCreateUserToken. Build clean.
> **Accepted:** PRIVILEGE_SET_ALL_NECESSARY and SE_PRIVILEGE_USED_FOR_ACCESS defined but not consumed; these are ABI constants for struct completeness; consumers planned in -> XREF: 02-kernel-core/TODO-15 §3 (SePrivilegeCheck).
> **Quality reviewed** (2026-04-14): dead code clean (all 24 privileges in g_priv_names[] + token.c). Consistency verified: g_priv_names[] index mapping validated by test iterating all 24 LUIDs. RtlPrivilegeSetToString: len < 8 guard prevents WHPX freeze in snprintf variadic truncation path (root cause: compiler code gen edge case with small buffer + variadic call inside loop; standalone snprintf passes; function-internal call freezes). Remaining-length arithmetic safe. Performance bounded: linear scan over 24 entries max. Tests: all-24-LUID name mapping, unknown LUID rejection, RtlPrivilegeSetToString output + NULL + guard (len=4) + truncation (len=32), privilege struct ABI sizes.

---

## 3. SECURITY_DESCRIPTOR, ACL & ACE Types

- [x] Define in `include/kernel/security/acl.h` (Win32 security ABI types; field layout is the contract):
  <!-- spec-block-ok: Win32 SECURITY_DESCRIPTOR/ACL/ACE ABI layout is the contract -->
  ```c
  /* Absolute SECURITY_DESCRIPTOR (pointers) */
  typedef struct {
      uint8_t  Revision;   /* 1 */
      uint8_t  Sbz1;
      uint16_t Control;    /* SE_OWNER_DEFAULTED, SE_DACL_PRESENT,
                              SE_DACL_PROTECTED, SE_SACL_PRESENT,
                              SE_SELF_RELATIVE */
      SID     *Owner;
      SID     *Group;
      ACL     *Sacl;
      ACL     *Dacl;
  } SECURITY_DESCRIPTOR;

  typedef struct { uint8_t AclRevision; uint8_t Sbz1;
                   uint16_t AclSize; uint16_t AceCount; uint16_t Sbz2; } ACL;

  typedef struct { uint8_t AceType; uint8_t AceFlags;
                   uint16_t AceSize; } ACE_HEADER;

  typedef struct { ACE_HEADER Header; uint32_t Mask;
                   uint32_t SidStart; } ACCESS_ALLOWED_ACE;
  typedef struct { ACE_HEADER Header; uint32_t Mask;
                   uint32_t SidStart; } ACCESS_DENIED_ACE;
  typedef struct { ACE_HEADER Header; uint32_t Mask;
                   uint32_t SidStart; } SYSTEM_AUDIT_ACE;
  typedef struct { ACE_HEADER Header; uint32_t Mask;
                   uint32_t SidStart; } SYSTEM_MANDATORY_LABEL_ACE;
  ```
- [x] ACE type constants: `ACCESS_ALLOWED_ACE_TYPE=0`, `ACCESS_DENIED_ACE_TYPE=1`, `SYSTEM_AUDIT_ACE_TYPE=2`, `SYSTEM_MANDATORY_LABEL_ACE_TYPE=0x11`
- [x] ACE flag constants: `OBJECT_INHERIT_ACE`, `CONTAINER_INHERIT_ACE`, `INHERIT_ONLY_ACE`, `INHERITED_ACE`, `SUCCESSFUL_ACCESS_ACE_FLAG`, `FAILED_ACCESS_ACE_FLAG`
- [x] `RtlCreateAcl(acl, size, rev)` -- initialise empty ACL
- [x] `RtlAddAccessAllowedAce(acl, rev, mask, sid)` -- append allowed ACE
- [x] `RtlAddAccessDeniedAce(acl, rev, mask, sid)` -- append denied ACE
- [x] `RtlAddMandatoryAce(acl, rev, flags, mask, type, integrity_sid)`
- [x] `RtlGetAce(acl, index, ace_ptr)` -- walk ACE by index
- [x] `RtlAclToCStr(acl, buf, len)` -- debug dump (`"D:(A;;FA;;;SY)(A;;FA;;;BA)"`)
- [x] `RtlCreateSecurityDescriptor(sd, rev)`
- [x] `RtlSetOwnerSecurityDescriptor(sd, owner, defaulted)`
- [x] `RtlSetGroupSecurityDescriptor(sd, group, defaulted)`
- [x] `RtlSetDaclSecurityDescriptor(sd, present, dacl, defaulted)`
- [x] `RtlSetSaclSecurityDescriptor(sd, present, sacl, defaulted)`
- [x] `RtlGetOwnerSecurityDescriptor(sd, owner, defaulted)`
- [x] `RtlGetDaclSecurityDescriptor(sd, present, dacl, defaulted)`
- [x] `RtlAbsoluteToSelfRelativeSD(abs, rel_buf, rel_len)` -- marshal to flat buffer
- [x] `RtlSelfRelativeToAbsoluteSD(rel, rel_len, abs, abs_buf, abs_buf_len)` -- bounded unmarshal from an untrusted flat buffer
- [x] `SeCreateDefaultSD(type)` -- returns a self-relative SD with:
  - Owner = `SeLocalSystemSid`
  - DACL: `(A;;GA;;;SY)(A;;GA;;;BA)(A;;GR;;;WD)` -- System+Admins=FullControl, Everyone=ReadControl
  - Stored as static blobs in `src/kernel/security/default_sds.c`
- [x] Object types needing custom defaults:
  - `OB_TYPE_PROCESS` -- `(A;;GA;;;SY)(A;;0x1FFFFF;;;BA)(A;;0x1000;;;WD)` (create/terminate restricted for Everyone)
  - `OB_TYPE_TOKEN` -- `(A;;GA;;;SY)(A;;0x0008;;;OW)` (Query only for owner)
  - `OB_TYPE_REGISTRY_KEY` -- `(A;;GA;;;SY)(A;;GA;;;BA)(A;;GR;;;BU)` (BuiltinUsers=Read)
- [x] SECURITY [Critical]: `RtlSelfRelativeToAbsoluteSD` gained a `rel_len` arg (acl.c); each `Offset*` bounds-checked (clears 20B header, `< rel_len`) before deref, SID/ACL via bounded helpers, dest subtraction-form.
- [x] SECURITY [H]: bounded ACL/ACE walk `RtlValidAcl(acl,avail)` + `RtlGetAceEx(acl,index,ace,avail)` (acl.c) -- AclSize/AceSize bounds + per-ACE inline-SID fits `AceSize-8`. Trusted `RtlGetAce`/`acl_append_ace` unchanged.
- [x] SECURITY [H]: `RtlLengthSidBounded(sid,avail)` (sid.c) -- `avail>=8`, Revision, `count<=15`, `len<=avail` (subtraction-form). Trusted `RtlLengthSid` kept; untrusted import uses the bounded variant only.
- [ ] Select per-type default SD: ob.c always uses `SE_SD_TYPE_DEFAULT`; map `ObpProcessType`->PROCESS, `ObpTokenType`->TOKEN so process/token get the §3 DACLs once §5 honors `hdr->security`. -> XREF: 02-kernel-core/TODO-15 §5
- [x] Moved `SECURITY_DESCRIPTOR_RELATIVE` typedef into `acl.h` with size (20) + offset `_Static_assert`s; removed the private acl.c copy so untrusted parsers can inspect offsets.
- [x] SECURITY [H]: callback ACEs (0x09/0x0A/0x0D/0x0E) treated SID-bearing so `ace_body_valid` bounds-validates their inline SID; `RtlValidAcl` rejects bad `AclRevision`; import rejects wrong Revision / non-self-relative headers.
- [ ] Object ACE types (0x05-0x08/0x0B-0x0C): variable Flags+GUID prefix before the SID; `RtlValidAcl` now REJECTS them (unparseable). Add constants + Flags-aware SID offset to SUPPORT them when AD-style extended-rights ACEs are needed.
- [ ] `RtlSelfRelativeToAbsoluteSD` packs SID/ACL bodies contiguously into `abs_buf`; an ACL body can land unaligned -> strict-alignment arch (ARM64) fault. Align each component copy to 4 bytes when the import path is wired.
- [x] Commit: `"kernel/security: SECURITY_DESCRIPTOR, ACL, ACE types and helpers"`

**Test checkpoint:** `RtlCreateSecurityDescriptor` + `RtlSetDaclSecurityDescriptor` + `RtlAbsoluteToSelfRelativeSD` round-trips (self-relative buffer parses back to identical ACEs via `RtlSelfRelativeToAbsoluteSD`). `SeCreateDefaultSD(SE_SD_TYPE_REGISTRY_KEY)` yields the `(A;;GA;;;SY)(A;;GA;;;BA)(A;;GR;;;BU)` DACL. `RtlAclToCStr` dumps `D:(A;;FA;;;SY)`. `RtlGetAce` walks each ACE by index. Test on: QEMU WHPX + TCG.

> **Test runner:** `scripts\debug\kernel\run-security-tests.bat` (SUITE=security) | 1041 passed, 0 failures
> **Notes:**
> - Shipped: Win32 SD/ACL/ACE types + Rtl* helpers + `SeCreateDefaultSD` blobs, plus bounded untrusted-input validators `RtlLengthSidBounded` (sid.c), `RtlValidAcl`/`RtlGetAceEx` and a `rel_len`-guarded `RtlSelfRelativeToAbsoluteSD` (acl.c).
> - Safety model: self-relative SD import bounds-checks every offset/SID/ACL against `rel_len` before deref (subtraction-form); trusted `RtlLengthSid`/`RtlGetAce` kept for kernel-built data, untrusted paths use bounded variants.
> - Tests: 3 new security tests exercise valid + malformed/OOB inputs (avail<hdr, count>15, offset in/past header, AceSize overrun/zero, callback-ACE corrupt SID, object-ACE reject, bad AclRevision); malformed cases run without faulting -- `SUITE=security` 1041 passed, 0 failed.
> - Latent: the three OOB fixes have NO production caller yet (IXFS/registry/`NtSetSecurityObject` import unwired); validators land ahead of that wiring. `SECURITY_DESCRIPTOR_RELATIVE` now in acl.h with size/offset asserts.
> - Scope boundary: §5 owns SeAccessCheck honoring `hdr->security`; per-type default-SD selection in ob.c (item 4) stays open, blocked on that §5 wiring.
> **Verified:** 2026-07-05 | commit `9c12e1d8` (+ review fixes) | 25/28 items | build OK | tests 1041/1041 PASS
> **Deferred:** [M] per-type default-SD selection in ob.c (`ObpProcessType`/`ObpTokenType` -> PROCESS/TOKEN DACLs) blocked until §5 honors `hdr->security` -> XREF: 02-kernel-core/TODO-15 §5 (item: "In `ObpReferenceObjectByHandle` ... call `SeAccessCheck(object->SecurityDescriptor ...)`")
> **Deferred:** [L] object ACE type SUPPORT (0x05-0x08/0x0B-0x0C) not implemented -- `RtlValidAcl` now REJECTS them (no OOB), support deferred -> XREF: 02-kernel-core/TODO-15 §3 (item: "Object ACE types (0x05-0x08/0x0B-0x0C)")
> **Deferred:** [L] `abs_buf` component copies can land unaligned -> strict-align arch (ARM64) fault when the import path is wired -> XREF: 02-kernel-core/TODO-15 §3 (item: "`RtlSelfRelativeToAbsoluteSD` packs SID/ACL bodies")
> **Deferred:** [L] `SE_DACL_PRESENT`/`OffsetDacl` presence-vs-flag cross-check on import -> XREF: 02-kernel-core/TODO-15 §5 (item: "Normalize: cross-check `SE_DACL_PRESENT`")
> **Quality reviewed:** 2026-07-05 | Codex 13x (design, adversarial, re-adversarial, consistency, perf) | 3H+3M+3L fixed, 1M+3L deferred | scope: kernel-code-quality

---

## 4. ACCESS_TOKEN Object

- [x] Define `struct ACCESS_TOKEN` in `include/kernel/security/token.h` (Win32 token ABI; field layout is the contract):
  <!-- spec-block-ok: Win32 ACCESS_TOKEN ABI layout is the contract -->
  ```c
  typedef struct {
      SID               *UserSid;
      uint32_t           GroupCount;
      SID_AND_ATTRIBUTES Groups[TOKEN_MAX_GROUPS];  /* 32 groups */
      uint32_t           PrivilegeCount;
      LUID_AND_ATTRIBUTES Privileges[TOKEN_MAX_PRIVS]; /* 36 privs */
      SID               *PrimaryGroup;
      ACL               *DefaultDacl;
      TOKEN_TYPE         TokenType;          /* Primary or Impersonation */
      SECURITY_IMPERSONATION_LEVEL ImpersonationLevel;
      LUID               TokenId;            /* unique, from NtAllocateLocallyUniqueId */
      LUID               AuthenticationId;   /* logon session LUID */
      LUID               ModifiedId;         /* changes on every AdjustPrivileges */
      uint32_t           SessionId;
      SID               *IntegrityLevelSid;  /* one of SeIL_* below */
      uint32_t           IntegrityPolicy;    /* TOKEN_MANDATORY_POLICY_* */
      bool               IsElevated;
      LUID               LinkedTokenId;      /* full admin token linked to filtered */
      TOKEN_ELEVATION_TYPE ElevationType;    /* Limited / Default / Full */
      SID               *RestrictedSids;     /* for NtFilterToken */
      uint32_t           RestrictedSidCount;
  } ACCESS_TOKEN;
  ```
- [x] Integrity level SID constants:
  - `SeILUntrusted` = `S-1-16-0`
  - `SeILLow` = `S-1-16-4096`
  - `SeILMedium` = `S-1-16-8192`
  - `SeILHigh` = `S-1-16-12288`
  - `SeILSystem` = `S-1-16-16384`
- [x] Register `ObpTokenType` via `ob_create_type("Token", sizeof(ACCESS_TOKEN), ...)` (→ XREF `TODO-05 §1`); token objects are reference-counted kernel objects
- [x] `SeCreateSystemToken()` -- called in Phase 0 of kernel init (→ XREF `TODO-01-kernel-init-sequencing.md §2`): UserSid=`SeLocalSystemSid`, Groups=`{SeBuiltinAdministratorsSid | SE_GROUP_ENABLED, SeWorldSid | SE_GROUP_ENABLED}`, all privileges enabled, IL=System; stored in `PsInitialSystemProcess->Token`
- [x] `SeCreateUserToken(user_sid, admin)` -- creates Medium IL primary token for interactive logon (called by login manager in `09-desktop-shell`); if `admin`, also creates High IL linked token for elevation; groups include `SeBuiltinUsersSid`; privileges: `SeChangeNotifyPrivilege` enabled by default; `SeShutdownPrivilege`/`SeUndockPrivilege` enabled by default; admin token adds `SeBackupPrivilege`, `SeRestorePrivilege`, `SeLoadDriverPrivilege` as disabled by default (present but off until elevated)
- [x] `NtOpenProcessToken(ProcessHandle, DesiredAccess, TokenHandle)` -- opens the primary token of a process; access check on the process object for `PROCESS_QUERY_INFORMATION`; returns handle with requested access
- [x] `NtOpenThreadToken(ThreadHandle, DesiredAccess, OpenAsSelf, TokenHandle)` -- returns impersonation token or `STATUS_NO_TOKEN` if thread is not impersonating
- [x] `NtQueryInformationToken(hToken, class, buf, len, retlen)` -- implement `TokenUser`, `TokenGroups`, `TokenPrivileges`, `TokenOwner`, `TokenPrimaryGroup`, `TokenDefaultDacl`, `TokenType`, `TokenImpersonationLevel`, `TokenStatistics`, `TokenIntegrityLevel`, `TokenElevationType`, `TokenLinkedToken`, `TokenIsElevated`
- [x] `NtDuplicateToken(Existing, Access, ObjAttr, EffectiveOnly, Type, New)` -- deep-copies token struct, allocates new `TokenId` LUID; `EffectiveOnly` strips disabled privileges and groups
- [x] `NtAdjustPrivilegesToken(hToken, DisableAll, NewState, BufferLen, PreviousState, ReturnLen)` -- for each LUID_AND_ATTRIBUTES in `NewState`: find matching privilege in token, apply SE_PRIVILEGE_ENABLED / SE_PRIVILEGE_REMOVED; requires `TOKEN_ADJUST_PRIVILEGES`; write previous state to `PreviousState`; return `STATUS_NOT_ALL_ASSIGNED` if any LUID not found
- [x] `NtAdjustGroupsToken(hToken, ResetToDefault, NewState, BufferLen, PreviousState, ReturnLen)` -- enable/disable group SIDs; cannot re-enable a `SE_GROUP_USE_FOR_DENY_ONLY` group (immutable once disabled)
- [ ] Deep-copy setters for `NtSetInformationToken` (IntegrityLevel/PrimaryGroup/DefaultDacl/Owner): validate + alloc-copy the SID/ACL BEFORE the lock; swap under lock; free the OLD field after unlock. Remove the reject at nt_token.c:299.
- [ ] TokenXxx query marshalling: `token.c` NtQueryInformationToken returns kernel SID pointers; TokenGroups uses the wrong header offset. Marshal each class into a self-contained caller buffer. XREF: TODO-12 §16.
- [ ] **Per-token lock for SMP safety** -- add `spinlock_t lock` to `ACCESS_TOKEN` covering Groups[], Privileges[], IntegrityLevelSid, DefaultDacl, and other mutable fields. All NtAdjust*Token / NtSet*Token / NtQuery*Token paths must acquire (write for mutate, read for query). Required to make TODO-12 §16 token syscalls SMP-safe when scheduler exposes them to multiple threads of the same process.
- [x] Commit: `"kernel/security: ACCESS_TOKEN object, SeCreateSystemToken, NtOpenProcessToken"`

**Test checkpoint:** `SeCreateSystemToken()` yields UserSid=`S-1-5-18`, IL=System, all privileges enabled. `NtDuplicateToken` produces a deep copy with a distinct `TokenId`. `NtAdjustPrivilegesToken` enable/disable round-trips; missing LUID returns `STATUS_NOT_ALL_ASSIGNED`. `NtQueryInformationToken(TokenIntegrityLevel)` returns the IL SID; `TokenUser` returns UserSid. `ObpTokenType` is a refcounted Ob type. Test on: QEMU WHPX + TCG.

> **Test runner:** `scripts\debug\kernel\run-security-tests.bat` (SUITE=security) | on implementation of the token-hardening items
> **Notes:**
> - Shipped: the core `ACCESS_TOKEN` object + `SeCreateSystemToken`/`SeCreateUserToken` + `NtOpen*/Query/Adjust*` handlers (token.c/nt_token.c). Types + trusted-path token ops work.
> - Downgraded [x]->[/]: design review specced the token-hardening (per-token lock, deep-copy setters, query-marshalling leak fix, `OwnerSid` field, `NtDuplicateToken` deep-copy) -- filed as precise [ ] items above.
> - BLOCKED/latent: setters need §5's handle-rights gate before enabling; the query kernel-pointer-leak is latent (§7 unwired, no live process token). Lock/alloc ordering per spinlock rules.
> - Scope boundary: §5 owns handle-rights enforcement + SeAccessCheck; §7 owns wiring tokens into processes.
> **Deferred:** [Critical] `NtSetInformationToken` setters must gate on `TOKEN_ADJUST_DEFAULT` before enabling -> XREF: 02-kernel-core/TODO-15 §5 (item: "Enforce per-handle `granted_access` on token mutation syscalls")
> **Deferred:** [H] token-hardening (per-token lock + deep-copy setters + query kernel-pointer-leak fix + OwnerSid + NtDuplicateToken deep-copy) design-specced, latent until §7 wires tokens into processes -> XREF: 02-kernel-core/TODO-15 §7 (item: "Add `ACCESS_TOKEN *Token;` to `struct task`")

---

## 5. SeAccessCheck Engine

- [ ] Define `SECURITY_SUBJECT_CONTEXT`:
  ```c
  typedef struct {
      ACCESS_TOKEN *ClientToken;    /* NULL if not impersonating */
      SECURITY_IMPERSONATION_LEVEL ImpersonationLevel;
      ACCESS_TOKEN *PrimaryToken;   /* always the process token */
      bool          LockHeld;
  } SECURITY_SUBJECT_CONTEXT;
  ```
- [ ] `SeCaptureSubjectContext(ctx)` -- reads `task_current()->token` (primary) and TEB impersonation token (if any) into `ctx`; thread-safe snapshot
- [ ] `SeReleaseSubjectContext(ctx)` -- dereferences token pointers
- [ ] Define `GENERIC_MAPPING` per object type in `include/kernel/security/generic_mapping.h`:
  ```c
  typedef struct {
      uint32_t GenericRead; uint32_t GenericWrite;
      uint32_t GenericExecute; uint32_t GenericAll;
  } GENERIC_MAPPING;
  ```
- [ ] Declare mappings for: File (standard Unix rwx mapping), Process, Thread, Token, Registry Key, Event, Mutex, Semaphore, Waitable Timer
- [ ] `RtlMapGenericMask(access, mapping)` -- replaces `GENERIC_READ`/`WRITE`/ `EXECUTE`/`ALL` bits with type-specific masks in-place
- [ ] Implement `SeAccessCheck(sd, ctx, ctx_locked, desired, prev_granted, privs, mapping, mode, granted, status)`:
  0. **SD-format normalize**: `ob_alloc_object` stores BOTH formats in `hdr->security` -- absolute creator SDs (`SeCreateCreatorSD`) and self-relative default SDs (`SeCreateDefaultSD`). Branch on the `SE_SELF_RELATIVE` control bit before dereferencing `Owner`/`Dacl` (self-relative fields are byte offsets, not pointers); reading one as the other is a wild access. Owned from TODO-05 §1 review.
  1. **Kernel bypass**: if `mode == KernelMode` → `*granted = desired`, return `TRUE`
  2. **Owner bypass**: if `ctx->PrimaryToken->UserSid` == SD owner → set `READ_CONTROL | WRITE_DAC` bits in accumulated access without DACL check
  3. **DACL absent**: if `sd->Dacl == NULL` → grant all; if DACL present but empty (AceCount=0) → deny all
  4. **MIC pre-check**: call `SeCheckMandatoryAccess(ctx, sd, desired)` (§6); if MIC denies → `*status = STATUS_ACCESS_DENIED`, return `FALSE`
  5. **ACE walk**: iterate DACL ACEs in order; for each ACE:
     - `ACCESS_DENIED_ACE`: if any SID in token groups or user matches ACE SID AND `(desired & ACE->Mask) != 0` → deny immediately
     - `ACCESS_ALLOWED_ACE`: if SID matches AND ACE was not `INHERIT_ONLY` → accumulate `ACE->Mask` bits into granted mask
  6. **Restricted token check**: if the effective token has `RestrictedSidCount > 0`, perform a **second DACL walk** using only the restricting SIDs as the subject identity; access is granted only if BOTH the normal DACL walk AND the restricted-SID DACL walk allow the desired access
  7. **Result**: if `(desired & accumulated) == desired` (and restricted check passes if applicable) → `*granted = desired`, return `TRUE`; else deny
- [ ] `SeAccessCheckByType(sd, ObjectType, ctx, desired, privs, mapping, mode, granted, status)` -- same as above but also checks object-type ACEs (optional, `OBJECT_ACCESS_ACE_TYPE` -- leave as stub for now)
- [ ] Write access-check unit test (pure kernel function, no QEMU needed):
  ```
  - SD: Owner=System, DACL=(Deny BA ReadControl)(Allow SY FullControl)(Allow BU ReadControl)
  - Subject: user=BA, groups=[BA, BU]
  - Desired: ReadControl → expect DENY (denial ACE hit first)
  - Subject: user=SY → expect GRANT
  - Subject: user=BU, desired=ReadControl → expect GRANT
  ```
- [ ] In `ObpReferenceObjectByHandle` (→ XREF `TODO-05 §2`): after locating the handle entry, call `SeAccessCheck(object->SecurityDescriptor, ctx, FALSE, desired, 0, NULL, object->Type->GenericMapping, UserMode, &granted, &status)`; return `STATUS_ACCESS_DENIED` if check fails
- [ ] `NtAccessCheck(sd, token, desired, mapping, privs, priv_len, granted, status)` syscall -- user-mode accessible wrapper for testing DACLs without opening an object
- [ ] **Enforce per-handle `granted_access` on token mutation syscalls**: TODO-12 §16 handlers (`NtAdjustPrivilegesToken`, `NtAdjustGroupsToken`, `NtSetInformationToken`, `NtQueryInformationToken`) in `src/kernel/nt/nt_token.c` currently resolve a token handle via `resolve_token_handle()` and operate without checking what rights the handle was opened with. Add a `resolve_token_handle_access(HANDLE h, ACCESS_MASK required)` helper that reads `HANDLE_TABLE_ENTRY.granted_access` and returns `STATUS_ACCESS_DENIED` if `(entry->granted_access & required) != required`. Required masks per Win32 SDK: `TOKEN_ADJUST_PRIVILEGES = 0x20` for `NtAdjustPrivilegesToken`; `TOKEN_ADJUST_GROUPS = 0x40` for `NtAdjustGroupsToken`; `TOKEN_QUERY = 0x08` for `NtQueryInformationToken`; `TOKEN_ADJUST_DEFAULT = 0x80` for `NtSetInformationToken`. This auto-closes TODO-12 §16 Accepted #2 (token handle granted_access not enforced).
- [ ] **Enforce per-handle `granted_access` on section syscalls**: TODO-12 §18 handlers (`NtMapViewOfSection`, `NtExtendSection`, `NtQuerySection`) in `src/kernel/nt/nt_section.c` currently call `ObpLookupHandle` + type check, then operate on `SECTION_OBJECT*` without inspecting `HANDLE_TABLE_ENTRY.granted_access`. Required masks per Windows SDK winnt.h: `SECTION_MAP_READ = 0x0004` for read-mapping, `SECTION_MAP_WRITE = 0x0002` for read-write map, `SECTION_MAP_EXECUTE = 0x0008` for image/exec, `SECTION_EXTEND_SIZE = 0x0010` for `NtExtendSection`, `SECTION_QUERY = 0x0001` for `NtQuerySection`. After the `ObpReferenceObjectByHandle` primitive lands (-> XREF `TODO-05 §2`), add an inline check `if ((granted & required) != required) return STATUS_ACCESS_DENIED;` at the top of each section handler. Add a unit test in `test_ob.c` opening a section with `SECTION_QUERY` only and asserting `NtMapViewOfSection` returns `STATUS_ACCESS_DENIED`. This auto-closes TODO-12 §18 Accepted (section handle granted_access not enforced).
- [ ] **Authorize process-termination syscalls**: the `SYS_KILL` (INT 0x80) handler in `src/kernel/sched/syscall.c` and `NtTerminateProcess` in `src/kernel/nt/nt_process.c` today let ANY ring-3 task mark ANY other task `TASK_DEAD` via PID lookup -- no parentage check, no ACL check, no privilege check. Required mask per Windows SDK winnt.h: `PROCESS_TERMINATE = 0x0001`. After `ObpReferenceObjectByHandle` + process OB type land, retrofit both handlers to: (a) accept only handle-based addressing (reject the current PID-only SYS_KILL shape), (b) require `PROCESS_TERMINATE` in the handle's granted_access, (c) honour the process OB type's security descriptor. Add a negative user-mode test (extend `00-infrastructure/TODO-04 §12 test_process.exe`) that asserts killing an unrelated task (non-child, e.g. the launcher task PID) fails with `STATUS_ACCESS_DENIED` or `-1`. Until this lands, SYS_KILL stamps a "cross-task kill authorization not gated" Accepted XREF via 00-infrastructure/TODO-04 §12.
- [ ] `TOKEN_WRITE_RESTRICTED` (§9): run the restricted-SID second DACL pass ONLY for write-class desired access (write mask via the object `GENERIC_MAPPING`); non-write access skips it. -> XREF: 02-kernel-core/TODO-15 §9
- [ ] Conditional/callback ACEs (`*_CALLBACK_ACE` 0x9/0xA): until expr-eval exists, DENY not skip -- an unknown conditional ACE whose Mask intersects desired with a matchable SID returns `STATUS_ACCESS_DENIED`. -> XREF: 02-kernel-core/TODO-15 §3
- [ ] SeAccessCheck DACL-walk + SD-normalize: §3's bounded validators (`RtlValidAcl`/`RtlGetAceEx`/`RtlLengthSidBounded`/`rel_len`) landed -- normalize untrusted SDs through them to a canonical ABSOLUTE form. -> XREF: 02-kernel-core/TODO-15 §3
- [ ] Normalize: cross-check `SE_DACL_PRESENT`/`SE_SACL_PRESENT` control flags against non-zero `OffsetDacl`/`OffsetSacl` on import so the flags cannot disagree with actual DACL/SACL presence. -> XREF: 02-kernel-core/TODO-15 §3
- [ ] Ordering (escalation-critical): MIC pre-check runs BEFORE any final DACL grant; owner-bypass only ACCUMULATES `READ_CONTROL|WRITE_DAC` (no early return); a matching DENY ACE wins over any later ALLOW.
- [ ] SHIP FIRST (unblocks §4, independent of the DACL core): the `resolve_token_handle_access` granted_access item below is a standalone commit -- reads `HANDLE_TABLE_ENTRY.granted_access` only, no ACL walk.
- [ ] Commit: `"kernel/security: SeAccessCheck engine and ObpReferenceObjectByHandle integration"`

**Test checkpoint:** `SeAccessCheck` with KernelMode → always grants. SD with deny-ACE for BA before allow-ACE → BA token denied (deny takes precedence). SD with NULL DACL → all granted. SD with empty DACL (AceCount=0) → all denied. Owner requesting READ_CONTROL → granted via owner bypass. Restricted token with restricting SID not in DACL → denied even though normal SIDs match. Serial log: `"[SRM] SeAccessCheck: desired=0x%x granted=0x%x result=%s"`. Test on: QEMU WHPX + TCG (pure kernel logic, no hardware).

> **Test runner:** `scripts\debug\kernel\run-security-tests.bat` (SUITE=security) | on implementation
> **Notes:**
> - Design-specced (Codex): SeAccessCheck 8-step engine + GENERIC_MAPPING + subject context + granted_access enforcement; ordering rules deny-before-allow, owner-bypass-accumulate-only, MIC-before-grant, conditional-ACE DENY-not-skip.
> - Unblocked: §3's bounded validators (rel_len, RtlValidAcl/RtlGetAceEx, RtlLengthSidBounded) have LANDED; the DACL-walk/SD-normalize core now normalizes untrusted SDs through them to a canonical validated absolute form.
> - Ships-first subset: `resolve_token_handle_access` granted_access enforcement is independent of the ACL walk and unblocks §4's setters.
> - Scope boundary: §3 owns the bounded SD/ACL validators; §6 owns the MIC pre-check; §7 owns token-to-process wiring.
> **Deferred:** [Critical] SeAccessCheck DACL-walk/SD-normalize engine design-captured but not yet implemented; §3's bounded validators now available to build it safely -> XREF: 02-kernel-core/TODO-15 §5 (item: "Implement `SeAccessCheck(sd, ctx, ...)`")
> **Deferred:** [H] granted_access enforcement (`resolve_token_handle_access`) ships first + the ACE-ordering/owner-bypass/WRITE_RESTRICTED/conditional-ACE engine, design-captured -> XREF: 02-kernel-core/TODO-15 §5 (item: "Enforce per-handle `granted_access` on token mutation syscalls")

---

## 6. Mandatory Integrity Control (MIC)

- [x] `SeGetTokenIntegrityLevel(token)` (mic.c) -- last SubAuthority of `token->IntegrityLevelSid` (0/4096/8192/12288/16384); MEDIUM default on NULL/malformed.
- [x] `SeGetObjectIntegrityLevel(sd)` (mic.c) -- SACL `SYSTEM_MANDATORY_LABEL_ACE` RID; MEDIUM if no SACL/label, SYSTEM (fail-closed) on malformed SACL via `RtlValidAcl`.
- [x] `SeCompareMandatoryLevels(subject_il, object_il)` (mic.c) -> -1/0/+1.
- [x] `SeCheckMandatoryAccess(token, sd, desired, mapping)` (mic.c; `token` not `ctx` -- SUBJECT_CONTEXT deferred; `mapping` is the object `GENERIC_MAPPING` so object-specific rights are classified) -- No-*-Up policy:
  - Compute `subject_il = SeGetTokenIntegrityLevel(effective_token)`
  - Compute `object_il = SeGetObjectIntegrityLevel(sd)`
  - Read `SYSTEM_MANDATORY_LABEL_ACE->Mask` policy bits:
    - `SYSTEM_MANDATORY_LABEL_NO_WRITE_UP (0x1)` -- default; if `subject_il < object_il` AND `desired` contains write access bits → deny (Windows value; matches acl.h)
    - `SYSTEM_MANDATORY_LABEL_NO_READ_UP (0x2)` -- optional; if `subject_il < object_il` AND `desired` contains read access bits → deny
    - `SYSTEM_MANDATORY_LABEL_NO_EXECUTE_UP (0x4)` -- if `subject_il < object_il` AND `desired` contains execute bits → deny
  - Returns `STATUS_ACCESS_DENIED` or `STATUS_SUCCESS`
- [x] `SeCheckMandatoryAccess` folds the object `GENERIC_MAPPING` (`generic_mapping.h`) into its access classes so object-specific rights cannot bypass No-*-Up; §5 owns per-type instances.
- [ ] Default object IL assignment: kernel objects created by System process get `SeILSystem`; objects created by user process inherit creator's IL
- [ ] UIPI note: MIC also gates cross-IL window-message sends (UIPI), but that path is compositor/Win32k-owned, not an SRM object DACL check. -> XREF: 08-graphics-ui/TODO-15-win32k-shadow-ssdt.md §25 (item: "NtUserChangeWindowMessageFilterEx")
- [ ] When `NtCreateProcess` copies the parent token (§7), child token IL = `min(parent_IL, process_image_IL)`; image IL read from PE/ELF resource (`RT_MANIFEST`, requested execution level: `asInvoker`→same, `requireAdministrator`→High)
- [ ] Low IL sandbox mode: token with `SeILLow` is denied write to `%USERPROFILE%\*` (only `%LOCALAPPDATA%\Low\*` writable); enforced by MIC at `SeCheckMandatoryAccess` time
- [ ] Commit: `"kernel/security: Mandatory Integrity Control, No-Write-Up policy"`

**Test checkpoint:** `SeCheckMandatoryAccess` with Low IL token + GENERIC_WRITE on Medium IL object → `STATUS_ACCESS_DENIED` (No-Write-Up). System IL writing Medium IL → allowed. No-Read-Up policy set, Low IL reading Medium IL → denied. Default policy (No-Write-Up only), Low IL reading Medium IL → allowed. Child process spawned from Medium IL parent → child IL == Medium. Serial log: `"[SRM] MIC: subject_il=%u object_il=%u policy=0x%x result=%s"`. Test on: QEMU WHPX + TCG.

> **Test runner:** `scripts\debug\kernel\run-security-tests.bat` (SUITE=security) | 1058 passed, 0 failures
> **Notes:**
> - Shipped: `mic.c`/`mic.h` -- `SeGetTokenIntegrityLevel`, `SeGetObjectIntegrityLevel`, `SeCompareMandatoryLevels`, `SeCheckMandatoryAccess` (No-*-Up, `GENERIC_MAPPING`-aware) + IL-RID/`MIC_*_MASK` constants + shared `generic_mapping.h`.
> - Integration: pure functions on a caller token/SD (no global state/locks/alloc); SACL walk uses §3's `RtlValidAcl`+`RtlGetAceEx` (imported SD cannot OOB; malformed SACL fails closed). No live caller yet -- SeAccessCheck consumes them.
> - Tests: `test_mic` covers token IL (incl non-IL-authority reject), object IL (Medium/High/System-fail-closed), compare, No-Write-Up + No-Read-Up, object-specific-bit bypass closed via mapping, malformed-SACL fail-closed; `SUITE=security` 1058 passed, 0 failed.
> - Design (Codex): `SeCheckMandatoryAccess(token, ...)` not `ctx` (SUBJECT_CONTEXT deferred); defensive SACL validation; Medium default on valid-but-unlabeled, fail-closed on malformed.
> - Scope boundary: SeAccessCheck owns MIC-before-DACL wiring + `GENERIC_MAPPING` precision + the `[SRM] MIC:` log; token assignment owns child-IL=min; object-IL-at-creation + Low-IL FS sandbox stay open.

---

## 7. Process/Thread Token Assignment & Impersonation

- [ ] Add `ACCESS_TOKEN *Token;` to `struct task` in `include/kernel/sched/task.h`
- [ ] Add `ACCESS_TOKEN *ImpersonationToken;` -- thread-level override; NULL = use process token
- [ ] `PsReferencePrimaryToken(task)` -- increments token refcount and returns ptr
- [ ] `PsDereferencePrimaryToken(token)` -- decrements; frees on zero
- [ ] In `task_exec()` (→ shared path with `TODO-21 §2`): call `NtDuplicateToken(parent->Token, TOKEN_ALL_ACCESS, NULL, FALSE, TokenPrimary, &child->Token)` -- child starts with a deep copy of parent's primary token
- [ ] `SeCreateSystemToken()` result assigned to `PsInitialSystemProcess->Token` during Phase 0 kernel init (→ XREF `TODO-01-kernel-init-sequencing.md §2`)
- [ ] `NtImpersonateThread(ThreadHandle, ImpThreadHandle, ImpLevel)` -- duplicate source thread token as impersonation token; set `ImpersonationToken`; needs `SeImpersonatePrivilege` if IL higher than current
- [ ] `NtSetInformationThread(ThreadHandle, ThreadImpersonationToken, TokenHandle, sizeof(HANDLE))` -- explicit impersonation token assignment (NULL handle = revert)
- [ ] `RevertToSelf()` Win32 wrapper -- sets impersonation token to NULL
- [ ] `ImpersonateSelf(ImpersonationLevel)` -- duplicates own primary token as impersonation token; used before adjusting privileges for a short operation
- [ ] `SeQuerySubjectContextToken(ctx)` -- returns impersonation token if present AND impersonation level ≥ `SecurityIdentification`; else returns primary token; used by SeAccessCheck §5 as the "effective" token
- [ ] Commit: `"kernel/security: task token field, spawn token copy, thread impersonation"`

**Test checkpoint:** After `task_exec()`, child task's `token` is non-NULL and distinct from parent's (deep copy). `PsReferencePrimaryToken(child)` returns valid token with same UserSid as parent. `NtImpersonateThread` sets impersonation token; subsequent `SeCaptureSubjectContext` uses it as effective token. `RevertToSelf()` clears impersonation; effective token reverts to primary. Serial log: `"[SRM] Token assigned to pid=%u"`, `"[SRM] Thread %u impersonating at level %u"`. Test on: QEMU WHPX + TCG.

---

## 8. SePrivilegeCheck & Per-Privilege Enforcement

- [ ] `SePrivilegeCheck(PrivilegeSet, ctx, AccessMode)`:
  - If `AccessMode == KernelMode` → return `TRUE`
  - For each LUID in `PrivilegeSet->Privilege`: scan effective token's `Privileges[]` for matching LUID with `SE_PRIVILEGE_ENABLED` attribute; if `PRIVILEGE_SET_ALL_NECESSARY`, all LUIDs must match; otherwise any one match suffices
  - Write SACL audit record if `SE_PRIVILEGE_USED_FOR_ACCESS` (future)
- [ ] `SeSinglePrivilegeCheck(Privilege, AccessMode)` -- common single-LUID shortcut; called throughout kernel for specific privilege gates
- [ ] `SeCheckPrivilegedObject(PrivReq, Object, Desired, Mode)` -- combines `SeSinglePrivilegeCheck` with `SeAccessCheck`; used by backup/restore paths
- [ ] `module_load()` -- `SeSinglePrivilegeCheck(SeLoadDriverPrivilege, UserMode)` before loading any `.kmod` from user request
- [ ] `NtShutdownSystem` / `NtReboot` -- `SeSinglePrivilegeCheck( SeShutdownPrivilege, UserMode)`
- [ ] `NtSystemDebugControl` / `NtOpenProcess` with `PROCESS_ALL_ACCESS` on another-user's process -- `SeSinglePrivilegeCheck(SeDebugPrivilege, UserMode)`
- [ ] `NtSetSystemTime` -- `SeSinglePrivilegeCheck(SeSystemtimePrivilege, UserMode)`
- [ ] `NtCreateSymbolicLinkObject` -- `SeSinglePrivilegeCheck( SeCreateSymbolicLinkPrivilege, UserMode)` for permanent symlinks
- [ ] `NtQuerySystemInformation(SystemPerformanceInformation)` -- requires `SeSystemProfilePrivilege` if `mode == UserMode`
- [ ] `RegSaveKey`/`RegRestoreKey` (+ `NtSaveKey`/`NtRestoreKey`) -- `SeSinglePrivilegeCheck(SeBackup/SeRestorePrivilege, UserMode)` then run the `hive_save`/`hive_load` body (today fail-closed) -> XREF: 02-kernel-core/TODO-14 §2
- [ ] Commit: `"kernel/security: SePrivilegeCheck, privilege gates for driver load, shutdown, debug"`

**Test checkpoint:** Token with SeShutdownPrivilege disabled: `SeSinglePrivilegeCheck(SeShutdownPrivilege, UserMode)` → FALSE. After `NtAdjustPrivilegesToken` to enable → TRUE. `PRIVILEGE_SET_ALL_NECESSARY` with two LUIDs, one missing → FALSE. KernelMode → always TRUE regardless of token. `NtShutdownSystem` from unprivileged token → `STATUS_PRIVILEGE_NOT_HELD`. Serial log: `"[SRM] SePrivilegeCheck: %s = %s"`. Test on: QEMU WHPX + TCG.

---

## 9. UAC Token Split & NtFilterToken

- [ ] `NtFilterToken(ExistingToken, Flags, SidsToDisable, PrivilegesToDelete, RestrictedSids, NewToken)`:
  - `DISABLE_MAX_PRIVILEGE` (flag): mark all privileges except `SeChangeNotifyPrivilege` as `SE_PRIVILEGE_REMOVED`
  - `SidsToDisable`: mark matched group SIDs with `SE_GROUP_USE_FOR_DENY_ONLY` (present in deny checks but not allow checks)
  - `PrivilegesToDelete`: remove matching privilege entries entirely
  - `RestrictedSids`: append to token as restricted SID list (second DACL pass required -- access must be allowed by BOTH the normal DACL walk AND a walk of the restricted SID list)
  - Sets `TOKEN_IS_RESTRICTED` flag on new token
  - Used internally by UAC to produce the "filtered" Medium token for admin users
- [ ] `SeCreateLinkedTokenPair(FullAdminToken, FilteredToken)`:
  - `FullAdminToken->LinkedTokenId` = `FilteredToken->TokenId`
  - `FilteredToken->LinkedTokenId` = `FullAdminToken->TokenId`
  - `FilteredToken->ElevationType = TokenElevationTypeLimited`
  - `FullAdminToken->ElevationType = TokenElevationTypeFull`
  - Both tokens reference each other; when user requests elevation, kernel resolves the linked token from the filtered one
- [ ] `NtQueryInformationToken(TokenLinkedToken)` -- returns handle to the linked token; requires `TOKEN_QUERY` and that the caller holds `SeTcbPrivilege` (prevents unprivileged elevation discovery)
- [ ] `NtRequestTokenElevation(ProcessHandle, hToken, ElevationType)` -- kernel side of the elevation flow:
  1. Check calling process has `TokenElevationTypeLimited` token
  2. Signal the consent UI process (→ XREF `09-desktop-shell/TODO-06-security-accounts.md §11`) via a dedicated kernel event object
  3. Wait for consent UI to signal approval or denial event
  4. On approval: `NtSetInformationProcess(ProcessHandle, ProcessAccessToken, &linked_token)` -- replaces the process token with the full-admin linked token
  5. On denial: return `STATUS_PRIVILEGE_NOT_HELD`
- [ ] Note: full consent UI implementation is in `08-desktop-shell` -- this TODO provides only the kernel side of the handshake
- [ ] `WRITE_RESTRICTED` flag: `TOKEN_WRITE_RESTRICTED` token flag; when set, §5's restricted-SID second pass applies ONLY to write-class access, so read/execute is not falsely denied. -> XREF: 02-kernel-core/TODO-15 §5
- [ ] Commit: `"kernel/security: NtFilterToken, linked token pair, UAC elevation protocol"`

**Test checkpoint:** `NtFilterToken` with `DISABLE_MAX_PRIVILEGE` → new token has only SeChangeNotifyPrivilege enabled. `NtFilterToken` with SidsToDisable=[BA] → BA group has `SE_GROUP_USE_FOR_DENY_ONLY`. Linked token pair: `NtQueryInformationToken(TokenLinkedToken)` on filtered → returns handle to full admin token. `ElevationType` of filtered == `TokenElevationTypeLimited`. Serial log: `"[SRM] NtFilterToken: %u privileges removed, %u groups disabled"`. Test on: QEMU WHPX + TCG.

---

## 10. Win32 Security API Wrappers

- [ ] `OpenProcessToken(hProcess, DesiredAccess, phToken)` → `NtOpenProcessToken`
- [ ] `OpenThreadToken(hThread, DesiredAccess, OpenAsSelf, phToken)` → `NtOpenThreadToken`
- [ ] `GetTokenInformation(hToken, class, buf, len, retlen)` → `NtQueryInformationToken`
- [ ] `SetTokenInformation(hToken, class, buf, len)` → `NtSetInformationToken`
- [ ] `AdjustTokenPrivileges(hToken, DisableAll, NewState, BufferLen, PreviousState, ReturnLen)` → `NtAdjustPrivilegesToken`; note: Win32 returns TRUE even for `STATUS_NOT_ALL_ASSIGNED` (set last-error instead)
- [ ] `CheckTokenMembership(hToken, SidToCheck, IsMember)` -- scan token groups for matching SID with `SE_GROUP_ENABLED` attribute; NULL token = current thread effective token
- [ ] `IsUserAnAdmin()` -- `CheckTokenMembership(NULL, SeBuiltinAdministratorsSid, &member)`; returns TRUE only if token is elevated admin (High IL)
- [ ] `IsTokenRestricted(hToken)` → check `TOKEN_IS_RESTRICTED` flag in token
- [ ] `CreateRestrictedToken(hToken, Flags, SidsToDisable[], PrivilegesToDelete[], SidsToRestrict[], NewToken)` → `NtFilterToken`; flags: `DISABLE_MAX_PRIVILEGE`, `SANDBOX_INERT`, `WRITE_RESTRICTED`, `LUA_TOKEN`
- [ ] `ConvertSidToStringSidW(Sid, StringSid)` -- formats `S-1-X-Y-...` into heap-allocated `WCHAR*` (caller frees with `LocalFree`)
- [ ] `ConvertStringSidToSidW(StringSid, Sid)` -- parse `S-1-...` string back to SID blob
- [ ] `AllocateAndInitializeSid(IdentifierAuthority, SubAuthorityCount, ...)` -- up to 8 sub-authority args; allocates SID blob (caller frees with `FreeSid`)
- [ ] `FreeSid(Sid)` -- wraps `kfree`
- [ ] `EqualSid`, `CopySid`, `LengthSid`, `IsValidSid` Win32 wrappers
- [ ] `GetSecurityInfo(handle, ObjectType, SecurityInfo, Owner, Group, Dacl, Sacl, SD)` → `NtQuerySecurityObject`
- [ ] `SetSecurityInfo(handle, ObjectType, SecurityInfo, Owner, Group, Dacl, Sacl)` → `NtSetSecurityObject`
- [ ] `GetNamedSecurityInfoW(name, ObjectType, SecurityInfo, ...)` -- resolves path to file handle, then calls `NtQuerySecurityObject`
- [ ] `SetNamedSecurityInfoW(name, ObjectType, SecurityInfo, ...)` -- resolve + `NtSetSecurityObject`; requires `WRITE_DAC` or `SE_SECURITY_PRIVILEGE` for SACL
- [ ] `ConvertStringSecurityDescriptorToSecurityDescriptorW(SDDL, Revision, SD, SDSize)` -- minimal SDDL parser: parse `O:XX G:XX D:...(A;;XX;;;XX)...` syntax; supports `SY`=System, `BA`=Admins, `BU`=Users, `WD`=Everyone aliases
- [ ] `ConvertSecurityDescriptorToStringSecurityDescriptorW(SD, Revision, SecurityInfo, StringSD, StringSDLen)` -- reverse; produces SDDL string
- [ ] Commit: `"kernel/security: Win32 token, SID, and security descriptor API wrappers"`

**Test checkpoint:** `OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)` → valid handle. `GetTokenInformation(TokenUser)` → returns correct UserSid. `ConvertSidToStringSidW(SeLocalSystemSid)` → `"S-1-5-18"`. `ConvertStringSecurityDescriptorToSecurityDescriptorW("D:(A;;GA;;;SY)")` → valid SD with one ACE. `AdjustTokenPrivileges` enable/disable round-trip succeeds. Serial log: `"[SRM] Win32 security API test passed"`. Test on: QEMU WHPX + TCG.

---

## 11. Live Token Inspector

- [ ] `src/apps/whoami/whoami.c` -- command-line tool; calls `OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)` then `GetTokenInformation` for each class; formats output as aligned table. CLI usage:
  ```
  whoami [/user] [/groups] [/priv] [/all]
    /user    -- print current user SID and account name
    /groups  -- print all group SIDs with attributes (Enabled/Disabled/DenyOnly)
    /priv    -- print all privileges with Enabled/Disabled/Removed status
    /all     -- equivalent to /user /groups /priv
  ```
- [ ] System tray right-click → "Token Info" → flyout showing: current user, integrity level badge (colour-coded: Low=yellow, Medium=green, High=orange, System=red), admin status, elevation type, top 5 privileges
- [ ] Useful for developers to verify elevation state without opening a terminal
- [ ] Commit: `"kernel/security: whoami.exe and token tray popout"`

**Test checkpoint:** `whoami /all` in QEMU serial console shows: user SID (`S-1-5-18` for system), group list with attributes (Enabled/Disabled/DenyOnly), and privilege table with status column. Output columns are tab-aligned. `whoami /priv` shows at least 20 privilege entries. Tray popout (stretch): right-click tray → "Token Info" → flyout renders. Serial log: `"[SRM] whoami: user=%s groups=%u privs=%u"`. Test on: QEMU WHPX + TCG.

---

## 12. Security and Token Syscalls Wired to SSDT
Register all token and access control NtXxx entry points in the SSDT. Most implementations already exist in `src/kernel/security/token.c` and `luid.c`. (→ XREF: TODO-12-native-api-ssdt.md §5, §16)

- [x] `NtOpenProcessToken(ProcessHandle, DesiredAccess, TokenHandle)` → SSDT 0x00B0 -- wired in TODO-12 §16 (nt_token.c)
- [x] `NtOpenProcessTokenEx(ProcessHandle, DesiredAccess, HandleAttributes, TokenHandle)` → SSDT 0x00B1 -- wired in TODO-12 §16 (nt_token.c)
- [x] `NtOpenThreadToken(ThreadHandle, DesiredAccess, OpenAsSelf, TokenHandle)` → SSDT 0x00B2 -- wired in TODO-12 §16 (nt_token.c)
- [x] `NtOpenThreadTokenEx(...)` → SSDT 0x00B3 -- wired in TODO-12 §16 (nt_token.c)
- [x] `NtQueryInformationToken(TokenHandle, TokenInformationClass, Buffer, Length, ReturnLength)` → SSDT 0x00B4 (14 info classes) -- wired in TODO-12 §16 (nt_token.c)
- [/] `NtSetInformationToken(TokenHandle, TokenInformationClass, Buffer, Length)` → SSDT 0x00B5 -- wired in TODO-12 §16 (nt_token.c); all settable classes currently return STATUS_INVALID_INFO_CLASS pending deep-copy setters (→ §5 above)
- [x] `NtAdjustPrivilegesToken(TokenHandle, DisableAll, NewState, BufLen, PrevState, RetLen)` → SSDT 0x00B6 -- wired in TODO-12 §16 (nt_token.c)
- [x] `NtAdjustGroupsToken(...)` → SSDT 0x00B7 -- wired in TODO-12 §16 (nt_token.c)
- [ ] `NtDuplicateToken(ExistingHandle, DesiredAccess, ObjAttrs, EffectiveOnly, TokenType, NewHandle)` → SSDT 0x00B8 -- deferred to TODO-12 §29 (lifecycle)
- [ ] `NtFilterToken(ExistingHandle, Flags, SidsToDisable, PrivsToDelete, RestrictedSids, NewHandle)` → SSDT 0x00B9
- [ ] `NtCreateToken(...)` → SSDT 0x00BA: privileged operation for LSA
- [ ] `NtCreateLowBoxToken(NewToken, ExistingToken, DesiredAccess, ObjectAttributes, AppContainerSid, CapabilityCount, Capabilities, HandleCount, Handles)` → SSDT 0x00BB: route to AppContainer token creation (§14)
- [ ] `NtAccessCheck(SD, ClientToken, DesiredAccess, GenericMapping, PrivSet, PrivSetLen, GrantedAccess, AccessStatus)` → SSDT 0x00BC: route to `SeAccessCheck()` (§5)
- [ ] `NtPrivilegeCheck(ClientToken, RequiredPrivileges, Result)` → SSDT 0x00BF: route to `SePrivilegeCheck()` (§8)
- [ ] `NtSetSecurityObject(Handle, SecurityInformation, SD)` → SSDT 0x00C1
- [ ] `NtQuerySecurityObject(Handle, SecurityInformation, SD, Length, LengthNeeded)` → SSDT 0x00C2
- [x] `NtAllocateLocallyUniqueId(Luid)` → SSDT 0x00C3 -- wired in TODO-12 §16 (nt_token.c, SSDT wrapper around luid.c)
- [ ] `NtAccessCheckAndAuditAlarm(...)` → SSDT 0x00C4: access check + gate on `SeAuditPrivilege`; audit-record SINK deferred (see Deferred features) -- return the check result, do NOT claim an audit was written.
- [ ] `NtQueryAccessDenialReason(Handle, DesiredAccess, DenialReason)` → SSDT 0x00C5: route to §15 denial explainer
- [ ] All functions return `NTSTATUS`; use codes from `include/kernel/nt/ntstatus.h` (TODO-12 §1)
- [ ] Commit: `"kernel/security: wire token and access control syscalls to SSDT (0x00B0–0x00C5)"`

**Test checkpoint:** `NtOpenProcessToken` on current process returns valid handle. `NtQueryInformationToken(TokenUser)` returns correct SID. `NtAccessCheck` against DACL returns correct granted access. `NtAdjustPrivilegesToken` enables `SeShutdownPrivilege`.

---

## 13. Security Descriptor Inheritance (SeAssignSecurity)

When a new object is created (file, directory, registry key, process) without an explicit security descriptor, the SRM must build one by inheriting ACEs from the parent container's DACL/SACL. Without this, every object gets a static default SD and fine-grained per-directory permissions are impossible.

- [ ] Define `SeAssignSecurity(ParentSD, CreatorSD, NewSD, IsDirectory, SubjectContext, GenericMapping, PoolType)`:
  1. If `CreatorSD` is non-NULL and has an explicit DACL (`SE_DACL_PRESENT` without `SE_DACL_DEFAULTED`): use `CreatorSD->Dacl` as the new DACL
  2. Else if `ParentSD` has inheritable ACEs: build new DACL from parent's inheritable ACEs (see inheritance rules below)
  3. Else: use the `DefaultDacl` from the creator's token (`SubjectContext->PrimaryToken->DefaultDacl`)
  4. Owner = `CreatorSD->Owner` if specified, else `SubjectContext->PrimaryToken->UserSid`
  5. Group = `CreatorSD->Group` if specified, else `SubjectContext->PrimaryToken->PrimaryGroup`
  6. Apply same logic for SACL (requires `SeSecurityPrivilege` to specify explicit SACL)
- [ ] ACE inheritance rules -- for each ACE in `ParentSD->Dacl`:
  - `OBJECT_INHERIT_ACE` flag: ACE is inherited by non-container children (files)
  - `CONTAINER_INHERIT_ACE` flag: ACE is inherited by container children (directories)
  - `NO_PROPAGATE_INHERIT` flag: inherited ACE does NOT propagate to grandchildren
  - `INHERIT_ONLY_ACE` flag: ACE applies only to children, not to the parent itself
  - Inherited ACEs get the `INHERITED_ACE` flag set to distinguish them from explicit ACEs
- [ ] Canonical ACE ordering in the new DACL:
  1. Explicit deny ACEs
  2. Explicit allow ACEs
  3. Inherited deny ACEs (in parent order)
  4. Inherited allow ACEs (in parent order)
- [ ] `SeAssignSecurityEx(ParentSD, CreatorSD, NewSD, ObjectType, IsDirectory, AutoInheritFlags, SubjectContext, GenericMapping, PoolType)` -- extended version with `SEF_DACL_AUTO_INHERIT` / `SEF_SACL_AUTO_INHERIT` flags for automatic propagation to existing children
- [ ] Wire `SeAssignSecurity` into `ObCreateObject` (→ XREF `TODO-05 §1`): when creating a named object in a directory, pass the parent directory's SD as `ParentSD`
- [ ] Wire into VFS `CreateFile` / `CreateDirectory` paths (→ XREF `05-storage-filesystems/TODO-05 §4`): IXFS inode creation inherits parent directory's SD
- [ ] `SE_DACL_PROTECTED` flag on child SD: if set, blocks all ACE inheritance from parent (child uses only its explicit ACEs)
- [ ] Commit: `"kernel/security: SeAssignSecurity -- SD inheritance, ACE propagation, canonical ordering"`

**Test checkpoint:** Create directory with DACL granting BA full control with `CONTAINER_INHERIT_ACE | OBJECT_INHERIT_ACE`. Create file inside directory with no explicit SD. Verify file's DACL contains inherited ACE for BA with `INHERITED_ACE` flag set. Create subdirectory -- verify inherited ACE propagates. Create file in subdirectory with `SE_DACL_PROTECTED` -- verify no inherited ACEs. Serial log: `"[SRM] SeAssignSecurity: inherited N ACEs from parent"`. Test on: QEMU WHPX + TCG.

---

## 14. AppContainer / LowBox Tokens

AppContainer is the primary process sandboxing mechanism in modern Windows (used by UWP apps, browsers, and sandboxed legacy apps). An AppContainer token runs at Low IL with an AppContainer SID (`S-1-15-2-<hash>`) and a capability SID list, enforcing strict resource isolation with file/registry virtualization.

> [!TIP]
> Neither Win11's AppContainer (requires UWP/MSIX packaging) nor Linux's namespaces+seccomp (complex multi-syscall setup) are developer-friendly. Impossible OS can provide a single `CreateAppContainerToken()` call with a human-readable capability list -- sandbox any process in one line.

- [ ] Define `SECURITY_CAPABILITIES` structure:
  ```c
  typedef struct {
      SID               *AppContainerSid;
      SID_AND_ATTRIBUTES *Capabilities;
      uint32_t           CapabilityCount;
      uint32_t           Reserved;
  } SECURITY_CAPABILITIES;
  ```
- [ ] Well-known capability SID constants (`S-1-15-3-<N>`):
  - `internetClient` (1), `internetClientServer` (2), `privateNetworkClientServer` (3)
  - `picturesLibrary` (4), `videosLibrary` (5), `musicLibrary` (6), `documentsLibrary` (7)
  - `enterpriseAuthentication` (8), `sharedUserCertificates` (9), `removableStorage` (10)
- [ ] `NtCreateLowBoxToken(NewToken, ExistingToken, DesiredAccess, ObjectAttributes, AppContainerSid, CapabilityCount, Capabilities, HandleCount, Handles)`:
  - Duplicate `ExistingToken` → strip all privileges except `SeChangeNotifyPrivilege`
  - Set IL to `SeILLow`
  - Attach `AppContainerSid` and capability SID list to the new token
  - Set `TOKEN_IS_APPCONTAINER` flag
  - Optionally inherit `HandleCount` handles from the parent process (e.g., pipes for IPC)
- [ ] AppContainer access check in `SeAccessCheck` (§5): when token has `TOKEN_IS_APPCONTAINER`:
  - Default-deny all named objects that don't have an ACE explicitly granting access to the AppContainer SID or one of its capability SIDs
  - `ALL_APPLICATION_PACKAGES` SID (`S-1-15-2-1`) grants access to all AppContainers
- [ ] Win32 API wrappers:
  - `CreateAppContainerProfile(AppContainerName, DisplayName, Description, Capabilities, CapCount, AppContainerSid)` -- generates AppContainer SID from SHA-256 of `AppContainerName`
  - `DeleteAppContainerProfile(AppContainerName)` -- removes profile
  - `GetAppContainerFolderPath(AppContainerSid, Path)` -- returns per-AppContainer data directory
  - `CreateAppContainerToken(Token, SecurityCapabilities, NewToken)` -- documented wrapper around `NtCreateLowBoxToken`
- [ ] `NtQueryInformationToken(TokenIsAppContainer)` -- return `TRUE` for AppContainer tokens
- [ ] `NtQueryInformationToken(TokenAppContainerSid)` -- return the AppContainer SID
- [ ] `NtQueryInformationToken(TokenCapabilities)` -- return capability SID list
- [ ] Wire `NtCreateLowBoxToken` to SSDT (index 0x00BB per NT convention)
- [ ] Derived capability SIDs (`RtlDeriveCapabilitySidsFromName`): uppercase + SHA-256 (§16/Monocypher) a cap name into `S-1-15-3-1024-<4 subauth>`; needed for mic/webcam/location/custom caps beyond the 10 fixed RIDs. Add test vectors.
- [ ] Add remaining fixed well-known capability SIDs (Appointments, Contacts, etc. per `WELL_KNOWN_SID_TYPE`) + the `ALL_RESTRICTED_APPLICATION_PACKAGES` SID (`S-1-15-2-2`) alongside `ALL_APPLICATION_PACKAGES`.
- [ ] Commit: `"kernel/security: AppContainer -- NtCreateLowBoxToken, capability SIDs, sandbox access check"`

**Test checkpoint:** Create an AppContainer token from the system token. Verify `TokenIsAppContainer` returns TRUE. Verify `TokenIntegrityLevel` returns Low. Create a file with no AppContainer ACE -- verify AppContainer token gets `STATUS_ACCESS_DENIED`. Add `ALL_APPLICATION_PACKAGES` ACE to the file -- verify access granted. `CreateAppContainerProfile("TestApp")` returns a deterministic SID. Serial log: `"[SRM] AppContainer token created: S-1-15-2-<hash>"`. Test on: QEMU WHPX + TCG.

---

## 15. Access Denial Explainer

> [!TIP]
> Neither Windows (buries denials in Event Viewer) nor Linux (returns `EACCES` with no context) tells the developer **why** access was denied. Impossible OS provides a kernel-side explainer that returns a structured denial reason -- which ACE denied, which SID failed, which MIC level blocked, which privilege was missing. This alone would save developers hours of debugging.

- [ ] Define `ACCESS_DENIAL_REASON` structure:
  ```c
  typedef struct {
      uint32_t ReasonCode;      /* DENIAL_REASON_DACL_ACE, DENIAL_REASON_MIC,
                                   DENIAL_REASON_PRIVILEGE, DENIAL_REASON_APPCONTAINER,
                                   DENIAL_REASON_RESTRICTED_SID */
      SID     *DeniedBySid;     /* the ACE SID that caused the denial (DACL) */
      uint32_t DeniedMask;      /* access bits that were denied */
      uint32_t SubjectIL;       /* subject integrity level (MIC denials) */
      uint32_t ObjectIL;        /* object integrity level (MIC denials) */
      LUID     MissingPrivilege; /* LUID of the missing privilege */
  } ACCESS_DENIAL_REASON;
  ```
- [ ] Extend `SeAccessCheck` with optional `ACCESS_DENIAL_REASON *DenialReason` parameter:
  - On DACL deny: fill `DeniedBySid` with the ACE's SID, `DeniedMask` with the conflicting bits
  - On MIC deny: fill `SubjectIL` and `ObjectIL`
  - On privilege deny: fill `MissingPrivilege`
  - On AppContainer deny: set `ReasonCode = DENIAL_REASON_APPCONTAINER`
  - On restricted SID deny: set `ReasonCode = DENIAL_REASON_RESTRICTED_SID`
- [ ] `NtQueryAccessDenialReason(Handle, DesiredAccess, DenialReason)` -- syscall that performs a speculative access check and returns the denial reason without actually opening the object; useful for diagnostic tools
- [ ] Win32 wrapper: `QueryAccessDenialReason(lpFileName, DesiredAccess, pReason)` -- resolves path, queries SD, runs speculative check, returns structured reason
- [ ] `accesswhy.exe` CLI tool:
  ```
  accesswhy C:\Impossible\System32\config\SAM
    DENIED by DACL: ACE #2 (ACCESS_DENIED for S-1-5-32-545 [Builtin\Users])
    Denied mask: FILE_READ_DATA (0x01)
    Subject IL: Medium (8192), Object IL: System (16384)
    MIC policy: SYSTEM_MANDATORY_LABEL_NO_READ_UP
  ```
- [ ] Wire `NtQueryAccessDenialReason` to SSDT (index 0x00C5)
- [ ] Commit: `"kernel/security: access denial explainer -- NtQueryAccessDenialReason, accesswhy.exe"`

**Test checkpoint:** Create file with DACL denying Everyone read. Call `NtQueryAccessDenialReason` with `FILE_READ_DATA` -- verify `ReasonCode == DENIAL_REASON_DACL_ACE` and `DeniedBySid == SeWorldSid`. Create Medium IL file, query with Low IL token and write access -- verify `ReasonCode == DENIAL_REASON_MIC`. Run `accesswhy.exe C:\test.txt` -- verify human-readable output on serial. Test on: QEMU WHPX + TCG.

---

## 16. Security Primitive Hardening

> [!NOTE]
> Addresses three accepted findings from the TODO-15 §1 review. These are correctness and hardening upgrades to the SID/LUID primitives that depend on a SHA library (`TODO-03-kernel-libraries.md §5` Monocypher for the kernel path; `09-desktop-shell/TODO-07 §1` CNG for the user path). Not blocking for current security functionality but required for Windows parity and long-term robustness.

- [ ] **SHA-1 service SID derivation:** replace `fnv1a_hash_name()` in `RtlCreateServiceSid` with Windows-compatible derivation: uppercase-normalize service name, compute SHA-1 hash (via `cng_sha1()`), map 160 bits into 5 SubAuthority DWORDs. Verify against known Windows service SID vectors (e.g., `S-1-5-80-...` for "TrustedInstaller"). Remove `fnv1a_hash_name()` dead code after replacement. (-> XREF: 09-desktop-shell/TODO-07 §1 for `cng_sha1()`)
- [ ] **64-bit LUID allocator:** upgrade `NtAllocateLocallyUniqueId` in `luid.c` to use a 64-bit atomic counter (`atomic64_t`). Split counter value into `LowPart` and `HighPart` on return. Eliminates wrap-around after ~4B allocations. Add regression test: allocate 2 LUIDs, verify `HighPart` is populated correctly when `LowPart` would wrap. (-> XREF: kernel/atomic.h for `atomic64_t` support)
- [ ] **Constant-time SID comparison:** replace `memcmp` in `RtlEqualSid` with `cng_consttime_compare()` to eliminate timing side-channel on SID equality checks. Gate on `cng_ready()` flag (fall back to `memcmp` during early boot before CNG is initialized). Add unit test: verify `RtlEqualSid` returns correct result and call duration does not correlate with prefix match length. (-> XREF: 09-desktop-shell/TODO-07 §1 for `cng_consttime_compare()`)
- [ ] Commit: `"kernel/security: SID/LUID hardening -- SHA-1 service SIDs, 64-bit LUID, constant-time compare"`

**Test checkpoint:** `RtlCreateServiceSid("TrustedInstaller")` produces the same SID as Windows (`S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464`). LUID allocator returns increasing values with non-zero `HighPart` after simulated 32-bit boundary. `RtlEqualSid` timing variance < 5% between full-match and first-byte-mismatch cases (kernel cycle counter measurement). Test on: QEMU WHPX + TCG.

---

## OS Comparison

| ⭐ | Feature                     | 🪟 Win11         | 🐧 Linux            | 🚀 Impossible OS    |
|----|-----------------------------|---------------|------------------|------------------|
| 💎 | Token-based identity        | ✅ Full       | ⚠️ UID/GID only  | ⬜ §4            |
| 💎 | DACL access check           | ✅ Full       | ⚠️ POSIX perms   | ⬜ §5            |
| 💎 | Mandatory Integrity Ctrl    | ✅ Vista+     | ⚠️ SELinux add-on | 🟡 No-Write-Up §6 |
| 💎 | Privilege separation        | ✅ Full       | ⚠️ Capabilities  | ⬜ §8            |
| 💎 | UAC filtered-token          | ✅ Full       | ❌ N/A           | ⬜ §9            |
| 💎 | Thread impersonation        | ✅ Full       | ❌ N/A           | ⬜ §7            |
| 💎 | SDDL string descriptors    | ✅ Full       | ❌ N/A           | ⬜ §10           |
| 💎 | Restricted tokens           | ✅ Full       | ❌ N/A           | ⬜ §9            |
| 💎 | SD inheritance              | ✅ Full auto  | ⚠️ POSIX ACL     | ⬜ §13           |
| 💎 | AppContainer sandbox        | ✅ Win8+      | ⚠️ ns + seccomp  | ⬜ §14           |
| 💎 | Restricted token dual check | ✅ Full       | ❌ N/A           | ⬜ §5            |
| ⭐ | Live token inspector        | ❌ CLI only   | ❌ CLI only      | ⬜ §11           |
| ⭐ | IL badge in File Mgr        | ❌ Hidden     | ❌ N/A           | ⬜ §11 + shell   |
| ⭐ | ACL denial toast            | ❌ Event log  | ❌ auditd        | ⬜ Planned       |
| ⭐ | Access denial explainer     | ❌ No API     | ❌ EACCES only   | ⬜ §15           |
| 💎 | Constant-time SID compare   | ✅ Implicit   | ✅ Implicit      | ⬜ §16           |
| 💎 | SHA-1 service SID parity    | ✅ Native     | ❌ N/A           | ⬜ §16           |

After §1–§13, Impossible OS reaches full Windows 11 security architecture parity -- SID tokens, DACL/SACL access checks with inheritance, MIC integrity levels, privilege separation, UAC elevation, and restricted tokens are all present. §14 (AppContainer) adds the modern sandboxing mechanism used by all UWP apps and browsers. Linux with only POSIX permissions and optional MAC add-ons (SELinux/AppArmor) is strictly weaker. The access denial explainer (§15), tray token inspector (§11), and integrated IL badges in the File Manager are exclusive features that make Impossible OS's security model visible, diagnosable, and actionable.

> **Deferred features (noted, not blocked):**
> - **SACL audit event generation**: the audit-record SINK (durable event log) is a `10-platform-services` concern (section TBD -- file the concrete owner when that domain is planned); §3 defines the `SYSTEM_AUDIT_ACE` type. Meanwhile `NtAccessCheckAndAuditAlarm` (§12) must gate on `SeAuditPrivilege`/`SE_AUDIT_NAME` and return the access-check result WITHOUT falsely claiming an audit was written (fail-closed, no silent success).
> - **Protected Process Light (PPL)**: the `ObRegisterCallbacks` handle-filter MECHANISM is `TODO-05 §13`; the PS_PROTECTION field + signer-level ENFORCEMENT need a concrete process-model owner (`TODO-21 §*`, file when planned) -- do NOT rely on the TODO-05 §13 <-> TODO-15 back-reference alone (it is circular; neither side currently owns PS_PROTECTION).
> - **Conditional/callback ACEs (XA/XD)**: reclassified parity-eventually (Win11 CLIENT ACLs ship these, not AD-only). Immediate owner: §5 fail-closed handling of unknown conditional ACEs (added this pass); full boolean-expression evaluation is a future TODO (section TBD).
> - **Unprivileged self-sandboxing (Landlock/seccomp-BPF style)**: an ordinary process narrowing its own syscall/path surface is owned by `02-kernel-core/TODO-21 §12` (pledge/unveil-style restriction) + `TODO-12 §25` (syscall-filter bitmap); the SRM token model (§9 restricted tokens, §14 AppContainer) does NOT cover it.
> - **Code signing verification (srm_verify_kernel_signature)**: image-signing POLICY/enforcement is `02-kernel-core/TODO-19-code-integrity-trust-policy.md`; the syscall SURFACE is `TODO-10-kernel-security-hardening.md §15` (Enclave and Code Signing Syscalls). Not the SRM.

---

## Unit Tests

> Wire into `test_runner_init()` via `test_register_security()`.
> Boot tests run with `debug=1` or `test=1` in boot.conf.

- [ ] Create `src/kernel/test/test_security.c` with:
  - `RtlEqualSid(SeLocalSystemSid, SeLocalSystemSid)` → true
  - `RtlEqualSid(SeLocalSystemSid, SeWorldSid)` → false
  - `RtlLengthSid(SeLocalSystemSid)` → 12 (8 + 4×1)
  - `RtlConvertSidToString(SeLocalSystemSid)` → `"S-1-5-18"`
  - `RtlConvertSidToString(SeBuiltinAdministratorsSid)` → `"S-1-5-32-544"`
  - `NtAllocateLocallyUniqueId()` × 2 → different LUIDs
  - `RtlPrivilegeLuidToName(&SeShutdownPrivilege)` → `"SeShutdownPrivilege"`
  - `RtlCreateAcl` + `RtlAddAccessAllowedAce` + `RtlGetAce` → ACE SID matches input
  - `RtlAbsoluteToSelfRelativeSD` + `RtlSelfRelativeToAbsoluteSD` round-trip preserves Owner SID
  - `SeCreateDefaultSD(SE_SD_TYPE_DEFAULT)` → non-NULL
  - `SeCreateSystemToken()` → non-NULL, PrivilegeCount=24, IL=System
  - `SeCreateUserToken(SeLocalSystemSid, 0)` → Medium IL, 3 privileges
  - `SeCreateUserToken(SeLocalSystemSid, 1)` → High IL, IsElevated=1
  - `NtQueryInformationToken(TokenUser)` → returns UserSid matching token
  - `NtAdjustPrivilegesToken` enable SeShutdownPrivilege → SE_PRIVILEGE_ENABLED set
  - `NtDuplicateToken` effective_only=1 → disabled privileges stripped from copy
  - `NtAdjustGroupsToken` deny-only group cannot be re-enabled
  - `SeAccessCheck` kernel bypass: KernelMode → always grant
  - `SeAccessCheck` DACL deny before allow: deny ACE for user → deny even if allow follows
  - `SeAccessCheck` restricted token: dual DACL pass, restricting SID denies → access denied
  - `SeCheckMandatoryAccess` No-Write-Up: Low IL writing Medium IL object → deny
  - `SeAssignSecurity` with parent SD containing `CONTAINER_INHERIT_ACE` → child has `INHERITED_ACE`
  - `SeAssignSecurity` with `SE_DACL_PROTECTED` → no inherited ACEs in result
  - `NtCreateLowBoxToken` → `TokenIsAppContainer` returns TRUE, IL is Low
  - AppContainer access check: object without AppContainer ACE → deny for LowBox token
  - `NtQueryAccessDenialReason` on DACL-denied access → `DENIAL_REASON_DACL_ACE` with correct SID
- [ ] Register in `test_runner_init()`: `test_register_security()`
- [ ] Commit: `"test: add security reference monitor test suite"`

---

## Verification

- [ ] **Unit test -- SeAccessCheck**: pure kernel call without QEMU; SD with deny+allow ACEs; verify correct grant/deny for System, Admin, User subjects.
- [ ] **Unit test -- MIC No-Write-Up**: Low IL token attempting `GENERIC_WRITE` on a Medium IL object → `STATUS_ACCESS_DENIED` from `SeCheckMandatoryAccess`.
- [ ] **Unit test -- AdjustTokenPrivileges**: create token with `SeShutdownPrivilege` disabled; call `AdjustTokenPrivileges` to enable it; verify privilege now reports `SE_PRIVILEGE_ENABLED`; call again with `DisableAll=TRUE`; verify all disabled.
- [ ] **`whoami.exe` in QEMU**: launch `whoami /all`; output must show the system token's SID, group list (System, Administrators, Everyone), and privilege table with all expected LUIDs present.
- [ ] **Object access check in QEMU**: create a file with a DACL that denies the current user; verify `CreateFile` returns `ERROR_ACCESS_DENIED`.
- [ ] **SD inheritance in QEMU**: create a directory with an inheritable DACL (grant BA full control with `CONTAINER_INHERIT_ACE | OBJECT_INHERIT_ACE`). Create a file inside -- verify inherited ACE is present with `INHERITED_ACE` flag. Create file with `SE_DACL_PROTECTED` -- verify zero inherited ACEs.
- [ ] **AppContainer sandbox in QEMU**: launch a process with an AppContainer token. Attempt to open a file without an AppContainer ACE -- verify `STATUS_ACCESS_DENIED`. Add `ALL_APPLICATION_PACKAGES` ACE -- verify access granted. Run `accesswhy.exe` on the denied file -- verify structured denial reason output.
- [ ] **Remaining limits**: full consent UI (UAC dialog) is in `09-desktop-shell/TODO-06-security-accounts.md §11`; SACL audit logging deferred to `10-platform-services`; kernel object type SACL auditing (file access audit events) deferred to the same; Protected Process Light (PPL) deferred to `TODO-05 §13` (ObRegisterCallbacks); Claims-based access control / Conditional ACEs deferred until core SRM is complete.
- [ ] Commit: `"kernel/security: SRM complete -- ACCESS_TOKEN, ACL/DACL, SeAccessCheck, MIC, impersonation, UAC, SD inheritance, AppContainer, denial explainer"`
