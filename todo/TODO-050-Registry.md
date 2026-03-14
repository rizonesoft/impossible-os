# P0102 — Registry System (Windows-Compatible)

> **Goal:** Replace the Codex registry with a full Windows-compatible **Registry** system
> using the same API surface as Win32 (`RegOpenKeyEx`, `RegSetValueEx`, etc.), the same
> root keys (`HKEY_LOCAL_MACHINE`, `HKEY_CURRENT_USER`, etc.), and the same value types
> (`REG_SZ`, `REG_DWORD`, `REG_BINARY`, etc.). Store registry data in binary hive files
> with crash-safe journaling. Provide a `regedit` shell command for inspection.

> [!CAUTION]
> **Memory Rule:** Use `pmm_alloc_contiguous()` for ALL buffers > 4 KB (hive file buffers, large binary values). `kmalloc` is ONLY for small kernel structs (≤ 4 KB). Violating this crashes the 2 MiB heap silently. See `rules.md` Known Gotchas.

> [!IMPORTANT]
> **Migration:** The current Codex system (`codex.c`, `codex.h`) must be fully replaced.
> All existing call sites (`main.c`, `panic.c`, `swap.c`, `desktop.c`, `icon_store.h`)
> must be updated to use the new Registry API. The disk format changes from `.codex` text
> files to `.hive` binary files. Existing Codex defaults must be re-populated under the
> new Registry key paths.


---

## 1. Core Registry Engine

### 1.1 Registry Data Structures

**Prompt:** Verify the correctness and consistency of the Registry core data structures (commit `d44a791`). Confirm that `include/registry.h` defines `reg_key_t` with a 256-char name, parent pointer, 16-bucket FNV-1a child hash map (`children[REG_CHILD_BUCKETS]`), `hash_next` collision chain, `child_count`, `values` linked list, `value_count`, `last_write_time`, and `flags`. Confirm `reg_value_t` has a 256-char name, `type` (uint32_t), `data[REG_MAX_VALUE_SIZE]` buffer, `data_size`, and `next` pointer. Confirm `HKEY` is defined as `reg_handle_t*` wrapping `reg_key_t*` + access mode, and that `HKEY_LOCAL_MACHINE` through `HKEY_CURRENT_CONFIG` use sentinel addresses `0x80000000`–`0x80000005`. Confirm `src/kernel/registry.c` defines static pools `reg_key_pool[512]` and `reg_value_pool[1024]`, FNV-1a hash function (`reg_fnv1a`) with case-insensitive folding, pool allocators `reg_alloc_key`/`reg_alloc_value`, `reg_resolve_predefined()` mapping, and `registry_init()` creating all 5 root keys. Run `bash scripts/build.sh clean` and confirm zero warnings. Verify `docs/architecture/registry.md` documents the §1.1 structures.

- [x] Define `reg_key_t` struct:
  - [x] `name[256]` — key name
  - [x] `parent` pointer — parent key
  - [x] `children` — hash map of child keys (FNV-1a hash → `reg_key_t*`)
  - [x] `child_count` — number of child keys
  - [x] `values` — linked list of `reg_value_t`
  - [x] `value_count` — number of values
  - [x] `last_write_time` — timestamp of last modification
  - [x] `flags` — access control flags
- [x] Define `reg_value_t` struct:
  - [x] `name[256]` — value name (empty string = default value)
  - [x] `type` — `REG_*` type code
  - [x] `data[REGISTRY_MAX_VALUE_SIZE]` — value data buffer
  - [x] `data_size` — actual bytes used
  - [x] `next` — linked list pointer
- [x] Define `HKEY` as opaque handle type (internally: pointer + access mode)
- [x] Define static pools: `key_pool[512]`, `value_pool[1024]`
- [x] Commit: `"registry: core data structures"` (`d44a791`)

### 1.2 Value Types

**Prompt:** Verify the Registry value type implementation. Confirm `registry.h` defines all Win32 type constants (`REG_NONE=0`, `REG_SZ=1`, `REG_EXPAND_SZ=2`, `REG_BINARY=3`, `REG_DWORD=4`, `REG_DWORD_BIG_ENDIAN=5`, `REG_LINK=6`, `REG_MULTI_SZ=7`, `REG_QWORD=11`) plus alias `REG_DWORD_LITTLE_ENDIAN`. Confirm `REG_FLAG_LINK=0x04` flag defined for symbolic link keys. Confirm declarations for: `reg_type_name()`, `reg_expand_sz()`, `reg_multi_sz_count/get/pack()`, `reg_key_is_link()`, `reg_key_get_link_target()`. In `registry.c`, verify `reg_expand_sz()` parses `%VAR%` tokens and looks up values under `HKLM\System\Environment` via case-insensitive tree walk. Verify `reg_multi_sz_count()` counts strings by scanning for nulls with double-null termination. Verify `reg_multi_sz_get()` returns the Nth string by index. Verify `reg_multi_sz_pack()` packs an array of C strings into double-null format. Verify `reg_key_is_link()` checks `REG_FLAG_LINK` and `reg_key_get_link_target()` finds the unnamed `REG_LINK` value. Run `bash scripts/build.sh clean` and confirm zero warnings. Verify `docs/architecture/registry.md` documents value types and helpers.

