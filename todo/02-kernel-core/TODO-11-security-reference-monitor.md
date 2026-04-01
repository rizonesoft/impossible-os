# TODO-11 — Security Reference Monitor

> **Goal:** Implement the Windows Security Reference Monitor (SRM) — the kernel subsystem that enforces every resource access decision in the OS. The SRM owns three things: the `ACCESS_TOKEN` object (who you are, what groups you belong to, what privileges you hold), the `SECURITY_DESCRIPTOR` + ACL machinery (who is allowed to do what to a named resource), and the `SeAccessCheck` engine that compares the two to produce an allow/deny decision. Without SRM, the OS has no file permissions, no process isolation, no privilege separation, and no UAC — it is a flat single-user system where every process can touch every resource.

> [!IMPORTANT]
> **Current state:** No token objects, no SIDs, no ACLs, and no access-check engine exist. The Object Manager (TODO-03 §8) has a placeholder dependency on a future SRM. Every file open, object open, and process creation currently grants full access unconditionally. The process model (TODO-09) does not attach a token to new processes.

---

## Inputs

- `src/kernel/sched/task.c` — `struct task`, `task_exec`, `NtCreateProcess` path
- `include/kernel/sched/task.h` — task struct (token field must be added)
- `src/kernel/mm/vmm.c` — kernel/user access mode (UserMode / KernelMode)
- → XREF: `TODO-03-object-manager.md §8` — Object Manager security descriptor integration; ObXxx calls `SeAccessCheck` before granting any handle
- → XREF: `TODO-04-peb-teb-user-abi.md §1` — TEB carries `ImpersonationInfo` pointer (thread token)
- → XREF: `TODO-05-native-api-layer.md §1` — NTSTATUS return codes used by all token/ACL syscalls
- → XREF: `TODO-09-process-model-extensions.md §2` — process spawn path (NtCreateProcess) must copy parent token and attach it
- → XREF: `05-storage-filesystems/TODO-05-win32-file-io-api.md §3` — file handle open calls `SeAccessCheck` with `FILE_GENERIC_READ`/`WRITE` desired access
- → XREF: `05-storage-filesystems/TODO-06-ixfs-core-win32-compat.md §5` — IXFS security descriptors stored as `SECURITY_DESCRIPTOR` on inodes; SRM is the enforcement engine
- → XREF: `09-desktop-shell/TODO-06-security-accounts.md §11` — UAC consent UI triggers token elevation; this TODO provides `NtFilterToken` + elevation protocol

---

## Outcome

- `ACCESS_TOKEN` kernel object: User SID, group SIDs (with attributes), privileges (enabled/default/removed), integrity level, primary/impersonation type.
- `SECURITY_DESCRIPTOR` type with absolute and self-relative formats, DACL/SACL, and per-object-type `GENERIC_MAPPING`.
- `SeAccessCheck` correctly grants or denies access based on DACL walk + owner bypass + kernel-mode bypass.
- Mandatory Integrity Control (MIC): No-Write-Up enforced; No-Read-Up optional.
- Every kernel object type (file, process, thread, event, mutex, registry key) has a default security descriptor and enforces access via `ObpReferenceObjectByHandle`.
- UAC filtered-token split is structurally in place; consent UI wired in `08-desktop-shell`.
- Win32 token API surface (`OpenProcessToken`, `GetTokenInformation`, `AdjustTokenPrivileges`, etc.) is complete.

---

## Implementation Order

| ⭐  | Order | Deliverable                                       | Depends On          | Status |
| --- | :---: | ------------------------------------------------- | ------------------- | :----: |
| 💎  |   1   | SID & LUID primitives                             | —                   |  [x]   |
| 💎  |   2   | Privilege constants & PRIVILEGE_SET               | 1                   |  [x]   |
| 💎  |   3   | SECURITY_DESCRIPTOR, ACL, ACE types               | 1                   |  [ ]   |
| 💎  |   4   | ACCESS_TOKEN object (primary)                     | 1, 2, 3, T03 §1     |  [ ]   |
| 💎  |   5   | SeAccessCheck engine                              | 3, 4                |  [ ]   |
| 💎  |   6   | Mandatory Integrity Control (MIC)                 | 4, 5                |  [ ]   |
| 💎  |   7   | Process/thread token assignment & impersonation   | 4, T09 §2           |  [ ]   |
| 💎  |   8   | SePrivilegeCheck & per-privilege enforcement      | 2, 4, 5             |  [ ]   |
| 💎  |   9   | UAC token split & NtFilterToken                   | 4, 6, 7             |  [ ]   |
| 💎  |  10   | Win32 security API wrappers                       | 4–9, T05 §1         |  [ ]   |
| ⭐  |  11   | Live token inspector (`whoami.exe` + tray popout) | 4–10                |  [ ]   |

