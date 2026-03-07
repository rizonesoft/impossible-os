# System Icon Store — Research

## Overview

Windows bundles system icons inside **Shell32.dll** (~300+ icons) and **ImageRes.dll** (~1000+ icons). Programs load icons by DLL path + index: `Shell32.dll,3` = folder icon. Impossible OS needs the same — a central icon store that all apps, the desktop, file manager, and Settings Panel pull from.

---

## How Windows Does It

```
Shell32.dll contains:
  Icon 0   = Default document
  Icon 1   = Default executable
  Icon 2   = Default application
  Icon 3   = Closed folder
  Icon 4   = Open folder
  Icon 5   = 5.25" floppy
  Icon 8   = Removable drive
  Icon 9   = Network drive
  Icon 15  = My Computer
  Icon 16  = Network
  Icon 17  = Windows directory
  Icon 19  = Start menu programs
  Icon 21  = Audio CD
  Icon 23  = Settings gear
  Icon 32  = Printer
  Icon 37  = Network share
  Icon 43  = Star / favorite
  ...
```

Icons are embedded as **PE resources** (RT_GROUP_ICON) at multiple sizes (16×16, 32×32, 48×48, 256×256).

---

## Impossible OS Design

Instead of embedding icons inside a DLL (which requires PE resource parsing), use an **icon pack file** — a single file that bundles all icons with an index.

### Option A: Icon directory on disk

```
C:\Impossible\Icons\
├── _index.codex              ← Icon name → filename mapping
├── folder.raw                ← 32×32 BGRA
├── folder_open.raw
├── file_default.raw
├── file_text.raw
├── file_image.raw
├── file_audio.raw
├── file_exe.raw
├── drive_local.raw
├── drive_network.raw
├── computer.raw
├── network.raw
├── settings.raw
├── start.raw
├── trash_empty.raw
├── trash_full.raw
├── ...
```

- **Pros**: Simple, each icon is a separate file, easy to replace
- **Cons**: Many small files, slower scan at boot

### Option B: Icon pack file (`.iconpack`)

```
C:\Impossible\Icons\system.iconpack
```

Single binary file with header + all icons:

```
┌────────────────────────┐
│ Header                 │
│  magic: "IIPK"         │
│  version: 1            │
│  icon_count: N         │
│  icon_size: 32         │  ← default size (32×32)
├────────────────────────┤
│ Index Table (N entries) │
│  [0] name="folder"     │
│      offset=1024       │
│      width=32 height=32│
│  [1] name="file"       │
│      offset=5120       │
│  ...                   │
├────────────────────────┤
│ Icon Data              │
│  [raw BGRA pixels]     │
│  [raw BGRA pixels]     │
│  ...                   │
└────────────────────────┘
```

- **Pros**: Single file, fast loading, easy to theme-swap
- **Cons**: Need a packing tool, can't edit individual icons without rebuilding

### Option C: Hybrid (recommended)

Use **Option A** (directory) during development, build into **Option B** (`.iconpack`) for release. The icon API supports both:

```c
/* Try iconpack first, fall back to directory */
icon_t *icon = icon_load("folder");
/* 1. Check C:\Impossible\Icons\system.iconpack index */
/* 2. Fall back to C:\Impossible\Icons\folder.raw */
```

---

## Icon API

