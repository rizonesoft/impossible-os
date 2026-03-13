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

**Prompt:** Define the core data structures for the Registry. A registry key (`reg_key_t`) has a name (max 255 chars), parent pointer, child hash map (FNV-1a based, for O(1) lookup), values list, last-write timestamp (PIT ticks), and security flags. A registry value (`reg_value_t`) has a name, type (`REG_*` enum), data buffer, and data size. Registry handles (`HKEY`) are opaque pointers to open key references with access-mode tracking. Use static pools initially (512 keys, 1024 values) like the current Codex, but design the structures so they can later be backed by memory-mapped hive files. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, run `bash scripts/build.sh clean`, and commit as `"registry: core data structures"`. Update `README.md` if it contains stale or incorrect references to the Codex or registry. Create or update documentation in `docs/` covering the reg_key_t and reg_value_t structs, HKEY handle system, and static pool sizes.

- [ ] Define `reg_key_t` struct:
  - [ ] `name[256]` — key name
  - [ ] `parent` pointer — parent key
  - [ ] `children` — hash map of child keys (FNV-1a hash → `reg_key_t*`)
  - [ ] `child_count` — number of child keys
  - [ ] `values` — linked list of `reg_value_t`
  - [ ] `value_count` — number of values
  - [ ] `last_write_time` — timestamp of last modification
  - [ ] `flags` — access control flags
- [ ] Define `reg_value_t` struct:
  - [ ] `name[256]` — value name (empty string = default value)
  - [ ] `type` — `REG_*` type code
  - [ ] `data[REGISTRY_MAX_VALUE_SIZE]` — value data buffer
  - [ ] `data_size` — actual bytes used
  - [ ] `next` — linked list pointer
- [ ] Define `HKEY` as opaque handle type (internally: pointer + access mode)
- [ ] Define static pools: `key_pool[512]`, `value_pool[1024]`
- [ ] Commit: `"registry: core data structures"`

### 1.2 Value Types

**Prompt:** Implement all standard Windows Registry value types. `REG_SZ` is a null-terminated UTF-8 string. `REG_EXPAND_SZ` is a string containing `%VARIABLE%` tokens that are expanded on read using the environment variable system (Phase 01 §7). `REG_MULTI_SZ` is an array of strings, each null-terminated, with a final double-null terminator. `REG_DWORD` (alias `REG_DWORD_LITTLE_ENDIAN`) is a 32-bit unsigned integer in little-endian. `REG_QWORD` is a 64-bit unsigned integer. `REG_BINARY` is raw bytes. `REG_NONE` indicates no type. `REG_LINK` is a Unicode string naming a symbolic link target key path — when a key with `REG_LINK` is opened, the Registry transparently redirects to the target. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, run `bash scripts/build.sh clean`, and commit as `"registry: value types"`. Update `README.md` if it contains stale or incorrect references to registry value types. Create or update documentation in `docs/` covering all REG_* types, expansion semantics, and MULTI_SZ encoding.

- [ ] Define type constants matching Windows:
  - [ ] `REG_NONE        = 0`
  - [ ] `REG_SZ          = 1` — null-terminated string
  - [ ] `REG_EXPAND_SZ   = 2` — string with `%VAR%` expansion
  - [ ] `REG_BINARY      = 3` — raw binary data
  - [ ] `REG_DWORD       = 4` — 32-bit integer (little-endian)
  - [ ] `REG_MULTI_SZ    = 7` — double-null-terminated string array
  - [ ] `REG_QWORD       = 11` — 64-bit integer
  - [ ] `REG_LINK        = 6` — symbolic link to another key
- [ ] Implement `REG_EXPAND_SZ` expansion (resolve `%PATH%` etc. on read)
- [ ] Implement `REG_MULTI_SZ` pack/unpack helpers
- [ ] Implement `REG_LINK` transparent redirection on `RegOpenKeyEx`
- [ ] Commit: `"registry: value types"`

### 1.3 Predefined Root Keys

