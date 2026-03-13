# Registry System

> **Status:** The kernel currently uses the **Codex** implementation (static-pool
> key-value tree in `codex.c`/`codex.h`). A full Win32-compatible **Registry**
> system is planned — see [TODO-P0102-Registry.md](../../todo/TODO-P0102-Registry.md)
> for the migration plan and implementation spec.

## Current Implementation (Codex)

| File | Purpose |
|------|---------|
| `include/codex.h` | Type definitions, structs, API declarations |
| `src/kernel/codex.c` | In-memory tree implementation |

### Architecture

```
Root
 ├── System\      → Display, Theme, Shell, Network, DateTime
 ├── Hardware\    → CPU, Memory
 ├── User\        → Default (HomeDir, Shell, Prompt)
 └── Apps\        → (per-application settings)
```

- **Static pools** — 256 keys + 512 values, no heap allocation
- **Tree structure** — `codex_key_t` nodes with parent/children/sibling pointers
- **Typed values** — `STRING`, `INT32`, `INT64`, `BINARY`, `BOOL`
- **Path notation** — backslash-separated: `System\Theme\DarkMode`

### API Summary

| Function | Description |
|----------|-------------|
| `codex_init()` | Create root keys |
| `codex_open(path)` | Navigate to key by backslash path |
| `codex_create(path)` | Create key (+ intermediates) |
| `codex_get_string/int32/int64/bool()` | Read typed values |
| `codex_set_string/int32/int64/bool()` | Write typed values |
| `codex_enum_keys/values()` | Iterate children |
| `codex_save/load/flush()` | Disk persistence (`.codex` INI files) |

### Disk Persistence

INI-style `.codex` files in `C:\Impossible\System\Config\Codex\`:
`system.codex`, `hardware.codex`, `user.codex`, `apps.codex`.
Auto-flushed every 2 seconds when dirty.

## Planned: Win32-Compatible Registry

The migration will replace Codex with a Win32-compatible Registry using:
- `HKLM`, `HKCU`, `HKU`, `HKCR` root keys
- `REG_SZ`, `REG_DWORD`, `REG_QWORD`, `REG_BINARY` types
- `RegOpenKeyEx`, `RegQueryValueEx`, `RegSetValueEx` API
- Binary `.hive` files instead of INI
- `advapi32.dll` stubs for Win32 app compatibility

Full spec: [TODO-P0102-Registry.md](../../todo/TODO-P0102-Registry.md)
