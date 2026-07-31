---
schema_version: 1
id: registry-completion
domain: 02-kernel-core
status: active
title: "TODO-14 -- Registry System Completion"
---

# TODO-14 -- Registry System Completion

> **Validated:** 2026-07-04 | validate-todo-file clean (structure / IO table / XREF / test wiring)
> **Gap-audited:** 2026-07-04 | gap-audit + codex-gap-audit; 3 findings filed (transacted + hive-op syscalls §4, advapi32 export surface §5, layered-key enum carveout §4/OS row)

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
> **What is NOT done** (scope of this TODO): `RegSaveKey`/`RegRestoreKey` bodies (privilege-gated, fail-closed until TODO-15 §8), change notifications, all Nt/Zw syscalls, `advapi32.dll` stubs, UTF-16 A/W variants, HKCR merged view, registry virtualization, `.reg` import/export, `regedit` command, dual-log WAJ, incremental delta flush, hive integrity reporter, format versioning, compaction, transactions, search API, snapshot/diff, per-PID quota, and performance optimisations (mmap, B-tree). (§1 access rights + API limits + `RegFlushKey`; §2 `RegCopyTree`/`RegRenameKey`/volatile/KCB shipped.)

> [!CAUTION]
> **Memory rule:** Use `pmm_alloc_contiguous()` for ALL buffers > 4 KiB (hive file read/write, large binary values). `kmalloc` is only for small structs ≤ 4 KiB. Violating this silently corrupts the 2 MiB kernel heap.

---

## Inputs

- `src/kernel/registry.c` -- 2 536-line implementation (engine complete)
- `include/registry.h` -- types, constants, API declarations
- → XREF: `TODO-12-native-api-ssdt.md §14, §15` -- SSDT: core registry entry points in §14 (0x0090--0x009B), advanced (flush/notify/save/hive) in §15 (0x009C--0x00A6 + extended range); §14 must exist before §5 of this TODO
- → XREF: `TODO-08-time-filetime-management.md §1` -- `ticks_to_filetime()` conversion needed by §1 for `LastWriteTime` FILETIME output (TODO-08 §1 is [x] done)
- → XREF: `TODO-15-security-reference-monitor.md §3,§5` -- `SECURITY_DESCRIPTOR` (§3) + `SeAccessCheck` (§5) are used to enforce `KEY_*` access rights on `RegOpenKeyEx` / `NtOpenKey`
- → XREF: `TODO-05-object-manager.md §3` -- registry `HKEY` handles must eventually be registered in the per-process handle table for `DuplicateHandle` parity; deferred to §3 of this TODO as a note
- → XREF: `TODO-22-environment-variables.md §2` -- system env vars from `Session Manager\Environment` once `NtEnumerateValueKey` / `NtQueryValueKey` (this file §4) are wired; boot uses `reg_expand_sz` today
- → XREF: `01-boot-platform/TODO-02-uefi-hardening-secureboot.md §9` -- optional DWORD values under `HKLM\SYSTEM\SecureBoot\` (db/dbx counts); must not regress `State` value written from TODO-01 §5
- → XREF: `01-boot-platform/TODO-05-boot-device-discovery.md §9` -- `HKLM\SYSTEM\Boot\Device\*` boot provenance is written from `boot_info` after `registry_init()` (coordinate value layout with that TODO)
- → XREF: `01-boot-platform/TODO-09-cpu-boot-sequencing.md §3,§9` -- optional `HKLM\HARDWARE\VM\*` hypervisor mirror from `boot_info` (§3 hypervisor detection); per-CPU `HKLM\HARDWARE\CPU\%u\Registers` audit strings per that TODO §9
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

| ⭐   | Order | Deliverable                                       | Depends On                | Status |
| --- | :---: | ------------------------------------------------- | ------------------------- | :----: |
| 💎   |   1   | Access rights, API limits, FILETIME & RegFlushKey | TODO-05 §2,§3, TODO-08 §1 |  [/]   |
| 💎   |   2   | Advanced key ops (copy, rename, save, volatile)   | §1                        |  [/]   |
| 💎   |   3   | Change notifications (core + exclusive extras)    | §2                        |  [/]   |
| 💎   |   4   | Nt/Zw registry syscalls & pointer validation      | §1, TODO-12 §14, §15      |  [/]   |
| 💎   |   5   | advapi32.dll compat (A/W, HKCR, error map)        | §4                        |  [/]   |
| 💎   |   6   | Registry virtualization & .reg import/export      | §5                        |  [/]   |
| 💎   |   7   | `regedit` shell tool                              | §4                        |  [/]   |
| ⭐   |   8   | Advanced hive features (dual-log, delta, compact) | §4                        |  [/]   |
| ⭐   |   9   | Transactions, search API & snapshot/diff          | §2, §3, §8                |  [/]   |
| ⭐   |  10   | Performance (mmap hive, B-tree cell format)       | §9                        |  [/]   |
| 💎   |  11   | KTM Transaction syscalls wired to SSDT            | §9, TODO-12 §14, §15      |  [/]   |
| 💎   |  12   | Registry symlink completion (create, open-link)   | §2, §4                    |  [/]   |
| ⭐   |  13   | Schema-validated registry keys                    | §3, §4                    |  [/]   |
| 💎   |  14   | Registry SMP synchronization                      | TODO-12 §14               |  [/]   |
| 💎   |  15   | Value size expansion (16 KiB names, 1 MiB data)   | §1, §14                   |  [/]   |
| 💎   | 16 | Post-ship follow-up backfill (2026-07-31 cohort) | -- | [ ] |

> 💎 = parity work -- matches what Windows 11 and Linux already do.
> ⭐ = exclusive work -- Impossible OS is superior or first.

> [!NOTE]
> **Lean structure:** §4 (Nt/Zw syscall surface) and §13 (schema) carry more than 8--10 top-level checklist bullets each. Before implementation, consider splitting them into additional top-level `##` sections (renumbering the file) so commits stay reviewable -- do not use `### N.M` or `**N.M**` pseudo-headings inside a section.

---

## 1. Access Rights, API Limits & RegFlushKey

> [!NOTE]
> **Self-contained execution:** `SeAccessCheck` (TODO-15 §5) and `SECURITY_DESCRIPTOR` (TODO-15 §3) are not yet implemented. §1 should stub `SeAccessCheck` as always-grant with a `#warning` reminder. `SECURITY_DESCRIPTOR` should be a `void*` placeholder. Full enforcement arrives when TODO-15 §3,§5 is complete.

- [x] Granted access mask recorded on the open handle (`reg_handle_t.access`); self-relative DACL on `reg_key_t.security_descriptor`; `reg_check_access(hKey, mask)` is the per-operation chokepoint
- [x] `RegOpenKeyEx`: SeAccessCheck stubbed always-grant (TODO-15 §5 unbuilt); `reg_effective_access` records the requested mask capped to the source handle's grant (anti-escalation); samDesired 0 / `MAXIMUM_ALLOWED` map to full (legacy compat)
- [x] `RegCreateKeyEx`: requires `KEY_CREATE_SUB_KEY` on the parent handle (`reg_check_access`) before creating a new subkey
- [x] `RegSetValueEx` / `RegDeleteValue`: require `KEY_SET_VALUE`; `RegDeleteKey` / `RegDeleteTree` require `DELETE`; `RegGetValue` requires `KEY_QUERY_VALUE`
- [x] `RegQueryValueEx` / `RegEnumValue` require `KEY_QUERY_VALUE`; `RegEnumKeyEx` requires `KEY_ENUMERATE_SUB_KEYS`; `RegQueryInfoKey` requires `KEY_QUERY_VALUE`
- [x] `KEY_READ`/`KEY_WRITE`/`KEY_ALL_ACCESS` already defined with correct values; added `KEY_NOTIFY` (0x10), `KEY_CREATE_LINK` (0x20), `DELETE` (0x10000), `MAXIMUM_ALLOWED`
- [x] Default DACL on new keys: `SeCreateDefaultSD(SE_SD_TYPE_REGISTRY_KEY)` (SY+BA=Full, BU=Read) -- non-owned static self-relative blob, never freed

- [x] Key name max 255: `reg_validate_path_limits` (wired into Create/Open/DeleteKey/DeleteTree/GetValue) rejects any component longer than `REG_MAX_KEY_NAME`; `reg_walk_path` also rejects over-long components
- [/] Value name max 16383 chars -- DEFERRED to §15 (hybrid inline/pointer migration is boot-critical); current 255-char limit enforced here
- [/] Value data max 1 MiB -- DEFERRED to §15 (heap-backed migration + 5 stack-buffer conversion, bare-metal stack hazard); current 512-byte limit enforced here
- [x] Key path depth max 512: `reg_validate_path_limits` rejects depth > `REG_MAX_KEY_DEPTH` with `ERROR_INVALID_PARAMETER`
- [x] Total key count: 90% soft-warn `klog`; hard limit via key-pool exhaustion returns `ERROR_OUTOFMEMORY`
- [x] `RegCreateKeyEx` atomic create-or-fail: reserve the handle slot (key=NULL) BEFORE `reg_walk_path` links anything, bind after; `reg_free_handle` on walk failure -- no handleless key on pool exhaustion

- [x] `last_write_time` (monotonic `uptime_ns`) converted to `FILETIME` at READ time via `reg_last_write_filetime()` (wall-clock anchor + mono delta); avoids the boot-order zero-wall-clock hazard
- [x] `RegQueryInfoKey` returns `lpftLastWriteTime` as FILETIME and `lpcbSecurityDescriptor` as the default-SD size
- [x] `NtQueryKey` / `NtEnumerateKey` info-class handlers return `LastWriteTime` as FILETIME (inherited from `RegEnumKeyEx` / `RegQueryInfoKey`)