**Prompt:** Create the predefined root key handles that match Windows. `HKEY_LOCAL_MACHINE` (HKLM) contains system-wide hardware and software configuration — its children are `SYSTEM`, `SOFTWARE`, `HARDWARE`. `HKEY_CURRENT_USER` (HKCU) is a per-user view that redirects to `HKEY_USERS\{current_username}`. `HKEY_USERS` (HKU) contains a sub-key for each user profile. `HKEY_CLASSES_ROOT` (HKCR) is a merged view of `HKLM\SOFTWARE\Classes` and `HKCU\SOFTWARE\Classes` — this is where file associations live (`.txt → notepad`, `.jpg → image viewer`). These root handles are global constants, pre-allocated at init time, and never closed. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, run `bash scripts/build.sh clean`, and commit as `"registry: root keys (HKLM, HKCU, HKU, HKCR)"`. Update `README.md` if it contains stale or incorrect references to Codex or registry root keys. Create or update documentation in `docs/` covering predefined root keys, HKCU redirection, and HKCR merged view.

- [ ] Create predefined root key handles:
  - [ ] `HKEY_LOCAL_MACHINE` (HKLM) — system-wide config
  - [ ] `HKEY_CURRENT_USER` (HKCU) — current user (redirects to HKU\{user})
  - [ ] `HKEY_USERS` (HKU) — all user profiles
  - [ ] `HKEY_CLASSES_ROOT` (HKCR) — merged file associations view
- [ ] Create default sub-keys under HKLM:
  - [ ] `HKLM\SYSTEM` — boot config, drivers, services
  - [ ] `HKLM\SOFTWARE` — installed software settings
  - [ ] `HKLM\HARDWARE` — detected hardware info
- [ ] Create default user profile: `HKU\Default`
- [ ] Implement HKCU → HKU\{username} redirection
- [ ] Implement HKCR merged view (HKLM\SOFTWARE\Classes + HKCU\SOFTWARE\Classes)
- [ ] Commit: `"registry: root keys (HKLM, HKCU, HKU, HKCR)"`

---

## 2. Win32-Compatible API

### 2.1 Key Operations

**Prompt:** Implement the core Win32 registry key operations. `RegOpenKeyEx(hKey, subKey, options, access, &result)` opens a sub-key relative to `hKey`, walking the path by backslash separators, and returns a handle with the requested access mode. `RegCreateKeyEx` creates the key if it doesn't exist (and sets `*disposition` to `REG_CREATED_NEW_KEY` or `REG_OPENED_EXISTING_KEY`). `RegCloseKey` releases the handle. `RegDeleteKey` removes a key and all its values (but not child keys — that's `RegDeleteTree`). `RegDeleteTree` recursively deletes a key and all its children. Return codes follow Windows: `ERROR_SUCCESS (0)`, `ERROR_FILE_NOT_FOUND (2)`, `ERROR_ACCESS_DENIED (5)`, `ERROR_INVALID_HANDLE (6)`. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, run `bash scripts/build.sh clean`, and commit as `"registry: key operations (open, create, close, delete)"`. Update `README.md` if it contains stale or incorrect references to registry key operations. Create or update documentation in `docs/` covering the key operation API, error codes, and REG_LINK redirection.

- [ ] Implement `RegOpenKeyEx(hKey, subKey, options, access, &result)`:
  - [ ] Walk backslash-separated path from hKey
  - [ ] Handle `REG_LINK` transparent redirection
  - [ ] Store access mode in returned handle
  - [ ] Return `ERROR_FILE_NOT_FOUND` if key doesn't exist
- [ ] Implement `RegCreateKeyEx(hKey, subKey, reserved, class, options, access, security, &result, &disposition)`:
  - [ ] Create intermediate keys as needed
  - [ ] Set disposition: `REG_CREATED_NEW_KEY` or `REG_OPENED_EXISTING_KEY`
  - [ ] Update parent's last-write time
