# Registry System

> **Status:** §1.1 (core data structures) implemented in commit `d44a791`.
> The Codex system (`codex.c`/`codex.h`) still exists alongside the new
> Registry and will be removed once the full API + migration are complete.

## New Registry Implementation (§1.1+)

| File | Purpose |
|------|---------|
| `include/registry.h` | Types, structs, HKEY handles, error codes, access rights |
| `src/kernel/registry.c` | Static pools, FNV-1a hash, allocators, root key init |

### Data Structures

#### `reg_key_t` — Registry Key

| Field | Type | Description |
|-------|------|-------------|
| `name[256]` | `char` | Key name (max 255 chars + null) |
| `parent` | `reg_key_t*` | Parent key (NULL for roots) |
| `children[16]` | `reg_key_t*` | FNV-1a hash buckets for child lookup |
| `hash_next` | `reg_key_t*` | Collision chain within bucket |
| `child_count` | `uint32_t` | Number of child keys |
| `values` | `reg_value_t*` | Linked list of values |
| `value_count` | `uint32_t` | Number of values |
| `last_write_time` | `uint64_t` | PIT ticks at last modification |
| `flags` | `uint32_t` | `REG_FLAG_VOLATILE`, `REG_FLAG_READONLY`, etc. |

#### `reg_value_t` — Registry Value

| Field | Type | Description |
|-------|------|-------------|
| `name[256]` | `char` | Value name (empty string = default value) |
| `type` | `uint32_t` | `REG_SZ`, `REG_DWORD`, `REG_BINARY`, etc. |
| `data[512]` | `uint8_t` | Value data buffer (max 512 bytes) |
| `data_size` | `uint32_t` | Actual bytes used in data[] |
| `next` | `reg_value_t*` | Linked list pointer |

### HKEY Handle System

`HKEY` is a pointer to `reg_handle_t`, which wraps:
- `reg_key_t *key` — the open key
- `uint32_t access` — access mode (`KEY_READ`, `KEY_WRITE`, `KEY_ALL_ACCESS`)

**Predefined handles** use sentinel addresses (never dereferenced directly):

| Handle | Sentinel | Root key name |
|--------|----------|---------------|
| `HKEY_CLASSES_ROOT` | `0x80000000` | `HKEY_CLASSES_ROOT` |
| `HKEY_CURRENT_USER` | `0x80000001` | `HKEY_CURRENT_USER` |
| `HKEY_LOCAL_MACHINE` | `0x80000002` | `HKEY_LOCAL_MACHINE` |
| `HKEY_USERS` | `0x80000003` | `HKEY_USERS` |
| `HKEY_CURRENT_CONFIG` | `0x80000005` | `HKEY_CURRENT_CONFIG` |

### Default Key Tree (§1.3)

Created by `registry_init()`:

```
HKLM (HKEY_LOCAL_MACHINE)
 ├── SYSTEM
 ├── SOFTWARE
 │    └── Classes        ← HKCR backing store
 └── HARDWARE

HKU (HKEY_USERS)
 └── Default

HKCU (HKEY_CURRENT_USER) → redirects to HKU\{current_user}
HKCR (HKEY_CLASSES_ROOT) → merged view of HKLM\SOFTWARE\Classes
```

### HKCU Redirection

- `reg_set_current_user(username)` — sets the current user (default: `"Default"`)
- `reg_resolve_hkcu()` — returns `HKU\{user}` key (auto-creates if missing)
- HKCU has `REG_FLAG_HKCU_REDIRECT` flag set

### HKCR Merged View

- `reg_resolve_hkcr()` — returns `HKLM\SOFTWARE\Classes`
- HKCR has `REG_FLAG_HKCR_MERGED` flag set
- Merged lookup (HKCU\SOFTWARE\Classes overlay) handled at query time

### Child Key Management

| Function | Description |
|----------|-------------|
| `reg_add_child(parent, child)` | Insert child into parent's FNV-1a buckets |
| `reg_find_child(parent, name)` | Case-insensitive lookup in hash buckets |
| `reg_create_child(parent, name)` | Find-or-create convenience wrapper |

### Win32 Key Operations (§2.1)

| Function | Description |
|----------|-------------|
| `RegOpenKeyEx(hKey, subKey, opts, access, &result)` | Open sub-key, walk path, follow REG_LINK |
| `RegCreateKeyEx(hKey, subKey, ...)` | Create-or-open with disposition tracking |
| `RegCloseKey(hKey)` | Release handle (no-op for predefined) |
| `RegDeleteKey(hKey, subKey)` | Delete leaf key + values (fails if children exist) |
| `RegDeleteTree(hKey, subKey)` | Recursive delete of entire subtree |