- [x] Define type constants matching Windows:
  - [x] `REG_NONE        = 0`
  - [x] `REG_SZ          = 1` — null-terminated string
  - [x] `REG_EXPAND_SZ   = 2` — string with `%VAR%` expansion
  - [x] `REG_BINARY      = 3` — raw binary data
  - [x] `REG_DWORD       = 4` — 32-bit integer (little-endian)
  - [x] `REG_MULTI_SZ    = 7` — double-null-terminated string array
  - [x] `REG_QWORD       = 11` — 64-bit integer
  - [x] `REG_LINK        = 6` — symbolic link to another key
- [x] Implement `REG_EXPAND_SZ` expansion (resolve `%PATH%` etc. on read)
- [x] Implement `REG_MULTI_SZ` pack/unpack helpers
- [x] Implement `REG_LINK` transparent redirection on `RegOpenKeyEx`
- [x] Commit: `"registry: value types"`

### 1.3 Predefined Root Keys

**Prompt:** Verify the predefined root key implementation. Confirm `registry_init()` creates 5 root keys (HKLM, HKCU, HKCR, HKU, HKCC) and sets `REG_FLAG_HKCU_REDIRECT` on HKCU and `REG_FLAG_HKCR_MERGED` on HKCR. Confirm default sub-keys: `HKLM\SYSTEM`, `HKLM\SOFTWARE`, `HKLM\HARDWARE`, `HKLM\SOFTWARE\Classes`, `HKU\Default`. Verify `reg_add_child()` inserts via FNV-1a bucket, `reg_find_child()` does case-insensitive lookup, `reg_create_child()` returns existing or allocates new. Verify `reg_set_current_user()` stores username, `reg_resolve_hkcu()` returns `HKU\{user}` (auto-creates if missing), `reg_resolve_hkcr()` returns `HKLM\SOFTWARE\Classes`. Verify boot log shows `Registry initialized (pool: X/512 keys, 0/1024 values)`. Run `bash scripts/build.sh clean` and confirm zero warnings.

- [x] Create predefined root key handles:
  - [x] `HKEY_LOCAL_MACHINE` (HKLM) — system-wide config
  - [x] `HKEY_CURRENT_USER` (HKCU) — current user (redirects to HKU\{user})
  - [x] `HKEY_USERS` (HKU) — all user profiles
  - [x] `HKEY_CLASSES_ROOT` (HKCR) — merged file associations view
- [x] Create default sub-keys under HKLM:
  - [x] `HKLM\SYSTEM` — boot config, drivers, services
  - [x] `HKLM\SOFTWARE` — installed software settings
  - [x] `HKLM\HARDWARE` — detected hardware info
- [x] Create default user profile: `HKU\Default`
- [x] Implement HKCU → HKU\{username} redirection
- [x] Implement HKCR merged view (HKLM\SOFTWARE\Classes + HKCU\SOFTWARE\Classes)
- [x] Commit: `"registry: root keys (HKLM, HKCU, HKU, HKCR)"`

---

## 2. Win32-Compatible API

### 2.1 Key Operations

**Prompt:** Verify the Win32-compatible key operations implementation. Confirm `registry.h` declares `RegOpenKeyEx`, `RegCreateKeyEx`, `RegCloseKey`, `RegDeleteKey`, `RegDeleteTree` with correct Win32 signatures. Confirm `REG_HANDLE_POOL_SIZE=128`, `REG_CREATED_NEW_KEY=1`, `REG_OPENED_EXISTING_KEY=2`. In `registry.c`, verify: handle pool (`reg_handle_pool[128]`, `reg_handle_used[128]`), `reg_alloc_handle`/`reg_free_handle` for handle lifecycle, `reg_is_predefined` checks sentinel range `0x80000000–0x80000005`, `reg_resolve_key` handles HKCU→`reg_resolve_hkcu()` and HKCR→`reg_resolve_hkcr()` redirection. Verify `reg_walk_path` walks backslash-separated paths with `REG_LINK` following. Verify `RegOpenKeyEx` returns `ERROR_FILE_NOT_FOUND` for missing keys. Verify `RegCreateKeyEx` sets disposition and updates `parent->last_write_time`. Verify `RegCloseKey` is no-op for predefined handles. Verify `RegDeleteKey` returns `ERROR_ACCESS_DENIED` if child_count>0. Verify `RegDeleteTree` recursively deletes via `reg_delete_subtree`. Run `bash scripts/build.sh clean` and confirm zero warnings.