- [ ] Implement `RegCloseKey(hKey)` — release handle resources
- [ ] Implement `RegDeleteKey(hKey, subKey)` — delete key + values (not children)
- [ ] Implement `RegDeleteTree(hKey, subKey)` — recursive delete
- [ ] Define error codes:
  - [ ] `ERROR_SUCCESS          = 0`
  - [ ] `ERROR_FILE_NOT_FOUND   = 2`
  - [ ] `ERROR_ACCESS_DENIED    = 5`
  - [ ] `ERROR_INVALID_HANDLE   = 6`
  - [ ] `ERROR_MORE_DATA        = 234`
  - [ ] `ERROR_NO_MORE_ITEMS    = 259`
- [ ] Commit: `"registry: key operations (open, create, close, delete)"`

### 2.2 Value Operations

**Prompt:** Implement the Win32 value read/write operations. `RegSetValueEx(hKey, valueName, reserved, type, data, dataSize)` creates or updates a named value under `hKey`. If `valueName` is NULL or empty string, it sets the key's default "(Default)" value. `RegQueryValueEx(hKey, valueName, reserved, &type, data, &dataSize)` reads a value — if the buffer is too small, it sets `*dataSize` to the required size and returns `ERROR_MORE_DATA`. `RegGetValue` is a convenience wrapper that can auto-expand `REG_EXPAND_SZ` strings. `RegDeleteValue` removes a named value. Mark the containing root tree as dirty after every write. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, run `bash scripts/build.sh clean`, and commit as `"registry: value operations (get, set, delete)"`. Update `README.md` if it contains stale or incorrect references to registry value operations. Create or update documentation in `docs/` covering the value operation API, ERROR_MORE_DATA handling, and default values.

- [ ] Implement `RegSetValueEx(hKey, valueName, reserved, type, data, dataSize)`:
  - [ ] Create value if it doesn't exist, update if it does
  - [ ] Support all `REG_*` types
  - [ ] Handle NULL/empty valueName as "(Default)" value
  - [ ] Mark hive as dirty
  - [ ] Update key's `last_write_time`
- [ ] Implement `RegQueryValueEx(hKey, valueName, reserved, &type, data, &dataSize)`:
  - [ ] Return `ERROR_MORE_DATA` if buffer too small (set required size)
  - [ ] Return `ERROR_FILE_NOT_FOUND` if value doesn't exist
  - [ ] If `data` is NULL, just return the required size
- [ ] Implement `RegGetValue(hKey, subKey, valueName, flags, &type, data, &dataSize)`:
  - [ ] Combines open + query in one call
  - [ ] `RRF_RT_REG_SZ` flag: auto-expand `REG_EXPAND_SZ`
  - [ ] `RRF_NOEXPAND` flag: return unexpanded string
- [ ] Implement `RegDeleteValue(hKey, valueName)` — remove named value
- [ ] Commit: `"registry: value operations (get, set, delete)"`

### 2.3 Enumeration

**Prompt:** Implement key and value enumeration — required for listing all settings, iterating config, and building a `regedit` tool. `RegEnumKeyEx(hKey, index, name, &nameSize, reserved, class, &classSize, &lastWriteTime)` returns the name of the child key at `index` (0-based). Returns `ERROR_NO_MORE_ITEMS` when index exceeds child count. `RegEnumValue(hKey, index, name, &nameSize, reserved, &type, data, &dataSize)` returns the value at `index`. `RegQueryInfoKey` returns stats about a key: number of sub-keys, max sub-key name length, number of values, max value name length, max value data size, last write time. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, run `bash scripts/build.sh clean`, and commit as `"registry: enumeration (keys, values, info)"`. Update `README.md` if it contains stale or incorrect references to registry enumeration. Create or update documentation in `docs/` covering the enumeration API, index-based iteration, and RegQueryInfoKey output.

- [ ] Implement `RegEnumKeyEx(hKey, index, name, &nameSize, ...)`:
  - [ ] Return child key name at given index
  - [ ] Return `ERROR_NO_MORE_ITEMS` when index out of range
  - [ ] Fill `lastWriteTime` from key metadata