```c
/* icon_store.h */

/* Standard icon sizes */
#define ICON_SIZE_16   16
#define ICON_SIZE_32   32
#define ICON_SIZE_48   48

/* System icon IDs (by index, like Shell32.dll) */
typedef enum {
    ICON_FILE_DEFAULT = 0,
    ICON_FILE_TEXT,
    ICON_FILE_IMAGE,
    ICON_FILE_AUDIO,
    ICON_FILE_VIDEO,
    ICON_FILE_ARCHIVE,
    ICON_FILE_EXE,
    ICON_FILE_DLL,
    ICON_FILE_CONFIG,
    ICON_FOLDER_CLOSED,
    ICON_FOLDER_OPEN,
    ICON_FOLDER_DOCUMENTS,
    ICON_FOLDER_PICTURES,
    ICON_FOLDER_MUSIC,
    ICON_FOLDER_DOWNLOADS,
    ICON_DRIVE_LOCAL,
    ICON_DRIVE_REMOVABLE,
    ICON_DRIVE_NETWORK,
    ICON_DRIVE_CDROM,
    ICON_COMPUTER,
    ICON_NETWORK,
    ICON_SETTINGS,
    ICON_START_MENU,
    ICON_TRASH_EMPTY,
    ICON_TRASH_FULL,
    ICON_PRINTER,
    ICON_SEARCH,
    ICON_LOCK,
    ICON_USER,
    ICON_POWER,
    ICON_INFO,
    ICON_WARNING,
    ICON_ERROR,
    ICON_QUESTION,
    ICON_APP_DEFAULT,
    ICON_COUNT
} system_icon_t;

/* Loaded icon data */
typedef struct {
    uint32_t *pixels;     /* BGRA pixel data */
    uint16_t  width;
    uint16_t  height;
} icon_t;

/* Initialize icon store — load system.iconpack or scan directory */
int icon_store_init(void);

/* Get a system icon by ID */
const icon_t *icon_get(system_icon_t id);

/* Get an icon by name (for extensions: "txt" → file_text icon) */
const icon_t *icon_get_by_name(const char *name);

/* Get file type icon based on extension */
const icon_t *icon_for_extension(const char *ext);

/* Draw an icon to a surface at (x, y) with alpha blending */
void icon_draw(gfx_surface_t *surface, const icon_t *icon, int x, int y);

/* Draw an icon scaled to a different size */
void icon_draw_scaled(gfx_surface_t *surface, const icon_t *icon,
                      int x, int y, int target_size);
```

---

## File Type → Icon Mapping

