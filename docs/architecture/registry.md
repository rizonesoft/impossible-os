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
| `REG_DWORD` | 4 | 32-bit unsigned integer |
| `REG_LINK` | 6 | Symbolic link to another key |
| `REG_MULTI_SZ` | 7 | Double-null-terminated string array |
| `REG_QWORD` | 11 | 64-bit unsigned integer |

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