- [ ] Implement `RegEnumValue(hKey, index, name, &nameSize, reserved, &type, data, &dataSize)`:
  - [ ] Return value name, type, and data at given index
  - [ ] Return `ERROR_NO_MORE_ITEMS` when index out of range
  - [ ] Return `ERROR_MORE_DATA` if data buffer too small
- [ ] Implement `RegQueryInfoKey(hKey, ...)`:
  - [ ] Return: sub-key count, max sub-key name length
  - [ ] Return: value count, max value name length, max value data size
  - [ ] Return: last write time
- [ ] Commit: `"registry: enumeration (keys, values, info)"`

### 2.4 Convenience Helpers

**Prompt:** Add typed convenience wrappers that simplify common registry access patterns. These wrap `RegQueryValueEx`/`RegSetValueEx` with type-safe signatures: `RegGetDword(hKey, valueName, &dword)`, `RegSetDword(hKey, valueName, dword)`, `RegGetString(hKey, valueName, buf, bufSize)`, `RegSetString(hKey, valueName, str)`, `RegGetQword(hKey, valueName, &qword)`. Also add a one-shot `RegReadKeyValue(rootKey, path, valueName, type, buf, size)` that opens, reads, and closes in one call — this covers 80% of registry access patterns. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, run `bash scripts/build.sh clean`, and commit as `"registry: convenience helpers"`. Update `README.md` if it contains stale or incorrect references to registry helpers. Create or update documentation in `docs/` covering the typed convenience API and one-shot RegReadKeyValue pattern.

- [ ] `RegGetDword(hKey, valueName, &value)` — read `REG_DWORD`
- [ ] `RegSetDword(hKey, valueName, value)` — write `REG_DWORD`
- [ ] `RegGetString(hKey, valueName, buf, bufSize)` — read `REG_SZ`
- [ ] `RegSetString(hKey, valueName, str)` — write `REG_SZ`
- [ ] `RegGetQword(hKey, valueName, &value)` — read `REG_QWORD`
- [ ] `RegSetQword(hKey, valueName, value)` — write `REG_QWORD`
- [ ] `RegReadKeyValue(root, path, valueName, type, buf, size)` — one-shot open+read+close
- [ ] Commit: `"registry: convenience helpers"`

---

## 3. Codex → Registry Migration

### 3.1 Rename All Source Files

**Prompt:** Rename the Codex source files to Registry. `include/codex.h` becomes `include/registry.h`. `src/kernel/codex.c` becomes `src/kernel/registry.c`. Update the Makefile to compile `registry.c` instead of `codex.c`. All `#include "codex.h"` becomes `#include "registry.h"`. This is a mechanical rename — the internal implementation is rewritten in §3.2. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, run `bash scripts/build.sh clean`, and commit as `"registry: rename codex → registry (files)"`. Update `README.md` if it contains stale or incorrect references to Codex. Create or update documentation in `docs/` covering the Codex → Registry migration and file rename mapping.

- [ ] Rename `include/codex.h` → `include/registry.h`
- [ ] Rename `src/kernel/codex.c` → `src/kernel/registry.c`
- [ ] Update Makefile: compile `registry.c` instead of `codex.c`
- [ ] Update all `#include "codex.h"` → `#include "registry.h"`
- [ ] Commit: `"registry: rename codex → registry (files)"`

### 3.2 Replace Codex API with Registry API

**Prompt:** Replace all Codex API calls throughout the codebase with the new Win32-compatible Registry API. Map the old API to the new one: `codex_open("System\\Display")` → `RegOpenKeyEx(HKLM, "SYSTEM\\Display", ...)`, `codex_get_int32(key, "Width", &w)` → `RegGetDword(hKey, "Width", &w)`, `codex_set_string(key, "Theme", "dark")` → `RegSetString(hKey, "Theme", "dark")`. Update all call sites:

**Files to update:**
- `src/kernel/main.c` — boot-time registry init, populate defaults
- `src/kernel/panic.c` — read `HKLM\SYSTEM\Recovery\AutoRestart`
- `src/kernel/mm/swap.c` — read `HKLM\SYSTEM\Memory\SwapSlots`
- `src/desktop/desktop.c` — read theme, display, wallpaper settings
- `include/icon_store.h` — read icon cache settings
- Boot splash status text: "Loading system configuration..." stays

After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, run `bash scripts/build.sh clean`, and commit as `"registry: migrate all codex call sites"`. Update `README.md` if it contains stale or incorrect references to Codex API calls. Create or update documentation in `docs/` covering the API migration mapping and updated call sites.

- [ ] Create migration mapping:
  - [ ] `codex_init()` → `registry_init()`
  - [ ] `codex_open(path)` → `RegOpenKeyEx(root, path, ...)`
  - [ ] `codex_create(path)` → `RegCreateKeyEx(root, path, ...)`
  - [ ] `codex_get_string()` → `RegGetString()`
  - [ ] `codex_get_int32()` → `RegGetDword()`
  - [ ] `codex_get_bool()` → `RegGetDword()` (0/1)
  - [ ] `codex_set_string()` → `RegSetString()`
  - [ ] `codex_set_int32()` → `RegSetDword()`
  - [ ] `codex_set_bool()` → `RegSetDword()` (0/1)
  - [ ] `codex_save()` → `registry_flush()`
  - [ ] `codex_load()` → `registry_load()`
- [ ] Update `src/kernel/main.c` — init, populate defaults, flush
- [ ] Update `src/kernel/panic.c` — AutoRestart setting
- [ ] Update `src/kernel/mm/swap.c` — SwapSlots setting
- [ ] Update `src/desktop/desktop.c` — theme, display, wallpaper
- [ ] Update `include/icon_store.h` — icon cache settings
- [ ] Delete old Codex files after migration verified
- [ ] Commit: `"registry: migrate all codex call sites"`

### 3.3 Migrate Default Values to Registry Paths

**Prompt:** Re-map all current Codex default values to proper Windows-style registry paths. The current Codex uses flat paths like `System\Display\Width` — these should map to `HKLM\SYSTEM\Display\Width`. User preferences move from `User\Default\...` to `HKU\Default\...`. Application settings move from `Apps\...` to `HKLM\SOFTWARE\...`. Hardware detection values move from `Hardware\...` to `HKLM\HARDWARE\...`. The boot splash status text should say "Loading registry..." instead of "Loading system configuration...". After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, run `bash scripts/build.sh clean`, and commit as `"registry: migrate default values to Win32 paths"`. Update `README.md` if it contains stale or incorrect references to Codex value paths. Create or update documentation in `docs/` covering the Codex-to-Registry path mapping and default value population.

- [ ] Map Codex paths to Registry paths:
  - [ ] `System\Display\*` → `HKLM\SYSTEM\Display\*`
  - [ ] `System\Theme\*` → `HKCU\Software\Impossible\Theme\*`
  - [ ] `System\Recovery\*` → `HKLM\SYSTEM\Recovery\*`
  - [ ] `System\Memory\*` → `HKLM\SYSTEM\Memory\*`
  - [ ] `System\Network\*` → `HKLM\SYSTEM\Network\*`
  - [ ] `Hardware\CPU\*` → `HKLM\HARDWARE\CPU\*`
  - [ ] `Hardware\Display\*` → `HKLM\HARDWARE\Display\*`
  - [ ] `User\Default\*` → `HKU\Default\*`
  - [ ] `Apps\*` → `HKLM\SOFTWARE\*`
- [ ] Update `codex_populate_defaults()` → `registry_populate_defaults()`
- [ ] Update boot splash: "Loading registry..."
- [ ] Commit: `"registry: migrate default values to Win32 paths"`

---

## 4. Disk Persistence (Hive Files)