> 💎 = parity work — matches what Windows 11 and Linux already do.
> ⭐ = exclusive work — Impossible OS is superior or first.

---

## 1. SID & LUID Primitives 
### 1.1 SID type and helpers

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
  - `RtlConvertSidToString(str, sid, buf_size)` — formats `S-1-X-Y-...`
  - `RtlCreateServiceSid(name, sid, len)` — generates `S-1-5-80-<hash>` for service accounts (used by Service Manager)

### 1.2 LUID type

- [x] Define `LUID` (64-bit opaque identifier):
  ```c
  typedef struct { uint32_t LowPart; int32_t HighPart; } LUID;
  ```
- [x] `RtlEqualLuid`, `RtlIsZeroLuid`
- [x] `NtAllocateLocallyUniqueId` — monotonically incrementing counter, returned to user mode for dynamic LUID allocation

### 1.3 Commit

- [x] Commit: `"kernel/security: SID primitives and well-known SID table"`

---

## 2. Privilege Constants & PRIVILEGE_SET 
### 2.1 Privilege LUID constants

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

### 2.2 PRIVILEGE_SET and TOKEN_PRIVILEGES

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
- [x] `RtlPrivilegeSetToString(ps, buf, len)` — debug helper

### 2.3 Commit

- [x] Commit: `"kernel/security: privilege LUID table and PRIVILEGE_SET types"`

---

## 3. SECURITY_DESCRIPTOR, ACL & ACE Types 
### 3.1 Core types

- [ ] Define in `include/kernel/security/acl.h`:
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
- [ ] ACE type constants: `ACCESS_ALLOWED_ACE_TYPE=0`, `ACCESS_DENIED_ACE_TYPE=1`, `SYSTEM_AUDIT_ACE_TYPE=2`, `SYSTEM_MANDATORY_LABEL_ACE_TYPE=0x11`
- [ ] ACE flag constants: `OBJECT_INHERIT_ACE`, `CONTAINER_INHERIT_ACE`, `INHERIT_ONLY_ACE`, `INHERITED_ACE`, `SUCCESSFUL_ACCESS_ACE_FLAG`, `FAILED_ACCESS_ACE_FLAG`

### 3.2 ACL helpers

- [ ] `RtlCreateAcl(acl, size, rev)` — initialise empty ACL
- [ ] `RtlAddAccessAllowedAce(acl, rev, mask, sid)` — append allowed ACE
- [ ] `RtlAddAccessDeniedAce(acl, rev, mask, sid)` — append denied ACE
- [ ] `RtlAddMandatoryAce(acl, rev, flags, mask, type, integrity_sid)`
- [ ] `RtlGetAce(acl, index, ace_ptr)` — walk ACE by index
- [ ] `RtlAclToCStr(acl, buf, len)` — debug dump (`"D:(A;;FA;;;SY)(A;;FA;;;BA)"`)

### 3.3 SECURITY_DESCRIPTOR helpers

- [ ] `RtlCreateSecurityDescriptor(sd, rev)`
- [ ] `RtlSetOwnerSecurityDescriptor(sd, owner, defaulted)`
- [ ] `RtlSetGroupSecurityDescriptor(sd, group, defaulted)`
- [ ] `RtlSetDaclSecurityDescriptor(sd, present, dacl, defaulted)`
- [ ] `RtlSetSaclSecurityDescriptor(sd, present, sacl, defaulted)`
- [ ] `RtlGetOwnerSecurityDescriptor(sd, owner, defaulted)`
- [ ] `RtlGetDaclSecurityDescriptor(sd, present, dacl, defaulted)`
- [ ] `RtlAbsoluteToSelfRelativeSD(abs, rel_buf, rel_len)` — marshal to flat buffer
- [ ] `RtlSelfRelativeToAbsoluteSD(rel, abs_buf, ...)` — unmarshal from flat buffer

### 3.4 Default SDs for kernel object types

