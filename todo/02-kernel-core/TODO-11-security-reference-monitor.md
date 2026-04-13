# TODO-11 -- Security Reference Monitor

> **Goal:** Implement the Windows Security Reference Monitor (SRM) -- the kernel subsystem that enforces every resource access decision in the OS. The SRM owns three things: the `ACCESS_TOKEN` object (who you are, what groups you belong to, what privileges you hold), the `SECURITY_DESCRIPTOR` + ACL machinery (who is allowed to do what to a named resource), and the `SeAccessCheck` engine that compares the two to produce an allow/deny decision. Without SRM, the OS has no file permissions, no process isolation, no privilege separation, and no UAC -- it is a flat single-user system where every process can touch every resource.

> [!IMPORTANT]
> **Current state:** SID/LUID primitives (§1), privilege constants (§2), SECURITY_DESCRIPTOR/ACL/ACE types (§3), and ACCESS_TOKEN objects (§4) are all implemented. The Object Manager (TODO-03 §8) integrates security descriptors. Remaining: SeAccessCheck engine (§5), MIC (§6), token assignment at process spawn (§7), privilege enforcement (§8), UAC/NtFilterToken (§9), Win32 wrappers (§10), SSDT wiring (§12), SD inheritance (§13), AppContainer tokens (§14), and the access denial explainer (§15).

---

## Inputs

- `src/kernel/sched/task.c` -- `struct task`, `task_exec`, `NtCreateProcess` path
- `include/kernel/sched/task.h` -- task struct (token field must be added)
- `src/kernel/mm/vmm.c` -- kernel/user access mode (UserMode / KernelMode)
- → XREF: `TODO-03-object-manager.md §8` -- Object Manager security descriptor integration; ObXxx calls `SeAccessCheck` before granting any handle
- → XREF: `TODO-04-peb-teb-user-abi.md §1` -- TEB carries `ImpersonationInfo` pointer (thread token)
- → XREF: `TODO-05-native-api-ssdt.md §1` -- NTSTATUS return codes used by all token/ACL syscalls
- → XREF: `TODO-05-native-api-ssdt.md §4` -- SSDT indices 0x00B0–0x00C5 reserved for security/token syscalls; §12 of this TODO wires NtOpenProcessToken, NtAccessCheck, NtCreateLowBoxToken, NtQueryAccessDenialReason, etc. into the SSDT
- → XREF: `TODO-09-process-model-extensions.md §2` -- process spawn path (`task_exec()` / future `NtCreateProcess`) is shared territory; §7 adds token duplication, T09 §2 adds standard handle wiring
- → XREF: `05-storage-filesystems/TODO-05-win32-file-io-api.md §3` -- file handle open calls `SeAccessCheck` with `FILE_GENERIC_READ`/`WRITE` desired access
- → XREF: `TODO-13-registry-completion.md §1` -- registry KEY_* access rights enforcement uses `SeAccessCheck` (§5) and `SECURITY_DESCRIPTOR` (§3); also `SeBackupPrivilege` / `SeRestorePrivilege` (§8) for `RegSaveKey` / `RegRestoreKey`
- → XREF: `05-storage-filesystems/TODO-06-ixfs-core-win32-compat.md §5` -- IXFS security descriptors stored as `SECURITY_DESCRIPTOR` on inodes; SRM is the enforcement engine
- → XREF: `09-desktop-shell/TODO-06-security-accounts.md §11` -- UAC consent UI triggers token elevation; this TODO provides `NtFilterToken` + elevation protocol
- → XREF: `TODO-03-object-manager.md §1` -- §13 (SeAssignSecurity) is called by ObCreateObject when a named object is created in a directory
- → XREF: `TODO-20-kernel-libraries.md §5` -- Monocypher SHA-256 used by §14 (AppContainer SID generation from package name hash)

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
| 💎  |   3   | SECURITY_DESCRIPTOR, ACL, ACE types               | §1                  |  [x]   |
| 💎  |   4   | ACCESS_TOKEN object (primary)                     | §1, §2, §3, T03 §1  |  [x]   |
| 💎  |   5   | SeAccessCheck engine                              | §3, §4              |  [ ]   |
| 💎  |   6   | Mandatory Integrity Control (MIC)                 | §4, §5              |  [ ]   |
| 💎  |   7   | Process/thread token assignment & impersonation   | §4                  |  [ ]   |
| 💎  |   8   | SePrivilegeCheck & per-privilege enforcement      | §2, §4, §5          |  [ ]   |
| 💎  |   9   | UAC token split & NtFilterToken                   | §4, §6, §7          |  [ ]   |
| 💎  |  10   | Win32 security API wrappers                       | §4–§9, T05 §1       |  [ ]   |
| ⭐  |  11   | Live token inspector (`whoami.exe` + tray popout) | §4–§10              |  [ ]   |
| 💎  |  12   | Security/token syscalls wired to SSDT             | §4, §8, T05 §4      |  [ ]   |
| 💎  |  13   | SD inheritance / SeAssignSecurity                 | §3, §4, §5, T03 §1  |  [ ]   |
| 💎  |  14   | AppContainer / LowBox tokens                      | §4, §5, §6, §9      |  [ ]   |
| ⭐  |  15   | Access denial explainer                           | §5, §6, §8, §14     |  [ ]   |

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

