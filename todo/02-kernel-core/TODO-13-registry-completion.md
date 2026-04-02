# TODO-13 — Registry System Completion

> **Goal:** The core registry engine (`reg_key_t`, Win32 API §2.1–2.4, hive
> persistence, crash-safe WAJ journaling) is fully implemented in `src/kernel/registry.c` (2 529 lines). This TODO delivers everything that is still pending: access rights enforcement, advanced key operations, change notifications, Nt/Zw user-mode syscalls, the `advapi32.dll` compatibility layer, registry virtualization, a `regedit` shell tool, advanced hive features (dual-log WAJ, delta flush, compaction), and the exclusive stretch features (atomic transactions, search API, snapshot diff, per-PID quota).
> When complete, Impossible OS has a native Windows-compatible registry that exceeds both Windows 11 and Linux's configuration store in every dimension.

> [!IMPORTANT]
> **Current state — what is already done:**
> - `reg_key_t` / `reg_value_t` structs, static pool allocator, FNV-1a hash
> - `RegOpenKeyEx`, `RegCreateKeyEx`, `RegCloseKey`, `RegDeleteKey/Tree`
> - `RegSetValueEx`, `RegQueryValueEx`, `RegGetValue`, `RegDeleteValue`
> - `RegEnumKeyEx`, `RegEnumValue`, `RegQueryInfoKey`
> - One-shot helpers: `RegGetDword`, `RegSetString`, `RegReadKeyValue`
> - Hive file format, disk layout, crash-safe `.hive.log` WAJ journaling
> - All root keys (`HKLM`, `HKCU`, `HKCR`, `HKU`, `HKCC`)
>
> **What is NOT done** (scope of this TODO): access rights enforcement, API
> limits, `RegFlushKey`, `RegCopyTree`, `RegRenameKey`, `RegSaveKey`, `RegRestoreKey`, `REG_OPTION_VOLATILE`, delayed close cache, change notifications, all Nt/Zw syscalls, `advapi32.dll` stubs, UTF-16 A/W variants, HKCR merged view, registry virtualization, `.reg` import/export, `regedit` command, dual-log WAJ, incremental delta flush, hive integrity reporter, format versioning, compaction, transactions, search API, snapshot/diff, per-PID quota, and performance optimisations (mmap, B-tree).

> [!CAUTION]
> **Memory rule:** Use `pmm_alloc_contiguous()` for ALL buffers > 4 KiB
> (hive file read/write, large binary values). `kmalloc` is only for small structs ≤ 4 KiB. Violating this silently corrupts the 2 MiB kernel heap.

---

## Inputs

- `src/kernel/registry.c` — 2 529-line implementation (engine complete)
- `include/registry.h` — types, constants, API declarations
- → XREF: `TODO-05-native-api-layer.md §4` — SSDT: all `NtXxx` registry entry points are SSDT slots; §4 must exist before §4 of this TODO
- → XREF: `TODO-11-security-reference-monitor.md §3–§5` — `SECURITY_DESCRIPTOR` + `SeAccessCheck` are used to enforce `KEY_*` access rights on `RegOpenKeyEx` / `NtOpenKey`
- → XREF: `TODO-03-object-manager.md §3` — registry `HKEY` handles must eventually be registered in the per-process handle table for `DuplicateHandle` parity; deferred to §4 of this TODO as a note

---

## Outcome