- [x] `RegFlushKey(hKey)` -- immediate save of the single hive containing `hKey` (walk to hive root + `hive_save`); a persisted-root handle (HKLM/HKU) flushes all hives; wired into `NtFlushKey`
- [x] `RegFlushKey` requires `KEY_QUERY_VALUE`; volatile key or normal flush -> `ERROR_SUCCESS`; save failure -> `ERROR_REGISTRY_IO_FAILED`; C: unmounted -> `ERROR_INVALID_HANDLE` (NtFlushKey no-op)

- [x] Commit: `"kernel/registry: KEY_* access rights enforcement, API limits, FILETIME timestamps, RegFlushKey"`

**Test checkpoint:** `RegOpenKeyEx(HKLM\SOFTWARE, KEY_SET_VALUE)` from a Medium-IL process against an Admins-only DACL → `ERROR_ACCESS_DENIED`; as SYSTEM → `ERROR_SUCCESS`. `RegCreateKeyEx` with 256-char name → `ERROR_INVALID_PARAMETER`; 255-char → succeeds. `RegQueryInfoKey.ftLastWriteTime` returns non-zero FILETIME after `RegSetValueEx`. `RegFlushKey` on volatile key → `ERROR_SUCCESS` (no-op). Serial log: `"[REG] Access denied: %s mask=0x%x required=0x%x"`. Test on: QEMU WHPX + TCG.

> **Test runner:** `scripts\debug\kernel\run-abi-tests.bat` (SUITE=abi) | 1074 suites, 0 failures
> **Notes:**
> - Shipped: `reg_check_access` KEY_* chokepoint on all public `RegXxx` + NT `DesiredAccess` propagation; `reg_last_write_filetime` read-time FILETIME; per-hive `RegFlushKey`; `reg_validate_path_limits`; default DACL via `SeCreateDefaultSD`.
> - Integrates: enforcement gates every public entry plus the `NtDeleteKey`/`NtFlushKey` direct-pointer paths; SeAccessCheck stubbed always-grant with an anti-escalation source-grant cap until TODO-15 §5 lands the real DACL check.
> - Downstream: the §4 per-key `NtFlushKey` scope item is satisfied here; §2/§4 NT rename/save/restore/unload mask enforcement filed as concrete items with reciprocal XREF §1.
> - Scope boundary: value name/data size expansion is §15; SMP locking is §14; real DACL `SeAccessCheck` is TODO-15 §5.
> **Verified:** 2026-07-04 | commit `45c1f862` | 17/19 items | build OK | tests 1074/1074 PASS + smoke PASS 3.0s
> **Accepted:** [H] NT `NtUnloadKey`/`NtLoadKey` bypass `reg_check_access` -> XREF: 02-kernel-core/TODO-14 §4 (item: "NT raw-HKEY access enforcement: route `NtUnloadKey`/`NtLoadKey`/`NtUnloadKey2`" at the §4 checklist)
> **Deferred:** [M] value-name 16383 + value-data 1 MiB size expansion (heap-backed migration + 5 stack-buffer conversion, bare-metal stack hazard) -> XREF: 02-kernel-core/TODO-14 §15 (item: "Migrate `reg_value_t.name`" at the §15 checklist)
> **Quality reviewed:** 2026-07-04 | Codex 10x (design, adversarial, consistency, perf, re-adversarial) | 6H+5M fixed, 2H accepted-XREF, 1M accepted | scope: kernel-code-quality

---

## 2. Advanced Key Operations

- [x] `RegCopyTree(hKeySrc, lpSubKey, hKeyDest)` -- recursive value+subkey copy via `reg_copy_subtree`; `KEY_READ` src + `KEY_WRITE` dst; rejects dst inside src subtree (self-amplify guard); `klog` count line
- [x] `RegRenameKey(hKey, lpSubKeyName, lpNewKeyName)` -- in-place via `RegRenameKeyDirect` (unlink/relink, not copy+delete); `DELETE` right; `ERROR_ALREADY_EXISTS`(183) for a different sibling only, same-name no-op

- [/] `RegSaveKey`/`RegRestoreKey` -- handle + SeBackup/SeRestorePrivilege gates shipped (`TODO-15 §8`); both return `ERROR_NOT_SUPPORTED` until the hive I/O bodies exist
- [/] `NtSaveKey`/`NtRestoreKey` FileHandle hive body -- resolve FileHandle->path then the same hive bodies (privilege gate done in `TODO-15 §8`; FileHandle path still open) -> XREF: 02-kernel-core/TODO-15 §8

- [x] `RegCreateKeyEx(REG_OPTION_VOLATILE 0x1)` sets `REG_FLAG_VOLATILE` on new-create only; excluded from `hive_count` + `child_count` + `hive_serialize_key` recursion (all three, else reload corrupts); RAM-only, gone on reboot
- [x] `RegFlushKey` on a volatile key returns `ERROR_SUCCESS` (no-op -- no hive backing)
- [/] `RegQueryInfoKey` volatility -- N/A by Win32 design (`lpClass` is a class string, no query API exposes volatility); confirmed no mis-report, classes not stored -> Accepted: not-applicable (Codex design verdict)

- [x] KCB LRU cache: 32 `(parent, name) -> child` bindings (`reg_kcb_cache`); `RegCloseKey` promotes the closing key; populated on every resolved `reg_walk_path` hop
- [x] `reg_walk_path` checks `reg_kcb_lookup` before `reg_find_child`; hit validated live (name[0]!=0 + parent + name -- stale misses, never mis-resolves); `reg_kcb_get_stats` feeds the >90% test
- [x] Cache evicted on LRU overflow + purged via `reg_kcb_purge_key` from `RegDeleteKeyDirect`/`reg_delete_subtree`/`RegUnloadHive`/`RegRenameKeyDirect`; lock-free (SMP sync owned by §14)
- [x] NT raw-HKEY enforcement: `NtRenameKey`(DELETE)/`NtSaveKey`(`KEY_READ`)/`NtRestoreKey`(`KEY_WRITE`) via `reg_check_access`, save/restore return `STATUS_PRIVILEGE_NOT_HELD` -> XREF §1

- [x] Commit: `"kernel/registry: RegCopyTree, RegRenameKey, RegSaveKey/RestoreKey, volatile keys, KCB cache"`

**Test checkpoint:** `RegCopyTree(hSrc, NULL, hDst)` → all subkeys and values copied recursively; copy into own descendant → `ERROR_INVALID_PARAMETER`. `RegRenameKey(hKey, "Old", "New")` → old key gone, new key has same values; rename to existing name → `ERROR_ALREADY_EXISTS`; same-name rename → no-op success. `RegCreateKeyEx(REG_OPTION_VOLATILE)` → `RegFlushKey` no-op success (reboot-absence is a bare-metal serial check). `RegSaveKey`/`RegRestoreKey` with access → `ERROR_PRIVILEGE_NOT_HELD`; restore w/o `KEY_WRITE` → `ERROR_ACCESS_DENIED`. Delayed close: open/close/reopen same key 1000x → KCB cache hit rate > 90%. Serial log: `"CopyTree: %s -> %s (%u keys, %u values)"`. Test on: QEMU WHPX + TCG.

> **Test runner:** `scripts\debug\kernel\run-abi-tests.bat` (SUITE=abi) | 154 suites, 0 failures

> **Notes:**
> - Shipped `RegCopyTree`/`RegRenameKey`/`RegSaveKey`/`RegRestoreKey` + `REG_OPTION_VOLATILE` + a 32-entry KCB LRU cache in `registry.c`; 7 new `test_registry.c` suites.
> - KCB caches `(parent, name) -> child`; monotonic-pool + tombstone make pointers stable, and a 3-part liveness check makes a stale entry miss (never mis-resolve); >90% hit rate asserted over a 1000x open/close loop.
> - `RegSaveKey`/`RegRestoreKey` gate on SeBackup/SeRestorePrivilege (`TODO-15 §8`) but return `ERROR_NOT_SUPPORTED`; the hive I/O bodies (kernel-buffer path marshaling + scratch-tree atomic replace) and the NT FileHandle path stay open.
> - Canonical doc: registry API surface in [`include/registry.h`](../../include/registry.h); §2 design decisions in `.claude/state/live-gotchas.md`.
> - Scope boundary: §2 owns advanced key ops + KCB cache; SMP locking is §14; value/name size is §15; real `SeAccessCheck` is TODO-15 §5, privilege eval is §8.
> **Verified:** 2026-07-04 | commit `a5b7b219` | 8/13 items | build OK | tests 1108 ABI PASS
> **Note:** RegSaveKey/RegRestoreKey SePrivilegeCheck gates wired 2026-07-05 by TODO-15 §8; the hive I/O bodies + NT FileHandle path stay `[/]`.
> **Accepted:** [H] NtRenameKey collision test uses the repo-wide ASCII-in-`UNICODE_STRING` convention (handler casts `Buffer` to `char*`); real UTF-16 decode is kernel-wide -> XREF: 02-kernel-core/TODO-14 §5 (item: "UTF-16 decode for `UNICODE_STRING` inputs (kernel-wide)" at line 291)
> **Accepted:** [L] KCB writes counters/clock on every `reg_walk_path` hop (read-side cacheline contention beyond the lock-free baseline) -> XREF: 02-kernel-core/TODO-14 §14 (item: "KCB cache globals" at line 622)
> **Quality reviewed:** 2026-07-04 | Codex 9x (design, adversarial, consistency, perf, re-adversarial) | 8H+2M+2L fixed, 1H+1L accepted-XREF | scope: kernel-code-quality

---

## 3. Change Notifications