> **Test runner:** `scripts\debug\run-security-tests.bat` (SUITE=security), 11 suites, 0 failures expected

> **Verified** (2026-04-13): SID struct at sid.h:25-30 matches Windows ABI (_Static_assert at sid.h:33-40). 13 well-known SIDs initialized in sid.c:34-50 with correct S-1-X-Y values. 7 SID utility functions implemented (RtlLengthSid, RtlEqualSid, RtlCopySid, RtlInitializeSid, RtlSubAuthoritySid, RtlConvertSidToString, RtlCreateServiceSid). LUID at luid.h:15-18, NtAllocateLocallyUniqueId at luid.c:13-19 (atomic counter, SMP-safe). All consumer functions guard with RtlValidSid. RtlInitializeSid clamps count to SID_MAX_SUB_AUTHORITIES. 48-bit authority formatting handles values above 32 bits. Build clean.
> **Accepted:** FNV-1a service SID derivation (32-bit entropy, not Windows SHA-1 compatible; acceptable for < 100 services, upgrade when crypto library available) -> XREF: 02-kernel-core/TODO-11 §5 or future crypto TODO. LUID 32-bit counter (HighPart always 0; wrap at ~4B allocations, not reachable in practice) -> accept. RtlEqualSid uses memcmp (timing variable; no user-mode oracle exists, constant-time is future hardening).
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

---

## 3. SECURITY_DESCRIPTOR, ACL & ACE Types

- [x] Define in `include/kernel/security/acl.h`:
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
- [x] `RtlSelfRelativeToAbsoluteSD(rel, abs_buf, ...)` -- unmarshal from flat buffer
- [x] `SeCreateDefaultSD(type)` -- returns a self-relative SD with:
  - Owner = `SeLocalSystemSid`
  - DACL: `(A;;GA;;;SY)(A;;GA;;;BA)(A;;GR;;;WD)` -- System+Admins=FullControl, Everyone=ReadControl
  - Stored as static blobs in `src/kernel/security/default_sds.c`
- [x] Object types needing custom defaults:
  - `OB_TYPE_PROCESS` -- `(A;;GA;;;SY)(A;;0x1FFFFF;;;BA)(A;;0x1000;;;WD)` (create/terminate restricted for Everyone)
  - `OB_TYPE_TOKEN` -- `(A;;GA;;;SY)(A;;0x0008;;;OW)` (Query only for owner)
  - `OB_TYPE_REGISTRY_KEY` -- `(A;;GA;;;SY)(A;;GA;;;BA)(A;;GR;;;BU)` (BuiltinUsers=Read)
- [x] Commit: `"kernel/security: SECURITY_DESCRIPTOR, ACL, ACE types and helpers"`

---

## 4. ACCESS_TOKEN Object

