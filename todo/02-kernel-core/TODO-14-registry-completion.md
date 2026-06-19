---
schema_version: 1
id: registry-completion
domain: 02-kernel-core
status: active
title: "TODO-14 -- Registry System Completion"
---

# TODO-14 -- Registry System Completion

> **Goal:** The core registry engine (`reg_key_t`, Win32 registry API surface per MSDN ch. 2.1 through 2.4, hive persistence, crash-safe WAJ journaling) is fully implemented in `src/kernel/registry.c` (2 536 lines). This TODO delivers everything that is still pending: access rights enforcement, advanced key operations, change notifications, Nt/Zw user-mode syscalls, the `advapi32.dll` compatibility layer, registry virtualization, a `regedit` shell tool, advanced hive features (dual-log WAJ, delta flush, compaction), and the exclusive stretch features (atomic transactions, search API, snapshot diff, per-PID quota).
> When complete, Impossible OS has a native Windows-compatible registry that exceeds both Windows 11 and Linux's configuration store in every dimension.

> [!IMPORTANT]
> **Current state -- what is already done:**
> - `reg_key_t` / `reg_value_t` structs, static pool allocator, FNV-1a hash
> - `RegOpenKeyEx`, `RegCreateKeyEx`, `RegCloseKey`, `RegDeleteKey/Tree`
> - `RegSetValueEx`, `RegQueryValueEx`, `RegGetValue`, `RegDeleteValue`
> - `RegEnumKeyEx`, `RegEnumValue`, `RegQueryInfoKey`
> - One-shot helpers: `RegGetDword`, `RegSetString`, `RegReadKeyValue`
> - Hive file format, disk layout, crash-safe `.hive.log` WAJ journaling
> - All root keys (`HKLM`, `HKCU`, `HKCR`, `HKU`, `HKCC`)
> - `reg_key_t.last_write_time` (PIT tick counter updated on every mutation; returned by `RegQueryInfoKey`)
> - `REG_LINK` type, `REG_FLAG_LINK` flag, `reg_key_is_link()`/`reg_key_get_link_target()` helpers, transparent symlink resolution in `reg_resolve_path()`
> - `%VAR%` expansion helpers: `reg_lookup_env_var()` / `reg_expand_sz()` (kernel boot path; full `NtQueryValueKey` / enumerate for env blocks is still TODO-22 + §4)
> **What is NOT done** (scope of this TODO): access rights enforcement, API limits, `RegFlushKey`, `RegCopyTree`, `RegRenameKey`, `RegSaveKey`, `RegRestoreKey`, `REG_OPTION_VOLATILE`, delayed close cache, change notifications, all Nt/Zw syscalls, `advapi32.dll` stubs, UTF-16 A/W variants, HKCR merged view, registry virtualization, `.reg` import/export, `regedit` command, dual-log WAJ, incremental delta flush, hive integrity reporter, format versioning, compaction, transactions, search API, snapshot/diff, per-PID quota, and performance optimisations (mmap, B-tree).

> [!CAUTION]
> **Memory rule:** Use `pmm_alloc_contiguous()` for ALL buffers > 4 KiB (hive file read/write, large binary values). `kmalloc` is only for small structs ≤ 4 KiB. Violating this silently corrupts the 2 MiB kernel heap.

---

## Inputs

- `src/kernel/registry.c` -- 2 536-line implementation (engine complete)
- `include/registry.h` -- types, constants, API declarations
- → XREF: `TODO-12-native-api-ssdt.md §14, §15` -- SSDT: core registry entry points in §14 (0x0090--0x009B), advanced (flush/notify/save/hive) in §15 (0x009C--0x00A6 + extended range); §14 must exist before §5 of this TODO
- → XREF: `TODO-08-time-filetime-management.md §4` -- `ticks_to_filetime()` conversion needed by §5 for `LastWriteTime` FILETIME output
- → XREF: `TODO-15-security-reference-monitor.md §6,§7,§8` -- `SECURITY_DESCRIPTOR` + `SeAccessCheck` are used to enforce `KEY_*` access rights on `RegOpenKeyEx` / `NtOpenKey`
- → XREF: `TODO-05-object-manager.md §2` -- registry `HKEY` handles must eventually be registered in the per-process handle table for `DuplicateHandle` parity; deferred to §3 of this TODO as a note
- → XREF: `TODO-22-environment-variables.md §2` -- system env vars from `Session Manager\Environment` once `NtEnumerateValueKey` / `NtQueryValueKey` (this file §4) are wired; boot uses `reg_expand_sz` today
- → XREF: `01-boot-platform/TODO-02-uefi-hardening-secureboot.md §9` -- optional DWORD values under `HKLM\SYSTEM\SecureBoot\` (db/dbx counts); must not regress `State` value written from TODO-01 §5
- → XREF: `01-boot-platform/TODO-05-boot-device-discovery.md §9` -- `HKLM\SYSTEM\Boot\Device\*` boot provenance is written from `boot_info` after `registry_init()` (coordinate value layout with that TODO)
- → XREF: `01-boot-platform/TODO-09-cpu-boot-sequencing.md §4,§10` -- optional `HKLM\HARDWARE\VM\*` hypervisor mirror from `boot_info`; per-CPU `HKLM\HARDWARE\CPU\%u\Registers` audit strings per that TODO §10
- → XREF: `TODO-02-kernel-configuration-policy.md §3, §4` -- `HKLM\SYSTEM\CurrentControlSet\Control\Kernel`, `Select`, and LastKnownGood semantics are consumed by the kernel config plane

---

## Outcome

- Every `RegXxx` call enforces `KEY_*` access-right bits against the key's DACL and rejects out-of-spec names/paths with correct `ERROR_*` codes; `LastWriteTime` returned as proper FILETIME.
- `RegNotifyChangeKeyValue` delivers asynchronous callbacks with change-detail payloads, coalescing, priority dispatch, and `REG_NOTIFY_THREAD_AGNOSTIC` persistence.
- User-mode processes access the registry via 30+ `NtXxx` syscalls (SSDT-wired) including `NtSetInformationKey`, `NtQueryMultipleValueKey`, `NtNotifyChangeMultipleKeys`, `NtFreezeRegistry`/`NtThawRegistry`, and all `KEY_INFORMATION_CLASS` / `KEY_VALUE_INFORMATION_CLASS` variants.
- `advapi32.dll` exports all standard `RegXxx` Win32 functions in both A and W variants; HKCR provides the merged HKCU+HKLM\Software\Classes view.
- Registry symlinks fully exposed via `REG_OPTION_CREATE_LINK` / `REG_OPTION_OPEN_LINK` with loop detection; `CurrentControlSet` symlink wired at boot.
- `.reg` files can be imported and exported; registry virtualization redirects low-IL writes to `HKCU\Software\VirtualStore\` with per-key control flags (`REG_KEY_DONT_VIRTUALIZE`).
- `regedit list/query/set/delete/tree/export/schema` shell subcommands are usable.
- Dual-log WAJ, incremental delta flush, and hive compaction improve crash safety and I/O efficiency beyond what Windows 11 provides.
- Atomic transactions, a pattern-search API, snapshot/diff, and schema-validated keys give Impossible OS exclusive registry capabilities.

---

## Implementation Order

| ⭐  | Order | Deliverable                                        | Depends On                   | Status |
| --- | :---: | -------------------------------------------------- | ---------------------------- | :----: |
| 💎  |   1   | Access rights, API limits, FILETIME & RegFlushKey  | TODO-05 §2,§3, TODO-17 §4 |  [ ]   |
| 💎  |   2   | Advanced key ops (copy, rename, save, volatile)    | §1                           |  [ ]   |
| 💎  |   3   | Change notifications (core + exclusive extras)     | §2                           |  [ ]   |
| 💎  |   4   | Nt/Zw registry syscalls & pointer validation       | §1, TODO-12 §14, §15         |  [ ]   |
| 💎  |   5   | advapi32.dll compat (A/W, HKCR, error map)         | §4                           |  [ ]   |
| 💎  |   6   | Registry virtualization & .reg import/export       | §5                           |  [ ]   |
| 💎  |   7   | `regedit` shell tool                               | §4                           |  [ ]   |
| ⭐  |   8   | Advanced hive features (dual-log, delta, compact)  | §4                           |  [ ]   |
| ⭐  |   9   | Transactions, search API & snapshot/diff           | §2, §3, §8                   |  [ ]   |
| ⭐  |  10   | Performance (mmap hive, B-tree cell format)        | §9                           |  [ ]   |
| 💎  |  11   | KTM Transaction syscalls wired to SSDT             | §9, TODO-12 §14, §15         |  [ ]   |
| 💎  |  12   | Registry symlink completion (create, open-link)    | §2, §4                       |  [ ]   |
| ⭐  |  13   | Schema-validated registry keys                     | §3, §4                       |  [ ]   |
| 💎  |  14   | Registry SMP synchronization                       | TODO-12 §14                  |  [ ]   |

> 💎 = parity work -- matches what Windows 11 and Linux already do.
> ⭐ = exclusive work -- Impossible OS is superior or first.

> [!NOTE]
> **Lean structure:** §4 (Nt/Zw syscall surface) and §13 (schema) carry more than 8--10 top-level checklist bullets each. Before implementation, consider splitting them into additional top-level `##` sections (renumbering the file) so commits stay reviewable -- do not use `### N.M` or `**N.M**` pseudo-headings inside a section.

---

## 1. Access Rights, API Limits & RegFlushKey

> [!NOTE]
> **Self-contained execution:** `SeAccessCheck` (TODO-15 §8) and `SECURITY_DESCRIPTOR` (TODO-15 §6) are not yet implemented. §1 should stub `SeAccessCheck` as always-grant with a `#warning` reminder. `SECURITY_DESCRIPTOR` should be a `void*` placeholder. Full enforcement arrives when TODO-15 §6,§7,§8 is complete.

- [ ] Add `uint32_t access_mask` field to `reg_key_t` (stored at open time)
- [ ] `RegOpenKeyEx`: validate `samDesired` against key's `SECURITY_DESCRIPTOR` DACL via `SeAccessCheck` (→ XREF `TODO-15-security-reference-monitor.md §8`); store granted mask in returned HKEY; return `ERROR_ACCESS_DENIED` on failure
- [ ] `RegCreateKeyEx`: requires `KEY_CREATE_SUB_KEY` on parent
- [ ] `RegSetValueEx` / `RegDeleteValue`: requires `KEY_SET_VALUE` on key
- [ ] `RegQueryValueEx` / `RegEnumKeyEx` / `RegEnumValue`: requires `KEY_QUERY_VALUE` / `KEY_ENUMERATE_SUB_KEYS`
- [ ] `KEY_READ = KEY_QUERY_VALUE | KEY_ENUMERATE_SUB_KEYS | KEY_NOTIFY | STANDARD_RIGHTS_READ`; `KEY_WRITE = KEY_SET_VALUE | KEY_CREATE_SUB_KEY | STANDARD_RIGHTS_WRITE`; `KEY_ALL_ACCESS = 0xF003F` -- all bits
- [ ] Default DACL on new keys: `(A;;KA;;;SY)(A;;KA;;;BA)(A;;KR;;;BU)` -- System+Admins = all access, BuiltinUsers = read only