> **Scope:** §3 ships the CORE kernel-internal CALLBACK notification engine. The Win32 event/semaphore-facing `RegNotifyChangeKeyValue` + `NtNotifyChangeKey` wiring (KEY_NOTIFY, referenced hEvent, sync completion status) is owned by §4; the ⭐ advanced features + REG_NOTIFY_THREAD_AGNOSTIC are deferred `[/]` in-section.

- [x] `include/registry.h`: `REG_NOTIFY_CHANGE_NAME/ATTRIBUTES/LAST_SET/SECURITY` (0x01-0x08), `reg_notify_fn` typedef, callback-based `reg_watcher_t` (adds `next`), and a `reg_watcher_t *watchers` head on `reg_key_t`
- [x] Static `reg_watcher_pool[64]` + monotonic u32 `reg_watcher_next_id` (0 reserved as never-valid); `active` marks a slot in use; slots threaded onto each key's `watchers` head
- [x] `reg_notify_register(HKEY, filter, watch_subtree, callback, ctx, coalesce_ms) -> id` (kernel-internal + trusted: no KEY_NOTIFY check; returns 0 on bad handle / pool exhaustion) + `RegUnregisterNotify(id)` + `reg_notify_unregister_all(key)`
- [x] `reg_dispatch_notify(key, change_type, value_name)` walks `key` UP its parent chain: fires subtree+non-subtree at `key`, subtree-only at ancestors (once); coalescing via `reg_now_ms()`; `hit_count`++; callback
- [x] `reg_is_descendant(ancestor, key)` exported subtree-membership primitive (the dispatch walk uses the parent chain directly)
- [x] Dispatch hooks: `reg_set_value_direct`(LAST_SET; covers RegSetValueEx+RegCopyTree), `RegDeleteValue`(LAST_SET), `RegCreateKeyEx` new-key(NAME), `RegDeleteKey` pre-tombstone(NAME), `RegCopyTree`(NAME)
- [x] `reg_notify_unregister_all` cleanup at EVERY tombstone -- `RegDeleteKey`, `RegDeleteKeyDirect`, `reg_delete_subtree` (RegDeleteTree/RegUnloadHive children), `RegUnloadHive` root -- so a freed slot can never stay linked

- [/] Win32 `RegNotifyChangeKeyValue`(hEvent, fAsync) + `NtNotifyChangeKey`: KEY_NOTIFY check, referenced hEvent, sync completion-status -> Deferred: `02-kernel-core/TODO-14 §4` (item: "Win32 RegNotifyChangeKeyValue" at line 298)
- [/] `RegCloseKey` handle-scoped unregister + wake blocked callers `ERROR_KEY_DELETED` (§3 cleans up on key DELETE) -> Deferred: `02-kernel-core/TODO-14 §4` (item: "Win32 RegNotifyChangeKeyValue" at line 298)
- [/] Change-detail payloads ⭐ (old/new value snapshot before mutation) -> Deferred: in-section (no old-value snapshot infra; core fires value_name only)
- [/] Telemetry ⭐ `HKLM\SYSTEM\Registry\WatcherStats` HitCount/LastFiredMs -> Deferred: in-section (needs the Registry substrate key; counts already tracked in `reg_watcher_t`)
- [/] Priority-based dispatch ⭐ (`priority` field, system>high>normal fire order) -> Deferred: in-section (needs theme/display/service consumers)
- [/] `REG_NOTIFY_THREAD_AGNOSTIC (0x10000000)` + `task` process/thread watcher lists + `thread_exit`/`task_exit` cleanup -> Deferred: in-section (needs new `task` fields; §3 cleans on key delete)
- [/] SD-change notification (`REG_NOTIFY_CHANGE_SECURITY`) -- `reg_set_security_descriptor` does not exist, per-key SDs read-only today -> Deferred: `02-kernel-core/TODO-15 §5` (mutable per-key SD + registry SeAccessCheck)

- [x] Commit: `"kernel/registry: change notifications, subtree watching, coalescing, detail payloads"`

**Test checkpoint:** Register a callback watcher with `REG_NOTIFY_CHANGE_LAST_SET`; `RegSetValueEx("Width", 1920)` → callback fires once with `value_name="Width"`, `change_type=LAST_SET`; after `RegUnregisterNotify` no further fire. Subtree: a `watch_subtree` watcher on the parent fires for a child value change, a non-subtree one does not. Coalescing: with a large `coalesce_ms`, two back-to-back writes fire once. NAME: sub-key create AND delete each fire a parent NAME watcher. Cleanup: deleting the watched key makes `RegUnregisterNotify` report `ERROR_FILE_NOT_FOUND` (slot freed). Test on: QEMU WHPX + TCG.

> **Test runner:** `scripts\debug\kernel\run-abi-tests.bat` (SUITE=abi) | 159 suites, 0 failures

> **Notes:**
> - Shipped the kernel-internal change-notification callback engine in `registry.c` (`reg_watcher_pool[64]`, register/dispatch/unregister, subtree walk, coalescing) + `reg_watcher_t`/filters in `registry.h`; 5 new `test_registry.c` suites.
> - Dispatch fires from the value/name mutation chokepoints; `reg_notify_unregister_all` runs at every key tombstone so a freed 64-slot pool entry never stays linked.
> - Lock-free (registry-wide SMP sync owned by §14, same regime as the KCB cache); Codex design adoptions in the commit message.
> - Canonical doc: notification API in [`include/registry.h`](../../include/registry.h).
> - Scope boundary: §3 owns the callback engine; the Win32 `RegNotifyChangeKeyValue`/`NtNotifyChangeKey` event/semaphore path is §4; ⭐ advanced features + THREAD_AGNOSTIC are deferred `[/]` in-section; SD-change is TODO-15 §5.
> **Verified:** 2026-07-04 | commit `7e90e9b6` | 7/14 items | build OK | tests 1140 ABI PASS
> **Accepted:** [H] concurrent watcher registration (unlocked slot-claim + `reg_watcher_next_id` + head-insert) can corrupt the pool -- lock-free-registry class -> XREF: 02-kernel-core/TODO-14 §14 (item: "Notification engine SMP" at line 595)
> **Accepted:** [M] global `reg_dispatch_depth` shares the recursion budget across CPUs (concurrent dispatch can drop a notification) -> XREF: 02-kernel-core/TODO-14 §14 (item: "Notification engine SMP" at line 595)
> **Quality reviewed:** 2026-07-04 | Codex 11x (design, adversarial, consistency, perf, re-adversarial) | 5H+7M+1L fixed, 1H+1M accepted-XREF | scope: kernel-code-quality

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
  NtCreateKeyTransacted(KeyHandle, DesiredAccess, ObjectAttributes, TitleIndex, Class, CreateOptions, TxHandle, Disposition)  0x0091
  NtOpenKey(KeyHandle, DesiredAccess, ObjectAttributes)                                                          0x0092
  NtOpenKeyTransacted(KeyHandle, DesiredAccess, ObjectAttributes, TransactionHandle)                             0x0093
  NtOpenKeyEx(KeyHandle, DesiredAccess, ObjectAttributes, OpenOptions)                                           0x0094
  NtOpenKeyTransactedEx(KeyHandle, DesiredAccess, ObjectAttributes, OpenOptions, TransactionHandle)               --
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
  NtLoadKey2(TargetKey, SourceFile, Flags)                                                                        --
  NtLoadKey3(TargetKey, SourceFile, Flags, ExtendedParameters[], ExtendedParameterCount, DesiredAccess, RootHandle, ...)  --
  NtUnloadKey(TargetKey)                                                                                         0x00A5
  NtUnloadKeyEx(TargetKey, Event)                                                                                0x00A6
  NtUnloadKey2(TargetKey, Flags)                                                                                  --
  NtSaveMergedKeys(HighPrecedenceKey, LowPrecedenceKey, FileHandle)                                               --
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
- [ ] Transacted key ops: `NtCreateKeyTransacted` (0x0091), `NtOpenKeyTransacted` (0x0093), `NtOpenKeyTransactedEx` bind the op to a KTM `TransactionHandle` (XREF §9, §11); NULL handle = non-transacted
- [ ] `NtLoadKey2(TargetKey, SourceFile, Flags)`: flag-scoped load; `NtLoadKey3`: extended-parameter array (`CmExtendedParameterFileAccessToken` for token-scoped load), not a bare token arg
- [ ] `NtUnloadKey2(TargetKey, Flags)`: `REG_FORCE_UNLOAD (0x1)` unloads a hive with open handles; `NtSaveMergedKeys(High, Low, FileHandle)`: High-over-Low precedence merge to one `.hive` file
- [ ] `ObjectAttributes` for key paths: `RootDirectory` handle + `ObjectName` (`UNICODE_STRING` for future UTF-16; for now accept UTF-8 `ANSI_STRING` wrapper); resolve absolute paths starting with `\Registry\Machine` → `HKLM`, `\Registry\User\{SID}` → `HKCU`
- [ ] Wrap every user-mode pointer argument in `ProbeForRead(ptr, size, align)` / `ProbeForWrite(ptr, size, align)` (→ XREF `TODO-23-exception-dispatch-seh.md §13`) before any dereference; return `STATUS_ACCESS_VIOLATION` if probe faults
- [ ] `UNICODE_STRING` / `ANSI_STRING` struct fields: validate both the struct pointer AND the embedded `Buffer` pointer separately
- [ ] **UTF-16 decode for `UNICODE_STRING` inputs (kernel-wide)**: current Nt handlers across §14/§15/§17 cast `UNICODE_STRING.Buffer` (uint16_t*) directly to `const char*`, which treats real UTF-16 input as a one-byte ASCII string truncated at the first high byte. Add `int nt_decode_unicode_string(const UNICODE_STRING *us, char *buf, uint32_t buf_size)` in NEW file `src/kernel/nt/nt_string.c` (declared in `include/kernel/nt/nt_string.h`). Implementation: read `us->Length` bytes (counted, NOT NUL-terminated), validate each UTF-16 code unit fits in ASCII (high byte == 0), validate `us->Length <= us->MaximumLength`, validate `us->Length / 2 + 1 <= buf_size`, write to `buf` as NUL-terminated ASCII, return `STATUS_SUCCESS` or `STATUS_INVALID_PARAMETER`. The helper is kernel-wide because all NT names (registry keys, token values, namespace paths, file paths) are ASCII per MSDN. **Retrofit consumers across all §-handlers**: (a) registry §14 `NtSetValueKey` / `NtQueryValueKey` / `NtDeleteValueKey` / `NtEnumerateValueKey` and §15 `NtRenameKey` in `src/kernel/nt/nt_registry.c` (replace `(const char *)vname->Buffer` casts and `reg_oa_path` ASCII assumption); (b) token §16 (no UNICODE_STRING parsing currently: handler args are HANDLE / ACCESS_MASK / struct-by-pointer); (c) namespace §17 `oa_name`, `path_within_bounds`, `NtCreateSymbolicLinkObject_handler` target read in `src/kernel/nt/nt_namespace.c` (cast UNICODE_STRING.Buffer to char*, scan with bounded_strlen); (d) section §18 `oa_probe_ascii_name` in `src/kernel/nt/nt_section.c:39` and timer §19 `oa_probe_ascii_name` in `src/kernel/nt/nt_timer.c` (both return the raw `UNICODE_STRING.Buffer` pointer after probing `Length` bytes; callers feed it to `snprintf("%s")` in `ObCreateSectionEx`/`ObCreateTimerEx`/`ObOpenTimer` which reads past `Length` if the buffer lacks an embedded NUL); (e) future NT handlers must use `nt_decode_unicode_string` directly: never cast.
- [x] **Per-key `NtFlushKey` scope**: §1's `RegFlushKey(hKey)` does single-hive flush (persisted-root handle flushes all hives); `NtFlushKey_handler` routes through it so an unrelated hive I/O error cannot fail the call
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
  - `KeyLayerInformation` (9): layered-key metadata -- DEFERRED with layered keys / hive stacking (see Deferred features note); Win11 defines 10 classes total, this section ships 0-8
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
  - `KeySetLayerInformation` (6): layered-key set metadata -- DEFERRED with layered keys / hive stacking (see Deferred features note)