- Every `RegXxx` call enforces `KEY_*` access-right bits against the key's DACL and rejects out-of-spec names/paths with correct `ERROR_*` codes.
- `RegNotifyChangeKeyValue` delivers asynchronous callbacks to watchers whenever a watched key's values or sub-key tree changes.
- User-mode processes access the registry via `NtOpenKey`/`NtSetValueKey` / `NtQueryValueKey` / `NtNotifyChangeKey` syscalls (SSDT-wired).
- `advapi32.dll` exports all standard `RegXxx` Win32 functions in both A and W variants; HKCR provides the merged HKCU+HKLM\Software\Classes view.
- `.reg` files can be imported and exported; registry virtualization redirects low-IL writes to `HKCU\Software\VirtualStore\`.
- `regedit list/query/set/delete/tree/export` shell subcommands are usable.
- Dual-log WAJ, incremental delta flush, and hive compaction improve crash safety and I/O efficiency beyond what Windows 11 provides.
- Atomic transactions, a pattern-search API, and snapshot/diff give Impossible OS exclusive registry capabilities.

---

## Implementation Order

| ⭐  | Order | Deliverable                                        | Depends On          | Status |
| --- | :---: | -------------------------------------------------- | ------------------- | :----: |
| 💎  |   1   | Access rights, API limits & RegFlushKey            | TODO-11 §3–§5       |  [ ]   |
| 💎  |   2   | Advanced key ops (copy, rename, save, volatile)    | 1                   |  [ ]   |
| 💎  |   3   | Change notifications (core + exclusive extras)     | 2                   |  [ ]   |
| 💎  |   4   | Nt/Zw registry syscalls & pointer validation       | 1, TODO-05 §4       |  [ ]   |
| 💎  |   5   | advapi32.dll compat (A/W, HKCR, error map)         | 4                   |  [ ]   |
| 💎  |   6   | Registry virtualization & .reg import/export       | 5                   |  [ ]   |
| 💎  |   7   | `regedit` shell tool                               | 4                   |  [ ]   |
| ⭐  |   8   | Advanced hive features (dual-log, delta, compact)  | 4                   |  [ ]   |
| ⭐  |   9   | Transactions, search API & snapshot/diff           | 2, 3, 8             |  [ ]   |
| ⭐  |  10   | Performance (mmap hive, B-tree cell format)        | 9                   |  [ ]   |

> 💎 = parity work — matches what Windows 11 and Linux already do.
> ⭐ = exclusive work — Impossible OS is superior or first.

---

## 1. Access Rights, API Limits & RegFlushKey `[Sonnet]`

### 1.1 KEY_* access rights enforcement

- [ ] Add `uint32_t access_mask` field to `reg_key_t` (stored at open time)
- [ ] `RegOpenKeyEx`: validate `samDesired` against key's `SECURITY_DESCRIPTOR` DACL via `SeAccessCheck` (→ XREF `TODO-11-security-reference-monitor.md §5`); store granted mask in returned HKEY; return `ERROR_ACCESS_DENIED` on failure
- [ ] `RegCreateKeyEx`: requires `KEY_CREATE_SUB_KEY` on parent
- [ ] `RegSetValueEx` / `RegDeleteValue`: requires `KEY_SET_VALUE` on key
- [ ] `RegQueryValueEx` / `RegEnumKeyEx` / `RegEnumValue`: requires `KEY_QUERY_VALUE` / `KEY_ENUMERATE_SUB_KEYS`
- [ ] `KEY_READ = KEY_QUERY_VALUE | KEY_ENUMERATE_SUB_KEYS | KEY_NOTIFY | STANDARD_RIGHTS_READ`; `KEY_WRITE = KEY_SET_VALUE | KEY_CREATE_SUB_KEY | STANDARD_RIGHTS_WRITE`; `KEY_ALL_ACCESS = 0xF003F` — all bits
- [ ] Default DACL on new keys: `(A;;KA;;;SY)(A;;KA;;;BA)(A;;KR;;;BU)` — System+Admins = all access, BuiltinUsers = read only

### 1.2 API limits enforcement

- [ ] Key name max: 255 chars (currently `REG_MAX_KEY_NAME = 255` ✅, but not enforced on `RegCreateKeyEx` input — add check)
- [ ] Value name max: **16 383** chars (increase `REG_MAX_VALUE_NAME` from
  255 → 16 383, backing store uses `pmm_alloc_contiguous` for names > 255)
- [ ] Value data max: **1 MiB** (increase `REG_MAX_VALUE_SIZE` from 512 →
  1 048 576; large values use `pmm_alloc_contiguous`)
- [ ] Key path depth max: 512 levels — `reg_path_depth(path)` count of `\\` separators; return `ERROR_INVALID_PARAMETER` if exceeded
- [ ] Total key count: soft warn at 90% of pool; hard limit returns `ERROR_OUTOFMEMORY`; pool size comment documents the limit

### 1.3 RegFlushKey

- [ ] `RegFlushKey(hKey)` — force immediate hive sync for the hive containing `hKey`; calls `hive_flush_sync(hive)` which writes dirty pages and the WAJ log without waiting for the 5-second lazy-writer timer
- [ ] Return `ERROR_SUCCESS` on flush; `ERROR_INVALID_HANDLE` for volatile keys (volatile keys have no hive backing — flush is a no-op, not an error)

### 1.4 Commit

- [ ] Commit: `"kernel/registry: KEY_* access rights enforcement, API limits, RegFlushKey"`

---

## 2. Advanced Key Operations `[Sonnet]`

### 2.1 RegCopyTree / RegRenameKey

- [ ] `RegCopyTree(hKeySrc, lpSubKey, hKeyDest)` — recursively copy all sub-keys and values from `hKeySrc\lpSubKey` into `hKeyDest`; preserves value types, data, and sub-key structure; uses existing `RegCreateKeyEx`
  + `RegSetValueEx` internally; requires `KEY_READ` on source and `KEY_WRITE` on destination
- [ ] `RegRenameKey(hKey, lpSubKeyName, lpNewKeyName)` — rename a sub-key in place: create new key, copy all values and children (recursive `RegCopyTree`), delete old key tree; atomic under `hKey->lock` spinlock; return `ERROR_ALREADY_EXISTS` if `lpNewKeyName` already exists

### 2.2 RegSaveKey / RegRestoreKey

- [ ] `RegSaveKey(hKey, lpFile, lpSecurityAttributes)` — serialise the sub-tree rooted at `hKey` to a standalone `.hive` file at `lpFile` (write a new hive header + all keys/values in the sub-tree; use the existing `hive_write_key` path); requires `SeBackupPrivilege` (→ XREF `TODO-11-security-reference-monitor.md §8`)
- [ ] `RegRestoreKey(hKey, lpFile, dwFlags)` — replace the sub-tree rooted at `hKey` with the contents of a `.hive` file; `REG_FORCE_RESTORE (0x8)` allows replacing in-use keys; requires `SeRestorePrivilege`

### 2.3 REG_OPTION_VOLATILE

- [ ] `RegCreateKeyEx` with `dwOptions = REG_OPTION_VOLATILE (0x1)`: set `REG_FLAG_VOLATILE` on the new `reg_key_t`; volatile keys are excluded from `hive_flush` and `hive_write_key`; they are destroyed on reboot
- [ ] `RegFlushKey` on a volatile key returns `ERROR_SUCCESS` (no-op, not an error)
- [ ] `RegQueryInfoKey` correctly reports `REG_OPTION_VOLATILE` in its `lpdwClass` output for volatile keys

### 2.4 Delayed close cache (KCB reuse)

- [ ] Add an LRU cache of 32 recently closed `reg_key_t*` pointers; on `RegCloseKey`: if the key still exists in the tree, move its handle to the LRU cache instead of immediately freeing the pool slot
- [ ] On `RegOpenKeyEx`: check the LRU cache first (O(1) name hash compare); if hit, promote the entry, bump refcount, return immediately — avoids tree walk for hot keys like `HKLM\SYSTEM\Display` that are opened and closed in a tight loop
- [ ] Cache entries are evicted on LRU overflow or when the underlying key is deleted; eviction releases the pool slot

### 2.5 Commit

- [ ] Commit: `"kernel/registry: RegCopyTree, RegRenameKey, RegSaveKey/RestoreKey, volatile keys, KCB cache"`

---

## 3. Change Notifications `[Sonnet]`

### 3.1 Watcher data structures

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

### 3.2 RegNotifyChangeKeyValue

- [ ] `RegNotifyChangeKeyValue(hKey, bWatchSubtree, dwNotifyFilter, hEvent, fAsynchronous)`:
  - Allocate `reg_watcher_t` slot from pool; if pool full → `ERROR_OUTOFMEMORY`
  - Set `key`, `filter = dwNotifyFilter`, `watch_subtree = bWatchSubtree`
  - If `fAsynchronous == FALSE`: store a semaphore in the watcher; caller will block on it after this call
  - If `fAsynchronous == TRUE` and `hEvent != NULL`: store the event handle; `SetEvent(hEvent)` will be called on each matching change
  - `reg_notify_register(watcher)` appends to the key's watcher list
  - Returns `ERROR_SUCCESS`; caller queries result via `WaitForSingleObject` on `hEvent` or the built-in semaphore

### 3.3 Notification dispatch from mutation paths

- [ ] `reg_dispatch_notify(key, change_type, value_name)` — called at the end of `RegSetValueEx`, `RegDeleteValue`, `RegCreateKeyEx`, `RegDeleteKey`, and `reg_set_security_descriptor`:
  1. Walk `key->watcher_list`; for each watcher where `(watcher->filter & change_type) != 0`:
     - Subtree check: if `!watcher->watch_subtree`, only fire if `key == watcher->key`; if `watcher->watch_subtree`, fire if `key` is a descendant of `watcher->key` (walk parent pointers)
     - **Coalescing** ⭐: if `watcher->coalesce_ms > 0` and `now_ms - watcher->last_fired_ms < coalesce_ms` → skip (dedup)
     - Set `watcher->last_fired_ms = now_ms`; increment `watcher->hit_count`
     - If async event: `SetEvent(watcher->hEvent)`
     - If sync: `semaphore_signal(watcher->sem)` to unblock the caller
     - If callback watcher (§3.5): call `watcher->callback(key_path, change_type, value_name, watcher->ctx)`
  2. After firing: if `!fAsynchronous` watcher is one-shot — mark `watcher->active = false` (Win32 contract: synchronous watchers fire once and must be re-registered)

### 3.4 Subtree watching & lifecycle

- [ ] Subtree walk: `reg_is_descendant(ancestor, key)` — follow `key->parent` chain up to the root; return true if `ancestor` is found
- [ ] On `RegCloseKey`: call `reg_notify_unregister_all(key)` — mark all watchers for this key `active = false`; wake any blocked callers with `ERROR_KEY_DELETED`
- [ ] On `RegDeleteKey`: before deletion, call `reg_dispatch_notify(key, REG_NOTIFY_CHANGE_NAME, NULL)` for all ancestor watchers watching the parent; then `reg_notify_unregister_all(key)`
- [ ] `RegUnregisterNotify(watcher_id)` (non-Win32 internal API) — mark watcher inactive; used by kernel subsystems that registered watchers directly (e.g., theme system watching `HKCU\...\Theme`)

### 3.5 Exclusive notification features ⭐

- [ ] **Change-detail payloads** ⭐ — extend `reg_notify_fn` signature to include `old_value` and `new_value` blobs when `change_type == REG_NOTIFY_CHANGE_LAST_SET`; store old value snapshot before mutation in `reg_dispatch_notify` and pass to callback; enables zero-parse diff for settings watchers
- [ ] **Telemetry** ⭐ — expose `HKLM\SYSTEM\Registry\WatcherStats\<key_path>` with `HitCount` (REG_QWORD) and `LastFiredMs` (REG_QWORD) auto-updated on each dispatch; survives reboots via hive flush
- [ ] **Priority-based dispatch** ⭐ — add `uint8_t priority` field (0=normal,
  1=high, 2=system) to `reg_watcher_t`; `reg_dispatch_notify` fires `priority=2` watchers first (in-order), then `priority=1`, then `priority=0`; system-priority watchers are those registered by `theme_init`, `display_init`, and `service_manager_init`

### 3.6 Commit

- [ ] Commit: `"kernel/registry: change notifications, subtree watching, coalescing, detail payloads"`

---

## 4. Nt/Zw Registry Syscalls `[Opus]`

### 4.1 Syscall entry points

- [ ] Add to SSDT (→ XREF `TODO-05-native-api-layer.md §4`):
  ```
  NtCreateKey(KeyHandle, DesiredAccess, ObjectAttributes, TitleIndex, Class, CreateOptions, Disposition)
  NtOpenKey(KeyHandle, DesiredAccess, ObjectAttributes)
  NtOpenKeyEx(KeyHandle, DesiredAccess, ObjectAttributes, OpenOptions)
  NtDeleteKey(KeyHandle)
  NtSetValueKey(KeyHandle, ValueName, TitleIndex, Type, Data, DataSize)
  NtQueryValueKey(KeyHandle, ValueName, KeyValueInfoClass, KeyValueInfo, Length, ResultLength)
  NtDeleteValueKey(KeyHandle, ValueName)
  NtEnumerateKey(KeyHandle, Index, KeyInfoClass, KeyInfo, Length, ResultLength)
  NtEnumerateValueKey(KeyHandle, Index, KeyValueInfoClass, KeyValueInfo, Length, ResultLength)
  NtQueryKey(KeyHandle, KeyInfoClass, KeyInfo, Length, ResultLength)
  NtFlushKey(KeyHandle)
  NtNotifyChangeKey(KeyHandle, Event, ApcRoutine, ApcContext, IoStatusBlock, CompletionFilter, WatchTree, Buffer, BufferSize, Asynchronous)
  NtSaveKey(KeyHandle, FileHandle)
  NtRestoreKey(KeyHandle, FileHandle, Flags)
  NtLoadKey(TargetKey, SourceFile)
  NtUnloadKey(TargetKey)
  ZwXxx aliases for each — kernel-mode bypass wrappers
  ```
- [ ] `ObjectAttributes` for key paths: `RootDirectory` handle + `ObjectName` (`UNICODE_STRING` for future UTF-16; for now accept UTF-8 `ANSI_STRING` wrapper); resolve absolute paths starting with `\Registry\Machine` → `HKLM`, `\Registry\User\{SID}` → `HKCU`

### 4.2 Pointer validation

- [ ] Wrap every user-mode pointer argument in `ProbeForRead(ptr, size, align)` / `ProbeForWrite(ptr, size, align)` (→ XREF `TODO-10-exception-dispatch-seh.md §10`) before any dereference; return `STATUS_ACCESS_VIOLATION` if probe faults
- [ ] `UNICODE_STRING` / `ANSI_STRING` struct fields: validate both the struct pointer AND the embedded `Buffer` pointer separately
- [ ] `KeyValueInfo` output buffer: `ProbeForWrite(KeyValueInfo, Length, 1)`; if probe succeeds but `Length` is too small to hold the result, return `STATUS_BUFFER_TOO_SMALL` with `*ResultLength` set to the required size

### 4.3 NtNotifyChangeKey

- [ ] `NtNotifyChangeKey`: validates `KeyHandle`, resolves to `reg_key_t`, calls `reg_notify_register()` from §3.2; if `Asynchronous == FALSE`, blocks calling thread on the watcher's semaphore (interruptible via APC)
- [ ] APC completion: if `ApcRoutine != NULL`, queue a user-mode APC to the calling thread when the notification fires (→ XREF `05-storage-filesystems/TODO-05-win32-file-io-api.md §9`)

### 4.4 Per-process sandbox ⭐

- [ ] `HKCU` redirect: `NtOpenKey` with a path under `\Registry\User` resolves to the *current process's* per-session user subtree rather than a global `HKCU`; each process inherits its user SID from its primary token (→ XREF `TODO-11-security-reference-monitor.md §7`); `\Registry\User\S-1-5-21-...-1001` is the actual physical path; `NtOpenKey` with `RootDirectory=HKCU` resolves via the task's token
- [ ] **Per-process sandbox** ⭐ — `NtSetInformationProcess(ProcessRegistrySandbox, root_path)`: future API that restricts all registry access for a process to a sub-tree; used by browser renderer and low-IL processes; deferred until process isolation (→ `TODO-09`) matures

### 4.5 Rate limiting & audit ⭐

- [ ] **Rate limiting** ⭐ — per-task `reg_ops_this_sec` counter reset every
  1 000 ms by scheduler tick; if > 10 000 registry ops per second: `schedule_yield()` and re-check; soft throttle prevents runaway registry hammering from buggy apps; counter tracked in `struct task`
- [ ] **Audit log** ⭐ — if `HKLM\SYSTEM\Registry\AuditEnabled = 1`: write a compact audit entry to `C:\Impossible\System\Logs\registry-audit.log` on each `NtSetValueKey` / `NtDeleteKey`: `{timestamp, pid, key_path, value_name, old_type, new_type, result_ntstatus}`; uses the existing `klog` ring buffer at LOG_AUDIT level; auto-rotated at 4 MiB

### 4.6 Commit

- [ ] Commit: `"kernel/registry: NtOpenKey/NtSetValueKey/NtNotifyChangeKey syscalls, pointer validation, audit"`

---

## 5. advapi32.dll Win32 Compatibility `[Sonnet]`

### 5.1 A/W variant shim

- [ ] All `RegXxx` functions that take string arguments have an `A` variant (UTF-8 / ANSI) and a `W` variant (UTF-16LE `WCHAR*`)
- [ ] `A` variants: call the internal UTF-8 kernel API directly (already exists)
- [ ] `W` variants: `wchar_to_utf8(src_w, buf, len)` → call internal UTF-8 API → convert any UTF-8 string results back to UTF-16LE via `utf8_to_wchar(src, buf, len)` before returning to caller
- [ ] `wchar_to_utf8` / `utf8_to_wchar`: implement in `src/libs/libc/wchar.c` (BMP-only initially; no surrogate pair support needed for registry paths); use existing `libc` string helpers

### 5.2 HKCR merged view

- [ ] `HKEY_CLASSES_ROOT` read path: `RegOpenKeyEx(HKCR, sub_key, ...)` → first look in `HKCU\Software\Classes\<sub_key>`; if not found, look in `HKLM\SOFTWARE\Classes\<sub_key>`; return whichever is found first
- [ ] `HKCR` write path: `RegCreateKeyEx(HKCR, sub_key, ...)` → always write to `HKCU\Software\Classes\<sub_key>` (per-user override)
- [ ] `RegEnumKeyEx(HKCR, ...)`: merge results from both HKCU\Software\Classes and HKLM\SOFTWARE\Classes, deduplicate by name, return union; HKCU entries shadow HKLM entries with same name

### 5.3 Error code mapping

- [ ] `reg_ntstatus_to_win32(NTSTATUS)` table: `STATUS_OBJECT_NAME_NOT_FOUND`
  → `ERROR_FILE_NOT_FOUND`, `STATUS_ACCESS_DENIED` → `ERROR_ACCESS_DENIED`, `STATUS_BUFFER_TOO_SMALL` → `ERROR_MORE_DATA`, `STATUS_NO_MORE_ENTRIES`
  → `ERROR_NO_MORE_ITEMS`, `STATUS_INSUFFICIENT_RESOURCES` → `ERROR_OUTOFMEMORY`, default → `ERROR_INVALID_FUNCTION`
- [ ] Win32 API wrappers call `SetLastError(reg_ntstatus_to_win32(status))` on failure before returning; `GetLastError()` returns the correct code

### 5.4 API call tracing ⭐

- [ ] **Tracing toggle** ⭐ — `HKLM\SYSTEM\Registry\TraceEnabled = 1` (default
  0) enables per-call tracing: each `RegXxx` call logs to the serial log at `LOG_TRACE` level: `[REG] RegSetValueEx HKLM\System\Display Width=1920 (pid=42)` with PID, key path, value name, type, and `NTSTATUS` result
- [ ] `reg_trace(func_name, hKey, value_name, type, status)` helper function; only evaluated when `reg_trace_enabled` global is 1 (set from `HKLM\SYSTEM\Registry\TraceEnabled` on `registry_init`)

### 5.5 Commit

- [ ] Commit: `"kernel/registry: advapi32 A/W shims, HKCR merged view, error mapping, API tracing"`

---

## 6. Registry Virtualization & .reg Import/Export `[Sonnet]`

### 6.1 Vista-style registry virtualization

- [ ] Low-IL and non-elevated processes that write to `HKLM\SOFTWARE\<path>` are silently redirected to `HKCU\Software\VirtualStore\MACHINE\SOFTWARE\<path>`
- [ ] Condition for redirect: `current_task()->Token->IntegrityLevelSid == SeILMedium` AND write is to `HKLM\SOFTWARE` subtree AND the key does not have `REG_FLAG_READONLY` AND the calling process is NOT running elevated (→ XREF `TODO-11-security-reference-monitor.md §6`)
- [ ] `NtCreateKey` / `NtSetValueKey`: check for virtualization condition before the write; if active, silently rewrite the path to VirtualStore and notify caller of success — the caller is unaware of the redirect
- [ ] Read path: `NtOpenKey` / `NtQueryValueKey` for virtualized paths: try VirtualStore first; fall back to real HKLM path

### 6.2 .reg file import

- [ ] `.reg` file format (v5.00, Windows 2000+):
  ```
  Windows Registry Editor Version 5.00

  [HKEY_LOCAL_MACHINE\SOFTWARE\Test]
  "StringValue"="hello"
  "DwordValue"=dword:0000002a
  "BinaryValue"=hex:01,02,03
  [-HKEY_LOCAL_MACHINE\SOFTWARE\DeletedKey]
  ```
- [ ] `reg_import(path)` — parse line-by-line:
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

### 6.3 .reg file export

- [ ] `reg_export(hKey, path)` — recursive export of a key sub-tree:
  - Write `Windows Registry Editor Version 5.00\r\n\r\n` header
  - For each key in DFS order: write `[HKXX\full\path]\r\n`
  - For each value: encode according to type (reverse of §6.2 parse rules)
  - For `REG_SZ` values: escape `\` → `\\`, `"` → `\"`
  - Recurse into sub-keys; write empty line between key sections