Stored in Codex at `System\FileTypes\`:

```ini
[Extensions]
txt = "file_text"
md = "file_text"
log = "file_text"
c = "file_code"
h = "file_code"
py = "file_code"
js = "file_code"
jpg = "file_image"
png = "file_image"
bmp = "file_image"
mp3 = "file_audio"
wav = "file_audio"
ogg = "file_audio"
mp4 = "file_video"
avi = "file_video"
zip = "file_archive"
tar = "file_archive"
exe = "file_exe"
dll = "file_dll"
spl = "file_config"
codex = "file_config"
```

```c
/* Usage in a file manager */
const icon_t *icon = icon_for_extension("txt");
/* → looks up "txt" in Codex → "file_text" → icon_get_by_name("file_text") */
```

---

## Win32 Shell32.dll Mapping

When Windows programs call icon-related APIs:

| Win32 Function | Impossible OS Equivalent |
|----------------|------------------------|
| `ExtractIconA("shell32.dll", 3)` | `icon_get(ICON_FOLDER_CLOSED)` |
| `SHGetFileInfo(path, SHGFI_ICON)` | `icon_for_extension(ext)` |
| `LoadIcon(NULL, IDI_APPLICATION)` | `icon_get(ICON_APP_DEFAULT)` |
| `LoadIcon(NULL, IDI_ERROR)` | `icon_get(ICON_ERROR)` |
| `LoadIcon(NULL, IDI_WARNING)` | `icon_get(ICON_WARNING)` |
| `LoadIcon(NULL, IDI_INFORMATION)` | `icon_get(ICON_INFO)` |
| `LoadIcon(NULL, IDI_QUESTION)` | `icon_get(ICON_QUESTION)` |

### Shell32.dll index → system_icon_t mapping

```c
static const system_icon_t shell32_map[] = {
    [0]  = ICON_FILE_DEFAULT,
    [1]  = ICON_FILE_EXE,
    [3]  = ICON_FOLDER_CLOSED,
    [4]  = ICON_FOLDER_OPEN,
    [8]  = ICON_DRIVE_REMOVABLE,
    [9]  = ICON_DRIVE_NETWORK,
    [15] = ICON_COMPUTER,
    [16] = ICON_NETWORK,
    [23] = ICON_SETTINGS,
    [31] = ICON_TRASH_EMPTY,
    [32] = ICON_PRINTER,
    [43] = ICON_SEARCH,
    [47] = ICON_LOCK,
    [109] = ICON_POWER,
};
```

---

## Icon Sources (Open Source)

| Icon Set | License | Redistributable | Icons | Style |
|----------|---------|-----------------|-------|-------|
| **Fluent UI System Icons** | MIT | ✅ Yes | 4000+ | Windows 11 style (Microsoft) |
| **Papirus** | GPL 3.0 | ✅ Yes | 6000+ | Modern, clean |
| **Tango** | Public Domain | ✅ Yes | 300+ | Classic, clear |
| **Adwaita** | LGPL / CC-BY-SA | ✅ Yes | 500+ | GNOME style |
| **Font Awesome** | OFL / MIT | ✅ Yes | 2000+ | Mostly UI glyphs |
| **Material Design Icons** | Apache 2.0 | ✅ Yes | 5000+ | Google style |

> [!TIP]
> **Fluent UI System Icons** are perfect — they're Microsoft's own open-source icons used in Windows 11, Office, and Teams. MIT licensed. Same visual language as our Selawik font and Windows 11 aesthetic.
>
> Repository: https://github.com/microsoft/fluentui-system-icons

---

## Build Pipeline

```makefile
# Convert each icon PNG → raw BGRA at 32×32
ICONS := folder file_default file_text file_image settings ...
$(foreach icon,$(ICONS),\
    $(BUILD_DIR)/tools/jpg2raw resources/icons/$(icon).png \
        $(BUILD_DIR)/initrd_files/icons/$(icon).raw 32 32;)

# Optional: pack all icons into system.iconpack
$(BUILD_DIR)/tools/iconpacker $(BUILD_DIR)/initrd_files/icons/ \
    $(BUILD_DIR)/initrd_files/system.iconpack
```

---

## Files

| Action | File | Purpose |
|--------|------|---------|
| **[NEW]** | `include/icon_store.h` | Icon types, system IDs, API (~80 lines) |
| **[NEW]** | `src/kernel/icon_store.c` | Loading, caching, extension mapping (~300 lines) |
| **[NEW]** | `tools/iconpacker.c` | Build tool: create `.iconpack` from directory |
| **[NEW]** | `resources/icons/*.png` | Source icon PNGs (from Fluent UI) |
| **[MODIFY]** | `Makefile` | Icon conversion + packing rules |

---

## Implementation Order

### Phase 1: Basics (2-3 days)
- [ ] Define `icon_store.h` with system icon enum
- [ ] Implement `icon_store_init()` — load icons from initrd directory
- [ ] Implement `icon_get()` — return icon by ID
- [ ] Implement `icon_draw()` — blit with alpha
- [ ] Download Fluent UI icons, convert ~35 core icons to 32×32 PNG

### Phase 2: File type mapping (1-2 days)
- [ ] `icon_for_extension()` — Codex lookup
- [ ] Populate `System\FileTypes` in Codex defaults
- [ ] Integrate with future file manager

### Phase 3: Win32 mapping (1-2 days)
- [ ] Shell32.dll index → `system_icon_t` table
- [ ] `ExtractIcon`, `LoadIcon`, `SHGetFileInfo` stubs
- [ ] Add to shell32.dll builtin stub table

### Phase 4: Icon pack format (future)
- [ ] `iconpacker` build tool
- [ ] `.iconpack` parser in `icon_store.c`
- [ ] Multi-size support (16, 32, 48)