- [x] Define `struct ACCESS_TOKEN` in `include/kernel/security/token.h`:
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
- [x] Register `ObpTokenType` via `ob_create_type("Token", sizeof(ACCESS_TOKEN), ...)` (→ XREF `TODO-03 §1`); token objects are reference-counted kernel objects
- [x] `SeCreateSystemToken()` -- called in Phase 0 of kernel init (→ XREF `TODO-01-kernel-init-sequencing.md §2`): UserSid=`SeLocalSystemSid`, Groups=`{SeBuiltinAdministratorsSid | SE_GROUP_ENABLED, SeWorldSid | SE_GROUP_ENABLED}`, all privileges enabled, IL=System; stored in `PsInitialSystemProcess->Token`
- [x] `SeCreateUserToken(user_sid, admin)` -- creates Medium IL primary token for interactive logon (called by login manager in `09-desktop-shell`); if `admin`, also creates High IL linked token for elevation; groups include `SeBuiltinUsersSid`; privileges: `SeChangeNotifyPrivilege` enabled by default; `SeShutdownPrivilege`/`SeUndockPrivilege` enabled by default; admin token adds `SeBackupPrivilege`, `SeRestorePrivilege`, `SeLoadDriverPrivilege` as disabled by default (present but off until elevated)
- [x] `NtOpenProcessToken(ProcessHandle, DesiredAccess, TokenHandle)` -- opens the primary token of a process; access check on the process object for `PROCESS_QUERY_INFORMATION`; returns handle with requested access
- [x] `NtOpenThreadToken(ThreadHandle, DesiredAccess, OpenAsSelf, TokenHandle)` -- returns impersonation token or `STATUS_NO_TOKEN` if thread is not impersonating
- [x] `NtQueryInformationToken(hToken, class, buf, len, retlen)` -- implement `TokenUser`, `TokenGroups`, `TokenPrivileges`, `TokenOwner`, `TokenPrimaryGroup`, `TokenDefaultDacl`, `TokenType`, `TokenImpersonationLevel`, `TokenStatistics`, `TokenIntegrityLevel`, `TokenElevationType`, `TokenLinkedToken`, `TokenIsElevated`
- [x] `NtDuplicateToken(Existing, Access, ObjAttr, EffectiveOnly, Type, New)` -- deep-copies token struct, allocates new `TokenId` LUID; `EffectiveOnly` strips disabled privileges and groups
- [x] `NtAdjustPrivilegesToken(hToken, DisableAll, NewState, BufferLen, PreviousState, ReturnLen)` -- for each LUID_AND_ATTRIBUTES in `NewState`: find matching privilege in token, apply SE_PRIVILEGE_ENABLED / SE_PRIVILEGE_REMOVED; requires `TOKEN_ADJUST_PRIVILEGES`; write previous state to `PreviousState`; return `STATUS_NOT_ALL_ASSIGNED` if any LUID not found
- [x] `NtAdjustGroupsToken(hToken, ResetToDefault, NewState, BufferLen, PreviousState, ReturnLen)` -- enable/disable group SIDs; cannot re-enable a `SE_GROUP_USE_FOR_DENY_ONLY` group (immutable once disabled)
- [x] Commit: `"kernel/security: ACCESS_TOKEN object, SeCreateSystemToken, NtOpenProcessToken"`

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
- [ ] In `ObpReferenceObjectByHandle` (→ XREF `TODO-03 §3`): after locating the handle entry, call `SeAccessCheck(object->SecurityDescriptor, ctx, FALSE, desired, 0, NULL, object->Type->GenericMapping, UserMode, &granted, &status)`; return `STATUS_ACCESS_DENIED` if check fails
- [ ] `NtAccessCheck(sd, token, desired, mapping, privs, priv_len, granted, status)` syscall -- user-mode accessible wrapper for testing DACLs without opening an object
- [ ] Commit: `"kernel/security: SeAccessCheck engine and ObpReferenceObjectByHandle integration"`

**Test checkpoint:** `SeAccessCheck` with KernelMode → always grants. SD with deny-ACE for BA before allow-ACE → BA token denied (deny takes precedence). SD with NULL DACL → all granted. SD with empty DACL (AceCount=0) → all denied. Owner requesting READ_CONTROL → granted via owner bypass. Restricted token with restricting SID not in DACL → denied even though normal SIDs match. Serial log: `"[SRM] SeAccessCheck: desired=0x%x granted=0x%x result=%s"`. Test on: QEMU WHPX + TCG (pure kernel logic, no hardware).

---

## 6. Mandatory Integrity Control (MIC)
### 6.1 Integrity level comparison

- [ ] `SeGetTokenIntegrityLevel(token)` -- returns `uint32_t` from `token->IntegrityLevelSid->SubAuthority[0]` (0, 4096, 8192, 12288, 16384)
- [ ] `SeGetObjectIntegrityLevel(sd)` -- reads `SYSTEM_MANDATORY_LABEL_ACE` from SACL; returns `SeILMedium` (8192) if no SACL or no mandatory label
- [ ] `SeCompareMandatoryLevels(subject_il, object_il)` → `int` (-1 / 0 / +1)

### 6.2 No-Write-Up policy

- [ ] `SeCheckMandatoryAccess(ctx, sd, desired_access)` -- called by §5.3 before DACL walk:
  - Compute `subject_il = SeGetTokenIntegrityLevel(effective_token)`
  - Compute `object_il = SeGetObjectIntegrityLevel(sd)`
  - Read `SYSTEM_MANDATORY_LABEL_ACE->Mask` policy bits:
    - `SYSTEM_MANDATORY_LABEL_NO_WRITE_UP (0x4)` -- default; if `subject_il < object_il` AND `desired` contains write access bits → deny
    - `SYSTEM_MANDATORY_LABEL_NO_READ_UP (0x2)` -- optional; if `subject_il < object_il` AND `desired` contains read access bits → deny
    - `SYSTEM_MANDATORY_LABEL_NO_EXECUTE_UP (0x1)` -- if `subject_il < object_il` AND `desired` contains execute bits → deny
  - Returns `STATUS_ACCESS_DENIED` or `STATUS_SUCCESS`