**Handle pool:** 128 slots (`REG_HANDLE_POOL_SIZE`). Predefined handles (HKLM, etc.) use sentinel addresses and are never pooled.

**Path walker:** `reg_walk_path()` splits on `\`, follows `REG_LINK` keys transparently, optionally creates missing intermediates.

**Error codes:**

| Code | Value | Meaning |
|------|-------|---------|
| `ERROR_SUCCESS` | 0 | Operation succeeded |
| `ERROR_FILE_NOT_FOUND` | 2 | Key not found |
| `ERROR_ACCESS_DENIED` | 5 | Cannot delete key with children |
| `ERROR_INVALID_HANDLE` | 6 | Bad HKEY |
| `ERROR_OUTOFMEMORY` | 14 | Pool exhausted |
| `ERROR_INVALID_PARAMETER` | 87 | NULL required parameter |
| `ERROR_MORE_DATA` | 234 | Buffer too small |
| `ERROR_NO_MORE_ITEMS` | 259 | Enumeration complete |

### Win32 Value Operations (§2.2)

| Function | Description |
|----------|-------------|
| `RegSetValueEx(hKey, name, 0, type, data, size)` | Create/update value; empty name = "(Default)" |
| `RegQueryValueEx(hKey, name, 0, &type, data, &size)` | Read value; `ERROR_MORE_DATA` if small buffer |
| `RegGetValue(hKey, subKey, name, flags, &type, data, &size)` | Open+query+auto-expand EXPAND_SZ |
| `RegDeleteValue(hKey, name)` | Remove named value |

**RRF flags** for `RegGetValue`: `RRF_RT_REG_SZ`, `RRF_RT_REG_DWORD`, `RRF_RT_REG_QWORD`, `RRF_RT_REG_BINARY`, `RRF_RT_ANY`, `RRF_NOEXPAND`.

### Enumeration (§2.3)

| Function | Description |
|----------|-------------|
| `RegEnumKeyEx(hKey, idx, name, &size, ...)` | Get child key name by 0-based index |
| `RegEnumValue(hKey, idx, name, &size, ...)` | Get value name, type, and data by index |
| `RegQueryInfoKey(hKey, ...)` | Key stats: child/value counts, max name/data sizes |

Index-based enumeration scans hash buckets linearly (`reg_get_child_by_index`). Returns `ERROR_NO_MORE_ITEMS` when exhausted.

### Convenience Helpers (§2.4)

| Function | Description |
|----------|-------------|
| `RegGetDword/RegSetDword` | Read/write `REG_DWORD` (uint32_t) |
| `RegGetString/RegSetString` | Read/write `REG_SZ` (null-terminated string) |
| `RegGetQword/RegSetQword` | Read/write `REG_QWORD` (uint64_t) |
| `RegReadKeyValue(root, path, name, ...)` | One-shot open + query + close |

`RegGetString` accepts both `REG_SZ` and `REG_EXPAND_SZ`. `RegSetString` includes the null terminator. `RegReadKeyValue` always closes the handle, even on query failure.

### Codex → Registry Migration (§3.1)

| Old Codex API | New Registry API |
|---------------|------------------|
| `codex_init()` | `registry_init()` |
| `codex_open(path)` | `RegOpenKeyEx(HKLM, path, ...)` |
| `codex_create(path)` | `RegCreateKeyEx(HKLM, path, ...)` |
| `codex_get_string()` | `RegGetString()` |
| `codex_get_int32()` | `RegGetDword()` |
| `codex_set_string()` | `RegSetString()` |
| `codex_set_int32()` | `RegSetDword()` |

**Migrated files:** `main.c`, `panic.c`, `swap.c`, `desktop.c`.

### Default Value Mapping (§3.2)

`registry_populate_defaults()` populates 32 factory defaults at boot:

| Old Codex Path | Win32 Registry Path | Values |
|---------------|---------------------|--------|
| `System\Display` | `HKLM\SYSTEM\Display` | Width, Height, DPI, Scale |
| `System\Theme` | `HKLM\SYSTEM\Theme` | AccentColor, DarkMode, Font, FontSize, CornerRadius, Wallpaper, WallpaperMode, EnableAnimations |
| `System\Shell` | `HKLM\SYSTEM\Shell` | TaskbarHeight, TaskbarPosition, ShowClock, ShowStartButton |
| `System\Network` | `HKLM\SYSTEM\Network` | Hostname, DHCP, DNS |
| `System\DateTime` | `HKLM\SYSTEM\DateTime` | Use24Hour, DateFormat, TimezoneOffset, TimezoneName, NTPEnabled |
| `System\Recovery` | `HKLM\SYSTEM\Recovery` | AutoRestart |
| `System\Memory` | `HKLM\SYSTEM\Memory` | SwapSlots |
| `Hardware\CPU` | `HKLM\HARDWARE\CPU` | Vendor, Model |
| `Hardware\Memory` | `HKLM\HARDWARE\Memory` | TotalMB, FreeMB |
| `User\Default` | `HKU\Default` | HomeDir, Shell |
| `User\Default\Shell` | `HKU\Default\Shell` | Prompt |
| `User\Default\Desktop` | `HKU\Default\Desktop` | Wallpaper |

### Value Types

| Constant | Value | Description |
|----------|-------|-------------|
| `REG_NONE` | 0 | No defined type |
| `REG_SZ` | 1 | Null-terminated string |
| `REG_EXPAND_SZ` | 2 | String with `%VAR%` expansion |
| `REG_BINARY` | 3 | Raw binary data |
| `REG_DWORD` | 4 | 32-bit unsigned integer (alias: `REG_DWORD_LITTLE_ENDIAN`) |
| `REG_DWORD_BIG_ENDIAN` | 5 | 32-bit big-endian (rare) |
| `REG_LINK` | 6 | Symbolic link to another key |
| `REG_MULTI_SZ` | 7 | Double-null-terminated string array |
| `REG_QWORD` | 11 | 64-bit unsigned integer |

### Value Type Helpers (§1.2)

| Function | Description |
|----------|-------------|
| `reg_type_name(type)` | Human-readable name for a `REG_*` code |
| `reg_expand_sz(src, dst, size)` | Expand `%VAR%` tokens from `HKLM\System\Environment` |
| `reg_multi_sz_count(data, size)` | Count strings in MULTI_SZ buffer |
| `reg_multi_sz_get(data, size, idx)` | Get Nth string from MULTI_SZ buffer |
| `reg_multi_sz_pack(strs, count, out, size)` | Pack string array into MULTI_SZ format |
| `reg_key_is_link(key)` | Check if key has `REG_FLAG_LINK` |
| `reg_key_get_link_target(key)` | Get link target path from unnamed `REG_LINK` value |

#### REG_EXPAND_SZ Expansion

Parses `%VARIABLE%` tokens and replaces them with values from
`HKLM\System\Environment`. If a variable is not found, the original
`%VARIABLE%` text is preserved. Case-insensitive variable matching.

#### REG_MULTI_SZ Encoding

```
"string1\0string2\0string3\0\0"
```

Each string is null-terminated. The entire sequence ends with a double null.

#### REG_LINK Redirection

A key with `REG_FLAG_LINK` set contains an unnamed value of type `REG_LINK`
whose data is the backslash-separated target key path. When `RegOpenKeyEx`
encounters a link key, it transparently redirects to the target.

### Static Pools

| Pool | Size | Struct size (approx) | Total memory |
|------|------|---------------------|--------------|
| `reg_key_pool` | 512 entries | ~420 bytes/key | ~210 KB |
| `reg_value_pool` | 1024 entries | ~780 bytes/value | ~780 KB |

Pool allocators use bump allocation (no free). Keys are indexed by
FNV-1a hash (case-insensitive, 16 buckets per parent) for O(1) child lookup.

### Child Key Lookup (FNV-1a)

```
hash = FNV1a_OFFSET_BASIS (0x811C9DC5)
for each char c in name:
    c = tolower(c)      // case-insensitive
    hash ^= c
    hash *= FNV1a_PRIME  // 0x01000193
bucket = hash & (REG_CHILD_BUCKETS - 1)
```

This makes key lookup case-insensitive (matching Windows behavior).

---

## Legacy: Codex (to be removed)

| File | Purpose |
|------|---------|\
| `include/codex.h` | Type definitions, structs, API declarations |
| `src/kernel/codex.c` | In-memory tree implementation |

The Codex uses linked-list children (O(n) lookup), 256-key / 512-value pools,
and INI-style `.codex` persistence files. It will be fully replaced once the
Registry API layer (§1.2+) and migration are complete.