- [ ] `SeCreateDefaultSD(type)` — returns a self-relative SD with:
  - Owner = `SeLocalSystemSid`
  - DACL: `(A;;GA;;;SY)(A;;GA;;;BA)(A;;GR;;;WD)` — System+Admins=FullControl, Everyone=ReadControl
  - Stored as static blobs in `src/kernel/security/default_sds.c`
- [ ] Object types needing custom defaults:
  - `OB_TYPE_PROCESS` — `(A;;GA;;;SY)(A;;0x1FFFFF;;;BA)(A;;0x1000;;;WD)` (create/terminate restricted for Everyone)
  - `OB_TYPE_TOKEN` — `(A;;GA;;;SY)(A;;0x0008;;;OW)` (Query only for owner)
  - `OB_TYPE_REGISTRY_KEY` — `(A;;GA;;;SY)(A;;GA;;;BA)(A;;GR;;;BU)` (BuiltinUsers=Read)

### 3.5 Commit

- [ ] Commit: `"kernel/security: SECURITY_DESCRIPTOR, ACL, ACE types and helpers"`

---

## 4. ACCESS_TOKEN Object 
### 4.1 Token struct

- [ ] Define `struct ACCESS_TOKEN` in `include/kernel/security/token.h`:
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
- [ ] Integrity level SID constants:
  - `SeILUntrusted` = `S-1-16-0`
  - `SeILLow` = `S-1-16-4096`
  - `SeILMedium` = `S-1-16-8192`
  - `SeILHigh` = `S-1-16-12288`
  - `SeILSystem` = `S-1-16-16384`
- [ ] Register `ObTypeToken` via `ObCreateObjectType("Token", ...)` (→ XREF `TODO-03 §1`); token objects are reference-counted kernel objects with `OB_TYPE_TOKEN` type index

### 4.2 Token creation and system tokens

- [ ] `SeCreateSystemToken()` — called in Phase 0 of kernel init (→ XREF `TODO-01-kernel-init-sequencing.md §2`): UserSid=`SeLocalSystemSid`, Groups=`{SeBuiltinAdministratorsSid | SE_GROUP_ENABLED, SeWorldSid | SE_GROUP_ENABLED}`, all privileges enabled, IL=System; stored in `PsInitialSystemProcess->Token`
- [ ] `SeCreateUserToken(user_sid, admin)` — creates Medium IL primary token for interactive logon (called by login manager in `08-desktop-shell`); if `admin`, also creates High IL linked token for elevation; groups include `SeBuiltinUsersSid`; privileges: `SeChangeNotifyPrivilege` enabled by default; `SeShutdownPrivilege`/`SeUndockPrivilege` enabled by default; admin token adds `SeBackupPrivilege`, `SeRestorePrivilege`, `SeLoadDriverPrivilege` as disabled by default (present but off until elevated)

### 4.3 Token query syscalls

- [ ] `NtOpenProcessToken(ProcessHandle, DesiredAccess, TokenHandle)` — opens the primary token of a process; access check on the process object for `PROCESS_QUERY_INFORMATION`; returns handle with requested access
- [ ] `NtOpenThreadToken(ThreadHandle, DesiredAccess, OpenAsSelf, TokenHandle)` — returns impersonation token or `STATUS_NO_TOKEN` if thread is not impersonating
- [ ] `NtQueryInformationToken(hToken, class, buf, len, retlen)` — implement `TokenUser`, `TokenGroups`, `TokenPrivileges`, `TokenOwner`, `TokenPrimaryGroup`, `TokenDefaultDacl`, `TokenType`, `TokenImpersonationLevel`, `TokenStatistics`, `TokenIntegrityLevel`, `TokenElevationType`, `TokenLinkedToken`, `TokenIsElevated`

### 4.4 Token mutation syscalls

- [ ] `NtDuplicateToken(Existing, Access, ObjAttr, EffectiveOnly, Type, New)` — deep-copies token struct, allocates new `TokenId` LUID; `EffectiveOnly` strips disabled privileges and groups
- [ ] `NtAdjustPrivilegesToken(hToken, DisableAll, NewState, BufferLen, PreviousState, ReturnLen)` — for each LUID_AND_ATTRIBUTES in `NewState`: find matching privilege in token, apply SE_PRIVILEGE_ENABLED / SE_PRIVILEGE_REMOVED; requires `TOKEN_ADJUST_PRIVILEGES`; write previous state to `PreviousState`; return `STATUS_NOT_ALL_ASSIGNED` if any LUID not found
- [ ] `NtAdjustGroupsToken(hToken, ResetToDefault, NewState, BufferLen, PreviousState, ReturnLen)` — enable/disable group SIDs; cannot re-enable a `SE_GROUP_USE_FOR_DENY_ONLY` group (immutable once disabled)