- [ ] Default object IL assignment: kernel objects created by System process get `SeILSystem`; objects created by user process inherit creator's IL

### 6.3 Token IL enforcement at spawn

- [ ] When `NtCreateProcess` copies the parent token (§7), child token IL = `min(parent_IL, process_image_IL)`; image IL read from PE/ELF resource (`RT_MANIFEST`, requested execution level: `asInvoker`→same, `requireAdministrator`→High)
- [ ] Low IL sandbox mode: token with `SeILLow` is denied write to `%USERPROFILE%\*` (only `%LOCALAPPDATA%\Low\*` writable); enforced by MIC at `SeCheckMandatoryAccess` time

### 6.4 Commit

- [ ] Commit: `"kernel/security: Mandatory Integrity Control, No-Write-Up policy"`

**Test checkpoint:** `SeCheckMandatoryAccess` with Low IL token + GENERIC_WRITE on Medium IL object → `STATUS_ACCESS_DENIED` (No-Write-Up). System IL writing Medium IL → allowed. No-Read-Up policy set, Low IL reading Medium IL → denied. Default policy (No-Write-Up only), Low IL reading Medium IL → allowed. Child process spawned from Medium IL parent → child IL == Medium. Serial log: `"[SRM] MIC: subject_il=%u object_il=%u policy=0x%x result=%s"`. Test on: QEMU WHPX + TCG.

---

## 7. Process/Thread Token Assignment & Impersonation
### 7.1 Token field in task struct

- [ ] Add `ACCESS_TOKEN *Token;` to `struct task` in `include/kernel/sched/task.h`
- [ ] Add `ACCESS_TOKEN *ImpersonationToken;` -- thread-level override; NULL = use process token
- [ ] `PsReferencePrimaryToken(task)` -- increments token refcount and returns ptr
- [ ] `PsDereferencePrimaryToken(token)` -- decrements; frees on zero

### 7.2 Token assignment at spawn

- [ ] In `task_exec()` (→ shared path with `TODO-09 §2`): call `NtDuplicateToken(parent->Token, TOKEN_ALL_ACCESS, NULL, FALSE, TokenPrimary, &child->Token)` -- child starts with a deep copy of parent's primary token
- [ ] `SeCreateSystemToken()` result assigned to `PsInitialSystemProcess->Token` during Phase 0 kernel init (→ XREF `TODO-01-kernel-init-sequencing.md §2`)

### 7.3 Thread impersonation

- [ ] `NtImpersonateThread(ThreadHandle, ImpersonationThreadHandle, ImpLevel)` -- duplicates the source thread's token as an impersonation token, sets `current_task()->ImpersonationToken`; requires `SeImpersonatePrivilege` if IL is higher than current thread
- [ ] `NtSetInformationThread(ThreadHandle, ThreadImpersonationToken, TokenHandle, sizeof(HANDLE))` -- explicit impersonation token assignment (NULL handle = revert)
- [ ] `RevertToSelf()` Win32 wrapper -- sets impersonation token to NULL
- [ ] `ImpersonateSelf(ImpersonationLevel)` -- duplicates own primary token as impersonation token; used before adjusting privileges for a short operation

### 7.4 Effective token selection

- [ ] `SeQuerySubjectContextToken(ctx)` -- returns impersonation token if present AND impersonation level ≥ `SecurityIdentification`; else returns primary token; used by SeAccessCheck §5 as the "effective" token

### 7.5 Commit

- [ ] Commit: `"kernel/security: task token field, spawn token copy, thread impersonation"`

**Test checkpoint:** After `task_exec()`, child task's `token` is non-NULL and distinct from parent's (deep copy). `PsReferencePrimaryToken(child)` returns valid token with same UserSid as parent. `NtImpersonateThread` sets impersonation token; subsequent `SeCaptureSubjectContext` uses it as effective token. `RevertToSelf()` clears impersonation; effective token reverts to primary. Serial log: `"[SRM] Token assigned to pid=%u"`, `"[SRM] Thread %u impersonating at level %u"`. Test on: QEMU WHPX + TCG.

---

## 8. SePrivilegeCheck & Per-Privilege Enforcement
### 8.1 Core privilege check