- [x] Implement `RegOpenKeyEx(hKey, subKey, options, access, &result)`:
  - [x] Walk backslash-separated path from hKey
  - [x] Handle `REG_LINK` transparent redirection
  - [x] Store access mode in returned handle
  - [x] Return `ERROR_FILE_NOT_FOUND` if key doesn't exist
- [x] Implement `RegCreateKeyEx(hKey, subKey, reserved, class, options, access, security, &result, &disposition)`:
  - [x] Create intermediate keys as needed
  - [x] Set disposition: `REG_CREATED_NEW_KEY` or `REG_OPENED_EXISTING_KEY`
  - [x] Update parent's last-write time
- [x] Implement `RegCloseKey(hKey)` — release handle resources
- [x] Implement `RegDeleteKey(hKey, subKey)` — delete key + values (not children)
- [x] Implement `RegDeleteTree(hKey, subKey)` — recursive delete
- [x] Define error codes:
  - [x] `ERROR_SUCCESS          = 0`
  - [x] `ERROR_FILE_NOT_FOUND   = 2`
  - [x] `ERROR_ACCESS_DENIED    = 5`
  - [x] `ERROR_INVALID_HANDLE   = 6`
  - [x] `ERROR_MORE_DATA        = 234`
  - [x] `ERROR_NO_MORE_ITEMS    = 259`
- [x] Commit: `"registry: key operations (open, create, close, delete)"`

### 2.2 Value Operations

**Prompt:** Verify the Win32 value operations implementation. Confirm `registry.h` declares `RegSetValueEx`, `RegQueryValueEx`, `RegGetValue`, `RegDeleteValue` with correct Win32 signatures, and `RRF_*` flags (`RRF_RT_REG_SZ`, `RRF_RT_REG_EXPAND_SZ`, `RRF_RT_REG_BINARY`, `RRF_RT_REG_DWORD`, `RRF_RT_REG_QWORD`, `RRF_RT_ANY`, `RRF_NOEXPAND`). In `registry.c`, verify: `reg_find_value_in_key` matches empty name for default values. `RegSetValueEx` creates or updates values, stores data via `reg_memcpy`, updates `last_write_time`. `RegQueryValueEx` returns `ERROR_MORE_DATA` when `*lpcbData < v->data_size`, returns size when `lpData=NULL`. `RegGetValue` walks subkey, filters by `RRF_RT_*` type flags, auto-expands `REG_EXPAND_SZ` via `reg_expand_sz` unless `RRF_NOEXPAND`, sets `*pdwType=REG_SZ` after expansion. `RegDeleteValue` unlinks value from chain and decrements `value_count`. Run `bash scripts/build.sh clean` and confirm zero warnings.

- [x] Implement `RegSetValueEx(hKey, valueName, reserved, type, data, dataSize)`:
  - [x] Create value if it doesn't exist, update if it does
  - [x] Support all `REG_*` types
  - [x] Handle NULL/empty valueName as "(Default)" value
  - [x] Mark hive as dirty
  - [x] Update key's `last_write_time`
- [x] Implement `RegQueryValueEx(hKey, valueName, reserved, &type, data, &dataSize)`:
  - [x] Return `ERROR_MORE_DATA` if buffer too small (set required size)
  - [x] Return `ERROR_FILE_NOT_FOUND` if value doesn't exist
  - [x] If `data` is NULL, just return the required size
- [x] Implement `RegGetValue(hKey, subKey, valueName, flags, &type, data, &dataSize)`:
  - [x] Combines open + query in one call
  - [x] `RRF_RT_REG_SZ` flag: auto-expand `REG_EXPAND_SZ`
  - [x] `RRF_NOEXPAND` flag: return unexpanded string
- [x] Implement `RegDeleteValue(hKey, valueName)` — remove named value
- [x] Commit: `"registry: value operations (get, set, delete)"`

### 2.3 Enumeration

**Prompt:** Verify the enumeration implementation. Confirm `registry.h` declares `RegEnumKeyEx`, `RegEnumValue`, `RegQueryInfoKey` with correct Win32 signatures. In `registry.c`, verify `reg_get_child_by_index` scans all 16 hash buckets linearly to map 0-based index to child key. Verify `reg_get_value_by_index` walks the value linked list. Verify `RegEnumKeyEx` returns child name and `last_write_time`, `ERROR_NO_MORE_ITEMS` when index out of range, `ERROR_MORE_DATA` if name buffer too small. Verify `RegEnumValue` returns value name, type, and data with same error handling. Verify `RegQueryInfoKey` returns `child_count`, `value_count`, `last_write_time`, and computes max sub-key name length and max value name/data sizes by iterating. Run `bash scripts/build.sh clean` and confirm zero warnings.