### 4.5 Commit

- [ ] Commit: `"kernel/security: ACCESS_TOKEN object, SeCreateSystemToken, NtOpenProcessToken"`

---

## 5. SeAccessCheck Engine 
### 5.1 Subject security context

- [ ] Define `SECURITY_SUBJECT_CONTEXT`:
  ```c
  typedef struct {
      ACCESS_TOKEN *ClientToken;    /* NULL if not impersonating */
      SECURITY_IMPERSONATION_LEVEL ImpersonationLevel;
      ACCESS_TOKEN *PrimaryToken;   /* always the process token */
      bool          LockHeld;
  } SECURITY_SUBJECT_CONTEXT;
  ```
- [ ] `SeCaptureSubjectContext(ctx)` — reads `current_task()->Token` (primary) and TEB impersonation token (if any) into `ctx`; thread-safe snapshot
- [ ] `SeReleaseSubjectContext(ctx)` — dereferences token pointers

### 5.2 Generic access mapping

- [ ] Define `GENERIC_MAPPING` per object type in `include/kernel/security/generic_mapping.h`:
  ```c
  typedef struct {
      uint32_t GenericRead; uint32_t GenericWrite;
      uint32_t GenericExecute; uint32_t GenericAll;
  } GENERIC_MAPPING;
  ```
- [ ] Declare mappings for: File (standard Unix rwx mapping), Process, Thread, Token, Registry Key, Event, Mutex, Semaphore, Waitable Timer
- [ ] `RtlMapGenericMask(access, mapping)` — replaces `GENERIC_READ`/`WRITE`/ `EXECUTE`/`ALL` bits with type-specific masks in-place

### 5.3 Access check algorithm

- [ ] Implement `SeAccessCheck(sd, ctx, ctx_locked, desired, prev_granted, privs, mapping, mode, granted, status)`:
  1. **Kernel bypass**: if `mode == KernelMode` → `*granted = desired`, return `TRUE`
  2. **Owner bypass**: if `ctx->PrimaryToken->UserSid` == SD owner → set `READ_CONTROL | WRITE_DAC` bits in accumulated access without DACL check
  3. **DACL absent**: if `sd->Dacl == NULL` → grant all; if DACL present but empty (AceCount=0) → deny all
  4. **MIC pre-check**: call `SeCheckMandatoryAccess(ctx, sd, desired)` (§6); if MIC denies → `*status = STATUS_ACCESS_DENIED`, return `FALSE`
  5. **ACE walk**: iterate DACL ACEs in order; for each ACE:
     - `ACCESS_DENIED_ACE`: if any SID in token groups or user matches ACE SID AND `(desired & ACE->Mask) != 0` → deny immediately
     - `ACCESS_ALLOWED_ACE`: if SID matches AND ACE was not `INHERIT_ONLY` → accumulate `ACE->Mask` bits into granted mask
  6. **Result**: if `(desired & accumulated) == desired` → `*granted = desired`, return `TRUE`; else deny
- [ ] `SeAccessCheckByType(sd, ObjectType, ctx, desired, privs, mapping, mode, granted, status)` — same as above but also checks object-type ACEs (optional, `OBJECT_ACCESS_ACE_TYPE` — leave as stub for now)
- [ ] Write access-check unit test (pure kernel function, no QEMU needed):
  ```
  - SD: Owner=System, DACL=(Deny BA ReadControl)(Allow SY FullControl)(Allow BU ReadControl)
  - Subject: user=BA, groups=[BA, BU]
  - Desired: ReadControl → expect DENY (denial ACE hit first)
  - Subject: user=SY → expect GRANT
  - Subject: user=BU, desired=ReadControl → expect GRANT
  ```

### 5.4 Integration with Object Manager

- [ ] In `ObpReferenceObjectByHandle` (→ XREF `TODO-03 §3`): after locating the handle entry, call `SeAccessCheck(object->SecurityDescriptor, ctx, FALSE, desired, 0, NULL, object->Type->GenericMapping, UserMode, &granted, &status)`; return `STATUS_ACCESS_DENIED` if check fails
- [ ] `NtAccessCheck(sd, token, desired, mapping, privs, priv_len, granted, status)` syscall — user-mode accessible wrapper for testing DACLs without opening an object