- [ ] `NtNotifyChangeKey`: **replace the existing `STATUS_NOT_IMPLEMENTED` stub** in `src/kernel/nt/nt_registry.c::NtNotifyChangeKey_handler` (currently tagged `SCOPE-GAP-ALLOWED: blocked on TODO-14 §4`) with real wiring: validates `KeyHandle`, resolves to `reg_key_t`, calls `reg_notify_register()` from §4; if `Asynchronous == FALSE`, blocks calling thread on the watcher's semaphore (interruptible via APC). Remove the `SCOPE-GAP-ALLOWED` comment when the stub is replaced.
- [ ] Win32 `RegNotifyChangeKeyValue`(hKey, hEvent, fAsync): KEY_NOTIFY-gated; references hEvent (`ObReferenceObjectByHandle`, deref on close); sync semaphore + `ERROR_KEY_DELETED`; `RegCloseKey` handle-scoped unregister -> XREF §3
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
- [ ] `HKCU` redirect: `NtOpenKey` with a path under `\Registry\User` resolves to the *current process's* per-session user subtree rather than a global `HKCU`; each process inherits its user SID from its primary token (→ XREF `TODO-15-security-reference-monitor.md §4`); `\Registry\User\S-1-5-21-...-1001` is the actual physical path; `NtOpenKey` with `RootDirectory=HKCU` resolves via the task's token
- [ ] **Per-process sandbox** ⭐ -- `NtSetInformationProcess(ProcessRegistrySandbox, root_path)`: future API that restricts all registry access for a process to a sub-tree; used by browser renderer and low-IL processes; deferred until process isolation (→ XREF `TODO-21-process-model-extensions.md`) matures
- [ ] **Rate limiting** ⭐ -- per-task `reg_ops_this_sec` counter reset every 1 000 ms by scheduler tick; if > 10 000 registry ops per second: `schedule_yield()` and re-check; soft throttle prevents runaway registry hammering from buggy apps; counter tracked in `struct task`
- [ ] **Audit log** ⭐ -- if `HKLM\SYSTEM\Registry\AuditEnabled = 1`: write a compact audit entry to `X:\Logs\registry-audit.log` on each `NtSetValueKey` / `NtDeleteKey`: `{timestamp, pid, key_path, value_name, old_type, new_type, result_ntstatus}`; uses the existing `klog` ring buffer at LOG_AUDIT level; auto-rotated at 4 MiB
- [ ] NT raw-HKEY access enforcement: route `NtUnloadKey`/`NtLoadKey`/`NtUnloadKey2` through the §1 checked resolver so hive unload/load direct-pointer paths enforce access rights -> XREF §1
- [ ] Commit: `"kernel/registry: NtOpenKey/NtSetValueKey/NtNotifyChangeKey syscalls, pointer validation, audit"`

**Test checkpoint:** `NtCreateKey` + `NtOpenKey` round-trip via SYSCALL from user-mode succeeds. `NtQueryValueKey(KeyValuePartialInformation)` returns correct data. `ProbeForWrite` with invalid pointer → `STATUS_ACCESS_VIOLATION`. `NtFreezeRegistry(5)` → all `NtSetValueKey` from another thread block; `NtThawRegistry()` → blocked writes complete. Audit log: enable `AuditEnabled=1` → `NtSetValueKey` entries appear in `registry-audit.log`. Serial log: `"[SSDT] NtOpenKey: \\Registry\\Machine\\... -> 0x%x"`. Test on: QEMU WHPX + TCG.

> **Test runner:** `scripts\debug\kernel\run-abi-tests.bat` (SUITE=abi) | registry NT syscalls covered via `ssdt_dispatch` in `test_registry.c`

> **Notes:**
> - Core registry NT CRUD syscalls (14 handlers, SSDT 0x0090-0x00A6) implemented + wired in `nt_registry.c`, tested via `ssdt_dispatch`; `NtSaveKey`/`NtRestoreKey` fail-closed (§2).
> - Deferred: `NtNotifyChangeKey`/Win32 `RegNotifyChangeKeyValue` need a safe watcher lifecycle (handle-close unregister + event deref) that rides on the HKEY->OB migration; the 6-arg SSDT ceiling hides `Asynchronous`.
> - Also deferred: kernel-wide `nt_decode_unicode_string` UTF-16 retrofit, HKEY->OB migration, `KeyNodeInformation` completion, transacted ops (KTM), ProbeForRead/Write (SEH).
> - Canonical doc: registry NT surface in [`src/kernel/nt/nt_registry.c`](../../src/kernel/nt/nt_registry.c).
> - Scope boundary: §4 is a >10-item mega-section -- the info-class enums + UTF-16 decode + OB migration warrant a section split before a fresh implementation pass.
> **Deferred:** [H] `NtNotifyChangeKey` + Win32 `RegNotifyChangeKeyValue` wiring blocked on safe watcher lifecycle (handle-close unregister + event deref) -> XREF: 02-kernel-core/TODO-14 §4 (item: "Migrate HKEY to OB handle table" at line 272)
> **Deferred:** [M] user-mode pointer validation on all Nt args -> XREF: 02-kernel-core/TODO-14 §4 (item: "Wrap every user-mode pointer argument in" at line 268)

---

## 5. advapi32.dll Win32 Compatibility

- [ ] All `RegXxx` functions that take string arguments have an `A` variant (UTF-8 / ANSI) and a `W` variant (UTF-16LE `WCHAR*`)
- [ ] `A` variants: call the internal UTF-8 kernel API directly (already exists)
- [ ] `W` variants: `wchar_to_utf8(src_w, buf, len)` → call internal UTF-8 API → convert any UTF-8 string results back to UTF-16LE via `utf8_to_wchar(src, buf, len)` before returning to caller
- [ ] `wchar_to_utf8` / `utf8_to_wchar`: implement in `src/libs/libc/wchar.c` (BMP-only initially; no surrogate pair support needed for registry paths); use existing `libc` string helpers

- [ ] Full advapi32 `RegXxx` export surface (Outcome parity): `RegLoadKey` / `RegUnLoadKey` / `RegReplaceKey` / `RegSaveKeyEx`, plus `RegLoadAppKey` (private per-process hive, auto-unloads on last `RegCloseKey`)
- [ ] advapi32 utility wrappers: `RegSetKeyValue` / `RegDeleteKeyValue`, `RegQueryMultipleValues` (over §4 `NtQueryMultipleValueKey`), `RegOpenCurrentUser` / `RegOpenUserClassesRoot` / `RegOverridePredefKey`
- [ ] advapi32 transacted wrappers `Reg{Create,Open,Delete}KeyTransacted` (pair with §4 transacted syscalls); implement or explicitly defer each documented export

- [ ] `HKEY_CLASSES_ROOT` read path: `RegOpenKeyEx(HKCR, sub_key, ...)` → first look in `HKCU\Software\Classes\<sub_key>`; if not found, look in `HKLM\SOFTWARE\Classes\<sub_key>`; return whichever is found first
- [ ] `HKCR` write path: `RegCreateKeyEx(HKCR, sub_key, ...)` → always write to `HKCU\Software\Classes\<sub_key>` (per-user override)
- [ ] `RegEnumKeyEx(HKCR, ...)`: merge results from both HKCU\Software\Classes and HKLM\SOFTWARE\Classes, deduplicate by name, return union; HKCU entries shadow HKLM entries with same name