- [x] Implement `RegEnumKeyEx(hKey, index, name, &nameSize, ...)`:
  - [x] Return child key name at given index
  - [x] Return `ERROR_NO_MORE_ITEMS` when index out of range
  - [x] Fill `lastWriteTime` from key metadata
- [x] Implement `RegEnumValue(hKey, index, name, &nameSize, reserved, &type, data, &dataSize)`:
  - [x] Return value name, type, and data at given index
  - [x] Return `ERROR_NO_MORE_ITEMS` when index out of range
  - [x] Return `ERROR_MORE_DATA` if data buffer too small
- [x] Implement `RegQueryInfoKey(hKey, ...)`:
  - [x] Return: sub-key count, max sub-key name length
  - [x] Return: value count, max value name length, max value data size
  - [x] Return: last write time
- [x] Commit: `"registry: enumeration (keys, values, info)"`

### 2.4 Convenience Helpers

**Prompt:** Verify the typed convenience helpers. Confirm `registry.h` declares `RegGetDword/RegSetDword`, `RegGetString/RegSetString`, `RegGetQword/RegSetQword`, and `RegReadKeyValue`. In `registry.c`, verify `RegGetDword` calls `RegQueryValueEx` with `sizeof(uint32_t)` and validates `type==REG_DWORD`. Verify `RegGetString` accepts both `REG_SZ` and `REG_EXPAND_SZ`. Verify `RegSetString` includes the null terminator in size (`reg_strlen+1`). Verify `RegReadKeyValue` performs `RegOpenKeyEx` + `RegQueryValueEx` + `RegCloseKey` in sequence, properly closing the handle even on query failure. Run `bash scripts/build.sh clean` and confirm zero warnings.

- [x] `RegGetDword(hKey, valueName, &value)` — read `REG_DWORD`
- [x] `RegSetDword(hKey, valueName, value)` — write `REG_DWORD`
- [x] `RegGetString(hKey, valueName, buf, bufSize)` — read `REG_SZ`
- [x] `RegSetString(hKey, valueName, str)` — write `REG_SZ`
- [x] `RegGetQword(hKey, valueName, &value)` — read `REG_QWORD`
- [x] `RegSetQword(hKey, valueName, value)` — write `REG_QWORD`
- [x] `RegReadKeyValue(root, path, valueName, type, buf, size)` — one-shot open+read+close
- [x] Commit: `"registry: convenience helpers"`

---

## 3. Codex → Registry Migration

> **Note:** The Registry implementation (`registry.h`, `registry.c`) was built as new files
> alongside the existing Codex system (§1–2). This section covers migrating all Codex
> call sites to the new Registry API, re-mapping default values to Win32 paths, and
> deleting the old Codex code.

### 3.1 Replace Codex API Calls with Registry API

**Prompt:** Verify the Codex → Registry migration. Confirm `main.c` includes `registry.h` and calls `registry_init()` (no `codex_init/load/populate_defaults/save`). Confirm `codex_flush()` is commented out pending §4. In `panic.c`, verify HKLM\SYSTEM\Recovery is accessed via `RegOpenKeyEx`/`RegGetDword`/`RegSetDword`/`RegCreateKeyEx`/`RegCloseKey`. In `swap.c`, verify HKLM\SYSTEM\Memory\SwapSlots uses `RegOpenKeyEx`/`RegGetDword`/`RegSetDword`/`RegCreateKeyEx`/`RegCloseKey`. In `desktop.c`, verify HKLM\SYSTEM\Theme uses `RegOpenKeyEx`/`RegGetString`/`RegCloseKey`. Confirm no remaining `#include "codex.h"` in any `.c` file except `codex.c` itself. Run `bash scripts/build.sh clean` and confirm zero warnings.

- [x] Create migration mapping:
  - [x] `codex_init()` → `registry_init()`
  - [x] `codex_open(path)` → `RegOpenKeyEx(root, path, ...)`
  - [x] `codex_create(path)` → `RegCreateKeyEx(root, path, ...)`
  - [x] `codex_get_string()` → `RegGetString()`
  - [x] `codex_get_int32()` → `RegGetDword()`
  - [x] `codex_get_bool()` → `RegGetDword()` (0/1)
  - [x] `codex_set_string()` → `RegSetString()`
  - [x] `codex_set_int32()` → `RegSetDword()`
  - [x] `codex_set_bool()` → `RegSetDword()` (0/1)
  - [x] `codex_save()` → `registry_flush()` (pending §4)
  - [x] `codex_load()` → `registry_load()` (pending §4)