- [ ] `regedit export HKLM\SOFTWARE\Test output.reg` shell wrapper

### 6.4 Commit

- [ ] Commit: `"kernel/registry: virtualization redirect, .reg import/export"`

---

## 7. `regedit` Shell Tool `[Sonnet]`

### 7.1 Subcommands

- [ ] `regedit list <path>` — list immediate sub-key names and value names + types under `<path>`; format:
  ```
  HKLM\SYSTEM\Display
    Keys:   (none)
    Values: Width    REG_DWORD  1920
            Height   REG_DWORD  1080
            Depth    REG_DWORD  32
  ```
- [ ] `regedit query <path> <valueName>` — print a single value; format: `HKLM\SYSTEM\Display\Width = 1920 (REG_DWORD)`
- [ ] `regedit set <path> <valueName> <type> <data>` — write a value; `type` is one of `REG_SZ`, `REG_DWORD`, `REG_QWORD`, `REG_BINARY`, `REG_MULTI_SZ`; `data` is the string representation (hex for binary, decimal for DWORD/QWORD, comma-separated hex bytes for binary)
- [ ] `regedit delete <path> [<valueName>]` — without `valueName`: delete the key and all children (`RegDeleteTree`); with `valueName`: delete only the named value (`RegDeleteValue`)
- [ ] `regedit tree <path> [--depth N]` — recursive ASCII tree; default depth 4; `--depth 0` = unlimited
- [ ] `regedit export <path> <file>` — call `reg_export` (§6.3)
- [ ] `regedit import <file>` — call `reg_import` (§6.2)

