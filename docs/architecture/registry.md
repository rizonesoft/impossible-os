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