- [ ] Key name max: 255 chars (currently `REG_MAX_KEY_NAME = 255` ✅, but not enforced on `RegCreateKeyEx` input -- add check)
- [ ] Value name max: **16 383** chars (increase `REG_MAX_VALUE_NAME` from 255 → 16 383, backing store uses `pmm_alloc_contiguous` for names > 255); **HIGH RISK** -- current `reg_value_t.name` is a fixed `char[256]` array; must change to pointer + `pmm_alloc_contiguous` for large names, keep inline for ≤ 255 chars; update all serialization paths in `hive_save` / `hive_load`; rollback: keep 255 limit if breakage detected
- [ ] Value data max: **1 MiB** (increase `REG_MAX_VALUE_SIZE` from 512 → 1 048 576; large values use `pmm_alloc_contiguous`); **HIGH RISK** -- current `reg_value_t.data` is a fixed `uint8_t[512]` array; must change to pointer-based allocation for data > 512 bytes; update `hive_save`/`hive_load`, `RegSetValueEx`, `RegQueryValueEx`; rollback: keep 512 limit if breakage detected
- [ ] Key path depth max: 512 levels -- `reg_path_depth(path)` count of `\\` separators; return `ERROR_INVALID_PARAMETER` if exceeded
- [ ] Total key count: soft warn at 90% of pool; hard limit returns `ERROR_OUTOFMEMORY`; pool size comment documents the limit
- [ ] **`RegCreateKeyEx` atomic create-or-fail** -- links the key, then allocates the `HKEY` separately; handle-pool exhaustion leaves a linked-but-handleless key. Reserve the handle before linking or roll back on failure. (TODO-09-boot §9.)

- [ ] `reg_key_t.last_write_time` currently stores raw PIT ticks; convert to Windows `FILETIME` (100-ns intervals since 1601-01-01) via `ticks_to_filetime()` (→ XREF `TODO-08-time-filetime-management.md §4`); if `ticks_to_filetime()` is not yet available, implement a minimal stub (`ticks * PIT_NS_PER_TICK / 100 + FILETIME_EPOCH_BIAS`) inline -- full implementation in TODO-17 §4
- [ ] `RegQueryInfoKey` `lpftLastWriteTime` output: return the FILETIME, not raw ticks
- [ ] `NtQueryKey(KeyBasicInformation)`, `NtQueryKey(KeyNodeInformation)`, `NtQueryKey(KeyFullInformation)` -- all return `LARGE_INTEGER LastWriteTime` in FILETIME format; output structs must match NT definitions

- [ ] `RegFlushKey(hKey)` -- force immediate hive sync for the hive containing `hKey`; calls `hive_flush_sync(hive)` which writes dirty pages and the WAJ log without waiting for the 5-second lazy-writer timer
- [ ] Return `ERROR_SUCCESS` on flush; `ERROR_INVALID_HANDLE` for volatile keys (volatile keys have no hive backing -- flush is a no-op, not an error)

- [ ] Commit: `"kernel/registry: KEY_* access rights enforcement, API limits, FILETIME timestamps, RegFlushKey"`

**Test checkpoint:** `RegOpenKeyEx(HKLM\SOFTWARE, KEY_SET_VALUE)` from a Medium-IL process against an Admins-only DACL → `ERROR_ACCESS_DENIED`; as SYSTEM → `ERROR_SUCCESS`. `RegCreateKeyEx` with 256-char name → `ERROR_INVALID_PARAMETER`; 255-char → succeeds. `RegQueryInfoKey.ftLastWriteTime` returns non-zero FILETIME after `RegSetValueEx`. `RegFlushKey` on volatile key → `ERROR_SUCCESS` (no-op). Serial log: `"[REG] Access denied: %s mask=0x%x required=0x%x"`. Test on: QEMU WHPX + TCG.

---

## 2. Advanced Key Operations

- [ ] `RegCopyTree(hKeySrc, lpSubKey, hKeyDest)` -- recursively copy all sub-keys and values from `hKeySrc\lpSubKey` into `hKeyDest`; preserves value types, data, and sub-key structure; uses existing `RegCreateKeyEx` + `RegSetValueEx` internally; requires `KEY_READ` on source and `KEY_WRITE` on destination
- [ ] `RegRenameKey(hKey, lpSubKeyName, lpNewKeyName)` -- rename a sub-key in place: create new key, copy all values and children (recursive `RegCopyTree`), delete old key tree; atomic under `hKey->lock` spinlock; return `ERROR_ALREADY_EXISTS` if `lpNewKeyName` already exists

- [ ] `RegSaveKey(hKey, lpFile, lpSecurityAttributes)` -- serialise the sub-tree rooted at `hKey` to a standalone `.hive` file at `lpFile` (write a new hive header + all keys/values in the sub-tree; use the existing `hive_write_key` path); requires `SeBackupPrivilege` (→ XREF `TODO-15-security-reference-monitor.md §3`)
- [ ] `RegRestoreKey(hKey, lpFile, dwFlags)` -- replace the sub-tree rooted at `hKey` with the contents of a `.hive` file; `REG_FORCE_RESTORE (0x8)` allows replacing in-use keys; requires `SeRestorePrivilege`

- [ ] `RegCreateKeyEx` with `dwOptions = REG_OPTION_VOLATILE (0x1)`: set `REG_FLAG_VOLATILE` on the new `reg_key_t`; volatile keys are excluded from `hive_flush` and `hive_write_key`; they are destroyed on reboot
- [ ] `RegFlushKey` on a volatile key returns `ERROR_SUCCESS` (no-op, not an error)
- [ ] `RegQueryInfoKey` correctly reports `REG_OPTION_VOLATILE` in its `lpdwClass` output for volatile keys

- [ ] Add an LRU cache of 32 recently closed `reg_key_t*` pointers; on `RegCloseKey`: if the key still exists in the tree, move its handle to the LRU cache instead of immediately freeing the pool slot
- [ ] On `RegOpenKeyEx`: check the LRU cache first (O(1) name hash compare); if hit, promote the entry, bump refcount, return immediately -- avoids tree walk for hot keys like `HKLM\SYSTEM\Display` that are opened and closed in a tight loop
- [ ] Cache entries are evicted on LRU overflow or when the underlying key is deleted; eviction releases the pool slot

- [ ] Commit: `"kernel/registry: RegCopyTree, RegRenameKey, RegSaveKey/RestoreKey, volatile keys, KCB cache"`

**Test checkpoint:** `RegCopyTree(hSrc, NULL, hDst)` → all subkeys and values copied recursively. `RegRenameKey(hKey, "Old", "New")` → old key gone, new key has same values; rename to existing name → `ERROR_ALREADY_EXISTS`. `RegCreateKeyEx(REG_OPTION_VOLATILE)` → reboot → key absent. Delayed close: open/close/reopen same key 1000x → KCB cache hit rate > 90%. Serial log: `"[REG] CopyTree: %s -> %s (%u keys, %u values)"`. Test on: QEMU WHPX + TCG.

---

## 3. Change Notifications

- [ ] Add to `include/registry.h`:
  ```c
  #define REG_NOTIFY_CHANGE_NAME       0x01 /* sub-key create/delete */
  #define REG_NOTIFY_CHANGE_ATTRIBUTES 0x02 /* key metadata change */
  #define REG_NOTIFY_CHANGE_LAST_SET   0x04 /* value create/modify/delete */
  #define REG_NOTIFY_CHANGE_SECURITY   0x08 /* security descriptor change */

  typedef void (*reg_notify_fn)(const char *key_path, uint32_t change_type,
                                const char *value_name,  /* NULL if not applicable */
                                void *ctx);
  typedef struct reg_watcher {
      uint32_t      watcher_id;
      reg_key_t    *key;
      uint32_t      filter;        /* REG_NOTIFY_CHANGE_* bitmask */
      bool          watch_subtree;
      reg_notify_fn callback;
      void         *ctx;
      bool          active;
      uint64_t      hit_count;     /* ⭐ telemetry counter */
      uint64_t      coalesce_ms;   /* ⭐ min ms between fires (0=off) */
      uint64_t      last_fired_ms; /* ⭐ last fire timestamp */
  } reg_watcher_t;
  ```
- [ ] Static pool of 64 watchers (`reg_watcher_t watcher_pool[64]`); `bool active` marks slot in use; `watcher_id` is a monotonic u32 counter

- [ ] `RegNotifyChangeKeyValue(hKey, bWatchSubtree, dwNotifyFilter, hEvent, fAsynchronous)`:
  - Allocate `reg_watcher_t` slot from pool; if pool full → `ERROR_OUTOFMEMORY`
  - Set `key`, `filter = dwNotifyFilter`, `watch_subtree = bWatchSubtree`
  - If `fAsynchronous == FALSE`: store a semaphore in the watcher; caller will block on it after this call
  - If `fAsynchronous == TRUE` and `hEvent != NULL`: store the event handle; `SetEvent(hEvent)` will be called on each matching change
  - `reg_notify_register(watcher)` appends to the key's watcher list
  - Returns `ERROR_SUCCESS`; caller queries result via `WaitForSingleObject` on `hEvent` or the built-in semaphore

- [ ] `reg_dispatch_notify(key, change_type, value_name)` -- called at the end of `RegSetValueEx`, `RegDeleteValue`, `RegCreateKeyEx`, `RegDeleteKey`, and `reg_set_security_descriptor`:
  1. Walk `key->watcher_list`; for each watcher where `(watcher->filter & change_type) != 0`:
     - Subtree check: if `!watcher->watch_subtree`, only fire if `key == watcher->key`; if `watcher->watch_subtree`, fire if `key` is a descendant of `watcher->key` (walk parent pointers)
     - **Coalescing** ⭐: if `watcher->coalesce_ms > 0` and `now_ms - watcher->last_fired_ms < coalesce_ms` → skip (dedup)
     - Set `watcher->last_fired_ms = now_ms`; increment `watcher->hit_count`
     - If async event: `SetEvent(watcher->hEvent)`
     - If sync: `semaphore_signal(watcher->sem)` to unblock the caller
     - If callback watcher (§3): call `watcher->callback(key_path, change_type, value_name, watcher->ctx)`
  2. After firing: if `!fAsynchronous` watcher is one-shot -- mark `watcher->active = false` (Win32 contract: synchronous watchers fire once and must be re-registered)

- [ ] Subtree walk: `reg_is_descendant(ancestor, key)` -- follow `key->parent` chain up to the root; return true if `ancestor` is found
- [ ] On `RegCloseKey`: call `reg_notify_unregister_all(key)` -- mark all watchers for this key `active = false`; wake any blocked callers with `ERROR_KEY_DELETED`
- [ ] On `RegDeleteKey`: before deletion, call `reg_dispatch_notify(key, REG_NOTIFY_CHANGE_NAME, NULL)` for all ancestor watchers watching the parent; then `reg_notify_unregister_all(key)`
- [ ] `RegUnregisterNotify(watcher_id)` (non-Win32 internal API) -- mark watcher inactive; used by kernel subsystems that registered watchers directly (e.g., theme system watching `HKCU\...\Theme`)

