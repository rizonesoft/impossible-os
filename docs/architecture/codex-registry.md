# Codex Registry System

The **Codex** is Impossible OS's hierarchical key-value configuration store —
equivalent to the Windows Registry. It stores system settings, hardware info,
user preferences, and application configuration.

## Key Files

| File | Purpose |
|------|---------|
| `include/codex.h` | Type definitions, structs, API declarations |
| `src/kernel/codex.c` | In-memory tree implementation |

## Architecture

```
Codex (root)
 ├── System\
 │   ├── Display\     → Width, Height, DPI, Scale
 │   ├── Theme\       → AccentColor, DarkMode, Font, Wallpaper, ...
 │   ├── Shell\       → TaskbarHeight, TaskbarPosition, ...
 │   ├── Network\     → Hostname, DHCP, DNS
 │   └── DateTime\    → Use24Hour, DateFormat, Timezone, NTP
 ├── Hardware\
 │   ├── CPU\         → Vendor (CPUID), Model (brand string)
 │   └── Memory\      → TotalMB, FreeMB (from PMM)
 ├── User\
 │   └── Default\     → HomeDir, Shell, Prompt, Wallpaper
 └── Apps\            → (per-application settings)
```

### Design Decisions

- **Static pools** — 256 keys + 512 values, no heap allocation required.
  Makes Codex usable very early in boot before the heap is heavily loaded.
- **Linked-list tree** — `codex_key_t` nodes with parent/children/sibling pointers.
  Children are stored as a singly-linked list (O(n) lookup, but n is small).
- **Typed values** — `codex_value_t` supports `STRING`, `INT32`, `INT64`,
  `BINARY`, `BOOL`. Each value has a name, type, data union, and data_size.
- **Path notation** — backslash-separated: `System\Theme\DarkMode`.
  Matches Windows Registry convention.

## Data Structures

```c
typedef enum {
    CODEX_STRING = 0,   /* null-terminated string */
    CODEX_INT32  = 1,   /* 32-bit signed integer  */
    CODEX_INT64  = 2,   /* 64-bit signed integer  */
    CODEX_BINARY = 3,   /* raw byte array         */
    CODEX_BOOL   = 4    /* 0 or 1                 */
} codex_type_t;

typedef struct codex_value {
    char           name[64];
    codex_type_t   type;
    uint32_t       data_size;
    union { char str[256]; int32_t i32; int64_t i64; uint8_t bin[512]; uint8_t boolean; } data;
    struct codex_value *next;
} codex_value_t;

typedef struct codex_key {
    char               name[64];
    struct codex_key  *parent, *children, *sibling;
    codex_value_t     *values;
} codex_key_t;
```

## API Reference

### Lifecycle

| Function | Description |
|----------|-------------|
| `codex_init()` | Create root keys (System, Hardware, User, Apps) |
| `codex_populate_defaults()` | Populate 32 default values from hardware detection |

### Key Operations

| Function | Description |
|----------|-------------|
| `codex_open(path)` | Navigate to key by backslash path, returns `codex_key_t*` |
| `codex_create(path)` | Create key (+ intermediates), returns `codex_key_t*` |
| `codex_delete_key(path)` | Delete key and all children/values |

### Value Accessors

| Function | Description |
|----------|-------------|
| `codex_get_string(key, name, buf, sz)` | Read string → buf |
| `codex_get_int32(key, name, &out)` | Read 32-bit int |
| `codex_get_int64(key, name, &out)` | Read 64-bit int |
| `codex_get_bool(key, name, &out)` | Read boolean (0/1) |
| `codex_set_string(key, name, val)` | Write/update string |
| `codex_set_int32(key, name, val)` | Write/update int32 |
| `codex_set_int64(key, name, val)` | Write/update int64 |
| `codex_set_bool(key, name, val)` | Write/update boolean |
| `codex_delete_value(key, name)` | Remove a value |

### Enumeration

| Function | Description |
|----------|-------------|
| `codex_enum_keys(key, idx, name, sz)` | Get child key name at index |
| `codex_enum_values(key, idx)` | Get `codex_value_t*` at index |

## Usage Example

```c
/* Create and populate a key */
codex_key_t *theme = codex_create("System\\Theme");
codex_set_string(theme, "AccentColor", "#0078D4");
codex_set_bool(theme, "DarkMode", 1);
codex_set_int32(theme, "CornerRadius", 8);

/* Read back */
codex_key_t *theme2 = codex_open("System\\Theme");
char color[32];
codex_get_string(theme2, "AccentColor", color, sizeof(color));
// color = "#0078D4"
```

## Pre-Populated Defaults (32 values)

| Key | Values |
|-----|--------|
| `System\Display` | Width, Height, DPI (96), Scale (100%) |
| `System\Theme` | AccentColor (#0078D4), DarkMode (1), Font (Selawik), FontSize (12), CornerRadius (8), Wallpaper, WallpaperMode (stretch), EnableAnimations (1) |
| `System\Shell` | TaskbarHeight (48), TaskbarPosition (bottom), ShowClock (1), ShowStartButton (1) |
| `System\Network` | Hostname (IMPOSSIBLE-PC), DHCP (1), DNS (8.8.8.8) |
| `System\DateTime` | Use24Hour (0), DateFormat (MM/DD/YYYY), TimezoneOffset (0), TimezoneName (UTC), NTPEnabled (1) |
| `Hardware\CPU` | Vendor (CPUID leaf 0), Model (CPUID brand string) |
| `Hardware\Memory` | TotalMB, FreeMB (from PMM) |
| `User\Default` | HomeDir, Shell, Prompt (C:\>), Wallpaper |

## Boot Output

```
[OK] Codex registry initialized (pool: 256 keys, 512 values)
[OK] Codex defaults populated (32 values, 32/512 pool used)
```

## Disk Persistence (.codex files)

INI-style files in `C:\Impossible\System\Config\Codex\`:

| File | Root Key |
|------|----------|
| `system.codex` | `System\` |
| `hardware.codex` | `Hardware\` |
| `user.codex` | `User\` |
| `apps.codex` | `Apps\` |

### File Format

```ini
; Codex Registry — System
; Auto-generated, do not edit manually

[Display]
Width:INT32=1280
Height:INT32=720
DPI:INT32=96

[Theme]
AccentColor:STRING=#0078D4
DarkMode:BOOL=1
Font:STRING=Selawik
CornerRadius:INT32=8
```

Value format: `Name:TYPE=Value` where TYPE is `STRING`, `INT32`, `INT64`, `BOOL`, or `BINARY`.

### Persistence API

| Function | Description |
|----------|-------------|
| `codex_save()` | Save all dirty root trees to .codex files |
| `codex_load()` | Load .codex files from disk at boot |
| `codex_flush()` | Auto-save dirty trees every 2 seconds |
| `codex_mark_dirty(root)` | Mark a root tree for save (auto-called by set/delete) |

### Boot Sequence

1. `codex_init()` — create root keys
2. `codex_load()` — load saved .codex files (if they exist)
3. `codex_populate_defaults()` — fill in any missing defaults
4. `codex_save()` — force persist (first boot writes all defaults)
5. Compositor loop calls `codex_flush()` every iteration (2-sec interval)

### Dirty Tracking

All `codex_set_*()` and `codex_delete_value()` functions automatically mark
the owning root tree as dirty. `codex_flush()` checks the 2-second interval
and only writes dirty trees to disk.