- [ ] `SePrivilegeCheck(PrivilegeSet, ctx, AccessMode)`:
  - If `AccessMode == KernelMode` → return `TRUE`
  - For each LUID in `PrivilegeSet->Privilege`: scan effective token's `Privileges[]` for matching LUID with `SE_PRIVILEGE_ENABLED` attribute; if `PRIVILEGE_SET_ALL_NECESSARY`, all LUIDs must match; otherwise any one match suffices
  - Write SACL audit record if `SE_PRIVILEGE_USED_FOR_ACCESS` (future)
- [ ] `SeSinglePrivilegeCheck(Privilege, AccessMode)` -- common single-LUID shortcut; called throughout kernel for specific privilege gates
- [ ] `SeCheckPrivilegedObject(PrivReq, Object, Desired, Mode)` -- combines `SeSinglePrivilegeCheck` with `SeAccessCheck`; used by backup/restore paths

### 8.2 Privilege gates in kernel subsystems

- [ ] `module_load()` -- `SeSinglePrivilegeCheck(SeLoadDriverPrivilege, UserMode)` before loading any `.kmod` from user request
- [ ] `NtShutdownSystem` / `NtReboot` -- `SeSinglePrivilegeCheck( SeShutdownPrivilege, UserMode)`
- [ ] `NtSystemDebugControl` / `NtOpenProcess` with `PROCESS_ALL_ACCESS` on another-user's process -- `SeSinglePrivilegeCheck(SeDebugPrivilege, UserMode)`
- [ ] `NtSetSystemTime` -- `SeSinglePrivilegeCheck(SeSystemtimePrivilege, UserMode)`
- [ ] `NtCreateSymbolicLinkObject` -- `SeSinglePrivilegeCheck( SeCreateSymbolicLinkPrivilege, UserMode)` for permanent symlinks
- [ ] `NtQuerySystemInformation(SystemPerformanceInformation)` -- requires `SeSystemProfilePrivilege` if `mode == UserMode`

### 8.3 Commit

- [ ] Commit: `"kernel/security: SePrivilegeCheck, privilege gates for driver load, shutdown, debug"`

**Test checkpoint:** Token with SeShutdownPrivilege disabled: `SeSinglePrivilegeCheck(SeShutdownPrivilege, UserMode)` → FALSE. After `NtAdjustPrivilegesToken` to enable → TRUE. `PRIVILEGE_SET_ALL_NECESSARY` with two LUIDs, one missing → FALSE. KernelMode → always TRUE regardless of token. `NtShutdownSystem` from unprivileged token → `STATUS_PRIVILEGE_NOT_HELD`. Serial log: `"[SRM] SePrivilegeCheck: %s = %s"`. Test on: QEMU WHPX + TCG.

---

## 9. UAC Token Split & NtFilterToken
### 9.1 NtFilterToken

- [ ] `NtFilterToken(ExistingToken, Flags, SidsToDisable, PrivilegesToDelete, RestrictedSids, NewToken)`:
  - `DISABLE_MAX_PRIVILEGE` (flag): mark all privileges except `SeChangeNotifyPrivilege` as `SE_PRIVILEGE_REMOVED`
  - `SidsToDisable`: mark matched group SIDs with `SE_GROUP_USE_FOR_DENY_ONLY` (present in deny checks but not allow checks)
  - `PrivilegesToDelete`: remove matching privilege entries entirely
  - `RestrictedSids`: append to token as restricted SID list (second DACL pass required -- access must be allowed by BOTH the normal DACL walk AND a walk of the restricted SID list)
  - Sets `TOKEN_IS_RESTRICTED` flag on new token
  - Used internally by UAC to produce the "filtered" Medium token for admin users

### 9.2 Linked token pair

- [ ] `SeCreateLinkedTokenPair(FullAdminToken, FilteredToken)`:
  - `FullAdminToken->LinkedTokenId` = `FilteredToken->TokenId`
  - `FilteredToken->LinkedTokenId` = `FullAdminToken->TokenId`
  - `FilteredToken->ElevationType = TokenElevationTypeLimited`
  - `FullAdminToken->ElevationType = TokenElevationTypeFull`
  - Both tokens reference each other; when user requests elevation, kernel resolves the linked token from the filtered one
- [ ] `NtQueryInformationToken(TokenLinkedToken)` -- returns handle to the linked token; requires `TOKEN_QUERY` and that the caller holds `SeTcbPrivilege` (prevents unprivileged elevation discovery)

### 9.3 Elevation request syscall