### 5.5 Commit

- [ ] Commit: `"kernel/security: SeAccessCheck engine and ObpReferenceObjectByHandle integration"`

---

## 6. Mandatory Integrity Control (MIC) 
### 6.1 Integrity level comparison

- [ ] `SeGetTokenIntegrityLevel(token)` — returns `uint32_t` from `token->IntegrityLevelSid->SubAuthority[0]` (0, 4096, 8192, 12288, 16384)
- [ ] `SeGetObjectIntegrityLevel(sd)` — reads `SYSTEM_MANDATORY_LABEL_ACE` from SACL; returns `SeILMedium` (8192) if no SACL or no mandatory label
- [ ] `SeCompareMandatoryLevels(subject_il, object_il)` → `int` (-1 / 0 / +1)

### 6.2 No-Write-Up policy

- [ ] `SeCheckMandatoryAccess(ctx, sd, desired_access)` — called by §5.3 before DACL walk:
  - Compute `subject_il = SeGetTokenIntegrityLevel(effective_token)`
  - Compute `object_il = SeGetObjectIntegrityLevel(sd)`
  - Read `SYSTEM_MANDATORY_LABEL_ACE->Mask` policy bits:
    - `SYSTEM_MANDATORY_LABEL_NO_WRITE_UP (0x4)` — default; if `subject_il < object_il` AND `desired` contains write access bits → deny
    - `SYSTEM_MANDATORY_LABEL_NO_READ_UP (0x2)` — optional; if `subject_il < object_il` AND `desired` contains read access bits → deny
    - `SYSTEM_MANDATORY_LABEL_NO_EXECUTE_UP (0x1)` — if `subject_il < object_il` AND `desired` contains execute bits → deny
  - Returns `STATUS_ACCESS_DENIED` or `STATUS_SUCCESS`
- [ ] Default object IL assignment: kernel objects created by System process get `SeILSystem`; objects created by user process inherit creator's IL

### 6.3 Token IL enforcement at spawn

- [ ] When `NtCreateProcess` copies the parent token (§7), child token IL = `min(parent_IL, process_image_IL)`; image IL read from PE/ELF resource (`RT_MANIFEST`, requested execution level: `asInvoker`→same, `requireAdministrator`→High)
- [ ] Low IL sandbox mode: token with `SeILLow` is denied write to `%USERPROFILE%\*` (only `%LOCALAPPDATA%\Low\*` writable); enforced by MIC at `SeCheckMandatoryAccess` time

### 6.4 Commit

- [ ] Commit: `"kernel/security: Mandatory Integrity Control, No-Write-Up policy"`

---

## 7. Process/Thread Token Assignment & Impersonation 
### 7.1 Token field in task struct

- [ ] Add `ACCESS_TOKEN *Token;` to `struct task` in `include/kernel/sched/task.h`
- [ ] Add `ACCESS_TOKEN *ImpersonationToken;` — thread-level override; NULL = use process token
- [ ] `PsReferencePrimaryToken(task)` — increments token refcount and returns ptr
- [ ] `PsDereferencePrimaryToken(token)` — decrements; frees on zero

### 7.2 Token assignment at spawn

- [ ] In `NtCreateProcess` (→ XREF `TODO-09 §2`): call `NtDuplicateToken(parent->Token, TOKEN_ALL_ACCESS, NULL, FALSE, TokenPrimary, &child->Token)` — child starts with a deep copy of parent's primary token
- [ ] `SeCreateSystemToken()` result assigned to `PsInitialSystemProcess->Token` during Phase 0 kernel init (→ XREF `TODO-01-kernel-init-sequencing.md §2`)

### 7.3 Thread impersonation

- [ ] `NtImpersonateThread(ThreadHandle, ImpersonationThreadHandle, ImpLevel)` — duplicates the source thread's token as an impersonation token, sets `current_task()->ImpersonationToken`; requires `SeImpersonatePrivilege` if IL is higher than current thread
- [ ] `NtSetInformationThread(ThreadHandle, ThreadImpersonationToken, TokenHandle, sizeof(HANDLE))` — explicit impersonation token assignment (NULL handle = revert)
- [ ] `RevertToSelf()` Win32 wrapper — sets impersonation token to NULL
- [ ] `ImpersonateSelf(ImpersonationLevel)` — duplicates own primary token as impersonation token; used before adjusting privileges for a short operation

### 7.4 Effective token selection