- [ ] **Change-detail payloads** ⭐ -- extend `reg_notify_fn` signature to include `old_value` and `new_value` blobs when `change_type == REG_NOTIFY_CHANGE_LAST_SET`; store old value snapshot before mutation in `reg_dispatch_notify` and pass to callback; enables zero-parse diff for settings watchers
- [ ] **Telemetry** ⭐ -- expose `HKLM\SYSTEM\Registry\WatcherStats\<key_path>` with `HitCount` (REG_QWORD) and `LastFiredMs` (REG_QWORD) auto-updated on each dispatch; survives reboots via hive flush
- [ ] **Priority-based dispatch** ⭐ -- add `uint8_t priority` field (0=normal, 1=high, 2=system) to `reg_watcher_t`; `reg_dispatch_notify` fires `priority=2` watchers first (in-order), then `priority=1`, then `priority=0`; system-priority watchers are those registered by `theme_init`, `display_init`, and `service_manager_init`

- [ ] `REG_NOTIFY_THREAD_AGNOSTIC (0x10000000)` -- new flag for `NtNotifyChangeKey`'s `CompletionFilter` parameter; when set, the notification registration is owned by the *process* rather than the calling thread; the watcher survives thread exit and is only cleaned up when the HKEY is closed or the process terminates
- [ ] Without this flag (default Win32 behavior): watcher is cleaned up when the registering thread exits, even if the HKEY is still open
- [ ] Implementation: if `(filter & REG_NOTIFY_THREAD_AGNOSTIC)`, attach `reg_watcher_t` to `task->process_watcher_list` instead of `task->thread_watcher_list`; `thread_exit()` only cleans up thread-local watchers; `process_exit()` cleans up process-level watchers

- [ ] Commit: `"kernel/registry: change notifications, subtree watching, coalescing, detail payloads"`

**Test checkpoint:** Register watcher on `HKLM\SYSTEM\Display` with `REG_NOTIFY_CHANGE_LAST_SET`. `RegSetValueEx("Width", 1920)` → callback fires with `value_name="Width"`. Coalescing: set `coalesce_ms=100`, fire 10 writes in 50ms → callback fires at most once. Priority: system watcher fires before normal watcher. `REG_NOTIFY_THREAD_AGNOSTIC`: register from thread A, exit thread A → watcher survives; modify key → notification fires. Serial log: `"[REG] Notify: %s change=0x%x watcher=%u"`. Test on: QEMU WHPX + TCG.

---

## 4. Nt/Zw Registry Syscalls

> [!NOTE]
> Minimal prerequisites from other TODOs -- full implementation there, stubs here:
> - **SSDT dispatch** (TODO-12 §5): the `ssdt_register(index, handler)` infrastructure must exist before §5 can wire syscalls. If not yet implemented, §5 must add a minimal SSDT stub table and `syscall` dispatch handler -- enough to route `NtXxx` calls from user-mode to kernel handlers. Minimal prerequisite -- full implementation in TODO-12 §4.
> - **ProbeForRead / ProbeForWrite** (TODO-23 §13): §4 requires pointer validation. If not yet implemented, add no-op stubs that accept all pointers -- real probing added by TODO-23 §13.
> - **APC queuing** (`05-storage-filesystems/TODO-05 §9`): `NtNotifyChangeKey` async path uses APC delivery. If not yet implemented, use synchronous-only path initially.

- [ ] Add to SSDT (→ XREF `TODO-12-native-api-ssdt.md §14, §15`); SSDT indices already reserved in TODO-12:
  ```
  NtCreateKey(KeyHandle, DesiredAccess, ObjectAttributes, TitleIndex, Class, CreateOptions, Disposition)         0x0090
  NtOpenKey(KeyHandle, DesiredAccess, ObjectAttributes)                                                          0x0092
  NtOpenKeyEx(KeyHandle, DesiredAccess, ObjectAttributes, OpenOptions)                                           0x0094
  NtDeleteKey(KeyHandle)                                                                                         0x0095
  NtRenameKey(KeyHandle, NewName)                                                                                0x009F
  NtSetValueKey(KeyHandle, ValueName, TitleIndex, Type, Data, DataSize)                                          0x0096
  NtQueryValueKey(KeyHandle, ValueName, KeyValueInfoClass, KeyValueInfo, Length, ResultLength)                   0x0097
  NtQueryMultipleValueKey(KeyHandle, ValueEntries, EntryCount, ValueBuffer, BufferLength, RequiredLength)         --
  NtDeleteValueKey(KeyHandle, ValueName)                                                                         0x0098
  NtEnumerateKey(KeyHandle, Index, KeyInfoClass, KeyInfo, Length, ResultLength)                                  0x0099
  NtEnumerateValueKey(KeyHandle, Index, KeyValueInfoClass, KeyValueInfo, Length, ResultLength)                   0x009A
  NtQueryKey(KeyHandle, KeyInfoClass, KeyInfo, Length, ResultLength)                                             0x009B
  NtSetInformationKey(KeyHandle, KeySetInfoClass, KeySetInfo, KeySetInfoLength)                                   --
  NtFlushKey(KeyHandle)                                                                                          0x009C
  NtNotifyChangeKey(KeyHandle, Event, ApcRoutine, ApcContext, IoStatusBlock, CompletionFilter, ...)              0x009D
  NtNotifyChangeMultipleKeys(MasterKeyHandle, Count, SubordinateObjects[], Event, ApcRoutine, ...)               0x009E
  NtSaveKey(KeyHandle, FileHandle)                                                                               0x00A0
  NtSaveKeyEx(KeyHandle, FileHandle, Format)                                                                     0x00A1
  NtRestoreKey(KeyHandle, FileHandle, Flags)                                                                     0x00A2
  NtReplaceKey(NewFile, TargetHandle, OldFile)                                                                   --
  NtLoadKey(TargetKey, SourceFile)                                                                               0x00A3
  NtLoadKeyEx(TargetKey, SourceFile, Flags, TrustClassKey, Event, DesiredAccess, RootHandle, IoStatus)           0x00A4
  NtUnloadKey(TargetKey)                                                                                         0x00A5
  NtUnloadKeyEx(TargetKey, Event)                                                                                0x00A6
  NtCompactKeys(Count, KeyArray[])                                                                               0x00A8
  NtCompressKey(KeyHandle)                                                                                       0x00A9
  NtQueryOpenSubKeys(TargetKey, HandleCount)                                                                     0x00A7
  NtLockRegistryKey(KeyHandle)                                                                                   0x00AA
  NtFreezeRegistry(TimeOutInSeconds)                                                                             --
  NtThawRegistry(VOID)                                                                                           --
  NtInitializeRegistry(BootCondition)                                                                            --
  ZwXxx aliases for each -- kernel-mode bypass wrappers
  ```
- [ ] Syscalls marked `--` need SSDT index allocation in TODO-12 §14/§15; add to the registry block (0x0090--0x00AA range or extended range)
- [ ] `ObjectAttributes` for key paths: `RootDirectory` handle + `ObjectName` (`UNICODE_STRING` for future UTF-16; for now accept UTF-8 `ANSI_STRING` wrapper); resolve absolute paths starting with `\Registry\Machine` → `HKLM`, `\Registry\User\{SID}` → `HKCU`
- [ ] Wrap every user-mode pointer argument in `ProbeForRead(ptr, size, align)` / `ProbeForWrite(ptr, size, align)` (→ XREF `TODO-23-exception-dispatch-seh.md §13`) before any dereference; return `STATUS_ACCESS_VIOLATION` if probe faults
- [ ] `UNICODE_STRING` / `ANSI_STRING` struct fields: validate both the struct pointer AND the embedded `Buffer` pointer separately
- [ ] **UTF-16 decode for `UNICODE_STRING` inputs (kernel-wide)**: current Nt handlers across §14/§15/§17 cast `UNICODE_STRING.Buffer` (uint16_t*) directly to `const char*`, which treats real UTF-16 input as a one-byte ASCII string truncated at the first high byte. Add `int nt_decode_unicode_string(const UNICODE_STRING *us, char *buf, uint32_t buf_size)` in NEW file `src/kernel/nt/nt_string.c` (declared in `include/kernel/nt/nt_string.h`). Implementation: read `us->Length` bytes (counted, NOT NUL-terminated), validate each UTF-16 code unit fits in ASCII (high byte == 0), validate `us->Length <= us->MaximumLength`, validate `us->Length / 2 + 1 <= buf_size`, write to `buf` as NUL-terminated ASCII, return `STATUS_SUCCESS` or `STATUS_INVALID_PARAMETER`. The helper is kernel-wide because all NT names (registry keys, token values, namespace paths, file paths) are ASCII per MSDN. **Retrofit consumers across all §-handlers**: (a) registry §14 `NtSetValueKey` / `NtQueryValueKey` / `NtDeleteValueKey` / `NtEnumerateValueKey` and §15 `NtRenameKey` in `src/kernel/nt/nt_registry.c` (replace `(const char *)vname->Buffer` casts and `reg_oa_path` ASCII assumption); (b) token §16 (no UNICODE_STRING parsing currently: handler args are HANDLE / ACCESS_MASK / struct-by-pointer); (c) namespace §17 `oa_name`, `path_within_bounds`, `NtCreateSymbolicLinkObject_handler` target read in `src/kernel/nt/nt_namespace.c` (cast UNICODE_STRING.Buffer to char*, scan with bounded_strlen); (d) section §18 `oa_probe_ascii_name` in `src/kernel/nt/nt_section.c:39` and timer §19 `oa_probe_ascii_name` in `src/kernel/nt/nt_timer.c` (both return the raw `UNICODE_STRING.Buffer` pointer after probing `Length` bytes; callers feed it to `snprintf("%s")` in `ObCreateSectionEx`/`ObCreateTimerEx`/`ObOpenTimer` which reads past `Length` if the buffer lacks an embedded NUL); (e) future NT handlers must use `nt_decode_unicode_string` directly: never cast.
- [ ] **Per-key `NtFlushKey` scope**: current TODO-12 §15 NtFlushKey flushes all dirty hives via `registry_flush_checked()`. Add `registry_flush_key(reg_key_t *key)` that walks the key's parent chain to find its owning hive index (matching `hive_table[i]`), then flushes only that one hive. Update NtFlushKey_handler to call `registry_flush_key(resolve_hkey(hkey))` instead of `registry_flush_checked()`. Rationale: Win32 `RegFlushKey` semantics expect per-hive scope; current implementation over-flushes on SMP systems with high registry write rates.
- [ ] **Migrate HKEY to OB handle table**: current TODO-12 §14 returns raw `HKEY` pointers (into `reg_handle_pool`) as NT `HANDLE` values. `NtClose` (TODO-12 §9) cannot release them, so long-running processes exhaust the 128-entry pool. Register a new `ObpKeyType` via `ob_create_type("Key", sizeof(reg_key_handle_t), ...)` with an `on_close` callback that calls `RegCloseKey` on the underlying `reg_handle_t*`, then change `NtCreateKey_handler` / `NtOpenKey_handler` / `NtOpenKeyEx_handler` / `NtLoadKey_handler` in `src/kernel/nt/nt_registry.c` to allocate via `ObpAllocateHandle(ht, reg_handle, access, 0)` instead of casting HKEY to HANDLE. All §14/§15 handlers that currently do `hkey = (HKEY)(uintptr_t)a1` then `hkey->key` must route through `ObpLookupHandle + type-check against ObpKeyType + recover reg_handle_t*`. This auto-closes Finding §14 "HKEY-to-HANDLE ABI gap" and removes the 128-handle exhaustion limit.
- [ ] **KeyNodeInformation output struct in NtQueryKey/NtEnumerateKey**: TODO-12 §14 `NtEnumerateKey` returns `STATUS_INVALID_PARAMETER` for `KeyFullInformation`/`KeyNodeInformation` (only `KeyBasicInformation` and `KeyNameInformation` are supported; `KeyFullInformation` is implemented via opening child + RegQueryInfoKey but `KeyNodeInformation` is rejected). Define `KEY_NODE_INFORMATION` struct in `include/kernel/nt/nt_registry.h` (`LastWriteTime`, `TitleIndex`, `ClassOffset`, `ClassLength`, `NameLength`, `Name[1]`), then extend both `NtQueryKey_handler` and `NtEnumerateKey_handler` to fill it. Current registry has no class string so `ClassOffset = (uint32_t)-1` and `ClassLength = 0` in the output.
- [ ] `KeyValueInfo` output buffer: `ProbeForWrite(KeyValueInfo, Length, 1)`; if probe succeeds but `Length` is too small to hold the result, return `STATUS_BUFFER_TOO_SMALL` with `*ResultLength` set to the required size
- [ ] `KEY_INFORMATION_CLASS` -- `NtQueryKey` / `NtEnumerateKey` info class enum:
  - `KeyBasicInformation` (0): `LastWriteTime`, `TitleIndex`, `NameLength`, `Name[]`
  - `KeyNodeInformation` (1): adds `ClassOffset`, `ClassLength`
  - `KeyFullInformation` (2): adds `SubKeys`, `MaxNameLen`, `MaxClassLen`, `Values`, `MaxValueNameLen`, `MaxValueDataLen`
  - `KeyNameInformation` (3): full key path name
  - `KeyCachedInformation` (4): cached version of KeyFullInformation (no class string)
  - `KeyFlagsInformation` (5): user flags and control flags (virtualization, wow64)
  - `KeyVirtualizationInformation` (6): virtualization open/enabled/target status
  - `KeyHandleTagsInformation` (7): handle tags for trusted keys
  - `KeyTrustInformation` (8): trust level information