### 7.2 Error handling

- [ ] Unknown path → print `Error: key not found: <path>`
- [ ] Access denied → print `Error: access denied (KEY_WRITE required)`
- [ ] Invalid type string → print `Error: unknown type '<type>'; valid: REG_SZ REG_DWORD REG_QWORD REG_BINARY REG_MULTI_SZ`

### 7.3 Commit

- [ ] Commit: `"shell: regedit list/query/set/delete/tree/export/import subcommands"`

---

## 8. Advanced Hive Features `[Opus]`

### 8.1 Dual-log WAJ (.log1 / .log2)

- [ ] Maintain two alternating journal files: `SYSTEM.hive.log1` and `SYSTEM.hive.log2`; current active log tracked in hive header `active_log` byte (0=log1, 1=log2)
- [ ] Write cycle: dirty pages + header written to active log → `fsync` → commit marker written → `fsync` → copy dirty pages into main hive file → clear journal → switch active log to the other file
- [ ] Recovery: on mount, check both log files for a valid commit marker; use the one with the higher sequence number; if both are valid but different, the newer one wins; if neither has a commit marker, hive is clean
- [ ] Advantage over single-log WAJ: if a crash occurs while clearing the journal, the other log still has the previous good state; matches Windows NT `.LOG1`/`.LOG2` behaviour exactly