- [ ] `reg_ntstatus_to_win32(NTSTATUS)` table: `STATUS_OBJECT_NAME_NOT_FOUND` → `ERROR_FILE_NOT_FOUND`, `STATUS_ACCESS_DENIED` → `ERROR_ACCESS_DENIED`, `STATUS_BUFFER_TOO_SMALL` → `ERROR_MORE_DATA`, `STATUS_NO_MORE_ENTRIES` → `ERROR_NO_MORE_ITEMS`, `STATUS_INSUFFICIENT_RESOURCES` → `ERROR_OUTOFMEMORY`, default → `ERROR_INVALID_FUNCTION`
- [ ] Win32 API wrappers call `SetLastError(reg_ntstatus_to_win32(status))` on failure before returning; `GetLastError()` returns the correct code

- [ ] **Tracing toggle** ⭐ -- `HKLM\SYSTEM\Registry\TraceEnabled = 1` (default 0) enables per-call tracing: each `RegXxx` call logs to the serial log at `LOG_TRACE` level: `[REG] RegSetValueEx HKLM\System\Display Width=1920 (pid=42)` with PID, key path, value name, type, and `NTSTATUS` result
- [ ] `reg_trace(func_name, hKey, value_name, type, status)` helper function; only evaluated when `reg_trace_enabled` global is 1 (set from `HKLM\SYSTEM\Registry\TraceEnabled` on `registry_init`)

- [ ] Commit: `"kernel/registry: advapi32 A/W shims, HKCR merged view, error mapping, API tracing"`

**Test checkpoint:** `RegOpenKeyExW(HKLM, L"SYSTEM\\Display", ...)` succeeds (UTF-16 path resolves). `RegOpenKeyExA(HKCR, "myapp.doc")` → reads HKCU\Software\Classes first, falls back to HKLM. Failed `RegQueryValueEx` → `GetLastError()` returns `ERROR_FILE_NOT_FOUND`. API trace: `TraceEnabled=1` → serial log shows `"[REG] RegSetValueEx HKLM\\... Width=1920 (pid=N)"`. Test on: QEMU WHPX + TCG.

> **Notes:**
> - Deferred: §5 advapi32 A/W compat is blocked -- the PE export table is name->SSDT-slot only (no user-mode advapi32 trampoline for wchar conversion + SetLastError), so A/W export entries would be non-functional (false completeness per design review).
> - Also blocked: `reg_resolve_hkcr` is HKLM-only (no HKCU-first / write-redirect / dedup-enum overlay); RegCloseKey needs the HKEY->OB handle migration (§4) or user open/close loops exhaust the 128-handle pool.
> - Canonical doc: registry Win32 surface in [`include/registry.h`](../../include/registry.h).
> - Scope boundary: §5 blocked on user-mode advapi32 surfacing (PE loader / user-mode libc) + HKEY->OB migration (§4) + HKCR overlay; wchar helpers ride with the §4 UTF-16 cluster.
> **Deferred:** [H] A/W advapi32 Reg* variants + export surface need a real user-mode advapi32 trampoline + the HKEY->OB handle migration before they can execute -> XREF: 02-kernel-core/TODO-14 §4 (item: "Migrate HKEY to OB handle table" at line 272)

---

## 6. Registry Virtualization & .reg Import/Export

- [ ] Low-IL and non-elevated processes that write to `HKLM\SOFTWARE\<path>` are silently redirected to `HKCU\Software\VirtualStore\MACHINE\SOFTWARE\<path>`
- [ ] Condition for redirect: `current_task()->Token->IntegrityLevelSid == SeILMedium` AND write is to `HKLM\SOFTWARE` subtree AND the key does not have `REG_FLAG_READONLY` AND the calling process is NOT running elevated (→ XREF `TODO-15-security-reference-monitor.md §6`)
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

> **Deferred:** [H] registry virtualization (low-IL `HKLM\SOFTWARE`->VirtualStore redirect) + `.reg` import/export blocked on the per-process token IntegrityLevel infra (TODO-15 §6 MIC) + the non-existent `NtSetInformationKey` control-flag path; also rides on §5 -> XREF: 02-kernel-core/TODO-14 §6 (item: "Condition for redirect" at line 373)

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

> **Notes:**
> - Deferred: a truthful `regedit` cannot be shipped as a shell stub today. User-mode `win32.h` explicitly excludes registry APIs, `cmd.exe` has no argv-preserving external command path for a standalone `regedit.exe`, and the live registry NT surface still has §4 gaps (`NtNotifyChangeKey` stub, HKEY-as-raw-pointer handles, UTF-16 decode, missing `NtSetInformationKey`).
> - `export`/`import` are also blocked by §6, where `reg_import`/`reg_export` are still unimplemented. Shipping only list/query/set/delete/tree would make the §7 deliverable falsely complete because the section contract explicitly includes export/import.
> **Verified:** 2026-07-04 | deferral evidence checked against `user/include/win32.h`, `user/cmd.c`, `src/kernel/nt/nt_registry.c`, and §4/§6 owners | no code change
> **Deferred:** [H] `regedit` shell tool blocked until the user-mode registry API/handle surface and `.reg` import/export exist; a stub command would be false completeness. -> XREF: 02-kernel-core/TODO-14 §4 (item: "Migrate HKEY to OB handle table" at line 272); -> XREF: 02-kernel-core/TODO-14 §6 (item: "`regedit import <file>` shell wrapper" at line 404)
> **Quality reviewed:** 2026-07-04 | Codex runner review loop (design, adversarial, consistency, perf; test-coverage N/A, no behavior change) | 1M fixed (stale review-infra note), 0 deferred

---

## 8. Advanced Hive Features

- [x] Harden `hive_load` value parsing (`registry.c:2515`, nested-key 2371): unchecked `pos += vdata_size` lets a crafted hive move `pos` past `data_size`. Use subtraction bounds + final `pos == data_size` check. XREF: TODO-12 §15.
- [/] Make `hive_load` transactional -- PARTIAL: validate pass (apply=0) catches malformed input before any live mutation, but apply still mutates incrementally with no rollback; full staging -> §14 reload-transactionality. XREF: TODO-12 §15.
- [x] `hive_validate_file` checks only the header CRC, then `hive_best_source` promotes a header-valid `.hive.log` over a good main hive before any data parse (`registry.c:2265-2305`). Validate the full candidate before promotion. XREF: TODO-12 §15.
- [/] DEFERRED: hive_parse_key recursion capped at HIVE_MAX_PARSE_DEPTH (12) for 8 KiB stack safety; full-depth support needs an iterative parser. -> XREF: 02-kernel-core/TODO-14 §10 (item: "Iterative/heap-backed hive parser" at the §10 checklist)
- [/] DEFERRED (boot-mount coupled, GPF history): wire `registry_load_hives()` into boot after defaults (needs C:); loads on-disk `ExternalEntropy`, so secure-delete MUST land together. -> XREF: `01-boot-platform/TODO-12 §9`
- [/] DEFERRED (with load trio): secure one-shot deletion across main + journal + `.bak` before cross-reboot absorb; then drop the `registry_persistence_active()` gate. -> XREF: `01-boot-platform/TODO-12 §9`
- [/] DEFERRED (with load trio): per-hive flush status (`registry_flush_hive_checked()`) so an unrelated dirty hive failing can't discard a persisted SYSTEM one-shot. -> XREF: `01-boot-platform/TODO-12 §9`
- [/] DEFERRED (needs secure-delete above): recovery tests -- post-consume corruption must NOT reintroduce `ExternalEntropy`; unrelated-hive flush failure must NOT discard a one-shot. -> XREF: `01-boot-platform/TODO-12 §9`
- [/] DEFERRED (with boot-wiring): registry_load_hives double-reads -- hive_best_source validates the full candidate, then hive_load re-reads/re-parses; reuse the validated buffer. -> XREF: `01-boot-platform/TODO-12 §9`
- [/] DEFERRED (recovery path needs the boot-mount wiring above): two alternating journal files `SYSTEM.hive.log1`/`log2`; active log tracked in hive header `active_log` byte (0=log1, 1=log2). -> XREF: `01-boot-platform/TODO-12 §9`
- [/] DEFERRED (dual-log): write cycle -- dirty pages + header to active log, fsync, commit marker, fsync, copy into main hive, clear journal, switch active log. -> XREF: `01-boot-platform/TODO-12 §9`
- [/] DEFERRED (dual-log): recovery -- on mount check both logs for a valid commit marker; higher sequence number wins; neither marker = hive clean. -> XREF: `01-boot-platform/TODO-12 §9`
- [/] DEFERRED (dual-log): crash while clearing journal leaves the other log with prior good state; matches Windows NT `.LOG1`/`.LOG2` exactly. -> XREF: `01-boot-platform/TODO-12 §9`

- [/] DEFERRED (dirty_bitmap is cross-CPU shared state; needs §14 registry SMP lock first): `uint8_t dirty_bitmap[HIVE_MAX_PAGES / 8]` on the hive struct, bit per 4 KiB page, set on every mutation. -> XREF: 02-kernel-core/TODO-14 §14
- [/] DEFERRED (needs §14 lock + dirty_bitmap): `hive_flush_incremental()` writes only dirty pages (seek+write each dirty 4 KiB page), clears bitmap; vs current full-rewrite `hive_flush()`. -> XREF: 02-kernel-core/TODO-14 §14
- [/] DEFERRED (with delta-flush): speedup -- a single `RegSetValueEx` on a 10 MiB hive flushes 4 KiB not 10 MiB; critical for fast boot. -> XREF: 02-kernel-core/TODO-14 §14
- [/] DEFERRED (with delta-flush): lazy writer -- call `hive_flush_incremental` every 5 s from a timer DPC, plus `RegFlushKey`/shutdown paths (DPC infra ready). -> XREF: `02-kernel-core/TODO-07 §4`