- [ ] `SeQuerySubjectContextToken(ctx)` — returns impersonation token if present AND impersonation level ≥ `SecurityIdentification`; else returns primary token; used by SeAccessCheck §5 as the "effective" token

### 7.5 Commit

- [ ] Commit: `"kernel/security: task token field, spawn token copy, thread impersonation"`

---

## 8. SePrivilegeCheck & Per-Privilege Enforcement 
### 8.1 Core privilege check

- [ ] `SePrivilegeCheck(PrivilegeSet, ctx, AccessMode)`:
  - If `AccessMode == KernelMode` → return `TRUE`
  - For each LUID in `PrivilegeSet->Privilege`: scan effective token's `Privileges[]` for matching LUID with `SE_PRIVILEGE_ENABLED` attribute; if `PRIVILEGE_SET_ALL_NECESSARY`, all LUIDs must match; otherwise any one match suffices
  - Write SACL audit record if `SE_PRIVILEGE_USED_FOR_ACCESS` (future)
- [ ] `SeSinglePrivilegeCheck(Privilege, AccessMode)` — common single-LUID shortcut; called throughout kernel for specific privilege gates
- [ ] `SeCheckPrivilegedObject(PrivReq, Object, Desired, Mode)` — combines `SeSinglePrivilegeCheck` with `SeAccessCheck`; used by backup/restore paths

### 8.2 Privilege gates in kernel subsystems

- [ ] `module_load()` — `SeSinglePrivilegeCheck(SeLoadDriverPrivilege, UserMode)` before loading any `.kmod` from user request
- [ ] `NtShutdownSystem` / `NtReboot` — `SeSinglePrivilegeCheck( SeShutdownPrivilege, UserMode)`
- [ ] `NtSystemDebugControl` / `NtOpenProcess` with `PROCESS_ALL_ACCESS` on another-user's process — `SeSinglePrivilegeCheck(SeDebugPrivilege, UserMode)`
- [ ] `NtSetSystemTime` — `SeSinglePrivilegeCheck(SeSystemtimePrivilege, UserMode)`
- [ ] `NtCreateSymbolicLinkObject` — `SeSinglePrivilegeCheck( SeCreateSymbolicLinkPrivilege, UserMode)` for permanent symlinks
- [ ] `NtQuerySystemInformation(SystemPerformanceInformation)` — requires `SeSystemProfilePrivilege` if `mode == UserMode`

### 8.3 Commit

- [ ] Commit: `"kernel/security: SePrivilegeCheck, privilege gates for driver load, shutdown, debug"`

---

## 9. UAC Token Split & NtFilterToken 
### 9.1 NtFilterToken

- [ ] `NtFilterToken(ExistingToken, Flags, SidsToDisable, PrivilegesToDelete, RestrictedSids, NewToken)`:
  - `DISABLE_MAX_PRIVILEGE` (flag): mark all privileges except `SeChangeNotifyPrivilege` as `SE_PRIVILEGE_REMOVED`
  - `SidsToDisable`: mark matched group SIDs with `SE_GROUP_USE_FOR_DENY_ONLY` (present in deny checks but not allow checks)
  - `PrivilegesToDelete`: remove matching privilege entries entirely
  - `RestrictedSids`: append to token as restricted SID list (second DACL pass required — access must be allowed by BOTH the normal DACL walk AND a walk of the restricted SID list)
  - Sets `TOKEN_IS_RESTRICTED` flag on new token
  - Used internally by UAC to produce the "filtered" Medium token for admin users

### 9.2 Linked token pair

- [ ] `SeCreateLinkedTokenPair(FullAdminToken, FilteredToken)`:
  - `FullAdminToken->LinkedTokenId` = `FilteredToken->TokenId`
  - `FilteredToken->LinkedTokenId` = `FullAdminToken->TokenId`
  - `FilteredToken->ElevationType = TokenElevationTypeLimited`
  - `FullAdminToken->ElevationType = TokenElevationTypeFull`
  - Both tokens reference each other; when user requests elevation, kernel resolves the linked token from the filtered one
- [ ] `NtQueryInformationToken(TokenLinkedToken)` — returns handle to the linked token; requires `TOKEN_QUERY` and that the caller holds `SeTcbPrivilege` (prevents unprivileged elevation discovery)

### 9.3 Elevation request syscall