### 8.2 Incremental delta flush ⭐

- [ ] Add `uint8_t dirty_bitmap[HIVE_MAX_PAGES / 8]` to the in-memory hive struct; each bit corresponds to one 4 KiB hive page; set on every mutation that touches a page
- [ ] `hive_flush_incremental()` — only write dirty pages to disk (seek+write each dirty 4 KiB page); clear bitmap after write; vs. the current `hive_flush()` which rewrites the entire hive from scratch
- [ ] Speedup: a single `RegSetValueEx` call on a 10 MiB hive flushes 4 KiB instead of 10 MiB; critical for fast boot (many small writes during init)
- [ ] Lazy writer timer: call `hive_flush_incremental` every 5 s from a kernel timer DPC (→ XREF `TODO-06-irql-model-dpcs.md §4`); also called from `RegFlushKey` and the shutdown path

### 8.3 Hive integrity reporter (`chkregistry`) ⭐

- [ ] `chkregistry <hive_path>` shell command:
  - Open hive file directly (not via the live registry)
  - Validate header CRC32; check `magic`, `version`, `key_count`, `value_count` fields
  - Walk all `reg_key_t` entries: verify parent pointers are valid, verify child-list linkage is consistent (no cycles, no dangling pointers)
  - For each `reg_value_t`: verify `type` is a known `REG_*` constant, verify `data_size ≤ REG_MAX_VALUE_SIZE`
  - Report: total keys, total values, errors found, estimated bytes wasted (deleted pool slots that could be compacted)
  - `--fix` flag: calls `hive_compact` (§8.4) on a copy and replaces the original if compact succeeds