- [/] DEFERRED (user-mode shell has no registry/hive parse surface; same block as §7 regedit -- needs advapi32 trampoline / user-mode hive parser) -> XREF: 02-kernel-core/TODO-14 §7 -- `chkregistry <hive_path>` shell command:
  - Open hive file directly (not via the live registry)
  - Validate header CRC32; check `magic`, `version`, `key_count`, `value_count` fields
  - Walk all `reg_key_t` entries: verify parent pointers are valid, verify child-list linkage is consistent (no cycles, no dangling pointers)
  - For each `reg_value_t`: verify `type` is a known `REG_*` constant, verify `data_size ≤ REG_MAX_VALUE_SIZE`
  - Report: total keys, total values, errors found, estimated bytes wasted (deleted pool slots that could be compacted)
  - `--fix` flag: calls `hive_compact` (§8) on a copy and replaces the original if compact succeeds

- [/] DEFERRED (pairs with compaction) **Format versioning**: hive header `version`; `registry_init` reads it; version > supported -> read-only mount; version < supported -> auto-migrate. -> XREF: 02-kernel-core/TODO-14 §10
- [/] DEFERRED (pool is monotonic bump-only, no live-count; slot reclaim + util<60% trigger need a non-monotonic allocator first) **Compaction** ⭐ -- `hive_compact(hive)`:
  - Allocate new hive buffer
  - Walk all live (non-deleted) `reg_key_t` and `reg_value_t` entries in BFS order; pack them tightly into the new buffer
  - Write new hive atomically: write to `.hive.tmp` → fsync → rename over old file (atomic on IXFS/NTFS)
  - Triggered automatically when pool utilisation < 60% (many deletions have occurred) or by `chkregistry --fix`
  - Reclaims memory: a registry with 10 000 creations and 8 000 deletions compacts from 10 000 slots to 2 000 slots

- [x] Boot lifecycle: `registry_init()` now `boot_result_t` (registry.c:296), BOOT_FATAL on root-key alloc failure wired to the OB/EX recovery branch; default-population best-effort. -> XREF: `02-kernel-core/TODO-01 §4,§8`
- [x] Commit: `"kernel/registry: registry_init boot_result_t -> boot recovery on root-key alloc failure"`

**Test checkpoint:** Dual-log: corrupt `SYSTEM.hive.log1` mid-write → `registry_init` mounts from `log2`; data intact. Delta flush: modify one value in 1 MiB hive → `hive_flush_incremental` writes exactly 4 KiB (one dirty page). `chkregistry SYSTEM.hive` → reports key/value counts, 0 errors. Compaction: create 10K keys, delete 8K → `hive_compact` reduces pool to ~2K slots. Serial log: `"[REG] Delta flush: %u dirty pages written"`. Test on: QEMU WHPX + TCG. **This-pass shipped item (registry_init boot_result_t):** boot to `C:\>` on WHPX proves the non-fatal path; the fatal path needs pool corruption to exercise.

> **Test runner:** N/A (registry_init is live boot infra -- forbidden in unit tests) | validation: `bash scripts/test-smoke.sh` boot to `C:\>`
> **Notes:**
> - Shipped: `registry_init()` `void`->`boot_result_t` (registry.c:296) -- BOOT_FATAL on root-key alloc failure; boot_storage.c wires it to the recovery branch. Hardening items shipped a prior pass.
> - Integrates: mirrors the OB/EX gate pattern (`kernel_subsystem_set_ready` + `boot_recovery_show` + `boot_halt`); registry.h gains `kernel/boot_init.h`; default-population stays best-effort.
> - Downstream: closes the TODO-01 §4/§8 typed-registry_init items; 2 of 3 hive_load hardening items (bounds, full-validate) swept TODO-12 §15's crafted-hive stamps; the transactional one is [/] partial, its stamp re-linked to §14.
> - Scope boundary: durability DEFERRED -- delta-flush needs §14 SMP lock; dual-log/load-wiring/secure-delete need boot TODO-12 §9; chkregistry needs user-mode surface (§7); compaction needs a non-monotonic allocator.
> **Verified:** 2026-07-04 | commit `dd29fad8` | 3/21 items | build OK | abi 1149/0 PASS + smoke PASS 3.1s
> **Deferred:** [H] hive_load apply pass mutates the live tree with no rollback + load-capacity preflight counts total records not missing allocations -> XREF: 02-kernel-core/TODO-14 §14 (item: "reload transactionality" at the §14 checklist)
> **Deferred:** [H] hive_parse_key recursion capped at HIVE_MAX_PARSE_DEPTH (12) for 8 KiB stack safety; full depth needs an iterative parser -> XREF: 02-kernel-core/TODO-14 §10 (item: "Iterative/heap-backed hive parser" at the §10 checklist)
> **Deferred:** [M] registry_load_hives double-reads (best_source validates then hive_load re-parses) -> XREF: 02-kernel-core/TODO-14 §8 (item: "double-reads" at the §8 checklist)
> **Quality reviewed:** 2026-07-04 | Codex 9x (design, adversarial, consistency, perf, re-adversarial) | 3H+2M fixed, 2H+1M deferred | scope: kernel-code-quality

---

## 9. Transactions, Search API & Snapshot/Diff

- [/] DEFERRED (transactions need the §14 lock + KTM §11): `RegBeginTransaction(hKey)` → `HREG_TXN`: `reg_txn_t` copy-on-write journal -- each mutation writes to the journal, not the live tree.
- [/] DEFERRED: `RegCommitTransaction(hTxn)` -- apply journalled mutations under the §14 registry lock; atomically visible after release; flush hive after commit.
- [/] DEFERRED: `RegAbortTransaction(hTxn)` -- discard the journal; live tree unchanged; free `reg_txn_t`.
- [/] DEFERRED: conflict detection -- key modified by another writer since `RegBeginTransaction` -> `STATUS_REGISTRY_TRANSACTION_CONFLICT`; caller retries/aborts.
- [/] DEFERRED: `NtCreateTransaction`/`NtCommitTransaction`/`NtRollbackTransaction` SSDT wrappers (KTM `RtlSetCurrentTransaction`) -> XREF: 02-kernel-core/TODO-14 §11
- [/] DEFERRED (concurrent tree read needs the §14 read lock): `RegFindKey(hRoot, lpPattern, dwFlags, phKey)` -- glob (`*`/`?`, case-insensitive) over key names under `hRoot`; `RegFindNextKey` iterates.
- [/] DEFERRED: `RegFindValue(hRoot, lpKeyPattern, lpValuePattern, dwType, phKey, lpValueName)` -- key+value name pattern search, optional `dwType` filter.
- [/] DEFERRED: `reg_find_state_t` -- DFS traversal stack so `RegFindNextKey` resumes without rescanning from the root.
- [/] DEFERRED (deep-copy allocates pool slots -- needs the §14 lock): `RegTakeSnapshot(hRoot, phSnapshot)` -- deep-copy the sub-tree into a new in-memory tree; opaque `HREG_SNAPSHOT`.
- [/] DEFERRED: `RegDiffSnapshots(hSnapshot1, hSnapshot2, callback, ctx)` -- compare two snapshots (or snapshot vs live via `NULL`):
  - For each key/value added: callback with `REG_DIFF_ADDED`
  - For each key/value deleted: callback with `REG_DIFF_DELETED`
  - For each value that changed type or data: callback with `REG_DIFF_MODIFIED` including old and new values
- [/] DEFERRED: `RegFreeSnapshot(hSnapshot)` -- free the deep-copy tree.
- [/] DEFERRED (needs §7 regedit, deferred): `regedit diff <f1> <f2>` -- `RegTakeSnapshot` two `.reg` exports, unified diff (`+`/`-`/`~`). -> XREF: 02-kernel-core/TODO-14 §7
- [/] DEFERRED (frees shared pool slots -- needs the §14 lock): **Orphan GC** ⭐ `reg_gc_orphans()` -- free pool slots for `active` keys unreachable from any root; auto-run on `registry_flush` if orphan count > 10.
- [/] DEFERRED (per-task tracking + NtSetValueKey hook -- needs §14 lock): **Per-PID quota** ⭐ track per-task registry bytes in `struct task`; > `QuotaBytes` (50 MiB) -> `STATUS_QUOTA_EXCEEDED`; reset on exit.

- [ ] Commit: `"kernel/registry: transactions, search API, snapshot/diff, orphan GC, per-PID quota"`

**Test checkpoint:** `RegBeginTransaction` + `RegSetValueEx` + `RegCommitTransaction` → value visible. `RegAbortTransaction` → value absent. Conflict: two concurrent txns modify same key → second commit → `STATUS_REGISTRY_TRANSACTION_CONFLICT`. `RegFindKey(HKLM, "Disp*")` → finds `Display`. `RegTakeSnapshot` + modify + `RegDiffSnapshots` → `REG_DIFF_MODIFIED`. Per-PID quota at 50 MiB → exceed → `STATUS_QUOTA_EXCEEDED`. Serial log: `"[REG] Transaction commit: %u mutations applied"`. Test on: QEMU WHPX + TCG.

> **Test runner:** N/A (deferred, no code) | validation: on implementation, `scripts\debug\kernel\run-abi-tests.bat`
> **Deferred:** [M] Transactions/search/snapshot/GC/quota deferred -- all add registry operations (pool alloc/free, tree walk under concurrent mutation, per-task writes) that must be SMP-safe under the §14 lock (deferred); transactions also need KTM (§11) and `regedit diff` needs §7 -> XREF: 02-kernel-core/TODO-14 §14 (item: "reg_dispatch_notify" at the §14 checklist)

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