- [x] Update `src/kernel/main.c` — init, populate defaults, flush
- [x] Update `src/kernel/panic.c` — AutoRestart setting
- [x] Update `src/kernel/mm/swap.c` — SwapSlots setting
- [x] Update `src/desktop/desktop.c` — theme, display, wallpaper
- [x] Update `include/icon_store.h` — no codex refs found (N/A)
- [x] Update all `#include "codex.h"` → `#include "registry.h"`
- [x] Commit: `"registry: migrate all codex call sites"`

### 3.2 Migrate Default Values to Registry Paths

**Prompt:** Verify the default value migration. In `registry.c`, confirm `registry_populate_defaults()` creates keys under `HKLM\SYSTEM` (Display, Theme, Shell, Network, DateTime, Recovery, Memory), `HKLM\HARDWARE` (CPU, Memory), and `HKU\Default` (root, Shell, Desktop). Confirm all `codex_set_*` calls are replaced with `RegSetDword/RegSetString/RegSetQword`. Verify CPU detection uses `reg_cpuid()` for vendor and brand strings. Verify memory stats use `pmm_get_total_frames()/pmm_get_free_frames()`. In `main.c`, confirm boot splash says "Loading registry..." and `registry_populate_defaults()` is called after `registry_init()`. Run `bash scripts/build.sh clean` and confirm zero warnings.

- [x] Map Codex paths to Registry paths:
  - [x] `System\Display\*` → `HKLM\SYSTEM\Display\*`
  - [x] `System\Theme\*` → `HKLM\SYSTEM\Theme\*`
  - [x] `System\Recovery\*` → `HKLM\SYSTEM\Recovery\*`
  - [x] `System\Memory\*` → `HKLM\SYSTEM\Memory\*`
  - [x] `System\Network\*` → `HKLM\SYSTEM\Network\*`
  - [x] `Hardware\CPU\*` → `HKLM\HARDWARE\CPU\*`
  - [x] `Hardware\Memory\*` → `HKLM\HARDWARE\Memory\*`
  - [x] `User\Default\*` → `HKU\Default\*`
  - [x] `Apps\*` → `HKLM\SOFTWARE\*` (no current values, path reserved)
- [x] Update `codex_populate_defaults()` → `registry_populate_defaults()`
- [x] Update boot splash: "Loading registry..."
- [x] Commit: `"registry: migrate default values to Win32 paths"`

### 3.3 Delete Old Codex Code

**Prompt:** Verify the Codex cleanup. Confirm `include/codex.h` and `src/kernel/codex.c` no longer exist. Grep for `codex_` in all `.c` and `.h` files — only architectural comments in `registry.h` should remain. Confirm the Legacy Codex section in `docs/architecture/registry.md` has been replaced with a tombstone note. Confirm stale "Codex" comments in `panic.h`, `icon_store.h`, and `desktop.c` have been updated to say "Registry". Run `bash scripts/build.sh clean` and confirm zero warnings.

- [x] Delete `include/codex.h`
- [x] Delete `src/kernel/codex.c`
- [x] Grep codebase for any remaining `codex_` references
- [x] Remove Codex "Legacy" section from `docs/architecture/registry.md`
- [x] Clean stale Codex comments in `panic.h`, `icon_store.h`, `desktop.c`
- [x] Commit: `"registry: remove legacy codex code"`


---

## 4. Disk Persistence (Hive Files)

### 4.1 Hive File Format

**Prompt:** Verify the hive file format implementation. In `registry.h`, confirm `hive_header_t` is defined as a packed struct with `magic` (HIVE_MAGIC = 0x48474552), `version` (1), `checksum` (CRC32), `timestamp`, `root_name[64]`, `total_keys`, `total_values`, `data_offset`, `data_size`, and `padding` to 4096 bytes. Confirm `hive_save` and `hive_load` are declared. In `registry.c`, confirm `hive_crc32` implements table-less CRC32 with polynomial 0xEDB88320. Confirm `hive_serialize_key` writes depth-first key records as `[name_len:u16][name:N][value_count:u16][child_count:u16]` and value records as `[name_len:u16][name:N][type:u32][data_size:u32][data:N]`. Confirm `hive_save` uses PMM for the buffer, fills the header, computes CRC32, and writes via VFS. Confirm `hive_load` validates magic, version, and CRC32, and returns -1 with a klog warning on corrupt files. Run `bash scripts/build.sh clean` and confirm zero warnings.