### 8.4 Hive format versioning & compaction ⭐

- [ ] **Format versioning**: hive header `version` field (currently 1); `registry_init` reads version; if version > current code supports → log warning + mount read-only; if version < supported → auto-migrate (bump version, add any new header fields at the end of the header page)
- [ ] **Compaction** ⭐ — `hive_compact(hive)`:
  - Allocate new hive buffer
  - Walk all live (non-deleted) `reg_key_t` and `reg_value_t` entries in BFS order; pack them tightly into the new buffer
  - Write new hive atomically: write to `.hive.tmp` → fsync → rename over old file (atomic on IXFS/NTFS)
  - Triggered automatically when pool utilisation < 60% (many deletions have occurred) or by `chkregistry --fix`
  - Reclaims memory: a registry with 10 000 creations and 8 000 deletions compacts from 10 000 slots to 2 000 slots

### 8.5 Commit

- [ ] Commit: `"kernel/registry: dual-log WAJ, incremental delta flush, chkregistry, hive compaction"`

---

## 9. Transactions, Search API & Snapshot/Diff `[Opus]`

### 9.1 Registry transactions

- [ ] `RegBeginTransaction(hKey)` → returns `HREG_TXN` (transaction handle): creates a `reg_txn_t` struct with a copy-on-write journal — each mutation during the transaction writes to the journal instead of the live tree
- [ ] `RegCommitTransaction(hTxn)` — apply all journalled mutations to the live tree under `hKey->lock` spinlock; atomically visible to all readers after the spinlock is released; flush hive after commit
- [ ] `RegAbortTransaction(hTxn)` — discard the journal; live tree is unchanged; free `reg_txn_t` struct
- [ ] Conflict detection: if a key modified in the transaction has also been modified by another writer since `RegBeginTransaction`, return `STATUS_REGISTRY_TRANSACTION_CONFLICT` from `RegCommitTransaction`; caller must retry or abort
- [ ] `NtCreateTransaction` / `NtCommitTransaction` / `NtRollbackTransaction` SSDT wrappers; compatible with Win32 `RtlSetCurrentTransaction` contract

### 9.2 Key search API ⭐

- [ ] `RegFindKey(hRoot, lpPattern, dwFlags, phKey)` — glob-pattern search (`*` = any sequence, `?` = single char, case-insensitive) over key names in the sub-tree rooted at `hRoot`; returns the first match via `phKey`; caller calls `RegFindNextKey(search_handle, phKey)` to iterate
- [ ] `RegFindValue(hRoot, lpKeyPattern, lpValuePattern, dwType, phKey, lpValueName)` — search for a value matching both a key-name pattern and a value-name pattern; optionally filter by `dwType` (0 = any type)
- [ ] `reg_find_state_t` internal struct: holds the DFS traversal stack (current path, depth) so `RegFindNextKey` can resume where it left off without rescanning from the root

### 9.3 Registry snapshot & diff ⭐