- [ ] `NtRequestTokenElevation(ProcessHandle, hToken, ElevationType)` -- kernel side of the elevation flow:
  1. Check calling process has `TokenElevationTypeLimited` token
  2. Signal the consent UI process (→ XREF `09-desktop-shell/TODO-06-security-accounts.md §11`) via a dedicated kernel event object
  3. Wait for consent UI to signal approval or denial event
  4. On approval: `NtSetInformationProcess(ProcessHandle, ProcessAccessToken, &linked_token)` -- replaces the process token with the full-admin linked token
  5. On denial: return `STATUS_PRIVILEGE_NOT_HELD`
- [ ] Note: full consent UI implementation is in `08-desktop-shell` -- this TODO provides only the kernel side of the handshake

### 9.4 Commit

- [ ] Commit: `"kernel/security: NtFilterToken, linked token pair, UAC elevation protocol"`

**Test checkpoint:** `NtFilterToken` with `DISABLE_MAX_PRIVILEGE` → new token has only SeChangeNotifyPrivilege enabled. `NtFilterToken` with SidsToDisable=[BA] → BA group has `SE_GROUP_USE_FOR_DENY_ONLY`. Linked token pair: `NtQueryInformationToken(TokenLinkedToken)` on filtered → returns handle to full admin token. `ElevationType` of filtered == `TokenElevationTypeLimited`. Serial log: `"[SRM] NtFilterToken: %u privileges removed, %u groups disabled"`. Test on: QEMU WHPX + TCG.

---

## 10. Win32 Security API Wrappers
### 10.1 Token Win32 API

- [ ] `OpenProcessToken(hProcess, DesiredAccess, phToken)` → `NtOpenProcessToken`
- [ ] `OpenThreadToken(hThread, DesiredAccess, OpenAsSelf, phToken)` → `NtOpenThreadToken`
- [ ] `GetTokenInformation(hToken, class, buf, len, retlen)` → `NtQueryInformationToken`
- [ ] `SetTokenInformation(hToken, class, buf, len)` → `NtSetInformationToken`
- [ ] `AdjustTokenPrivileges(hToken, DisableAll, NewState, BufferLen, PreviousState, ReturnLen)` → `NtAdjustPrivilegesToken`; note: Win32 returns TRUE even for `STATUS_NOT_ALL_ASSIGNED` (set last-error instead)
- [ ] `CheckTokenMembership(hToken, SidToCheck, IsMember)` -- scan token groups for matching SID with `SE_GROUP_ENABLED` attribute; NULL token = current thread effective token
- [ ] `IsUserAnAdmin()` -- `CheckTokenMembership(NULL, SeBuiltinAdministratorsSid, &member)`; returns TRUE only if token is elevated admin (High IL)
- [ ] `IsTokenRestricted(hToken)` → check `TOKEN_IS_RESTRICTED` flag in token
- [ ] `CreateRestrictedToken(hToken, Flags, DisableSidCount, SidsToDisable, DeletePrivilegeCount, PrivilegesToDelete, RestrictedSidCount, SidsToRestrict, NewToken)` → `NtFilterToken`; flags: `DISABLE_MAX_PRIVILEGE`, `SANDBOX_INERT`, `WRITE_RESTRICTED`, `LUA_TOKEN`

### 10.2 SID Win32 API

- [ ] `ConvertSidToStringSidW(Sid, StringSid)` -- formats `S-1-X-Y-...` into heap-allocated `WCHAR*` (caller frees with `LocalFree`)
- [ ] `ConvertStringSidToSidW(StringSid, Sid)` -- parse `S-1-...` string back to SID blob
- [ ] `AllocateAndInitializeSid(IdentifierAuthority, SubAuthorityCount, ...)` -- up to 8 sub-authority args; allocates SID blob (caller frees with `FreeSid`)
- [ ] `FreeSid(Sid)` -- wraps `kfree`
- [ ] `EqualSid`, `CopySid`, `LengthSid`, `IsValidSid` Win32 wrappers

### 10.3 Security descriptor Win32 API

- [ ] `GetSecurityInfo(handle, ObjectType, SecurityInfo, Owner, Group, Dacl, Sacl, SD)` → `NtQuerySecurityObject`
- [ ] `SetSecurityInfo(handle, ObjectType, SecurityInfo, Owner, Group, Dacl, Sacl)` → `NtSetSecurityObject`
- [ ] `GetNamedSecurityInfoW(name, ObjectType, SecurityInfo, ...)` -- resolves path to file handle, then calls `NtQuerySecurityObject`
- [ ] `SetNamedSecurityInfoW(name, ObjectType, SecurityInfo, ...)` -- resolve + `NtSetSecurityObject`; requires `WRITE_DAC` or `SE_SECURITY_PRIVILEGE` for SACL
- [ ] `ConvertStringSecurityDescriptorToSecurityDescriptorW(SDDL, Revision, SD, SDSize)` -- minimal SDDL parser: parse `O:XX G:XX D:...(A;;XX;;;XX)...` syntax; supports `SY`=System, `BA`=Admins, `BU`=Users, `WD`=Everyone aliases
- [ ] `ConvertSecurityDescriptorToStringSecurityDescriptorW(SD, Revision, SecurityInfo, StringSD, StringSDLen)` -- reverse; produces SDDL string