- [ ] `KEY_VALUE_INFORMATION_CLASS` -- `NtQueryValueKey` / `NtEnumerateValueKey` info class enum:
  - `KeyValueBasicInformation` (0): `TitleIndex`, `Type`, `NameLength`, `Name[]`
  - `KeyValueFullInformation` (1): adds `DataOffset`, `DataLength`, `Data[]`
  - `KeyValuePartialInformation` (2): `TitleIndex`, `Type`, `DataLength`, `Data[]` (no name)
  - `KeyValueFullInformationAlign64` (3): 64-bit aligned variant
  - `KeyValuePartialInformationAlign64` (4): 64-bit aligned variant
  - `KeyValueLayerInformation` (5): layered key metadata (Win10+)
- [ ] `KEY_SET_INFORMATION_CLASS` -- `NtSetInformationKey` info class enum:
  - `KeyWriteTimeInformation` (0): set `LastWriteTime` to a specific FILETIME
  - `KeyWow64FlagsInformation` (1): WoW64 flags
  - `KeyControlFlagsInformation` (2): virtualization control flags (`REG_KEY_DONT_VIRTUALIZE`, etc.)
  - `KeySetVirtualizationInformation` (3): enable/disable virtualization per key
  - `KeySetDebugInformation` (4): debug information
  - `KeySetHandleTagsInformation` (5): set handle tags
- [ ] `NtNotifyChangeKey`: **replace the existing `STATUS_NOT_IMPLEMENTED` stub** in `src/kernel/nt/nt_registry.c::NtNotifyChangeKey_handler` (currently tagged `SCOPE-GAP-ALLOWED: blocked on TODO-14 §4`) with real wiring: validates `KeyHandle`, resolves to `reg_key_t`, calls `reg_notify_register()` from §4; if `Asynchronous == FALSE`, blocks calling thread on the watcher's semaphore (interruptible via APC). Remove the `SCOPE-GAP-ALLOWED` comment when the stub is replaced.
- [ ] APC completion: if `ApcRoutine != NULL`, queue a user-mode APC to the calling thread when the notification fires (→ XREF `05-storage-filesystems/TODO-05-win32-file-io-api.md §9`)
- [ ] `NtNotifyChangeMultipleKeys(MasterKeyHandle, Count, SubordinateObjects[], ...)`: register notifications on `MasterKeyHandle` plus up to `Count` additional subordinate keys in a single call; fires when *any* of the watched keys changes; shares implementation with §3 but registers multiple watchers atomically
- [ ] `NtSetInformationKey(KeyHandle, KeySetInfoClass, KeySetInfo, Length)`: set key metadata; initial implementation covers `KeyWriteTimeInformation` (set `LastWriteTime`) and `KeyControlFlagsInformation` (set virtualization control flags -- see §6)
- [ ] `NtQueryMultipleValueKey(KeyHandle, ValueEntries[], EntryCount, ValueBuffer, BufferLength, RequiredLength)`: query multiple values in a single syscall; `ValueEntries` is an array of `KEY_VALUE_ENTRY` structs (name + type + data offset); reduces syscall overhead for batch reads (e.g., reading 10 display settings at once)
- [ ] `NtReplaceKey(NewFile, TargetHandle, OldFile)`: atomically replace a hive file; used during upgrade/restore; copies `NewFile` over the hive backing `TargetHandle`, saves current hive to `OldFile`; requires `SeRestorePrivilege`
- [ ] `NtFreezeRegistry(TimeOutInSeconds)`: temporarily freeze all registry write operations system-wide for up to `TimeOutInSeconds`; used by backup software (VSS) to get a consistent snapshot; pending writes block until `NtThawRegistry()` is called or timeout expires
- [ ] `NtThawRegistry()`: unfreeze registry after `NtFreezeRegistry`; resumes all blocked writes
- [ ] `NtInitializeRegistry(BootCondition)`: called by SMSS during boot to signal that the registry is ready; `REG_INIT_BOOT_SM (0)` = normal, `REG_INIT_BOOT_SETUP (1)` = setup/install mode, `REG_INIT_BOOT_ACCEPTED_BASE (2)` = accept the current control set as "last known good"
- [ ] `NtQueryOpenSubKeys(TargetKey, HandleCount)` / `NtQueryOpenSubKeysEx(...)`: return how many handles are open to subkeys of `TargetKey`; used before `NtUnloadKey` to check if a hive can be safely unloaded
- [ ] `NtCompactKeys(Count, KeyArray[])`: move the specified keys into the same hive bin to improve spatial locality; `NtCompressKey(KeyHandle)`: compress a key and all subkeys (reduces hive file size)
- [ ] `NtLockRegistryKey(KeyHandle)`: mark a key as immutable for the remainder of the boot session; used for security-critical keys like `HKLM\SAM`
- [ ] `HKCU` redirect: `NtOpenKey` with a path under `\Registry\User` resolves to the *current process's* per-session user subtree rather than a global `HKCU`; each process inherits its user SID from its primary token (→ XREF `TODO-15-security-reference-monitor.md §2`); `\Registry\User\S-1-5-21-...-1001` is the actual physical path; `NtOpenKey` with `RootDirectory=HKCU` resolves via the task's token
- [ ] **Per-process sandbox** ⭐ -- `NtSetInformationProcess(ProcessRegistrySandbox, root_path)`: future API that restricts all registry access for a process to a sub-tree; used by browser renderer and low-IL processes; deferred until process isolation (→ XREF `TODO-21-process-model-extensions.md`) matures
- [ ] **Rate limiting** ⭐ -- per-task `reg_ops_this_sec` counter reset every 1 000 ms by scheduler tick; if > 10 000 registry ops per second: `schedule_yield()` and re-check; soft throttle prevents runaway registry hammering from buggy apps; counter tracked in `struct task`
- [ ] **Audit log** ⭐ -- if `HKLM\SYSTEM\Registry\AuditEnabled = 1`: write a compact audit entry to `X:\Logs\registry-audit.log` on each `NtSetValueKey` / `NtDeleteKey`: `{timestamp, pid, key_path, value_name, old_type, new_type, result_ntstatus}`; uses the existing `klog` ring buffer at LOG_AUDIT level; auto-rotated at 4 MiB
- [ ] Commit: `"kernel/registry: NtOpenKey/NtSetValueKey/NtNotifyChangeKey syscalls, pointer validation, audit"`

**Test checkpoint:** `NtCreateKey` + `NtOpenKey` round-trip via SYSCALL from user-mode succeeds. `NtQueryValueKey(KeyValuePartialInformation)` returns correct data. `ProbeForWrite` with invalid pointer → `STATUS_ACCESS_VIOLATION`. `NtFreezeRegistry(5)` → all `NtSetValueKey` from another thread block; `NtThawRegistry()` → blocked writes complete. Audit log: enable `AuditEnabled=1` → `NtSetValueKey` entries appear in `registry-audit.log`. Serial log: `"[SSDT] NtOpenKey: \\Registry\\Machine\\... -> 0x%x"`. Test on: QEMU WHPX + TCG.

---

## 5. advapi32.dll Win32 Compatibility

- [ ] All `RegXxx` functions that take string arguments have an `A` variant (UTF-8 / ANSI) and a `W` variant (UTF-16LE `WCHAR*`)
- [ ] `A` variants: call the internal UTF-8 kernel API directly (already exists)
- [ ] `W` variants: `wchar_to_utf8(src_w, buf, len)` → call internal UTF-8 API → convert any UTF-8 string results back to UTF-16LE via `utf8_to_wchar(src, buf, len)` before returning to caller
- [ ] `wchar_to_utf8` / `utf8_to_wchar`: implement in `src/libs/libc/wchar.c` (BMP-only initially; no surrogate pair support needed for registry paths); use existing `libc` string helpers

- [ ] `HKEY_CLASSES_ROOT` read path: `RegOpenKeyEx(HKCR, sub_key, ...)` → first look in `HKCU\Software\Classes\<sub_key>`; if not found, look in `HKLM\SOFTWARE\Classes\<sub_key>`; return whichever is found first
- [ ] `HKCR` write path: `RegCreateKeyEx(HKCR, sub_key, ...)` → always write to `HKCU\Software\Classes\<sub_key>` (per-user override)
- [ ] `RegEnumKeyEx(HKCR, ...)`: merge results from both HKCU\Software\Classes and HKLM\SOFTWARE\Classes, deduplicate by name, return union; HKCU entries shadow HKLM entries with same name

- [ ] `reg_ntstatus_to_win32(NTSTATUS)` table: `STATUS_OBJECT_NAME_NOT_FOUND` → `ERROR_FILE_NOT_FOUND`, `STATUS_ACCESS_DENIED` → `ERROR_ACCESS_DENIED`, `STATUS_BUFFER_TOO_SMALL` → `ERROR_MORE_DATA`, `STATUS_NO_MORE_ENTRIES` → `ERROR_NO_MORE_ITEMS`, `STATUS_INSUFFICIENT_RESOURCES` → `ERROR_OUTOFMEMORY`, default → `ERROR_INVALID_FUNCTION`
- [ ] Win32 API wrappers call `SetLastError(reg_ntstatus_to_win32(status))` on failure before returning; `GetLastError()` returns the correct code