- [ ] `RegTakeSnapshot(hRoot, phSnapshot)` — deep-copy the sub-tree rooted at `hRoot` into a new in-memory tree (not persisted to disk); returns an opaque `HREG_SNAPSHOT` handle
- [ ] `RegDiffSnapshots(hSnapshot1, hSnapshot2, callback, ctx)` — compare two snapshots (or a snapshot and the live tree via `NULL`):
  - For each key/value added: callback with `REG_DIFF_ADDED`
  - For each key/value deleted: callback with `REG_DIFF_DELETED`
  - For each value that changed type or data: callback with `REG_DIFF_MODIFIED` including old and new values
- [ ] `RegFreeSnapshot(hSnapshot)` — free the deep-copy tree
- [ ] `regedit diff <snapshot_file1> <snapshot_file2>` — call `RegTakeSnapshot` on two `.reg` exports and diff them; output in unified diff style with `+` / `-` / `~` prefixes

### 9.4 Orphan key GC & per-PID quota ⭐

- [ ] **Orphan GC** ⭐ — `reg_gc_orphans()`: walk all pool slots; find `reg_key_t` entries with `active=true` but no path from any root key reachable (parent pointer chain never reaches HKLM/HKCU/etc.); log each orphan and free its pool slot; run automatically on `registry_flush` if orphan count > 10
- [ ] **Per-PID quota** ⭐ — `HKLM\SYSTEM\Registry\QuotaEnabled = 1` (default 0): track per-task total bytes of registry data written since process start in `struct task`; if > `QuotaBytes` (default 50 MiB): `NtSetValueKey` returns `STATUS_QUOTA_EXCEEDED`; quota reset on process exit; enables per-app registry footprint limiting

### 9.5 Commit

- [ ] Commit: `"kernel/registry: transactions, search API, snapshot/diff, orphan GC, per-PID quota"`

---

## 10. Performance Optimisations `[Opus]`

### 10.1 Hash map child lookup

- [ ] Replace the linked-list `reg_key_t.children` with a fixed-size FNV-1a hash map; `REG_CHILD_BUCKETS = 16` (already defined in `registry.h`); each bucket is a pointer to a chain of children with the same hash bucket
- [ ] `reg_child_lookup(parent, name)` → O(1) average vs. O(n) linear scan
- [ ] Load factor tracking: if any bucket chain length > 8, double the bucket count (re-hash into `pmm_alloc_contiguous` if > pool threshold)
- [ ] `RegQueryInfoKey` reports the new `MaxSubKeyNameLen` correctly after the hash expansion

### 10.2 Memory-mapped hive files ⭐

- [ ] `hive_open_mmap(path)` — map the hive file directly into kernel address space via `NtMapViewOfSection` (→ XREF `TODO-03-object-manager.md §7`); keys/values reference offsets into the mapped region instead of the static pool
- [ ] Zero-copy reads: `RegQueryValueEx` for a mapped hive reads directly from the mapped page; no `memcpy` for read-only queries
- [ ] Dirty page tracking: maintain `dirty_bitmap` (§8.2) over the mapped region; on flush, only `msync` dirty pages to disk
- [ ] Prerequisite: `NtMapViewOfSection` must be stable (→ XREF `TODO-03-object-manager.md §7`); implement after that TODO

### 10.3 B-tree cell format ⭐

- [ ] Redesign the on-disk hive format to use variable-size "cells" in a B-tree structure matching NT hive internals (`regf` format):
  - `HIVE_BLOCK` header (`regf` signature, sequence numbers, checksum)
  - Allocated cells: `NK_RECORD` (named key), `VK_RECORD` (value key), `SK_RECORD` (security key), `LF_RECORD` / `LH_RECORD` (sub-key lists)
  - Free cells: linked list of free space blocks
- [ ] Benefit: random-access reads without loading the entire hive into RAM; a 20 MiB hive can be queried for a single value by reading only 3–4 disk sectors (header + NK record + VK record)
- [ ] Compatibility: the B-tree format is compatible with Windows NT hive internals; external `chntpw` and `python-registry` tools can read it
- [ ] Prerequisite: §10.2 (mmap) and §8.4 (compaction) should be stable first

### 10.4 Commit

- [ ] Commit: `"kernel/registry: hash map child lookup, mmap hive, B-tree cell format"`

---

## OS Comparison


