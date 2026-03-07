# Codex — Impossible OS Registry System

## Overview

**Codex** is Impossible OS's system configuration database — equivalent to the Windows Registry. It stores system settings, application preferences, hardware configuration, and user settings in a hierarchical key-value store.

Windows programs that call Win32 registry APIs (`RegOpenKeyEx`, `RegQueryValueEx`, etc.) get transparently mapped to Codex operations.

---

## Design Philosophy

| Windows Registry | Codex |
|-----------------|-------|
| Monolithic binary hive files | Clean text-based storage (readable, debuggable) |
| Opaque `regedit.exe` needed to edit | Human-readable `.codex` files on disk |
| HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER, etc. | `System`, `User`, `Apps`, `Hardware` trees |
| Bloats over time, hard to clean | Per-app isolation, easy cleanup on uninstall |
| Stored in `C:\Windows\System32\config\` | Stored in `C:\Impossible\System\Config\Codex\` |

---

## Codex Tree Structure

```
Codex Root
├── System\                          ← OS-wide settings (like HKLM\SOFTWARE)
│   ├── Display\
│   │   ├── Resolution = "1280x720"
│   │   ├── RefreshRate = 60
│   │   └── DPI = 96
│   ├── Theme\
│   │   ├── AccentColor = "#0078D4"
│   │   ├── DarkMode = 1
│   │   ├── Wallpaper = "C:\Impossible\Wallpapers\default.raw"
│   │   ├── CornerRadius = 8
│   │   └── Font = "Selawik"
│   ├── Shell\
│   │   ├── TaskbarHeight = 48
│   │   ├── TaskbarPosition = "bottom"
│   │   └── StartMenuStyle = "centered"
│   ├── Network\
│   │   ├── Hostname = "impossible"
│   │   ├── DHCP = 1
│   │   └── DNS = "8.8.8.8"
│   ├── Sound\
│   │   ├── Volume = 80
│   │   └── Mute = 0
│   └── Boot\
│       ├── Timeout = 5
│       └── LastBootTime = 1709827200
├── Hardware\                        ← Detected hardware (like HKLM\HARDWARE)
│   ├── CPU\
│   │   ├── Vendor = "GenuineIntel"
│   │   ├── Model = "QEMU Virtual CPU"
│   │   └── Cores = 1
│   ├── Memory\
│   │   └── TotalMB = 2048
│   └── Display\
│       ├── Device = "VGA"
│       └── VRAM = 16777216
├── User\                            ← Per-user settings (like HKCU)
│   └── {Username}\
│       ├── Desktop\
│       │   ├── IconSize = "medium"
│       │   └── SortBy = "name"
│       └── Shell\
│           ├── RecentFiles = ["doc1.txt", "doc2.txt"]
│           └── Favorites = ["C:\Programs\Notepad"]
└── Apps\                            ← Per-application settings (like HKCU\Software)
    └── {AppName}\
        └── (app-specific keys)
```

---

## Storage Format

### On-disk: `.codex` files (INI-like, human-readable)

```ini
; C:\Impossible\System\Config\Codex\system.codex
[Display]
Resolution = "1280x720"
RefreshRate = 60
DPI = 96

[Theme]
AccentColor = "#0078D4"
DarkMode = 1
Wallpaper = "C:\Impossible\Wallpapers\default.raw"
CornerRadius = 8
Font = "Selawik"

[Shell]
TaskbarHeight = 48
TaskbarPosition = "bottom"
```

### In-memory: Tree of key-value nodes

```c
/* Value types */
typedef enum {
    CODEX_TYPE_STRING = 0,   /* null-terminated string */
    CODEX_TYPE_INT32,        /* 32-bit integer */
    CODEX_TYPE_INT64,        /* 64-bit integer */
    CODEX_TYPE_BINARY,       /* raw bytes */
    CODEX_TYPE_BOOL          /* 0 or 1 */
} codex_type_t;

/* A single registry value */
typedef struct codex_value {
    char           name[64];
    codex_type_t   type;
    union {
        char       str[256];
        int32_t    i32;
        int64_t    i64;
        uint8_t    bin[256];
        uint8_t    boolean;
    } data;
    uint32_t       data_size;
} codex_value_t;

/* A registry key (directory node) */
typedef struct codex_key {
    char               name[64];
    struct codex_key  *parent;
    struct codex_key  *children;     /* linked list of subkeys */
    struct codex_key  *next_sibling;
    codex_value_t     *values;       /* linked list of values */
    uint32_t           num_values;
    uint32_t           num_children;
} codex_key_t;
```

---

## Codex API

```c
/* codex.h */