- [ ] **Tracing toggle** ⭐ -- `HKLM\SYSTEM\Registry\TraceEnabled = 1` (default 0) enables per-call tracing: each `RegXxx` call logs to the serial log at `LOG_TRACE` level: `[REG] RegSetValueEx HKLM\System\Display Width=1920 (pid=42)` with PID, key path, value name, type, and `NTSTATUS` result
- [ ] `reg_trace(func_name, hKey, value_name, type, status)` helper function; only evaluated when `reg_trace_enabled` global is 1 (set from `HKLM\SYSTEM\Registry\TraceEnabled` on `registry_init`)

- [ ] Commit: `"kernel/registry: advapi32 A/W shims, HKCR merged view, error mapping, API tracing"`

**Test checkpoint:** `RegOpenKeyExW(HKLM, L"SYSTEM\\Display", ...)` succeeds (UTF-16 path resolves). `RegOpenKeyExA(HKCR, "myapp.doc")` → reads HKCU\Software\Classes first, falls back to HKLM. Failed `RegQueryValueEx` → `GetLastError()` returns `ERROR_FILE_NOT_FOUND`. API trace: `TraceEnabled=1` → serial log shows `"[REG] RegSetValueEx HKLM\\... Width=1920 (pid=N)"`. Test on: QEMU WHPX + TCG.

---

## 6. Registry Virtualization & .reg Import/Export

- [ ] Low-IL and non-elevated processes that write to `HKLM\SOFTWARE\<path>` are silently redirected to `HKCU\Software\VirtualStore\MACHINE\SOFTWARE\<path>`
- [ ] Condition for redirect: `current_task()->Token->IntegrityLevelSid == SeILMedium` AND write is to `HKLM\SOFTWARE` subtree AND the key does not have `REG_FLAG_READONLY` AND the calling process is NOT running elevated (→ XREF `TODO-15-security-reference-monitor.md §5`)
- [ ] `NtCreateKey` / `NtSetValueKey`: check for virtualization condition before the write; if active, silently rewrite the path to VirtualStore and notify caller of success -- the caller is unaware of the redirect
- [ ] Read path: `NtOpenKey` / `NtQueryValueKey` for virtualized paths: try VirtualStore first; fall back to real HKLM path
- [ ] **Virtualization control flags** -- per-key opt-out via `NtSetInformationKey(KeyControlFlagsInformation)`:
  - `REG_KEY_DONT_VIRTUALIZE (0x02)`: disable virtualization for this key; writes from low-IL processes fail with `STATUS_ACCESS_DENIED` instead of being silently redirected
  - `REG_KEY_DONT_SILENT_FAIL (0x04)`: when set and virtualization would apply, the write fails loudly instead of being redirected; debugging aid for app developers
  - `REG_KEY_RECURSE_FLAG (0x08)`: apply the parent's control flags to all children recursively
  - Store flags in `reg_key_t.control_flags` field; check during virtualization condition evaluation in `NtCreateKey` / `NtSetValueKey`

- [ ] `.reg` file format (v5.00, Windows 2000+):
  ```
  Windows Registry Editor Version 5.00

  [HKEY_LOCAL_MACHINE\SOFTWARE\Test]
  "StringValue"="hello"
  "DwordValue"=dword:0000002a
  "BinaryValue"=hex:01,02,03
  [-HKEY_LOCAL_MACHINE\SOFTWARE\DeletedKey]
  ```
- [ ] `reg_import(path)` -- parse line-by-line:
  - `[KEY_PATH]` → `RegCreateKeyEx` on the key path
  - `[-KEY_PATH]` → `RegDeleteKey`
  - `"name"=type:value` → decode type:
    - `"string"` (no prefix) → `REG_SZ`
    - `dword:XXXXXXXX` → `REG_DWORD` (hex 8 digits)
    - `hex:xx,xx,...` → `REG_BINARY`
    - `hex(7):...` → `REG_MULTI_SZ` (decode hex, NUL-NUL terminated)
    - `hex(2):...` → `REG_EXPAND_SZ`
    - `hex(b):...` → `REG_QWORD`
  - `@="..."` → sets the default value (empty name `""`)
  - Skip blank lines and `;` comment lines
- [ ] `regedit import <file>` shell wrapper

- [ ] `reg_export(hKey, path)` -- recursive export of a key sub-tree:
  - Write `Windows Registry Editor Version 5.00\r\n\r\n` header
  - For each key in DFS order: write `[HKXX\full\path]\r\n`
  - For each value: encode according to type (reverse of §6 parse rules)
  - For `REG_SZ` values: escape `\` → `\\`, `"` → `\"`
  - Recurse into sub-keys; write empty line between key sections
- [ ] `regedit export HKLM\SOFTWARE\Test output.reg` shell wrapper

- [ ] Commit: `"kernel/registry: virtualization redirect, .reg import/export"`

**Test checkpoint:** Low-IL process writes `HKLM\SOFTWARE\Test` → redirected to `HKCU\Software\VirtualStore\MACHINE\SOFTWARE\Test`. Read: VirtualStore value returned first. `REG_KEY_DONT_VIRTUALIZE` on key → low-IL write → `STATUS_ACCESS_DENIED`. `.reg` round-trip: export → delete → import → all values restored byte-for-byte. Serial log: `"[REG] Virtualize: %s -> VirtualStore\\%s"`. Test on: QEMU WHPX + TCG.

---

## 7. `regedit` Shell Tool

- [ ] `regedit list <path>` -- list immediate sub-key names and value names + types under `<path>`; format:
  ```
  HKLM\SYSTEM\Display
    Keys:   (none)
    Values: Width    REG_DWORD  1920
            Height   REG_DWORD  1080
            Depth    REG_DWORD  32
  ```
- [ ] `regedit query <path> <valueName>` -- print a single value; format: `HKLM\SYSTEM\Display\Width = 1920 (REG_DWORD)`
- [ ] `regedit set <path> <valueName> <type> <data>` -- write a value; `type` is one of `REG_SZ`, `REG_DWORD`, `REG_QWORD`, `REG_BINARY`, `REG_MULTI_SZ`; `data` is the string representation (hex for binary, decimal for DWORD/QWORD, comma-separated hex bytes for binary)
- [ ] `regedit delete <path> [<valueName>]` -- without `valueName`: delete the key and all children (`RegDeleteTree`); with `valueName`: delete only the named value (`RegDeleteValue`)
- [ ] `regedit tree <path> [--depth N]` -- recursive ASCII tree; default depth 4; `--depth 0` = unlimited
- [ ] `regedit export <path> <file>` -- call `reg_export` (§6)
- [ ] `regedit import <file>` -- call `reg_import` (§6)

- [ ] Unknown path → print `Error: key not found: <path>`
- [ ] Access denied → print `Error: access denied (KEY_WRITE required)`
- [ ] Invalid type string → print `Error: unknown type '<type>'; valid: REG_SZ REG_DWORD REG_QWORD REG_BINARY REG_MULTI_SZ`

- [ ] Commit: `"shell: regedit list/query/set/delete/tree/export/import subcommands"`

**Test checkpoint:** `regedit list HKLM\SYSTEM\Display` → shows Width/Height/Depth with types. `regedit set HKLM\Test val1 REG_DWORD 42` + `regedit query HKLM\Test val1` → `42 (REG_DWORD)`. `regedit delete HKLM\Test` → key removed. `regedit tree HKLM --depth 2` → ASCII tree output. Invalid path → `"Error: key not found: ..."`. Test on: QEMU WHPX + TCG.

---

## 8. Advanced Hive Features

- [ ] Wire `registry_load_hives()` into boot (`boot_storage.c` after defaults, needs C: mounted); this loads on-disk `ExternalEntropy`, so the secure-delete items below MUST land together. -> XREF: `01-boot-platform/TODO-12 §9`
- [ ] Secure one-shot deletion across main + journal + `.bak` before cross-reboot absorb (else recovery reintroduces the value = reuse); then drop the `registry_persistence_active()` gate. -> XREF: `01-boot-platform/TODO-12 §9`
- [ ] Per-hive flush status (`registry_flush_hive_checked()`) so an unrelated dirty hive failing can't discard a persisted SYSTEM one-shot in `entropy_external_consume()`. -> XREF: `01-boot-platform/TODO-12 §9`
- [ ] Recovery tests (with the secure-delete work): post-consume main-hive corruption must NOT reintroduce `ExternalEntropy`; an unrelated-hive flush failure must NOT discard a deleted SYSTEM one-shot.
- [ ] Maintain two alternating journal files: `SYSTEM.hive.log1` and `SYSTEM.hive.log2`; current active log tracked in hive header `active_log` byte (0=log1, 1=log2)
- [ ] Write cycle: dirty pages + header written to active log → `fsync` → commit marker written → `fsync` → copy dirty pages into main hive file → clear journal → switch active log to the other file
- [ ] Recovery: on mount, check both log files for a valid commit marker; use the one with the higher sequence number; if both are valid but different, the newer one wins; if neither has a commit marker, hive is clean
- [ ] Advantage over single-log WAJ: if a crash occurs while clearing the journal, the other log still has the previous good state; matches Windows NT `.LOG1`/`.LOG2` behaviour exactly

- [ ] Add `uint8_t dirty_bitmap[HIVE_MAX_PAGES / 8]` to the in-memory hive struct; each bit corresponds to one 4 KiB hive page; set on every mutation that touches a page
- [ ] `hive_flush_incremental()` -- only write dirty pages to disk (seek+write each dirty 4 KiB page); clear bitmap after write; vs. the current `hive_flush()` which rewrites the entire hive from scratch
- [ ] Speedup: a single `RegSetValueEx` call on a 10 MiB hive flushes 4 KiB instead of 10 MiB; critical for fast boot (many small writes during init)
- [ ] Lazy writer timer: call `hive_flush_incremental` every 5 s from a kernel timer DPC (→ XREF `TODO-07-irql-model-dpcs.md §4`); also called from `RegFlushKey` and the shutdown path

- [ ] `chkregistry <hive_path>` shell command:
  - Open hive file directly (not via the live registry)
  - Validate header CRC32; check `magic`, `version`, `key_count`, `value_count` fields
  - Walk all `reg_key_t` entries: verify parent pointers are valid, verify child-list linkage is consistent (no cycles, no dangling pointers)
  - For each `reg_value_t`: verify `type` is a known `REG_*` constant, verify `data_size ≤ REG_MAX_VALUE_SIZE`
  - Report: total keys, total values, errors found, estimated bytes wasted (deleted pool slots that could be compacted)
  - `--fix` flag: calls `hive_compact` (§8) on a copy and replaces the original if compact succeeds

- [ ] **Format versioning**: hive header `version` field (currently 1); `registry_init` reads version; if version > current code supports → log warning + mount read-only; if version < supported → auto-migrate (bump version, add any new header fields at the end of the header page)
- [ ] **Compaction** ⭐ -- `hive_compact(hive)`:
  - Allocate new hive buffer
  - Walk all live (non-deleted) `reg_key_t` and `reg_value_t` entries in BFS order; pack them tightly into the new buffer
  - Write new hive atomically: write to `.hive.tmp` → fsync → rename over old file (atomic on IXFS/NTFS)
  - Triggered automatically when pool utilisation < 60% (many deletions have occurred) or by `chkregistry --fix`
  - Reclaims memory: a registry with 10 000 creations and 8 000 deletions compacts from 10 000 slots to 2 000 slots

