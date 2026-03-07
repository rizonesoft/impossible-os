# Settings Panel — Research

## Overview

**Settings Panel** is Impossible OS's system configuration app — equivalent to Windows Control Panel / Settings. It loads **`.spl` files** (Settings Panel Libraries) the same way Windows loads `.cpl` files — each `.spl` is a plugin that manages one category of settings.

---

## How Windows `.cpl` Files Work

Windows `.cpl` files are regular DLLs that export a single function:

```c
LONG CALLBACK CPlApplet(HWND hwnd, UINT msg, LPARAM lParam1, LPARAM lParam2);
```

The Control Panel host sends messages:

| Message | When | Purpose |
|---------|------|---------|
| `CPL_INIT` | First loaded | Initialize, return TRUE to continue |
| `CPL_GETCOUNT` | After init | How many applets does this .cpl contain? |
| `CPL_INQUIRE` | For each applet | Get icon, name, description |
| `CPL_DBLCLK` | User clicks applet | Open the settings panel |
| `CPL_STOP` | Closing applet | Clean up per-applet resources |
| `CPL_EXIT` | Unloading DLL | Final cleanup |

---

## Impossible OS `.spl` Design

### Same concept, cleaner API

`.spl` files are ELF shared objects (or PE DLLs) that export a `SplApplet` function:

```c
/* spl.h — Settings Panel Library interface */

/* Messages sent to SplApplet */
#define SPL_INIT       0   /* Initialize — return 1 on success */
#define SPL_GETINFO    1   /* Fill in spl_info_t — name, icon, description */
#define SPL_OPEN       2   /* User opened this applet — draw your UI */
#define SPL_CLOSE      3   /* User closed this applet — save & clean up */
#define SPL_SAVE       4   /* Explicitly save settings to Codex */

/* Applet info structure */
typedef struct {
    char     name[64];          /* Display name: "Display", "Network", etc. */
    char     description[128];  /* Short description */
    char     icon_path[128];    /* Path to icon: "C:\Impossible\Icons\display.raw" */
    char     category[32];      /* "System", "Personalization", "Network", etc. */
    uint32_t version;           /* Applet version */
} spl_info_t;

/* The single export every .spl must provide */
typedef int (*spl_applet_fn)(uint32_t msg, void *param, void *context);

/*
 * msg = SPL_INIT:    param = NULL, context = NULL
 *                    Return 1 = success, 0 = fail
 *
 * msg = SPL_GETINFO: param = spl_info_t* to fill in
 *                    Return 1
 *
 * msg = SPL_OPEN:    param = spl_panel_t* (drawing surface + events)
 *                    The applet draws its UI here
 *                    Return 1
 *
 * msg = SPL_CLOSE:   param = NULL
 *                    Save settings, free resources
 *                    Return 1
 *
 * msg = SPL_SAVE:    param = NULL
 *                    Explicit save trigger
 *                    Return 1
 */
```

### Panel context (what the applet gets to work with)

```c
/* Provided to the applet on SPL_OPEN */
typedef struct {
    gfx_surface_t *surface;    /* Drawing surface for this panel */
    uint32_t       width;      /* Panel content area width */
    uint32_t       height;     /* Panel content area height */
    int32_t        mouse_x;    /* Current mouse position (relative to panel) */
    int32_t        mouse_y;
    uint8_t        mouse_btn;  /* Mouse button state */
    /* Codex access — read/write settings */
    codex_key_t   *codex_root; /* Pre-opened Codex key for this applet */
} spl_panel_t;
```

---

## Settings Panel Host App