- [ ] Iterative/heap-backed hive parser: replace `hive_parse_key` recursion (capped at HIVE_MAX_PARSE_DEPTH=12 for 8 KiB-stack safety) with a heap-backed parse stack so hives up to REG_MAX_KEY_DEPTH round-trip. -> XREF: 02-kernel-core/TODO-14 §8
- [ ] Commit: `"kernel/registry: hash map child lookup, mmap hive, B-tree cell format"`

**Test checkpoint:** Hash map: create 100 children under one key → `reg_child_lookup` finds each by name. Load factor: bucket chain > 8 triggers rehash. Mmap hive: `hive_open_mmap` → `RegQueryValueEx` reads directly from mapped page (zero-copy). B-tree: write hive in `regf` format → external `python-registry` tool parses it. Serial log: `"[REG] Hash map: bucket_count=%u max_chain=%u"`. Test on: QEMU WHPX + TCG.

> **Test runner:** N/A (deferred, no code) | validation: on implementation, `scripts\debug\kernel\run-abi-tests.bat`
> **Deferred:** [M] Performance (hash-map child lookup, mmap hive, B-tree cell format) deferred -- mmap needs `NtMapViewOfSection` (05-storage/TODO-05 §7) + §9; all add registry surface needing the §14 SMP lock (deferred) -> XREF: 02-kernel-core/TODO-14 §14 (item: "reg_dispatch_notify" at the §14 checklist)

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

> **Test runner:** N/A (deferred, no code) | validation: on implementation, `scripts\debug\kernel\run-abi-tests.bat`
> **Deferred:** [M] KTM transaction SSDT surface deferred -- needs the KTM transaction-manager infrastructure and §9 registry transactions (both deferred) -> XREF: 02-kernel-core/TODO-14 §9 (item: "RegBeginTransaction" at the §9 checklist)

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

> **Test runner:** N/A (deferred, no code) | validation: on implementation, `scripts\debug\kernel\run-abi-tests.bat`
> **Deferred:** [M] Symlink create/open-link completion deferred -- REG_LINK creation/open needs the HKEY->OB handle-table migration (§4, deferred) and the §14 SMP lock -> XREF: 02-kernel-core/TODO-14 §4 (item: "Migrate HKEY to OB handle table" at line 272)

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

> **Test runner:** N/A (deferred, no code) | validation: on implementation, `scripts\debug\kernel\run-abi-tests.bat`
> **Deferred:** [M] Schema-validated keys deferred -- schema enforcement hooks into the write path (§3 notify + §4 handles) and needs the §14 SMP lock; §4 is deferred -> XREF: 02-kernel-core/TODO-14 §4 (item: "Migrate HKEY to OB handle table" at line 272)

---

## 14. Registry SMP Synchronization

> [!NOTE]
> The registry engine (`registry.c`) was designed as a single-threaded subsystem used only during boot. TODO-12 §14 exposed it to concurrent SSDT dispatch from user-mode tasks, making three unsynchronized shared resources vulnerable to SMP races: pool allocators, handle pool, and tree structure. This section adds a registry-wide rwlock so read-heavy workloads (enumerate, query) can run concurrently while mutations (create, delete, set value) serialize safely.

> [!WARNING]
> **Design-review-confirmed prerequisites (2026-07-04, Codex `b9pq875ym`).** A naive coarse spinlock does NOT work here; four hard constraints must be designed first: (1) **IRQ context** -- `RegCreateKeyEx`/`RegSetString`/`RegSetDword`/`RegCloseKey` run in ISR context via `ahci_hotplug_check` (`ahci_core.c` `ahci_irq_body`), so `reg_lock` must be `spin_lock_irqsave`, and the lock must NOT be held across `hive_save`/`hive_load`/`registry_flush` PMM+VFS I/O (an ISR would spin behind disk I/O). (2) **Reentrant callbacks** -- `reg_dispatch_notify` fires watcher callbacks synchronously inside every mutation, and a LIVE test (`test_registry_notify_reentrant`) re-enters `RegSetValueEx` from a callback; a non-recursive lock held across the notify call deadlocks. Requires DEFERRED dispatch: under lock, apply + collect watcher payload snapshots (callback, ctx, change_type, COPIED key_path + value_name, generation) into a bounded array, release lock, then fire lock-free. (3) **Watcher lifetime** -- `RegUnregisterNotify` frees watchers immediately, so a deferred `{fn,ctx}` array is a UAF; needs a generation/epoch or in-flight refcount. (4) **Depth guard** -- global `reg_dispatch_depth` must become per-thread/per-CPU once dispatch is outside the lock (else unrelated CPUs share the budget = silent notification loss). Non-trivial wrappers (`RegRenameKey` walks the tree before `RegRenameKeyDirect`) must lock their FULL body, not just the callee.

- [/] DEFERRED (needs the redesign in the WARNING above): declare `static spinlock_t reg_lock` (irqsave) in `registry.c`; static `SPINLOCK_INIT`.
- [/] DEFERRED: `spin_lock_irqsave` wrap write-path entry points -- `RegCreateKeyEx`, `RegOpenKeyEx`, `RegCloseKey`, `RegDeleteKey`, `RegDeleteTree`, `RegDeleteKeyDirect`.
- [/] DEFERRED: `RegSetValueEx`, `RegDeleteValue` under the lock (both fire notify -> need the deferred-dispatch redesign first).
- [/] DEFERRED: read-path entry points `RegQueryValueEx`, `RegGetValue`, `RegEnumKeyEx`, `RegEnumValue`, `RegQueryInfoKey` under the lock (single spinlock v1).
- [/] DEFERRED: pool allocators (`reg_alloc_key`/`reg_alloc_value`/`reg_alloc_handle`/`reg_free_handle`) + `reg_create_child`/`reg_walk_path` called lock-held; non-recursive, never self-lock.
- [ ] Extract the `registry_init` two-pool allocation into a pure transaction helper with injectable alloc/free, so the partial-rollback branch (value pool fails after key pool succeeds) gets coverage -> XREF: `02-kernel-core/TODO-33 §10`
- [/] DEFERRED: cross-call hazards -- `RegRenameKey` locks its FULL body (pre-callee walk is a TOCTOU race); `Reg{Set,Get}{Dword,String}` wrappers stay lock-free, lock in the `RegSetValueEx`/`RegQueryValueEx` callee.
- [/] DEFERRED: `reg_dispatch_notify` -> deferred dispatch (snapshot payload under lock, fire lock-free) + watcher refcount/epoch (RegUnregisterNotify frees = UAF) + per-CPU depth. -> XREF: 02-kernel-core/TODO-14 §3
- [/] DEFERRED: `hive_save`/`hive_load`/`registry_flush`/`registry_save_all`/`registry_load_hives` -- lock only the tree-walk portion, release before PMM+VFS I/O (ISR must not spin behind disk I/O).
- [/] DEFERRED: `hive_load` reload transactionality: hold lock across validate-to-apply; preflight counts MISSING allocs not total; roll back partial mutations on apply-fail into an existing key. -> XREF: 02-kernel-core/TODO-14 §8
- [/] DEFERRED: SMP stress tests -- concurrent create/delete/enum; unregister-during-dispatch (UAF); unrelated-CPU dispatch depth; RegRenameKey racing delete/rename. WHPX 2 vCPU.
- [/] DEFERRED: upgrade to rwlock if profiling shows read contention (spinlock is v1).
- [/] DEFERRED: KCB cache globals (`reg_kcb_cache`/`reg_kcb_clock`/counters, §2) written on every `reg_walk_path` hop -- covered by `reg_lock` above. -> XREF: 02-kernel-core/TODO-14 §2
- [/] DEFERRED: notification-engine SMP (§3) -- watcher slot-claim + `reg_watcher_next_id` + per-key list head-insert + `reg_dispatch_depth` covered by the lock + deferred-dispatch redesign. -> XREF §3
- [ ] Batched atomic multi-value write under one `reg_lock` hold (default + `Path` pair) so `app_paths_register`/`app_paths_lookup` (TODO-22 §17) get an atomic write + consistent read snapshot -> XREF: 02-kernel-core/TODO-22 §17
- [ ] Commit: `"kernel/registry: SMP-safe registry with spinlock around all pool and tree operations"`

**Test checkpoint:** Two tasks concurrently creating and deleting keys under `\Registry\Machine\Software\SmpTest` for 1000 iterations. No kernel fault, no duplicate handles, enumeration sees consistent child counts. `RegQueryInfoKey` returns correct `lpcSubKeys` under concurrent mutation. Serial log: `"[REG] SMP lock: %u contention events"` (informational). Test on: QEMU WHPX (2 vCPU).

> **Test runner:** N/A (deferred, no code) | validation: on implementation, `scripts\debug\kernel\run-abi-tests.bat` + WHPX 2-vCPU SMP stress
> **Deferred:** [H] Registry SMP lock deferred pending a notification-engine deferred-dispatch redesign (payload snapshots + watcher refcount/epoch), IRQ-safe (`spin_lock_irqsave`) lock boundaries that exclude hive PMM+VFS I/O, and per-CPU dispatch depth (design review `b9pq875ym`; a naive coarse lock deadlocks the live reentrant-callback test) -- see the WARNING callout -> XREF: 02-kernel-core/TODO-14 §14 (item: "reg_dispatch_notify" at the §14 checklist)

---

## 15. Registry Value Size Expansion (16 KiB Names, 1 MiB Data)