- [ ] `NtRequestTokenElevation(ProcessHandle, hToken, ElevationType)` — kernel side of the elevation flow:
  1. Check calling process has `TokenElevationTypeLimited` token
  2. Signal the consent UI process (→ XREF `09-desktop-shell/TODO-06-security-accounts.md §11`) via a dedicated kernel event object
  3. Wait for consent UI to signal approval or denial event
  4. On approval: `NtSetInformationProcess(ProcessHandle, ProcessAccessToken, &linked_token)` — replaces the process token with the full-admin linked token
  5. On denial: return `STATUS_PRIVILEGE_NOT_HELD`
- [ ] Note: full consent UI implementation is in `08-desktop-shell` — this TODO provides only the kernel side of the handshake

### 9.4 Commit

- [ ] Commit: `"kernel/security: NtFilterToken, linked token pair, UAC elevation protocol"`

---

## 10. Win32 Security API Wrappers 
### 10.1 Token Win32 API

- [ ] `OpenProcessToken(hProcess, DesiredAccess, phToken)` → `NtOpenProcessToken`
- [ ] `OpenThreadToken(hThread, DesiredAccess, OpenAsSelf, phToken)` → `NtOpenThreadToken`
- [ ] `GetTokenInformation(hToken, class, buf, len, retlen)` → `NtQueryInformationToken`
- [ ] `SetTokenInformation(hToken, class, buf, len)` → `NtSetInformationToken`
- [ ] `AdjustTokenPrivileges(hToken, DisableAll, NewState, BufferLen, PreviousState, ReturnLen)` → `NtAdjustPrivilegesToken`; note: Win32 returns TRUE even for `STATUS_NOT_ALL_ASSIGNED` (set last-error instead)
- [ ] `CheckTokenMembership(hToken, SidToCheck, IsMember)` — scan token groups for matching SID with `SE_GROUP_ENABLED` attribute; NULL token = current thread effective token
- [ ] `IsUserAnAdmin()` — `CheckTokenMembership(NULL, SeBuiltinAdministratorsSid,
  &member)`; returns TRUE only if token is elevated admin (High IL)
- [ ] `IsTokenRestricted(hToken)` → check `TOKEN_IS_RESTRICTED` flag in token

### 10.2 SID Win32 API

- [ ] `ConvertSidToStringSidW(Sid, StringSid)` — formats `S-1-X-Y-...` into heap-allocated `WCHAR*` (caller frees with `LocalFree`)
- [ ] `ConvertStringSidToSidW(StringSid, Sid)` — parse `S-1-...` string back to SID blob
- [ ] `AllocateAndInitializeSid(IdentifierAuthority, SubAuthorityCount, ...)`
  — up to 8 sub-authority args; allocates SID blob (caller frees with `FreeSid`)
- [ ] `FreeSid(Sid)` — wraps `kfree`
- [ ] `EqualSid`, `CopySid`, `LengthSid`, `IsValidSid` Win32 wrappers

### 10.3 Security descriptor Win32 API

- [ ] `GetSecurityInfo(handle, ObjectType, SecurityInfo, Owner, Group, Dacl, Sacl, SD)` → `NtQuerySecurityObject`
- [ ] `SetSecurityInfo(handle, ObjectType, SecurityInfo, Owner, Group, Dacl, Sacl)` → `NtSetSecurityObject`
- [ ] `GetNamedSecurityInfoW(name, ObjectType, SecurityInfo, ...)` — resolves path to file handle, then calls `NtQuerySecurityObject`
- [ ] `SetNamedSecurityInfoW(name, ObjectType, SecurityInfo, ...)` — resolve + `NtSetSecurityObject`; requires `WRITE_DAC` or `SE_SECURITY_PRIVILEGE` for SACL
- [ ] `ConvertStringSecurityDescriptorToSecurityDescriptorW(SDDL, Revision, SD, SDSize)` — minimal SDDL parser: parse `O:XX G:XX D:...(A;;XX;;;XX)...` syntax; supports `SY`=System, `BA`=Admins, `BU`=Users, `WD`=Everyone aliases
- [ ] `ConvertSecurityDescriptorToStringSecurityDescriptorW(SD, Revision, SecurityInfo, StringSD, StringSDLen)` — reverse; produces SDDL string

### 10.4 Commit

- [ ] Commit: `"kernel/security: Win32 token, SID, and security descriptor API wrappers"`

---

## 11. Live Token Inspector 
### 11.1 whoami.exe