### 4.1 Hive File Format

**Prompt:** Design a binary hive file format for disk persistence. Each root tree is stored as a separate `.hive` file. The file starts with a 4096-byte header containing: magic (`"REGH"`), format version (1), checksum (CRC32 of header), timestamp, root key name, total key count, total value count. After the header, keys and values are serialized sequentially: each key is stored as `[name_len][name][value_count][child_count]`, each value as `[name_len][name][type][data_size][data]`. Child keys follow their parent (depth-first order). This flat format is simple to load (single pass read) but can be replaced with a B-tree cell format later for random access. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, run `bash scripts/build.sh clean`, and commit as `"registry: hive file format"`. Update `README.md` if it contains stale or incorrect references to registry persistence. Create or update documentation in `docs/` covering the hive file header, serialization format, and CRC32 validation.

- [ ] Define hive file header struct (4096 bytes):
  - [ ] Magic: `"REGH"` (4 bytes)
  - [ ] Version: `1` (uint32)
  - [ ] Checksum: CRC32 of header (uint32)
  - [ ] Timestamp: PIT ticks at save time (uint64)
  - [ ] Root key name (64 bytes)
  - [ ] Total key count (uint32)
  - [ ] Total value count (uint32)
  - [ ] Reserved padding to 4096 bytes
- [ ] Implement `hive_save(root_key, filepath)` — serialize tree to file
- [ ] Implement `hive_load(filepath, &root_key)` — deserialize file into tree
- [ ] Add CRC32 checksum validation on load
- [ ] Handle corrupt hive: log warning, skip file, use defaults
- [ ] Commit: `"registry: hive file format"`

### 4.2 Hive File Layout on Disk

**Prompt:** Store hive files in `C:\Impossible\System\Config\Registry\`. Each predefined root tree gets its own hive file. Create the directory structure at first boot if it doesn't exist. Auto-flush dirty hives every 2 seconds from the compositor loop or timer tick (same as current Codex flush). Add a `registry_flush()` function that writes only dirty hives. Add a `registry_save_all()` function for clean shutdown. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, run `bash scripts/build.sh clean`, and commit as `"registry: hive file disk layout"`. Update `README.md` if it contains stale or incorrect references to registry file paths. Create or update documentation in `docs/` covering the hive file directory structure, dirty-flag tracking, and flush intervals.

- [ ] Define hive file paths:
  - [ ] `C:\Impossible\System\Config\Registry\SYSTEM.hive` → HKLM\SYSTEM
  - [ ] `C:\Impossible\System\Config\Registry\SOFTWARE.hive` → HKLM\SOFTWARE
  - [ ] `C:\Impossible\System\Config\Registry\HARDWARE.hive` → HKLM\HARDWARE
  - [ ] `C:\Impossible\System\Config\Registry\DEFAULT.hive` → HKU\Default
- [ ] Create Registry directory at first boot if missing
- [ ] Implement dirty-flag tracking per hive
- [ ] Implement `registry_flush()` — write only dirty hives (2-second interval)
- [ ] Implement `registry_save_all()` — for clean shutdown
- [ ] Buffer writes during early boot (before VFS is mounted)
- [ ] Commit: `"registry: hive file disk layout"`

### 4.3 Crash-Safe Journaling

**Prompt:** Implement write-ahead journaling so a power loss during hive write doesn't corrupt the registry. Before writing the main hive file, write the new data to a `.hive.log` journal file first. If both files exist on boot, the journal is replayed to recover from an interrupted write. The sequence is: (1) write new hive to `.hive.log`, (2) fsync `.hive.log`, (3) rename `.hive.log` → `.hive` (atomic on most filesystems), (4) delete old `.hive.bak`. If step 3 fails, the next boot detects the `.log` file and replays it. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, run `bash scripts/build.sh clean`, and commit as `"registry: crash-safe journaling"`. Update `README.md` if it contains stale or incorrect references to registry persistence. Create or update documentation in `docs/` covering the journaling sequence, crash recovery, and backup rotation.

- [ ] Before saving: write new data to `SYSTEM.hive.log` first
- [ ] After log written: rename `.hive.log` → `.hive` (atomic replace)
- [ ] On boot: if `.hive.log` exists, replay it (recover from crash)
- [ ] On boot: if `.hive` is corrupt (bad CRC), fall back to `.hive.bak`
- [ ] Keep one backup: rename old `.hive` → `.hive.bak` before replacing
- [ ] Commit: `"registry: crash-safe journaling"`

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

## 7. File Associations (HKCR)

### 7.1 File Extension → Application Mapping

**Prompt:** `HKEY_CLASSES_ROOT` stores file associations — the mapping from file extensions to applications. When a user double-clicks a `.txt` file in the file manager, the shell looks up `HKCR\.txt\(Default)` to get the prog ID (e.g., `"txtfile"`), then reads `HKCR\txtfile\shell\open\command\(Default)` to get the command line (e.g., `"C:\Programs\notepad.exe "%1"`). Populate default associations for built-in types. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, run `bash scripts/build.sh clean`, and commit as `"registry: file associations (HKCR)"`. Update `README.md` if it contains stale or incorrect references to file associations. Create or update documentation in `docs/` covering HKCR file association lookup, prog ID resolution, and command-line expansion.