### 10.4 Commit

- [ ] Commit: `"kernel/security: Win32 token, SID, and security descriptor API wrappers"`

**Test checkpoint:** `OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)` → valid handle. `GetTokenInformation(TokenUser)` → returns correct UserSid. `ConvertSidToStringSidW(SeLocalSystemSid)` → `"S-1-5-18"`. `ConvertStringSecurityDescriptorToSecurityDescriptorW("D:(A;;GA;;;SY)")` → valid SD with one ACE. `AdjustTokenPrivileges` enable/disable round-trip succeeds. Serial log: `"[SRM] Win32 security API test passed"`. Test on: QEMU WHPX + TCG.

---

## 11. Live Token Inspector
### 11.1 whoami.exe

- [ ] `src/apps/whoami/whoami.c` -- command-line tool; calls `OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)` then `GetTokenInformation` for each class; formats output as aligned table. CLI usage:
  ```
  whoami [/user] [/groups] [/priv] [/all]
    /user    -- print current user SID and account name
    /groups  -- print all group SIDs with attributes (Enabled/Disabled/DenyOnly)
    /priv    -- print all privileges with Enabled/Disabled/Removed status
    /all     -- equivalent to /user /groups /priv
  ```

### 11.2 Token tray popout (stretch)

- [ ] System tray right-click → "Token Info" → flyout showing: current user, integrity level badge (colour-coded: Low=yellow, Medium=green, High=orange, System=red), admin status, elevation type, top 5 privileges
- [ ] Useful for developers to verify elevation state without opening a terminal

### 11.3 Commit

- [ ] Commit: `"kernel/security: whoami.exe and token tray popout"`

**Test checkpoint:** `whoami /all` in QEMU serial console shows: user SID (`S-1-5-18` for system), group list with attributes (Enabled/Disabled/DenyOnly), and privilege table with status column. Output columns are tab-aligned. `whoami /priv` shows at least 20 privilege entries. Tray popout (stretch): right-click tray → "Token Info" → flyout renders. Serial log: `"[SRM] whoami: user=%s groups=%u privs=%u"`. Test on: QEMU WHPX + TCG.

---

## 12. Security and Token Syscalls Wired to SSDT
Register all token and access control NtXxx entry points in the SSDT. Most implementations already exist in `src/kernel/security/token.c` and `luid.c`. (→ XREF: TODO-05-native-api-ssdt.md §4, §16)