- [ ] `src/apps/whoami/whoami.c` — command-line tool; calls `OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)` then `GetTokenInformation` for each class; formats output as aligned table. CLI usage:
  ```
  whoami [/user] [/groups] [/priv] [/all]
    /user    — print current user SID and account name
    /groups  — print all group SIDs with attributes (Enabled/Disabled/DenyOnly)
    /priv    — print all privileges with Enabled/Disabled/Removed status
    /all     — equivalent to /user /groups /priv
  ```

### 11.2 Token tray popout (stretch)

- [ ] System tray right-click → "Token Info" → flyout showing: current user, integrity level badge (colour-coded: Low=yellow, Medium=green, High=orange, System=red), admin status, elevation type, top 5 privileges
- [ ] Useful for developers to verify elevation state without opening a terminal

### 11.3 Commit

- [ ] Commit: `"kernel/security: whoami.exe and token tray popout"`

---

## OS Comparison

| ⭐ | Feature                             | Win11                   | Linux                        | Impossible OS                    |
|----|-------------------------------------|-------------------------|------------------------------|----------------------------------|
| 💎 | Token-based identity                | ✅ Full                 | ⚠️ UID/GID only              | ⬜ §4                            |
| 💎 | DACL access check on every object   | ✅ Full                 | ⚠️ POSIX permission bits     | ⬜ §5                            |
| 💎 | Mandatory Integrity Control         | ✅ Vista+               | ⚠️ SELinux/AppArmor (add-on) | ⬜ §6                            |
| 💎 | Privilege separation                | ✅ Full                 | ⚠️ Capabilities only         | ⬜ §8                            |
| 💎 | UAC filtered-token + elevation      | ✅ Full                 | ❌ Not applicable            | ⬜ §9                            |
| 💎 | Thread impersonation                | ✅ Full                 | ❌ Not available             | ⬜ §7                            |
| 💎 | SDDL string security descriptors    | ✅ Full                 | ❌ Not available             | ⬜ §10                           |
| 💎 | NtFilterToken / restricted tokens   | ✅ Full                 | ❌ Not available             | ⬜ §9                            |
| ⭐ | Live token inspector in tray        | ❌ CLI only (whoami)    | ❌ CLI only                  | ⬜ §11 — 🚀                      |
| ⭐ | Integrated IL badge in File Manager | ❌ Hidden in properties | ❌ Not available             | ⬜ §11 + `09-desktop-shell` 🚀   |
| ⭐ | Real-time ACL denial toast          | ❌ Event log only       | ❌ auditd log only           | ⬜ Planned — `10-services` 🚀    |

After §1–10, Impossible OS reaches full Windows 11 security architecture parity — SID tokens, DACL/SACL access checks, MIC integrity levels, privilege separation, and UAC elevation are all present. Linux with only POSIX permissions and optional MAC add-ons (SELinux/AppArmor) is strictly weaker. The tray token inspector (§11) and integrated IL badges in the File Manager are exclusive features that make Impossible OS's security model visible and actionable to developers and power users.

---

## Verification

- [ ] **Unit test — SeAccessCheck**: pure kernel call without QEMU; SD with deny+allow ACEs; verify correct grant/deny for System, Admin, User subjects.
- [ ] **Unit test — MIC No-Write-Up**: Low IL token attempting `GENERIC_WRITE` on a Medium IL object → `STATUS_ACCESS_DENIED` from `SeCheckMandatoryAccess`.
- [ ] **Unit test — AdjustTokenPrivileges**: create token with `SeShutdownPrivilege` disabled; call `AdjustTokenPrivileges` to enable it; verify privilege now reports `SE_PRIVILEGE_ENABLED`; call again with `DisableAll=TRUE`; verify all disabled.
- [ ] **`whoami.exe` in QEMU**: launch `whoami /all`; output must show the system token's SID, group list (System, Administrators, Everyone), and privilege table with all expected LUIDs present.
- [ ] **Object access check in QEMU**: create a file with a DACL that denies the current user; verify `CreateFile` returns `ERROR_ACCESS_DENIED`.
- [ ] **Remaining limits**: full consent UI (UAC dialog) is in `09-desktop-shell/TODO-06-security-accounts.md §11`; SACL audit logging deferred to `09-services-security`; kernel object type SACL auditing (file access audit events) deferred to the same.
- [ ] Commit: `"kernel/security: SRM complete — ACCESS_TOKEN, ACL/DACL, SeAccessCheck, MIC, impersonation, UAC token split, Win32 security API"`