- [x] Define hive file header struct (4096 bytes):
  - [x] Magic: `"REGH"` (4 bytes)
  - [x] Version: `1` (uint32)
  - [x] Checksum: CRC32 of header (uint32)
  - [x] Timestamp: PIT ticks at save time (uint64)
  - [x] Root key name (64 bytes)
  - [x] Total key count (uint32)
  - [x] Total value count (uint32)
  - [x] Reserved padding to 4096 bytes
- [x] Implement `hive_save(root_key, filepath)` — serialize tree to file
- [x] Implement `hive_load(filepath, &root_key)` — deserialize file into tree
- [x] Add CRC32 checksum validation on load
- [x] Handle corrupt hive: log warning, skip file, use defaults
- [x] Commit: `"registry: hive file format"`

### 4.2 Hive File Layout on Disk

**Prompt:** Verify the hive file disk layout. In `registry.h`, confirm `REG_HIVE_DIR` is `"C:\Impossible\System\Config\Registry"` and `REG_HIVE_COUNT` is 4. Confirm `registry_flush()`, `registry_save_all()`, and `registry_load_hives()` are declared. In `registry.c`, confirm `hive_table` maps 4 descriptors: SYSTEM.hive→HKLM\SYSTEM, SOFTWARE.hive→HKLM\SOFTWARE, HARDWARE.hive→HKLM\HARDWARE, DEFAULT.hive→HKU\Default. Confirm `registry_mark_dirty()` walks up the parent chain to mark the correct hive dirty. Confirm it's called from `RegSetValueEx` and `RegDeleteValue`. Confirm `registry_flush()` only writes dirty hives. Confirm `hive_ensure_dir()` creates the directory chain. In `main.c`, confirm `registry_flush()` is enabled (not commented out). Run `bash scripts/build.sh clean` and confirm zero warnings.

- [x] Define hive file paths:
  - [x] `C:\Impossible\System\Config\Registry\SYSTEM.hive` → HKLM\SYSTEM
  - [x] `C:\Impossible\System\Config\Registry\SOFTWARE.hive` → HKLM\SOFTWARE
  - [x] `C:\Impossible\System\Config\Registry\HARDWARE.hive` → HKLM\HARDWARE
  - [x] `C:\Impossible\System\Config\Registry\DEFAULT.hive` → HKU\Default
- [x] Create Registry directory at first boot if missing
- [x] Implement dirty-flag tracking per hive
- [x] Implement `registry_flush()` — write only dirty hives
- [x] Implement `registry_save_all()` — for clean shutdown
- [x] Hook dirty tracking into `RegSetValueEx` and `RegDeleteValue`
- [x] Enable `registry_flush()` in `main.c` compositor loop
- [x] Commit: `"registry: hive file disk layout"`

### 4.3 Crash-Safe Journaling

**Prompt:** Verify crash-safe journaling. In `registry.c`, confirm `hive_save` follows the 4-step sequence: (1) write `.hive.log`, (2) copy `.hive` → `.hive.bak`, (3) overwrite `.hive`, (4) invalidate `.hive.log` by zeroing magic. Confirm `hive_validate_file` checks magic, version, and CRC32. Confirm `hive_best_source` checks `.hive.log` → `.hive` → `.hive.bak` in priority order and copies the best source to `.hive`. Confirm `registry_load_hives` calls `hive_best_source` for each hive before loading. Confirm `hive_copy_file` does a byte-by-byte copy via VFS. Run `bash scripts/build.sh clean` and confirm zero warnings.

- [x] Before saving: write new data to `.hive.log` first (journal)
- [x] After log + main hive written: invalidate `.hive.log` by zeroing magic
- [x] On boot: if `.hive.log` has valid header, replay it (crash recovery via `hive_best_source`)
- [x] On boot: if `.hive` is corrupt (bad CRC), fall back to `.hive.bak`
- [x] Keep one backup: copy old `.hive` → `.hive.bak` before overwriting
- [x] Commit: `"registry: crash-safe journaling"`

---

## 5. Change Notifications

### 5.1 Registry Watchers

**Prompt:** Implement `RegNotifyChangeKeyValue` so applications can watch for registry changes without polling. This is how Windows apps detect settings changes in real time — for example, the desktop compositor watches `HKCU\Software\Impossible\Theme\DarkMode` and switches themes instantly. Internally, maintain a linked list of "watcher" structs, each containing the watched key path, filter flags (`REG_NOTIFY_CHANGE_NAME` for key add/delete, `REG_NOTIFY_CHANGE_LAST_SET` for value changes), and a callback function pointer. When `RegSetValueEx`, `RegCreateKeyEx`, or `RegDeleteKey` modifies a watched key, fire all matching watchers. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, run `bash scripts/build.sh clean`, and commit as `"registry: change notifications"`. Update `README.md` if it contains stale or incorrect references to registry notifications. Create or update documentation in `docs/` covering the watcher API, filter flags, and callback dispatch.