Raise registry value-name and value-data limits to Windows 11 parity (16 383-char names, 1 MiB data). Split out of §1 because it is a boot-critical data-structure + on-disk-format migration that converts fixed inline arrays to heap-backed storage across five stack buffers, not just the `reg_value_t` fields.

> [!WARNING]
> **Bare-metal hazard:** the kernel stack is 8 KiB (`TASK_STACK_SIZE`). Five `uint8_t buf[REG_MAX_VALUE_SIZE]` stack buffers (`registry.c:1296`, `2380`, `2524`; `nt_registry.c:360`, `595`) MUST be converted to heap/PMM-backed BEFORE raising `REG_MAX_VALUE_SIZE`, or the first large-value call guard-page-faults. SMP locking stays with §14.

- [ ] Migrate `reg_value_t.name` (`char[256]`) to hybrid inline-or-pointer: inline <=255 chars, `pmm_alloc_contiguous` for longer; raise `REG_MAX_VALUE_NAME` to 16383; keep the empty-name tombstone (`registry.c:1355`) pointer-safe
- [ ] Migrate `reg_value_t.data` (`uint8_t[512]`) to pointer-backed for data > 512 bytes; raise `REG_MAX_VALUE_SIZE` to 1048576; update `reg_alloc_value` + free paths
- [ ] Convert the five oversized stack buffers to heap/PMM-backed: `registry.c:1296` (RegGetValue expand), `2380` (hive_deserialize_key), `2524` (hive_load root), `nt_registry.c:360` + `595` (NtQueryValueKey/enum)
- [ ] Update on-disk wire format (`hive_serialize_key`/`hive_deserialize_key`, `registry.c:2049`/`2344`): name len is u16 (16383 fits), data len already u32; bump `HIVE_VERSION` only if record layout changes + add a v1 read path
- [ ] Free heap-backed name/data on value delete + key free; no leak on `RegDeleteValue` / `RegDeleteKey` / hive unload
- [ ] Charge registry names/data bytes and watchers via `quota_charge_current` once value slots are reusable; route `hive_parse_value` through the same transaction so hive load cannot bypass the cap. -> XREF: `02-kernel-core/TODO-25 §6`
- [ ] Commit: `"kernel/registry: value size expansion to 16KiB names + 1MiB data (heap-backed)"`

**Test checkpoint:** `RegSetValueEx` with a 4096-byte `REG_BINARY` succeeds; a 1 MiB value round-trips through `hive_save`/`hive_load`; a 300-char value name round-trips; no kernel-stack guard fault on a large-value query. Serial log: `"[REG] large value: name=%u data=%u bytes"`. Test on: QEMU WHPX + TCG.

> **Test runner:** N/A (deferred, no code) | validation: on implementation, `scripts\debug\kernel\run-abi-tests.bat`
> **Deferred:** [M] Value-size expansion (16 KiB names, 1 MiB data) deferred -- heap-backed migration of the 5 stack buffers + on-disk format bump must land under the §14 SMP lock (deferred) -> XREF: 02-kernel-core/TODO-14 §14 (item: "reg_dispatch_notify" at the §14 checklist)

---

## 16. Post-Ship Follow-Up Backfill (orphan cohort 2026-07-31)

Items moved here VERBATIM from their original, already-stamped sections, where they were unreachable: the triage oracle classifies a stamped section DONE without reading its body, so an item appended after the stamp is invisible to every later pass. Source section noted per group. Cohort context: `todo/overnight-runner-improvements/overnight-runner-improvements-v05.md` item 3.

From the stamped section 2:
- [ ] `RegSaveKey`/`RegRestoreKey` hive I/O bodies -- probe/copy lpFile into a bounded kernel buffer (no TOCTOU), drive `hive_save`; restore stages into a scratch tree + atomic swap (removes stale entries, preserves original on failure)

**Test checkpoint:** per moved item; each carries its original acceptance text.

---

## OS Comparison

| ⭐   | Feature                               | 🪟 Win11                     | 🐧 Linux                           | 🚀 Impossible OS                                        |
| --- | ------------------------------------- | --------------------------- | --------------------------------- | ------------------------------------------------------ |
| 💎   | Hierarchical typed key/value store    | ✅ Full                      | ⚠️ dconf (GNOME), ini files       | ✅ Done -- `reg_key_t` tree                             |
| 💎   | Win32 `RegXxx` API                    | ✅ Native                    | ❌ N/A                             | ✅ Done -- complete native API                          |
| 💎   | Persistent hive + crash-safe WAJ      | ✅ `.LOG1`/`.LOG2`           | ⚠️ dconf binary db, no WAJ        | ✅ Done -- `.hive.log` WAJ                              |
| 💎   | Key `LastWriteTime` (FILETIME)        | ✅ Every key                 | ❌ N/A                             | ✅ FILETIME read-time (§1)                              |
| 💎   | KEY_* access rights enforcement       | ✅ Full                      | ❌ N/A                             | 🔄 handle-mask enforced (§1); DACL SeAccessCheck T15 §5 |
| 💎   | Registry symlinks (REG_LINK)          | ✅ CurrentControlSet, etc.   | ❌ N/A                             | ⚠️ Resolution done; API ⬜ §12                          |
| 💎   | `RegCopyTree` / `RegRenameKey`        | ✅ Full                      | ❌ N/A                             | ✅ Done -- recursive copy + in-place rename (§2)        |
| 💎   | Volatile keys (`REG_OPTION_VOLATILE`) | ✅ Full                      | ❌ N/A                             | ✅ Done -- RAM-only, no-persist (§2)                    |
| 💎   | `RegSaveKey` / `RegRestoreKey`        | ✅ SeBackup/SeRestore        | ❌ N/A                             | 🔄 fail-closed; body T15 §2 (§2)                        |
| ⭐   | KCB hot-key close-cache               | ✅ CmpCache pushlock         | ❌ N/A                             | ✅ Done -- 32-entry LRU (§2)                            |
| 💎   | Change notifications                  | ✅ `RegNotifyChangeKeyValue` | ⚠️ inotify (file-level)           | 🔄 callback engine (§3); Win32 event path §4            |
| 💎   | `REG_NOTIFY_THREAD_AGNOSTIC`          | ✅ Win8+                     | ❌ N/A                             | ⬜ §3                                                   |
| 💎   | Full NT registry syscall surface      | ✅ 30+ syscalls              | ❌ No registry concept             | ⬜ §4                                                   |
| 💎   | `KEY_INFORMATION_CLASS` completeness  | ✅ 10 info classes           | ❌ N/A                             | ⬜ §4                                                   |
| 💎   | advapi32.dll W variants + HKCR        | ✅ Full                      | ⚠️ Wine reimplements              | ⬜ §5                                                   |
| 💎   | Registry virtualization               | ✅ Vista+ VirtualStore       | ❌ N/A                             | ⬜ §6                                                   |
| 💎   | Virtualization control flags          | ✅ `DONT_VIRTUALIZE` etc.    | ❌ N/A                             | ⬜ §6                                                   |
| 💎   | `.reg` import/export                  | ✅ regedit.exe built-in      | ⚠️ Wine `regedit`                 | ⬜ §6 §7                                                |
| 💎   | Dual-log WAJ failover                 | ✅ `.LOG1`/`.LOG2`           | ❌ N/A                             | ⬜ §8                                                   |
| ⭐   | Incremental delta flush               | ❌ Full hive rewrite         | ❌ Full db rewrite                 | ⬜ §8                                                   |
| ⭐   | Change-detail payloads                | ❌ Signal only               | ❌ N/A                             | ⬜ §3                                                   |
| ⭐   | Priority-based notification dispatch  | ❌ All watchers equal        | ❌ N/A                             | ⬜ §3                                                   |
| ⭐   | Atomic registry transactions          | ⚠️ KTM deprecated           | ⚠️ dconf change_set (no rollback) | ⬜ §9                                                   |
| ⭐   | Native pattern-search API             | ❌ Manual enumerate+match    | ❌ N/A                             | ⬜ §9                                                   |
| ⭐   | Snapshot & diff                       | ❌ Needs RegShot (3rd-party) | ❌ N/A                             | ⬜ §9                                                   |
| ⭐   | Hive integrity reporter               | ❌ No built-in               | ❌ N/A                             | ⬜ §8                                                   |
| ⭐   | Idle-time hive compaction             | ❌ No defrag                 | ❌ N/A                             | ⬜ §8                                                   |
| ⭐   | Per-process registry sandbox          | ❌ HKCU shared               | ❌ N/A                             | ⬜ §4                                                   |
| ⭐   | Built-in API call tracing             | ❌ Needs ProcMon/ETW         | ❌ N/A                             | ⬜ §5                                                   |
| ⭐   | Per-PID registry quota                | ❌ Global limit only         | ❌ N/A                             | ⬜ §9                                                   |
| ⭐   | Memory-mapped hive                    | ❌ Static pool               | ✅ dconf mmap reads                | ⬜ §10                                                  |
| ⭐   | B-tree cell format                    | ✅ `regf` format             | ❌ N/A                             | ⬜ §10                                                  |
| ⭐   | Schema-validated keys                 | ❌ No type enforcement       | ⚠️ GSettings XML schemas          | ⬜ §13                                                  |
| 💎   | SMP-safe registry operations          | ✅ CmpLock pushlock          | ✅ dconf GVDB atomic               | ⬜ §14                                                  |
| 💎   | NtFreezeRegistry / NtThawRegistry     | ✅ VSS backup support        | ❌ N/A                             | ⬜ §4                                                   |
| 💎   | NtInitializeRegistry boot signal      | ✅ SMSS boot sequence        | ❌ N/A                             | ⬜ §4                                                   |

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