/* Open/create a key by path */
codex_key_t *codex_open(const char *path);
codex_key_t *codex_create(const char *path);

/* Read values */
int codex_get_string(codex_key_t *key, const char *name,
                     char *buf, uint32_t buf_size);
int codex_get_int32(codex_key_t *key, const char *name, int32_t *out);
int codex_get_bool(codex_key_t *key, const char *name, uint8_t *out);

/* Write values */
int codex_set_string(codex_key_t *key, const char *name, const char *value);
int codex_set_int32(codex_key_t *key, const char *name, int32_t value);
int codex_set_bool(codex_key_t *key, const char *name, uint8_t value);

/* Delete */
int codex_delete_value(codex_key_t *key, const char *name);
int codex_delete_key(const char *path);

/* Enumerate */
int codex_enum_keys(codex_key_t *key, uint32_t index, char *name, uint32_t size);
int codex_enum_values(codex_key_t *key, uint32_t index, codex_value_t *out);

/* Flush to disk */
int codex_save(const char *tree);  /* "System", "User", "Apps" */
int codex_load(const char *tree);

/* Initialize Codex from disk files at boot */
int codex_init(void);
```

---

## Windows Registry → Codex Mapping

### Hive mapping

| Windows Hive | Codex Path | Purpose |
|-------------|-----------|---------|
| `HKEY_LOCAL_MACHINE\SOFTWARE` | `System\` | OS & installed software settings |
| `HKEY_LOCAL_MACHINE\HARDWARE` | `Hardware\` | Detected hardware info |
| `HKEY_CURRENT_USER` | `User\{Username}\` | Current user's settings |
| `HKEY_CURRENT_USER\Software\{App}` | `Apps\{App}\` | Per-app user settings |
| `HKEY_CLASSES_ROOT` | `System\FileTypes\` | File type associations |
| `HKEY_LOCAL_MACHINE\SYSTEM` | `System\Boot\` | Boot/service configuration |

### API mapping (Win32 → Codex)

| Win32 Function | Codex Equivalent | Notes |
|----------------|-----------------|-------|
| `RegOpenKeyExA(hkey, path, ...)` | `codex_open(mapped_path)` | Map hive + subpath to Codex path |
| `RegCreateKeyExA(hkey, path, ...)` | `codex_create(mapped_path)` | |
| `RegQueryValueExA(key, name, ...)` | `codex_get_string/int32(key, name)` | Map REG_SZ → string, REG_DWORD → int32 |
| `RegSetValueExA(key, name, ...)` | `codex_set_string/int32(key, name)` | |
| `RegDeleteKeyA(hkey, path)` | `codex_delete_key(mapped_path)` | |
| `RegDeleteValueA(key, name)` | `codex_delete_value(key, name)` | |
| `RegEnumKeyExA(key, index, ...)` | `codex_enum_keys(key, index, ...)` | |
| `RegEnumValueA(key, index, ...)` | `codex_enum_values(key, index, ...)` | |
| `RegCloseKey(key)` | No-op (or reference counting) | |

### Value type mapping

| Windows Type | Codex Type | Size |
|-------------|-----------|------|
| `REG_SZ` (string) | `CODEX_TYPE_STRING` | Variable |
| `REG_DWORD` (32-bit) | `CODEX_TYPE_INT32` | 4 bytes |
| `REG_QWORD` (64-bit) | `CODEX_TYPE_INT64` | 8 bytes |
| `REG_BINARY` (raw) | `CODEX_TYPE_BINARY` | Variable |
| `REG_EXPAND_SZ` | `CODEX_TYPE_STRING` | Expand `%VAR%` references |
| `REG_MULTI_SZ` | `CODEX_TYPE_STRING` | Join with `\n` |

### Win32 stub implementation

```c
/* In src/kernel/win32/advapi32.c */

LONG win32_RegOpenKeyExA(HKEY hKey, LPCSTR subkey,
                         DWORD options, REGSAM access, PHKEY result)
{
    char codex_path[256];

    /* Map Windows hive to Codex path */
    switch ((uintptr_t)hKey) {
        case HKEY_LOCAL_MACHINE:
            snprintf(codex_path, 256, "System\\%s", subkey);
            break;
        case HKEY_CURRENT_USER:
            snprintf(codex_path, 256, "User\\Default\\%s", subkey);
            break;
        case HKEY_CLASSES_ROOT:
            snprintf(codex_path, 256, "System\\FileTypes\\%s", subkey);
            break;
        default:
            /* hKey is a previously opened key — navigate relative */
            codex_key_t *parent = (codex_key_t *)hKey;
            snprintf(codex_path, 256, "%s\\%s", parent->name, subkey);
    }

    codex_key_t *key = codex_open(codex_path);
    if (!key) return ERROR_FILE_NOT_FOUND;

    *result = (HKEY)key;
    return ERROR_SUCCESS;
}