- [ ] `NtOpenProcessToken(ProcessHandle, DesiredAccess, TokenHandle)` → SSDT 0x00B0 (token.c exists -- wire to SSDT)
- [ ] `NtOpenProcessTokenEx(ProcessHandle, DesiredAccess, HandleAttributes, TokenHandle)` → SSDT 0x00B1
- [ ] `NtOpenThreadToken(ThreadHandle, DesiredAccess, OpenAsSelf, TokenHandle)` → SSDT 0x00B2 (token.c exists)
- [ ] `NtOpenThreadTokenEx(...)` → SSDT 0x00B3
- [ ] `NtQueryInformationToken(TokenHandle, TokenInformationClass, Buffer, Length, ReturnLength)` → SSDT 0x00B4 (token.c exists -- 13 info classes)
- [ ] `NtSetInformationToken(TokenHandle, TokenInformationClass, Buffer, Length)` → SSDT 0x00B5
- [ ] `NtAdjustPrivilegesToken(TokenHandle, DisableAll, NewState, BufLen, PrevState, RetLen)` → SSDT 0x00B6 (token.c exists)
- [ ] `NtAdjustGroupsToken(...)` → SSDT 0x00B7 (token.c exists)
- [ ] `NtDuplicateToken(ExistingHandle, DesiredAccess, ObjAttrs, EffectiveOnly, TokenType, NewHandle)` → SSDT 0x00B8 (token.c exists)
- [ ] `NtFilterToken(ExistingHandle, Flags, SidsToDisable, PrivsToDelete, RestrictedSids, NewHandle)` → SSDT 0x00B9
- [ ] `NtCreateToken(...)` → SSDT 0x00BA: privileged operation for LSA
- [ ] `NtCreateLowBoxToken(NewToken, ExistingToken, DesiredAccess, ObjectAttributes, AppContainerSid, CapabilityCount, Capabilities, HandleCount, Handles)` → SSDT 0x00BB: route to AppContainer token creation (§14)
- [ ] `NtAccessCheck(SD, ClientToken, DesiredAccess, GenericMapping, PrivSet, PrivSetLen, GrantedAccess, AccessStatus)` → SSDT 0x00BC: route to `SeAccessCheck()` (§5)
- [ ] `NtPrivilegeCheck(ClientToken, RequiredPrivileges, Result)` → SSDT 0x00BF: route to `SePrivilegeCheck()` (§8)
- [ ] `NtSetSecurityObject(Handle, SecurityInformation, SD)` → SSDT 0x00C1
- [ ] `NtQuerySecurityObject(Handle, SecurityInformation, SD, Length, LengthNeeded)` → SSDT 0x00C2
- [ ] `NtAllocateLocallyUniqueId(Luid)` → SSDT 0x00C3 (luid.c exists)
- [ ] `NtAccessCheckAndAuditAlarm(SubsystemName, HandleId, ObjectTypeName, ObjectName, SD, DesiredAccess, GenericMapping, ObjectCreation, GrantedAccess, AccessStatus, GenerateOnClose)` → SSDT 0x00C4: combines access check + SACL audit
- [ ] `NtQueryAccessDenialReason(Handle, DesiredAccess, DenialReason)` → SSDT 0x00C5: route to §15 denial explainer
- [ ] All functions return `NTSTATUS`; use codes from `include/kernel/nt/ntstatus.h` (TODO-05 §1)
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
- [ ] Wire `SeAssignSecurity` into `ObCreateObject` (→ XREF `TODO-03 §1`): when creating a named object in a directory, pass the parent directory's SD as `ParentSD`
- [ ] Wire into VFS `CreateFile` / `CreateDirectory` paths (→ XREF `05-storage-filesystems/TODO-05 §3`): IXFS inode creation inherits parent directory's SD
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

## OS Comparison

| ⭐ | Feature                     | 🪟 Win11         | 🐧 Linux            | 🚀 Impossible OS    |
|----|-----------------------------|---------------|------------------|------------------|
| 💎 | Token-based identity        | ✅ Full       | ⚠️ UID/GID only  | ⬜ §4            |
| 💎 | DACL access check           | ✅ Full       | ⚠️ POSIX perms   | ⬜ §5            |
| 💎 | Mandatory Integrity Ctrl    | ✅ Vista+     | ⚠️ SELinux add-on | ⬜ §6            |
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

After §1–§13, Impossible OS reaches full Windows 11 security architecture parity -- SID tokens, DACL/SACL access checks with inheritance, MIC integrity levels, privilege separation, UAC elevation, and restricted tokens are all present. §14 (AppContainer) adds the modern sandboxing mechanism used by all UWP apps and browsers. Linux with only POSIX permissions and optional MAC add-ons (SELinux/AppArmor) is strictly weaker. The access denial explainer (§15), tray token inspector (§11), and integrated IL badges in the File Manager are exclusive features that make Impossible OS's security model visible, diagnosable, and actionable.

> **Deferred features (noted, not blocked):**
> - **SACL audit event generation**: deferred to `10-services-security`; infrastructure (SYSTEM_AUDIT_ACE type) is defined in §3
> - **Protected Process Light (PPL)**: deferred to `TODO-03 §13` (ObRegisterCallbacks) + process model; PS_PROTECTION field and signer levels are process-model concerns, not SRM
> - **Claims-based access control / Conditional ACEs**: advanced Dynamic Access Control feature; deferred until core SRM is complete; add as a future TODO when conditional ACE evaluation is needed
> - **Code signing verification (srm_verify_kernel_signature)**: noted in `TODO-01-uefi-hardening §5`; belongs to `TODO-17 §15` (Enclave and code signing) not SRM

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
- [ ] **Remaining limits**: full consent UI (UAC dialog) is in `09-desktop-shell/TODO-06-security-accounts.md §11`; SACL audit logging deferred to `10-services-security`; kernel object type SACL auditing (file access audit events) deferred to the same; Protected Process Light (PPL) deferred to `TODO-03 §13` (ObRegisterCallbacks); Claims-based access control / Conditional ACEs deferred until core SRM is complete.
- [ ] Commit: `"kernel/security: SRM complete -- ACCESS_TOKEN, ACL/DACL, SeAccessCheck, MIC, impersonation, UAC, SD inheritance, AppContainer, denial explainer"`