| ⭐ | Feature                              | 🪟 Win11                          | 🐧 Linux                             | 🚀 Impossible OS                   |
|----|--------------------------------------|--------------------------------|-----------------------------------|---------------------------------|
| 💎 | Hierarchical typed key/value store   | ✅ Full                        | ⚠️ dconf (GNOME), ini files       | ✅ Done — `reg_key_t` tree, all |
| 💎 | Win32 `RegXxx` API                   | ✅ Native                      | ❌ Not available                  | ✅ Done — complete native API   |
| 💎 | Persistent hive + crash-safe WAJ     | ✅ `.LOG1`/`.LOG2`             | ⚠️ dconf binary db, no WAJ        | ✅ Done — `.hive.log` WAJ       |
| 💎 | KEY_* access rights enforcement      | ✅ Full                        | ❌ Not applicable                 | ⬜ §1                           |
| 💎 | Change notifications                 | ✅ `RegNotifyChangeKeyValue`   | ⚠️ inotify (file-level only)      | ⬜ §3                           |
| 💎 | NtXxx registry syscalls              | ✅ Native                      | ❌ No registry concept            | ⬜ §4                           |
| 💎 | advapi32.dll W variants + HKCR       | ✅ Full                        | ⚠️ Wine reimplements              | ⬜ §5                           |
| 💎 | Registry virtualization              | ✅ Vista+ VirtualStore         | ❌ Not applicable                 | ⬜ §6                           |
| 💎 | `.reg` import/export                 | ✅ regedit.exe built-in        | ⚠️ Wine `regedit`                 | ⬜ §6–§7                        |
| 💎 | Dual-log WAJ failover                | ✅ `.LOG1`/`.LOG2`             | ❌ Not available                  | ⬜ §8                           |
| ⭐ | Incremental delta flush              | ❌ Full hive rewrite           | ❌ Full db rewrite                | ⬜ §8 — .2 🚀                   |
| ⭐ | Change-detail payloads               | ❌ Signal only                 | ❌ Not available                  | ⬜ §3 — .5 🚀                   |
| ⭐ | Priority-based notification dispatch | ❌ All watchers equal          | ❌ Not available                  | ⬜ §3 — .5 🚀                   |
| ⭐ | Atomic registry transactions         | ❌ KTM deprecated              | ⚠️ dconf change_set (no rollback) | ⬜ §9 — .1 🚀                   |
| ⭐ | Native pattern-search API            | ❌ Manual enumerate+match      | ❌ Not available                  | ⬜ §9 — .2 🚀                   |
| ⭐ | Snapshot & diff                      | ❌ Needs RegShot (third-party) | ❌ Not available                  | ⬜ §9 — .3 🚀                   |
| ⭐ | Hive integrity reporter              | ❌ No built-in                 | ❌ Not applicable                 | ⬜ §8 — .3 🚀                   |
| ⭐ | Idle-time hive compaction            | ❌ No defragmentation          | ❌ Not applicable                 | ⬜ §8 — .4 🚀                   |
| ⭐ | Per-process registry sandbox         | ❌ HKCU shared                 | ❌ Not applicable                 | ⬜ §4 — .4 🚀                   |
| ⭐ | Built-in API call tracing            | ❌ Needs ProcMon/ETW           | ❌ Not applicable                 | ⬜ §5 — .4 🚀                   |
| ⭐ | Per-PID registry quota               | ❌ Global limit only           | ❌ Not available                  | ⬜ §9 — .4 🚀                   |
| ⭐ | Memory-mapped hive                   | ❌ Static pool                 | ❌ Not applicable                 | ⬜ §10 — .2 🚀                  |
| ⭐ | B-tree cell format                   | ✅ `regf` format               | ❌ Not applicable                 | ⬜ §10 — .3 🚀                  |

After §1–7, Impossible OS reaches full Windows 11 parity on every registry feature including access rights, notifications, syscalls, HKCR, virtualization,
`.reg` I/O, and the regedit tool. Linux has no equivalent in-kernel typed store — it relies on user-space GNOME dconf or scattered ini files.
Sections §8–10 deliver a set of exclusive features that exceed Windows 11: incremental delta flush saves I/O on large hives, notification coalescing and change-detail payloads eliminate the need to re-query after a change, atomic transactions fill a gap Windows deprecated, the snapshot/diff and search API match what previously required third-party tools like RegShot, and hive compaction and mmap together make the registry faster and more memory- efficient than any existing platform ships by default.

---

## Unit Tests

> Wire into `test_runner_init()` via `test_register_registry_ext()` (→ XREF: `00-infrastructure/TODO-03-kernel-test-framework.md`).
> Existing `test_registry.c` covers basic REG_DWORD/REG_SZ. This extends it.

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
- [ ] Register in `test_runner_init()`: `test_register_registry_ext()`
- [ ] Commit: `"test: add extended registry test suite"`

---

## Verification

- [ ] **Access rights**: `RegOpenKeyEx(HKLM\SOFTWARE, KEY_SET_VALUE)` from a `Medium` IL process against a key with `Admins-only` DACL → returns `ERROR_ACCESS_DENIED`; `RegOpenKeyEx` as SYSTEM → returns `ERROR_SUCCESS`.
- [ ] **API limits**: `RegCreateKeyEx` with a 256-char key name → returns `ERROR_INVALID_PARAMETER`; with a 255-char name → succeeds.
- [ ] **Notifications**: register watcher on `HKLM\SYSTEM\Display`; call `RegSetValueEx("Width", ...)` from another thread; verify callback fires within 1 ms with correct `key_path`, `change_type == REG_NOTIFY_CHANGE_LAST_SET`, `value_name = "Width"`, and correct old/new values.
- [ ] **NtOpenKey syscall**: user-mode process calls `NtOpenKey` with `\Registry\Machine\SYSTEM\Display`; reads `Width` via `NtQueryValueKey`; value matches `HKLM\SYSTEM\Display\Width` in kernel.
- [ ] **.reg round-trip**: `regedit export HKLM\SYSTEM\Display /tmp/test.reg`; delete key; `regedit import /tmp/test.reg`; verify all values restored byte-for-byte.
- [ ] **Dual-log WAJ**: corrupt `SYSTEM.hive.log1` mid-write simulation; `registry_init` mounts from `log2`; all data intact.
- [ ] **Delta flush timing**: modify one value in a 1 MiB hive; measure `hive_flush_incremental` write size — must be exactly 4 KiB (one dirty page), not 1 MiB.
- [ ] **Remaining limits**: mmap hive (§10.2) requires → XREF `TODO-03-object-manager.md §7` (Section Object / NtMapViewOfSection) to be stable; B-tree cell format (§10.3) is a stretch-goal redesign of the on-disk format and should be prototyped in a separate branch first.
- [ ] Commit: `"kernel/registry: registry system complete — access rights, notifications, NtXxx syscalls, advapi32, virtualization, regedit, dual-log WAJ, transactions, search API, snapshot/diff, B-tree hive"`