- [ ] Boot lifecycle: change `registry_init()` (`registry.c:272`) `void`→`boot_result_t`, return BOOT_FATAL on hive-mount/default-population failure so `boot_phase2` branches to recovery. -> XREF: `02-kernel-core/TODO-01 §4,§8`
- [ ] Commit: `"kernel/registry: dual-log WAJ, incremental delta flush, chkregistry, hive compaction"`

**Test checkpoint:** Dual-log: corrupt `SYSTEM.hive.log1` mid-write → `registry_init` mounts from `log2`; data intact. Delta flush: modify one value in 1 MiB hive → `hive_flush_incremental` writes exactly 4 KiB (one dirty page). `chkregistry SYSTEM.hive` → reports key/value counts, 0 errors. Compaction: create 10K keys, delete 8K → `hive_compact` reduces pool to ~2K slots. Serial log: `"[REG] Delta flush: %u dirty pages written"`. Test on: QEMU WHPX + TCG.

---

## 9. Transactions, Search API & Snapshot/Diff

- [ ] `RegBeginTransaction(hKey)` → returns `HREG_TXN` (transaction handle): creates a `reg_txn_t` struct with a copy-on-write journal -- each mutation during the transaction writes to the journal instead of the live tree
- [ ] `RegCommitTransaction(hTxn)` -- apply all journalled mutations to the live tree under `hKey->lock` spinlock; atomically visible to all readers after the spinlock is released; flush hive after commit
- [ ] `RegAbortTransaction(hTxn)` -- discard the journal; live tree is unchanged; free `reg_txn_t` struct
- [ ] Conflict detection: if a key modified in the transaction has also been modified by another writer since `RegBeginTransaction`, return `STATUS_REGISTRY_TRANSACTION_CONFLICT` from `RegCommitTransaction`; caller must retry or abort
- [ ] `NtCreateTransaction` / `NtCommitTransaction` / `NtRollbackTransaction` SSDT wrappers; compatible with Win32 `RtlSetCurrentTransaction` contract

- [ ] `RegFindKey(hRoot, lpPattern, dwFlags, phKey)` -- glob-pattern search (`*` = any sequence, `?` = single char, case-insensitive) over key names in the sub-tree rooted at `hRoot`; returns the first match via `phKey`; caller calls `RegFindNextKey(search_handle, phKey)` to iterate
- [ ] `RegFindValue(hRoot, lpKeyPattern, lpValuePattern, dwType, phKey, lpValueName)` -- search for a value matching both a key-name pattern and a value-name pattern; optionally filter by `dwType` (0 = any type)
- [ ] `reg_find_state_t` internal struct: holds the DFS traversal stack (current path, depth) so `RegFindNextKey` can resume where it left off without rescanning from the root

- [ ] `RegTakeSnapshot(hRoot, phSnapshot)` -- deep-copy the sub-tree rooted at `hRoot` into a new in-memory tree (not persisted to disk); returns an opaque `HREG_SNAPSHOT` handle
- [ ] `RegDiffSnapshots(hSnapshot1, hSnapshot2, callback, ctx)` -- compare two snapshots (or a snapshot and the live tree via `NULL`):
  - For each key/value added: callback with `REG_DIFF_ADDED`
  - For each key/value deleted: callback with `REG_DIFF_DELETED`
  - For each value that changed type or data: callback with `REG_DIFF_MODIFIED` including old and new values
- [ ] `RegFreeSnapshot(hSnapshot)` -- free the deep-copy tree
- [ ] `regedit diff <snapshot_file1> <snapshot_file2>` -- call `RegTakeSnapshot` on two `.reg` exports and diff them; output in unified diff style with `+` / `-` / `~` prefixes

- [ ] **Orphan GC** ⭐ -- `reg_gc_orphans()`: walk all pool slots; find `reg_key_t` entries with `active=true` but no path from any root key reachable (parent pointer chain never reaches HKLM/HKCU/etc.); log each orphan and free its pool slot; run automatically on `registry_flush` if orphan count > 10
- [ ] **Per-PID quota** ⭐ -- `HKLM\SYSTEM\Registry\QuotaEnabled = 1` (default 0): track per-task total bytes of registry data written since process start in `struct task`; if > `QuotaBytes` (default 50 MiB): `NtSetValueKey` returns `STATUS_QUOTA_EXCEEDED`; quota reset on process exit; enables per-app registry footprint limiting

- [ ] Commit: `"kernel/registry: transactions, search API, snapshot/diff, orphan GC, per-PID quota"`

**Test checkpoint:** `RegBeginTransaction` + `RegSetValueEx` + `RegCommitTransaction` → value visible. `RegAbortTransaction` → value absent. Conflict: two concurrent txns modify same key → second commit → `STATUS_REGISTRY_TRANSACTION_CONFLICT`. `RegFindKey(HKLM, "Disp*")` → finds `Display`. `RegTakeSnapshot` + modify + `RegDiffSnapshots` → `REG_DIFF_MODIFIED`. Per-PID quota at 50 MiB → exceed → `STATUS_QUOTA_EXCEEDED`. Serial log: `"[REG] Transaction commit: %u mutations applied"`. Test on: QEMU WHPX + TCG.

---

## 10. Performance Optimisations

- [ ] Replace the linked-list `reg_key_t.children` with a fixed-size FNV-1a hash map; `REG_CHILD_BUCKETS = 16` (already defined in `registry.h`); each bucket is a pointer to a chain of children with the same hash bucket
- [ ] `reg_child_lookup(parent, name)` → O(1) average vs. O(n) linear scan
- [ ] Load factor tracking: if any bucket chain length > 8, double the bucket count (re-hash into `pmm_alloc_contiguous` if > pool threshold)
- [ ] `RegQueryInfoKey` reports the new `MaxSubKeyNameLen` correctly after the hash expansion

- [ ] `hive_open_mmap(path)` -- map the hive file directly into kernel address space via `NtMapViewOfSection` (→ XREF `TODO-05-object-manager.md §7`); keys/values reference offsets into the mapped region instead of the static pool
- [ ] Zero-copy reads: `RegQueryValueEx` for a mapped hive reads directly from the mapped page; no `memcpy` for read-only queries
- [ ] Dirty page tracking: maintain `dirty_bitmap` (§8) over the mapped region; on flush, only `msync` dirty pages to disk
- [ ] Prerequisite: `NtMapViewOfSection` must be stable (→ XREF `TODO-05-object-manager.md §7`); implement after that TODO

- [ ] Redesign the on-disk hive format to use variable-size "cells" in a B-tree structure matching NT hive internals (`regf` format):
  - `HIVE_BLOCK` header (`regf` signature, sequence numbers, checksum)
  - Allocated cells: `NK_RECORD` (named key), `VK_RECORD` (value key), `SK_RECORD` (security key), `LF_RECORD` / `LH_RECORD` (sub-key lists)
  - Free cells: linked list of free space blocks
- [ ] Benefit: random-access reads without loading the entire hive into RAM; a 20 MiB hive can be queried for a single value by reading only 3--4 disk sectors (header + NK record + VK record)
- [ ] Compatibility: the B-tree format is compatible with Windows NT hive internals; external `chntpw` and `python-registry` tools can read it
- [ ] Prerequisite: §10 (mmap) and §8 (compaction) should be stable first

- [ ] Commit: `"kernel/registry: hash map child lookup, mmap hive, B-tree cell format"`

**Test checkpoint:** Hash map: create 100 children under one key → `reg_child_lookup` finds each by name. Load factor: bucket chain > 8 triggers rehash. Mmap hive: `hive_open_mmap` → `RegQueryValueEx` reads directly from mapped page (zero-copy). B-tree: write hive in `regf` format → external `python-registry` tool parses it. Serial log: `"[REG] Hash map: bucket_count=%u max_chain=%u"`. Test on: QEMU WHPX + TCG.

---

## 11. KTM Transaction Syscalls Wired to SSDT

Register the full Kernel Transaction Manager (KTM) syscall surface in the SSDT for transactional registry, file, and resource management. (→ XREF `TODO-12-native-api-ssdt.md §14, §15`)

- [ ] Implement KTM Transaction Manager syscalls: `NtCreateTransactionManager` (0x01A0), `NtOpenTransactionManager` (0x01A1)
- [ ] Implement Transaction syscalls: `NtCreateTransaction` (0x01A2), `NtOpenTransaction` (0x01A3), `NtCommitTransaction` (0x01A4), `NtRollbackTransaction` (0x01A5), `NtQueryInformationTransaction` (0x01A6), `NtSetInformationTransaction` (0x01A7)
- [ ] Implement Resource Manager syscalls: `NtCreateResourceManager` (0x01A8), `NtOpenResourceManager` (0x01A9), `NtQueryInformationResourceManager` (0x01AA), `NtSetInformationResourceManager` (0x01AB)
- [ ] Implement Enlistment syscalls: `NtCreateEnlistment` (0x01AC), `NtOpenEnlistment` (0x01AD), `NtQueryInformationEnlistment` (0x01AE), `NtSetInformationEnlistment` (0x01AF), `NtPrepareEnlistment` (0x01B0), `NtPrePrepareEnlistment` (0x01B1), `NtCommitEnlistment` (0x01B2), `NtRollbackEnlistment` (0x01B3)
- [ ] Implement Recovery syscalls: `NtRecoverTransactionManager` (0x01B4), `NtRecoverResourceManager` (0x01B5), `NtRecoverEnlistment` (0x01B6)
- [ ] Implement misc KTM: `NtPropagationComplete` (0x01B7), `NtPropagationFailed` (0x01B8), `NtFreezeTransactions` (0x01B9), `NtThawTransactions` (0x01BA)
- [ ] Implement Registry Transaction syscalls: `NtCreateRegistryTransaction` (0x01BB), `NtOpenRegistryTransaction` (0x01BC), `NtCommitRegistryTransaction` (0x01BD), `NtRollbackRegistryTransaction` (0x01BE)
- [ ] All functions return `NTSTATUS`
- [ ] Commit: `"kernel/registry: wire KTM transaction syscalls to SSDT (0x01A0--0x01BE)"`

**Test checkpoint:** `NtCreateTransactionManager` + `NtCreateTransaction` + `NtCommitTransaction` round-trip succeeds. `NtRollbackTransaction` reverts changes. `NtCreateRegistryTransaction` + `NtCommitRegistryTransaction` atomically applies registry mutations. Test on: QEMU WHPX, QEMU TCG, VirtualBox, and bare metal.

---

## 12. Registry Symlink Completion

Basic `REG_LINK` type, `REG_FLAG_LINK` flag, and transparent symlink resolution in `reg_resolve_path()` are already implemented. This section completes the symlink surface: creation via NT API, link-open semantics, built-in symlinks, and loop detection.