- [ ] Populate default file associations:
  - [ ] `HKCR\.txt` → `txtfile` → `notepad.exe "%1"`
  - [ ] `HKCR\.exe` → `exefile` → `"%1" %*`
  - [ ] `HKCR\.jpg` → `jpegfile` → `imageview.exe "%1"`
  - [ ] `HKCR\.png` → `pngfile` → `imageview.exe "%1"`
  - [ ] `HKCR\.bmp` → `bmpfile` → `imageview.exe "%1"`
- [ ] Implement `registry_get_assoc(extension, cmd_buf, buf_size)`:
  - [ ] Look up `HKCR\{ext}\(Default)` → prog ID
  - [ ] Look up `HKCR\{progID}\shell\open\command\(Default)` → command
  - [ ] Expand `%1` with the file path
- [ ] Commit: `"registry: file associations (HKCR)"`

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

| Priority | Section | Description |
|----------|---------|-------------|
| 🔴 P0 | 1.1 Data Structures | Foundation — everything depends on this |
| 🔴 P0 | 1.2 Value Types | Must support all REG_* types |
| 🔴 P0 | 1.3 Root Keys | HKLM, HKCU, HKU, HKCR |
| 🔴 P0 | 2.1 Key Operations | Core API: open, create, close, delete |
| 🔴 P0 | 2.2 Value Operations | Core API: get, set, delete values |
| 🔴 P0 | 3.1 Rename Files | Codex → Registry file rename |
| 🔴 P0 | 3.2 Replace API | Migrate all codex call sites |
| 🔴 P0 | 3.3 Migrate Defaults | Re-map values to Win32 paths |
| 🟠 P1 | 2.3 Enumeration | Needed for regedit + iteration |
| 🟠 P1 | 2.4 Convenience Helpers | Simplify common access patterns |
| 🟠 P1 | 4.1 Hive File Format | Binary disk persistence |
| 🟠 P1 | 4.2 Disk Layout | File paths + auto-flush |
| 🟡 P2 | 5.1 Change Notifications | Real-time settings updates |
| 🟡 P2 | 6.1 Syscalls | User-mode app access |
| 🟡 P2 | 7.1 File Associations | Double-click → open with app |
| 🟡 P2 | 8.1 Regedit Command | Debugging + inspection |
| 🟢 P3 | 4.3 Crash-Safe Journaling | Power-loss protection |
| 🔵 P4 | 9.1 Hash Map Lookup | O(1) performance |
| 🔵 P4 | 9.2 Memory-Mapped Hives | Zero-copy reads |
| 🔵 P4 | 9.3 B-Tree Format | Windows NT hive compat |