The Settings Panel app (`settings.exe`) is the host that:
1. Scans `C:\Impossible\System\Settings\` for `.spl` files
2. Loads each one and calls `SPL_INIT` + `SPL_GETINFO`
3. Displays a grid of applet icons organized by category
4. When user clicks an applet, calls `SPL_OPEN` with a drawing surface
5. On close, calls `SPL_CLOSE`

### Host UI layout (Windows 11 Settings style)

```
┌──────────────────────────────────────────────────────┐
│  ← Settings                                    ─ □ × │
├────────────┬─────────────────────────────────────────┤
│            │                                         │
│  System    │   Display                               │
│  > Display │   ─────────────────────────              │
│    Sound   │   Resolution:  [1280 × 720  ▼]         │
│    Network │   DPI Scale:   [100%         ▼]         │
│            │                                         │
│  Personal  │   Brightness:  ████████░░  80%          │
│    Theme   │                                         │
│    Wallpap │   Dark Mode:   [■ On]                   │
│    Fonts   │                                         │
│            │   [ Apply ]  [ Cancel ]                 │
│  Apps      │                                         │
│  Privacy   │                                         │
│  Update    │                                         │
│            │                                         │
└────────────┴─────────────────────────────────────────┘
```

---

## Built-in `.spl` Applets

| File | Name | Category | Codex Path | Settings |
|------|------|----------|-----------|----------|
| `display.spl` | Display | System | `System\Display` | Resolution, DPI, brightness |
| `sound.spl` | Sound | System | `System\Sound` | Volume, mute, output device |
| `network.spl` | Network | System | `System\Network` | IP, DHCP, DNS, hostname |
| `theme.spl` | Theme | Personalization | `System\Theme` | Accent color, dark mode, corner radius |
| `wallpaper.spl` | Wallpaper | Personalization | `System\Theme` | Wallpaper selection, fit mode |
| `fonts.spl` | Fonts | Personalization | `System\Fonts` | Installed fonts, default font |
| `cursors.spl` | Cursors | Personalization | `System\Cursors` | Cursor theme, size |
| `taskbar.spl` | Taskbar | Personalization | `System\Shell` | Height, position, auto-hide |
| `datetime.spl` | Date & Time | System | `System\DateTime` | Timezone, format, NTP |
| `power.spl` | Power | System | `System\Power` | Shutdown, sleep, screen timeout |
| `about.spl` | About | System | — | OS version, hardware info, memory |
| `accounts.spl` | Accounts | System | `User\` | User management (future) |
| `apps.spl` | Apps | Apps | `Apps\` | Installed programs, uninstall |
| `storage.spl` | Storage | System | `Hardware\Storage` | Disk usage, drives |

---

## `.spl` Loading Process

```
1. Settings Panel host starts
2. Scan C:\Impossible\System\Settings\*.spl
3. For each .spl file:
   a. Load as ELF shared object (or PE DLL)
   b. Find exported symbol "SplApplet"
   c. Call SplApplet(SPL_INIT, NULL, NULL)
   d. Call SplApplet(SPL_GETINFO, &info, NULL)
   e. Add to category list with icon + name
4. Draw the category sidebar + icon grid
5. User clicks an applet:
   a. Create a panel surface
   b. Open Codex key for the applet
   c. Call SplApplet(SPL_OPEN, &panel, NULL)
   d. Applet draws its UI and handles events
6. User closes:
   a. Call SplApplet(SPL_CLOSE, NULL, NULL)
   b. Applet saves to Codex
   c. Free panel surface
```

### How `.spl` files are loaded (before dynamic linking exists)

Until we have a full dynamic linker, `.spl` files can be **statically linked ELF executables** that the Settings Panel hosts in-process:

```c
/* Phase 1: Static — each .spl is a regular ELF loaded into memory */
elf_load("display.spl");  /* loads at a unique base address */
spl_fn = find_symbol("SplApplet");  /* scan ELF symbol table */
spl_fn(SPL_INIT, NULL, NULL);