- [ ] Define watcher struct (key path, filter, callback, user context)
- [ ] Implement `RegNotifyChangeKeyValue(hKey, watchSubtree, filter, callback, ctx)`:
  - [ ] `REG_NOTIFY_CHANGE_NAME` — fires on sub-key create/delete
  - [ ] `REG_NOTIFY_CHANGE_LAST_SET` — fires on value change
  - [ ] `watchSubtree` — also watch all descendant keys
- [ ] Fire watchers from `RegSetValueEx`, `RegCreateKeyEx`, `RegDeleteKey`
- [ ] `RegUnregisterNotify(watcherId)` — remove a watcher
- [ ] Test: desktop theme change detected via watcher
- [ ] Commit: `"registry: change notifications"`

---

## 6. Syscalls & User-Mode Access

### 6.1 Registry Syscalls

**Prompt:** Expose the registry to user-mode applications via syscalls. User apps need to store settings (window positions, preferences, recent files). Add syscalls that wrap the kernel Registry API: `SYS_REG_OPEN`, `SYS_REG_CREATE`, `SYS_REG_CLOSE`, `SYS_REG_QUERY`, `SYS_REG_SET`, `SYS_REG_DELETE_KEY`, `SYS_REG_DELETE_VALUE`, `SYS_REG_ENUM_KEY`, `SYS_REG_ENUM_VALUE`. Each syscall validates user pointers before accessing them. User-mode apps can only write to `HKCU` and `HKLM\SOFTWARE` — writes to `HKLM\SYSTEM` and `HKLM\HARDWARE` require kernel privilege. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, run `bash scripts/build.sh clean`, and commit as `"registry: user-mode syscalls"`. Update `README.md` if it contains stale or incorrect references to registry syscalls. Create or update documentation in `docs/` covering the registry syscall numbers, pointer validation, and access control policy.

- [ ] Add syscalls:
  - [ ] `SYS_REG_OPEN(root, path, access, &handle)`
  - [ ] `SYS_REG_CREATE(root, path, access, &handle, &disposition)`
  - [ ] `SYS_REG_CLOSE(handle)`
  - [ ] `SYS_REG_QUERY(handle, valueName, &type, data, &size)`
  - [ ] `SYS_REG_SET(handle, valueName, type, data, size)`
  - [ ] `SYS_REG_DELETE_KEY(handle, subKey)`
  - [ ] `SYS_REG_DELETE_VALUE(handle, valueName)`
  - [ ] `SYS_REG_ENUM_KEY(handle, index, name, &nameSize)`
  - [ ] `SYS_REG_ENUM_VALUE(handle, index, name, &nameSize, &type, data, &dataSize)`
- [ ] Validate user pointers in all syscalls
- [ ] Access control: user apps can write HKCU + HKLM\SOFTWARE only
- [ ] Add user-mode wrapper functions in `user/lib/registry.c`
- [ ] Commit: `"registry: user-mode syscalls"`

---

## 7. Win32 Compatibility Layer
> *Merged from Phase 03 §1.5 (Win32 Registry Mapping)*

### 7.1 advapi32.dll Registry Stubs