LONG win32_RegQueryValueExA(HKEY hKey, LPCSTR name, LPDWORD reserved,
                            LPDWORD type, LPBYTE data, LPDWORD size)
{
    codex_key_t *key = (codex_key_t *)hKey;
    codex_value_t val;

    if (codex_get_value(key, name, &val) < 0)
        return ERROR_FILE_NOT_FOUND;

    /* Map Codex type to Windows type */
    if (type) {
        switch (val.type) {
            case CODEX_TYPE_STRING: *type = REG_SZ; break;
            case CODEX_TYPE_INT32:  *type = REG_DWORD; break;
            case CODEX_TYPE_INT64:  *type = REG_QWORD; break;
            case CODEX_TYPE_BINARY: *type = REG_BINARY; break;
        }
    }

    /* Copy data */
    memcpy(data, &val.data, min(*size, val.data_size));
    *size = val.data_size;
    return ERROR_SUCCESS;
}
```

---

## Pre-populated Codex Entries

At first boot, Codex is populated with detected hardware and default settings:

```c
void codex_populate_defaults(void) {
    /* Display */
    codex_set_int32(codex_create("System\\Display"), "Width", fb_get_width());
    codex_set_int32(codex_create("System\\Display"), "Height", fb_get_height());
    codex_set_int32(codex_create("System\\Display"), "DPI", 96);

    /* Theme — Windows 11-like defaults */
    codex_set_string(codex_create("System\\Theme"), "AccentColor", "#0078D4");
    codex_set_bool(codex_create("System\\Theme"), "DarkMode", 1);
    codex_set_string(codex_create("System\\Theme"), "Font", "Selawik");
    codex_set_int32(codex_create("System\\Theme"), "CornerRadius", 8);

    /* Shell */
    codex_set_int32(codex_create("System\\Shell"), "TaskbarHeight", 48);

    /* Hardware */
    codex_set_string(codex_create("Hardware\\CPU"), "Vendor", cpu_vendor_string());
    codex_set_int32(codex_create("Hardware\\Memory"), "TotalMB", total_ram_mb());
}
```

---

## Files

| Action | File | Purpose |
|--------|------|---------|
| **[NEW]** | `include/codex.h` | Codex types, API declarations (~80 lines) |
| **[NEW]** | `src/kernel/codex.c` | Codex tree, get/set/enum, save/load (~500 lines) |
| **[NEW]** | `src/kernel/win32/advapi32.c` | Win32 registry stubs → Codex (~200 lines) |
| **[NEW]** | `resources/config/system.codex` | Default system settings file |
| **[MODIFY]** | `src/kernel/main.c` | Call `codex_init()` at boot |

---

## Implementation Order

### Phase 1: Core Codex (3-5 days)
- [ ] `codex_key_t` tree structure in memory
- [ ] `codex_open`, `codex_create`, `codex_delete_key`
- [ ] `codex_get_*`, `codex_set_*` for all types
- [ ] `codex_enum_keys`, `codex_enum_values`
- [ ] Pre-populate defaults at boot

### Phase 2: Persistence (2-3 days)
- [ ] `.codex` file parser (INI-like format)
- [ ] `codex_save()` — serialize tree to `C:\Impossible\System\Config\Codex\`
- [ ] `codex_load()` — load from disk at boot
- [ ] Auto-save on value changes (or periodic flush)

### Phase 3: Win32 mapping (2-3 days)
- [ ] Hive → Codex path translation
- [ ] `RegOpenKeyExA/W`, `RegQueryValueExA/W`, `RegSetValueExA/W`
- [ ] `RegCreateKeyExA/W`, `RegDeleteKeyA/W`
- [ ] `RegEnumKeyExA/W`, `RegEnumValueA/W`
- [ ] Add to `advapi32.dll` builtin stub table

### Phase 4: Codex shell command (1 day)
- [ ] `codex` shell command to browse/edit Codex from the terminal
- [ ] `codex list System\Theme` — show values
- [ ] `codex set System\Theme\DarkMode 0` — edit values