/* Phase 2: Dynamic — proper .so/.dll loading with relocations */
void *handle = dlopen("display.spl");
spl_fn = dlsym(handle, "SplApplet");
spl_fn(SPL_INIT, NULL, NULL);
```

---

## Win32 `.cpl` → `.spl` Mapping

When a Windows program calls Control Panel APIs, we translate:

| Win32 Call | Impossible OS Equivalent |
|-----------|------------------------|
| `LoadLibrary("desk.cpl")` | Load `C:\Impossible\System\Settings\display.spl` |
| `CPlApplet(hwnd, CPL_INIT, ...)` | `SplApplet(SPL_INIT, ...)` |
| `CPlApplet(hwnd, CPL_GETCOUNT, ...)` | Return 1 (one applet per .spl) |
| `CPlApplet(hwnd, CPL_INQUIRE, ...)` | `SplApplet(SPL_GETINFO, ...)` |
| `CPlApplet(hwnd, CPL_DBLCLK, ...)` | `SplApplet(SPL_OPEN, ...)` |
| `CPlApplet(hwnd, CPL_EXIT, ...)` | `SplApplet(SPL_CLOSE, ...)` |

### `.cpl` → `.spl` name mapping

| Windows `.cpl` | Maps to `.spl` |
|----------------|---------------|
| `desk.cpl` | `display.spl` / `wallpaper.spl` |
| `mmsys.cpl` | `sound.spl` |
| `ncpa.cpl` | `network.spl` |
| `main.cpl` (Mouse) | `cursors.spl` |
| `intl.cpl` | `datetime.spl` |
| `powercfg.cpl` | `power.spl` |
| `appwiz.cpl` | `apps.spl` |
| `firewall.cpl` | `network.spl` |
| `sysdm.cpl` | `about.spl` |

---

## Example `.spl` Implementation

```c
/* display.spl — Display settings applet */

#include "spl.h"
#include "codex.h"
#include "gfx.h"

static int current_width = 1280;
static int current_height = 720;

int SplApplet(uint32_t msg, void *param, void *context) {
    (void)context;

    switch (msg) {
    case SPL_INIT:
        return 1;

    case SPL_GETINFO: {
        spl_info_t *info = (spl_info_t *)param;
        strcpy(info->name, "Display");
        strcpy(info->description, "Change resolution, DPI, and brightness");
        strcpy(info->icon_path, "C:\\Impossible\\Icons\\display.raw");
        strcpy(info->category, "System");
        info->version = 1;
        return 1;
    }

    case SPL_OPEN: {
        spl_panel_t *panel = (spl_panel_t *)param;
        /* Read current settings from Codex */
        codex_get_int32(panel->codex_root, "Width", &current_width);
        codex_get_int32(panel->codex_root, "Height", &current_height);
        /* Draw the UI */
        draw_display_settings(panel);
        return 1;
    }

    case SPL_SAVE: {
        /* Write to Codex */
        codex_key_t *key = codex_open("System\\Display");
        codex_set_int32(key, "Width", current_width);
        codex_set_int32(key, "Height", current_height);
        return 1;
    }

    case SPL_CLOSE:
        return 1;
    }
    return 0;
}
```

---

## Files

| Action | File | Purpose |
|--------|------|---------|
| **[NEW]** | `include/spl.h` | SPL interface, message types, structs (~60 lines) |
| **[NEW]** | `src/apps/settings/settings.c` | Settings Panel host app (~400 lines) |
| **[NEW]** | `src/apps/settings/applets/display.spl.c` | Display settings applet |
| **[NEW]** | `src/apps/settings/applets/theme.spl.c` | Theme settings applet |
| **[NEW]** | `src/apps/settings/applets/network.spl.c` | Network settings applet |
| **[NEW]** | `src/apps/settings/applets/sound.spl.c` | Sound settings applet |
| **[NEW]** | `src/apps/settings/applets/about.spl.c` | About system applet |
| **[MODIFY]** | `Makefile` | Build .spl applets, include in initrd |

---

## Implementation Order

### Phase 1: SPL framework + host (1 week)
- [ ] Define `spl.h` interface
- [ ] Build Settings Panel host app (sidebar + panel area)
- [ ] Implement `.spl` scanning and loading
- [ ] SPL message dispatch loop

### Phase 2: Core applets (1-2 weeks)
- [ ] `about.spl` — show OS version, CPU, RAM (simplest applet, good test)
- [ ] `theme.spl` — accent color picker, dark mode toggle, reads/writes Codex
- [ ] `display.spl` — resolution selector, DPI
- [ ] `network.spl` — IP/DHCP/DNS display, uses existing netinfo

### Phase 3: Applet UI widgets (1 week)
- [ ] Toggle switch (on/off)
- [ ] Dropdown selector
- [ ] Slider (volume, brightness)
- [ ] Color picker
- [ ] Text input field
- [ ] Apply / Cancel buttons

### Phase 4: Win32 `.cpl` mapping (3-5 days)
- [ ] CPL → SPL message translation
- [ ] `.cpl` name → `.spl` name lookup table
- [ ] Windows programs calling Control Panel APIs get routed to Settings Panel