**Prompt:** Windows apps access the registry via `RegOpenKeyExA/W` / `RegQueryValueExA/W` / `RegSetValueExA/W` — these must be wrapped as A/W (ANSI/Wide) variants and registered in the `advapi32.dll` builtin stub table in the Win32 compatibility layer (Phase 10). The hive paths (`HKEY_LOCAL_MACHINE`, `HKEY_CURRENT_USER`, etc.) map directly to the native Registry root keys. Each Win32 registry type maps 1:1 to our `REG_*` types. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"win32: registry API stubs"`. Create or update documentation in `docs/` covering the Win32-to-native registry mapping.

- [ ] Map Windows hives to Registry paths:
  - [ ] `HKEY_LOCAL_MACHINE\SOFTWARE` → `HKLM\SOFTWARE`
  - [ ] `HKEY_LOCAL_MACHINE\HARDWARE` → `HKLM\HARDWARE`
  - [ ] `HKEY_CURRENT_USER` → `HKCU` (redirects to `HKU\{user}`)
  - [ ] `HKEY_CURRENT_USER\Software\{App}` → `HKCU\Software\{App}`
  - [ ] `HKEY_CLASSES_ROOT` → `HKCR` (merged view)
- [ ] Implement `RegOpenKeyExA/W` → `RegOpenKeyEx(mapped_root, mapped_path, ...)`
- [ ] Implement `RegCreateKeyExA/W` → `RegCreateKeyEx(mapped_root, mapped_path, ...)`
- [ ] Implement `RegQueryValueExA/W` → `RegQueryValueEx()` with type mapping
- [ ] Implement `RegSetValueExA/W` → `RegSetValueEx()`
- [ ] Implement `RegDeleteKeyA/W`, `RegDeleteValueA/W`
- [ ] Implement `RegEnumKeyExA/W`, `RegEnumValueA/W`
- [ ] `RegCloseKey` → `RegCloseKey()`
- [ ] Add to `advapi32.dll` builtin stub table
- [ ] Commit: `"win32: registry API stubs"`

### 7.2 File Associations (HKCR)

> **See [TODO-240-Resources.md](TODO-240-Resources.md) §1** — File type icon mapping, extension-to-app mapping, default associations.

---

## 8. Tools & Debugging

### 8.1 Regedit Shell Command

**Prompt:** Add a `regedit` shell command for inspecting and modifying the registry from the command line. Subcommands: `regedit list HKLM\SYSTEM` (list sub-keys), `regedit query HKLM\SYSTEM\Display Width` (read a value), `regedit set HKLM\SYSTEM\Display Width REG_DWORD 1920` (write a value), `regedit delete HKLM\SYSTEM\OldKey` (delete a key), `regedit export HKLM\SYSTEM output.reg` (export as text), `regedit tree HKLM` (show full tree). After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, run `bash scripts/build.sh clean`, and commit as `"shell: regedit command"`. Update `README.md` if it contains stale or incorrect references to registry tools. Create or update documentation in `docs/` covering the regedit command syntax, subcommands, and .reg export format.

- [ ] `regedit list <path>` — list sub-keys and values
- [ ] `regedit query <path> <valueName>` — read and display a value
- [ ] `regedit set <path> <valueName> <type> <data>` — write a value
- [ ] `regedit delete <path>` — delete a key or value
- [ ] `regedit tree <path>` — recursive tree display
- [ ] `regedit export <path> <file>` — export sub-tree as `.reg` text file
- [ ] Commit: `"shell: regedit command"`

---

## 9. Performance Optimizations (Future)

### 9.1 Hash Map Child Lookup

- [ ] Replace linked-list children with FNV-1a hash map
- [ ] Target: O(1) key lookup instead of O(n) linear scan
- [ ] Resize hash table when load factor exceeds 0.75

### 9.2 Memory-Mapped Hive Files

- [ ] `mmap` hive files directly (requires Phase 01 §3.2 mmap)
- [ ] Zero-copy reads — keys/values point directly into mapped pages
- [ ] Dirty page tracking — only write changed pages on flush

### 9.3 B-Tree Cell Format

- [ ] Replace flat sequential format with B-tree bins and cells
- [ ] Random-access reads without loading entire hive into RAM
- [ ] Matches Windows NT hive internals for maximum compatibility

---

## Priority Order

| Priority | Section                   | Description                                        |
|----------|---------------------------|----------------------------------------------------|
| 🔴 P0     | 3.1 Rename Files          | Codex → Registry file rename (unblocks everything) |
| 🔴 P0     | 3.2 Replace API           | Migrate all Codex call sites to Registry API       |
| 🔴 P0     | 1.1 Data Structures       | Registry engine foundation                         |
| 🔴 P0     | 1.2 Value Types           | Must support all REG_* types                       |
| 🔴 P0     | 1.3 Root Keys             | HKLM, HKCU, HKU, HKCR                              |
| 🔴 P0     | 2.1 Key Operations        | Core API: open, create, close, delete              |
| 🔴 P0     | 2.2 Value Operations      | Core API: get, set, delete values                  |
| 🔴 P0     | 3.3 Migrate Defaults      | Re-map Codex defaults to Win32 paths               |
| 🟠 P1     | 2.3 Enumeration           | Needed for regedit + iteration                     |
| 🟠 P1     | 2.4 Convenience Helpers   | Simplify common access patterns                    |
| 🟠 P1     | 4.1 Hive File Format      | Binary disk persistence                            |
| 🟠 P1     | 4.2 Disk Layout           | File paths + auto-flush                            |
| 🟡 P2     | 5.1 Change Notifications  | Real-time settings updates                         |
| 🟡 P2     | 6.1 Syscalls              | User-mode app access                               |
| 🟡 P2     | 7.1 Win32 Stubs           | advapi32.dll registry wrappers                     |
| 🟡 P2     | 8.1 Regedit Command       | Debugging + inspection                             |
| 🟢 P3     | 4.3 Crash-Safe Journaling | Power-loss protection                              |
| 🔵 P4     | 9.1 Hash Map Lookup       | O(1) performance                                   |
| 🔵 P4     | 9.2 Memory-Mapped Hives   | Zero-copy reads                                    |
| 🔵 P4     | 9.3 B-Tree Format         | Windows NT hive compat                             |