- [ ] `REG_OPTION_CREATE_LINK (0x02)` -- when passed as `CreateOptions` to `NtCreateKey` / `RegCreateKeyEx`, the created key is marked as a symbolic link (`REG_FLAG_LINK`); the caller must immediately set the default (unnamed) value to a `REG_LINK` type string containing the target path; if no `REG_LINK` value is set before the key is used, path resolution treats it as a dead link and returns `STATUS_OBJECT_NAME_NOT_FOUND`
- [ ] `REG_OPTION_OPEN_LINK (0x08)` -- when passed as `OpenOptions` to `NtOpenKeyEx`, open the link key itself rather than following the symlink; without this flag, `NtOpenKey` transparently follows the link (existing behavior); `RegOpenKeyEx` does NOT support this flag -- it always follows links (Win32 contract)
- [ ] Symlink loop detection: `reg_resolve_path()` already follows `REG_LINK` transparently; add a `max_link_depth = 32` counter; if exceeded, return `STATUS_REPARSE_POINT_NOT_RESOLVED`; prevents infinite loops from circular symlinks (`A → B → A`)
- [ ] Built-in symlinks created during `registry_populate_defaults()`:
  - `\Registry\Machine\System\CurrentControlSet` → `\Registry\Machine\System\ControlSet001` (selected by `Select` / `Current` value)
  - `\Registry\Machine\Software\Classes` → `\Registry\Machine\Software\Classes` (physical; HKCR overlay is handled by §5 merge logic, not a symlink)
- [ ] `regedit` shell tool (§7): `regedit list` output marks symlink keys with `→ <target>` suffix; `regedit tree` follows symlinks but marks them with `[LINK]`
- [ ] Commit: `"kernel/registry: REG_OPTION_CREATE_LINK, REG_OPTION_OPEN_LINK, symlink loop detection"`

**Test checkpoint:** Create a symlink key `HKLM\Test\Link` → `HKLM\Test\Target` via `NtCreateKey(REG_OPTION_CREATE_LINK)` + `NtSetValueKey(REG_LINK)`. `NtOpenKey("HKLM\\Test\\Link")` transparently returns target key. `NtOpenKeyEx("HKLM\\Test\\Link", REG_OPTION_OPEN_LINK)` returns the link key itself. Create circular symlink `A → B → A`; `NtOpenKey("A")` returns `STATUS_REPARSE_POINT_NOT_RESOLVED` (not infinite loop). `CurrentControlSet` symlink resolves to `ControlSet001`. Serial log: `"[REG] Symlink resolve: %s -> %s (depth=%u)"`. Test on: QEMU WHPX + TCG.

---

## 13. Schema-Validated Registry Keys ⭐

> [!TIP]
> Neither Windows 11 nor Linux provides built-in type-schema enforcement for their configuration stores. Win11 registry accepts any type for any value -- a `REG_DWORD` can be overwritten with `REG_SZ` without warning. Linux dconf has external XML schema files validated by GSettings, but the dconf binary store itself enforces nothing. Impossible OS embeds optional per-key type schemas directly in the registry, catching mistyped writes at the kernel level before they corrupt configuration state.

- [ ] Define schema storage convention: `HKLM\SYSTEM\Schema\<relative_key_path>` contains metadata values describing the schema for `HKLM\<relative_key_path>`:
  - `_AllowedTypes` (REG_MULTI_SZ): list of allowed `REG_*` type names, e.g., `"REG_DWORD\0REG_QWORD\0"`
  - `_MinValue` (REG_QWORD): minimum value for `REG_DWORD` / `REG_QWORD` writes
  - `_MaxValue` (REG_QWORD): maximum value for `REG_DWORD` / `REG_QWORD` writes
  - `_MaxDataSize` (REG_DWORD): maximum data size in bytes for `REG_BINARY` / `REG_SZ` / `REG_MULTI_SZ`
  - `_Required` (REG_MULTI_SZ): list of value names that must exist (checked on key creation / deletion)
  - `_ReadOnly` (REG_DWORD): if 1, all values under this key are immutable after first write (boot-time lock)
- [ ] `reg_validate_schema(key_path, value_name, type, data, data_size)` → `NTSTATUS`: called from `RegSetValueEx` / `NtSetValueKey` before mutation; returns `STATUS_OBJECT_TYPE_MISMATCH` for type violations, `STATUS_INTEGER_OVERFLOW` for range violations, `STATUS_BUFFER_OVERFLOW` for size violations
- [ ] Schema lookup: `reg_find_schema(key_path)` → looks up `HKLM\SYSTEM\Schema\<key_path>`; result is cached in `reg_key_t.cached_schema` pointer (invalidated on schema key change via §3 watcher); if no schema key exists, validation is skipped (opt-in, not opt-out)
- [ ] Pre-populate schemas for critical system keys:
  - `HKLM\SYSTEM\Display`: `Width`/`Height` = `REG_DWORD`, range `[640, 7680]`; `Depth` = `REG_DWORD`, range `[8, 32]`
  - `HKLM\SYSTEM\Registry\*`: `AuditEnabled`/`TraceEnabled`/`QuotaEnabled` = `REG_DWORD`, range `[0, 1]`
- [ ] `regedit schema <path>` -- show the schema for a key (types, ranges, required values); print `(no schema)` if none defined
- [ ] Commit: `"kernel/registry: schema-validated keys -- type, range, and size enforcement"`

**Test checkpoint:** Set schema on `HKLM\Test\SchemaKey`: `_AllowedTypes = "REG_DWORD"`, `_MinValue = 0`, `_MaxValue = 100`. `RegSetValueEx("Value", REG_DWORD, 50)` → succeeds. `RegSetValueEx("Value", REG_DWORD, 200)` → `STATUS_INTEGER_OVERFLOW`. `RegSetValueEx("Value", REG_SZ, "hello")` → `STATUS_OBJECT_TYPE_MISMATCH`. Key with no schema → all writes succeed (opt-in). Serial log: `"[REG] Schema reject: %s type=%u expected=%s"`. Test on: QEMU WHPX + TCG.

---

## 14. Registry SMP Synchronization

> [!NOTE]
> The registry engine (`registry.c`) was designed as a single-threaded subsystem used only during boot. TODO-12 §14 exposed it to concurrent SSDT dispatch from user-mode tasks, making three unsynchronized shared resources vulnerable to SMP races: pool allocators, handle pool, and tree structure. This section adds a registry-wide rwlock so read-heavy workloads (enumerate, query) can run concurrently while mutations (create, delete, set value) serialize safely.

- [ ] Declare `static spinlock_t reg_lock` in `registry.c`; initialize in `registry_init()`
- [ ] Wrap all `RegCreateKeyEx`, `RegOpenKeyEx`, `RegCloseKey`, `RegDeleteKey`, `RegDeleteTree`, `RegDeleteKeyDirect` entry points with `spin_lock(&reg_lock)` / `spin_unlock(&reg_lock)` (write path)
- [ ] Wrap all `RegSetValueEx`, `RegDeleteValue` entry points with write lock
- [ ] Wrap all `RegQueryValueEx`, `RegGetValue`, `RegEnumKeyEx`, `RegEnumValue`, `RegQueryInfoKey` entry points with read lock (or shared spinlock if available; single spinlock is acceptable for v1)
- [ ] Protect pool allocators: `reg_alloc_key()`, `reg_alloc_value()`, `reg_alloc_handle()`, `reg_free_handle()` must hold the lock when called (verify callers already hold it, or acquire internally)
- [ ] Protect `hive_save()` / `hive_load()` / `registry_flush()` / `registry_save_all()` / `registry_load_hives()` with the lock (these walk the entire tree)
- [ ] SMP stress test: concurrent `NtCreateKey` + `NtDeleteKey` + `NtEnumerateKey` from multiple tasks; verify no pool corruption, no stale pointer dereference, no duplicate handle allocation
- [ ] Upgrade to rwlock if profiling shows read contention (deferred; spinlock is correct first step for a 512-key pool)
- [ ] Commit: `"kernel/registry: SMP-safe registry with spinlock around all pool and tree operations"`

**Test checkpoint:** Two tasks concurrently creating and deleting keys under `\Registry\Machine\Software\SmpTest` for 1000 iterations. No kernel fault, no duplicate handles, enumeration sees consistent child counts. `RegQueryInfoKey` returns correct `lpcSubKeys` under concurrent mutation. Serial log: `"[REG] SMP lock: %u contention events"` (informational). Test on: QEMU WHPX (2 vCPU).

---

## OS Comparison

| ⭐   | Feature                              | 🪟 Win11                     | 🐧 Linux                           | 🚀 Impossible OS               |
| --- | ------------------------------------ | --------------------------- | --------------------------------- | ----------------------------- |
| 💎   | Hierarchical typed key/value store   | ✅ Full                      | ⚠️ dconf (GNOME), ini files       | ✅ Done -- `reg_key_t` tree    |
| 💎   | Win32 `RegXxx` API                   | ✅ Native                    | ❌ N/A                             | ✅ Done -- complete native API |
| 💎   | Persistent hive + crash-safe WAJ     | ✅ `.LOG1`/`.LOG2`           | ⚠️ dconf binary db, no WAJ        | ✅ Done -- `.hive.log` WAJ     |
| 💎   | Key `LastWriteTime` (FILETIME)       | ✅ Every key                 | ❌ N/A                             | ⚠️ Ticks done; FILETIME ⬜ §1  |
| 💎   | KEY_* access rights enforcement      | ✅ Full                      | ❌ N/A                             | ⬜ §1                          |
| 💎   | Registry symlinks (REG_LINK)         | ✅ CurrentControlSet, etc.   | ❌ N/A                             | ⚠️ Resolution done; API ⬜ §12 |
| 💎   | Change notifications                 | ✅ `RegNotifyChangeKeyValue` | ⚠️ inotify (file-level)           | ⬜ §3                          |
| 💎   | `REG_NOTIFY_THREAD_AGNOSTIC`         | ✅ Win8+                     | ❌ N/A                             | ⬜ §3                          |
| 💎   | Full NT registry syscall surface     | ✅ 30+ syscalls              | ❌ No registry concept             | ⬜ §4                          |
| 💎   | `KEY_INFORMATION_CLASS` completeness | ✅ 9 info classes            | ❌ N/A                             | ⬜ §4                          |
| 💎   | advapi32.dll W variants + HKCR       | ✅ Full                      | ⚠️ Wine reimplements              | ⬜ §5                          |
| 💎   | Registry virtualization              | ✅ Vista+ VirtualStore       | ❌ N/A                             | ⬜ §6                          |
| 💎   | Virtualization control flags         | ✅ `DONT_VIRTUALIZE` etc.    | ❌ N/A                             | ⬜ §6                          |
| 💎   | `.reg` import/export                 | ✅ regedit.exe built-in      | ⚠️ Wine `regedit`                 | ⬜ §6 §7                       |
| 💎   | Dual-log WAJ failover                | ✅ `.LOG1`/`.LOG2`           | ❌ N/A                             | ⬜ §8                          |
| ⭐   | Incremental delta flush              | ❌ Full hive rewrite         | ❌ Full db rewrite                 | ⬜ §8                          |
| ⭐   | Change-detail payloads               | ❌ Signal only               | ❌ N/A                             | ⬜ §3                          |
| ⭐   | Priority-based notification dispatch | ❌ All watchers equal        | ❌ N/A                             | ⬜ §3                          |
| ⭐   | Atomic registry transactions         | ⚠️ KTM deprecated           | ⚠️ dconf change_set (no rollback) | ⬜ §9                          |
| ⭐   | Native pattern-search API            | ❌ Manual enumerate+match    | ❌ N/A                             | ⬜ §9                          |
| ⭐   | Snapshot & diff                      | ❌ Needs RegShot (3rd-party) | ❌ N/A                             | ⬜ §9                          |
| ⭐   | Hive integrity reporter              | ❌ No built-in               | ❌ N/A                             | ⬜ §8                          |
| ⭐   | Idle-time hive compaction            | ❌ No defrag                 | ❌ N/A                             | ⬜ §8                          |
| ⭐   | Per-process registry sandbox         | ❌ HKCU shared               | ❌ N/A                             | ⬜ §4                          |
| ⭐   | Built-in API call tracing            | ❌ Needs ProcMon/ETW         | ❌ N/A                             | ⬜ §5                          |
| ⭐   | Per-PID registry quota               | ❌ Global limit only         | ❌ N/A                             | ⬜ §9                          |
| ⭐   | Memory-mapped hive                   | ❌ Static pool               | ✅ dconf mmap reads                | ⬜ §10                         |
| ⭐   | B-tree cell format                   | ✅ `regf` format             | ❌ N/A                             | ⬜ §10                         |
| ⭐   | Schema-validated keys                | ❌ No type enforcement       | ⚠️ GSettings XML schemas          | ⬜ §13                         |
| 💎   | SMP-safe registry operations         | ✅ CmpLock pushlock          | ✅ dconf GVDB atomic               | ⬜ §14                         |
| 💎   | NtFreezeRegistry / NtThawRegistry    | ✅ VSS backup support        | ❌ N/A                             | ⬜ §4                          |
| 💎   | NtInitializeRegistry boot signal     | ✅ SMSS boot sequence        | ❌ N/A                             | ⬜ §4                          |

After §1--7 + §12 + §14, Impossible OS reaches full Windows 11 parity on every registry feature including access rights, FILETIME timestamps, symlink creation, complete NT syscall surface (30+ syscalls with all info classes), notifications, HKCR, virtualization with control flags, `.reg` I/O, the regedit tool, and SMP-safe concurrent access. Linux has no equivalent in-kernel typed store; it relies on user-space GNOME dconf or scattered ini files.
Sections §8--10 + §13 deliver exclusive features that exceed Windows 11: incremental delta flush saves I/O on large hives, notification coalescing and change-detail payloads eliminate the need to re-query after a change, atomic transactions fill a gap Windows deprecated, snapshot/diff and search API replace third-party RegShot, hive compaction and mmap make the registry faster, and schema-validated keys provide type enforcement that neither Windows nor Linux offers natively.

> [!NOTE]
> **Deferred features:** Layered keys / hive stacking (Win10+ overlay hives for containerization) are deferred -- they require container/namespace isolation infrastructure not yet in scope. `NtLockProductActivationKeys` is Windows licensing-specific and not applicable.

---

## Unit Tests

> Wire into `test_runner_init()` via `test_register_registry_ext()`. Runs when `test=1` in boot.conf.
> Existing `src/kernel/test/test_registry.c` covers basic REG_DWORD/REG_SZ. This extends it.

- [ ] Create `src/kernel/test/test_registry_ext.c` with:
  - `RegCreateKeyEx` + `RegCloseKey` round-trip: key exists after create
  - `RegSetValueEx(REG_DWORD)` + `RegQueryValueEx` → same value back
  - `RegSetValueEx(REG_SZ)` + `RegQueryValueEx` → same string back
  - `RegSetValueEx(REG_MULTI_SZ)` → multiple NUL-separated strings preserved
  - `RegSetValueEx(REG_BINARY)` → raw bytes preserved
  - `RegDeleteValue` → subsequent query returns `ERROR_FILE_NOT_FOUND`
  - `RegDeleteKey` → subsequent open returns `ERROR_FILE_NOT_FOUND`
  - `RegEnumKeyEx` → enumerates subkeys in creation order
  - `RegEnumValue` → enumerates values in creation order
  - Hive flush: `RegFlushKey` → re-read from disk matches in-memory state
  - Key path depth: 10 nested subkeys → all accessible
  - Value size limit: write 4096-byte REG_BINARY → succeeds; write 65536 → fails gracefully
  - `LastWriteTime`: set value → `RegQueryInfoKey` returns non-zero `ftLastWriteTime`; set again → timestamp increases
  - Symlink: create link key via `REG_OPTION_CREATE_LINK` → `NtOpenKey` follows link transparently; `NtOpenKeyEx(REG_OPTION_OPEN_LINK)` returns link key itself
  - Symlink loop: circular `A → B → A` → open returns error, not infinite loop
  - `NtQueryMultipleValueKey`: query 3 values in one call → all returned correctly
  - `NtNotifyChangeMultipleKeys`: watch 2 keys → notification fires when either is modified
  - Virtualization control: set `REG_KEY_DONT_VIRTUALIZE` → low-IL write returns `STATUS_ACCESS_DENIED` instead of redirect
  - Schema validation: set schema on key → write with wrong type fails; write with correct type succeeds; key without schema accepts all types
- [ ] Register in `test_runner_init()`: `test_register_registry_ext()`
- [ ] Commit: `"test: add extended registry test suite"`

**Test checkpoint:** `bash scripts/test.sh SUITE=abi` shows all new `test_registry_ext` cases PASS; test body obeys CLAUDE.md (no `boot_progress`, `panic`, live `serial_init`, etc.).

---

## Verification

- [ ] **Access rights**: `RegOpenKeyEx(HKLM\SOFTWARE, KEY_SET_VALUE)` from a `Medium` IL process against a key with `Admins-only` DACL → returns `ERROR_ACCESS_DENIED`; `RegOpenKeyEx` as SYSTEM → returns `ERROR_SUCCESS`.
- [ ] **API limits**: `RegCreateKeyEx` with a 256-char key name → returns `ERROR_INVALID_PARAMETER`; with a 255-char name → succeeds.
- [ ] **Notifications**: register watcher on `HKLM\SYSTEM\Display`; call `RegSetValueEx("Width", ...)` from another thread; verify callback fires within 1 ms with correct `key_path`, `change_type == REG_NOTIFY_CHANGE_LAST_SET`, `value_name = "Width"`, and correct old/new values.
- [ ] **NtOpenKey syscall**: user-mode process calls `NtOpenKey` with `\Registry\Machine\SYSTEM\Display`; reads `Width` via `NtQueryValueKey`; value matches `HKLM\SYSTEM\Display\Width` in kernel.
- [ ] **.reg round-trip**: `regedit export HKLM\SYSTEM\Display /tmp/test.reg`; delete key; `regedit import /tmp/test.reg`; verify all values restored byte-for-byte.
- [ ] **Dual-log WAJ**: corrupt `SYSTEM.hive.log1` mid-write simulation; `registry_init` mounts from `log2`; all data intact.
- [ ] **Delta flush timing**: modify one value in a 1 MiB hive; measure `hive_flush_incremental` write size -- must be exactly 4 KiB (one dirty page), not 1 MiB.
- [ ] **Symlinks**: `HKLM\SYSTEM\CurrentControlSet` resolves to `ControlSet001`; `NtOpenKey("...\\CurrentControlSet\\Control")` returns same key as `NtOpenKey("...\\ControlSet001\\Control")`. Create and follow custom symlink via `NtCreateKey(REG_OPTION_CREATE_LINK)` + `NtSetValueKey(REG_LINK)`.
- [ ] **Schema enforcement**: set schema `_AllowedTypes = "REG_DWORD"`, `_MaxValue = 1000` on a test key; write `REG_DWORD 500` → success; write `REG_DWORD 2000` → `STATUS_INTEGER_OVERFLOW`; write `REG_SZ "hello"` → `STATUS_OBJECT_TYPE_MISMATCH`.
- [ ] **Freeze/thaw**: `NtFreezeRegistry(5)` → all `NtSetValueKey` calls from another thread block; `NtThawRegistry()` → blocked writes complete.
- [ ] **NtQueryMultipleValueKey**: query 5 values in a single syscall; all values returned with correct types and data.
- [ ] **Remaining limits**: mmap hive (§10) requires → XREF `TODO-05-object-manager.md §7` (Section Object / NtMapViewOfSection) to be stable; B-tree cell format (§10) is a stretch-goal redesign of the on-disk format and should be prototyped in a separate branch first. Layered keys / hive stacking deferred until container infrastructure matures.
- [ ] Commit: `"kernel/registry: registry system complete -- access rights, notifications, NtXxx syscalls, advapi32, virtualization, regedit, dual-log WAJ, transactions, search API, snapshot/diff, B-tree hive"`

**Test checkpoint:** Every unchecked Verification bullet above passes on QEMU WHPX, QEMU TCG, VirtualBox, and bare metal. When `test_registry_ext` lands, `bash scripts/test.sh SUITE=abi` (or `make test-abi`) shows new cases PASS.

**Test runner:** `scripts\debug\kernel\run-abi-tests.bat` (SUITE=abi)

---

## History

| Date | Action | Summary |
| --- | --- | --- |
| 2026-04-10 | validate | validate-todo-file: removed 52 `### N.M` subheadings (flat `##` checklists); `§M.N` refs folded to `§M`; ASCII `--` for ranges; Impl row 1 `TODO-05 §2,§3`; Inputs XREF aligned; collapsed excess blank lines; OS Comparison re-padded + `§6 §7` cell; Verification test checkpoint + `run-abi-tests.bat`; History added. Parity: OS rows map to §1--§13; external deps TODO-12 §14 / TODO-26 / TODO-17 flagged as blockers for §5/§4. |
| 2026-04-10 | gap-analysis | Web research: Learn `RegNotifyChangeKeyValue` (filters + `REG_NOTIFY_THREAD_AGNOSTIC`); Suhanov regf spec (dual `.LOG1`/`.LOG2`, HvLE log, thaw GUID fields); web search on `NtFreezeRegistry` (undocumented, VSS-adjacent). Code-truth: no `RegFlushKey`/`RegCopyTree`/`ticks_to_filetime` in `registry.c`; `reg_lookup_env_var`/`reg_expand_sz` present; `ProbeForRead`/`ProbeForWrite` in `ssdt.c`. Inputs: line count 2536, TODO-31 XREF, `→` for TODO-01; lean-structure NOTE; back-XREF TODO-32 Inputs. No new `##` sections; OS table unchanged. |
| 2026-04-10 | validate | validate-todo-file: joined Goal + CAUTION wraps; IMPORTANT callout tightened (no blank `>`); §11 XREF style + test platforms; sandbox deferred XREF `TODO-21-process-model-extensions.md`; Inputs/registry.h + `run-abi-tests.bat` exist; all external `→ XREF` targets and `## N.` anchors verified; TODO-17 Inputs `§1.3` corrected to `§5`. Flags: Implementation Order still blocked on TODO-26 §3--§5 / TODO-17 §4 / TODO-12 §14; §5 + §13 remain over lean item count (NOTE present). |
| 2026-04-10 | validate | Inputs: `→ XREF` `TODO-05-boot-device-discovery.md §9` for `HKLM\SYSTEM\Boot\Device\*` boot provenance (bidirectional with boot-platform TODO-32 validate pass). |
