---
description: Impossible OS conventions — Windows-style file structure, paths, and system architecture
---

# Impossible OS Conventions

Impossible OS follows **Windows file structure and system conventions**. When writing code, creating paths, or organizing files within the OS, always follow these rules.

## File System Structure

Impossible OS uses **Windows-style drive letters** and **backslash paths**. The root filesystem is `C:\`.

### System Folders (on `C:\`)

```
C:\
├── Impossible\                  ← System root (like C:\Windows)
│   ├── System\                  ← Core system files, drivers, DLLs
│   │   ├── Drivers\             ← Device drivers
│   │   ├── Config\              ← System configuration files
│   │   │   └── Registry\        ← Hive files (.hive, .hive.bak)
│   │   ├── Cursors\             ← Cursor theme files (XCursor format)
│   │   └── Logs\                ← System logs (kernel.log)
│   ├── System32\                ← System executables (shell, tools)
│   ├── Fonts\                   ← System fonts (.ttf, .otf)
│   ├── Icons\                   ← System icons
│   ├── Media\                   ← System sound files (like C:\Windows\Media)
│   ├── Web\                     ← Web-related assets
│   │   └── Wallpaper\           ← Default wallpapers (like C:\Windows\Web\Wallpaper)
│   ├── Themes\                  ← UI theme definitions
│   └── Temp\                    ← Temporary files (like C:\Windows\Temp)
├── Program Files\               ← Installed applications (like C:\Program Files)
│   └── {AppName}\               ← Each app gets its own folder
├── Users\                       ← User home directories
│   └── {Username}\
│       ├── Desktop\             ← Desktop shortcuts/files
│       ├── Documents\           ← User documents
│       ├── Downloads\           ← Downloaded files
│       ├── Pictures\            ← User images
│       ├── Music\               ← User audio files
│       └── AppData\             ← Per-user app settings
│           └── {AppName}\
└── Recycle\                     ← Recycle Bin storage
```

### Path Conventions

| Rule | Example |
|------|---------| 
| Use backslashes | `C:\Impossible\Fonts\Inter.ttf` |
| Drive letter + colon | `C:\`, `D:\` (secondary drives) |
| Case-insensitive paths | `C:\impossible\fonts` == `C:\Impossible\Fonts` |
| No trailing backslash on files | `C:\Impossible\System\config.ini` |
| Trailing backslash OK on dirs | `C:\Impossible\Fonts\` |

### In C Code

Always use double backslashes in C strings:

```c
/* Correct */
vfs_open("C:\\Impossible\\Fonts\\Inter.ttf", VFS_O_READ);
vfs_create("C:\\Impossible\\System\\Drivers", VFS_DIRECTORY);

/* Wrong — do not use forward slashes */
vfs_open("C:/Impossible/Fonts/Inter.ttf", VFS_O_READ);  /* NO */
```

## Where Things Go

| Asset Type | Path | Notes |
|-----------|------|-------|
| **System fonts** | `C:\Impossible\Fonts\` | `.ttf`, `.otf` files |
| **Cursors** | `C:\Impossible\System\Cursors\` | Adwaita XCursor format |
| **System icons** | `C:\Impossible\Icons\` | IRES icon pack |
| **System sounds** | `C:\Impossible\Media\` | Startup, click, error sounds |
| **Wallpapers** | `C:\Impossible\Web\Wallpaper\` | Default wallpapers (JPEG) |
| **Themes** | `C:\Impossible\Themes\` | Color schemes, UI settings |
| **System binaries** | `C:\Impossible\System32\` | Shell, system tools |
| **Drivers** | `C:\Impossible\System\Drivers\` | Device driver files |
| **Config files** | `C:\Impossible\System\Config\` | Registry hive files |
| **System logs** | `C:\Impossible\System\Logs\` | kernel.log, crashdump.log |
| **User files** | `C:\Users\{name}\Documents\` | Per-user documents |
| **Installed apps** | `C:\Program Files\{AppName}\` | Third-party applications |
| **Temp files** | `C:\Impossible\Temp\` | Temporary/scratch files |

## Naming Conventions

| Type | Convention | Example |
|------|-----------|---------| 
| System folders | PascalCase | `Impossible`, `System`, `Fonts` |
| System files | PascalCase or lowercase | `Config.ini`, `kernel32.dll` |
| User folders | PascalCase | `Documents`, `Desktop`, `Downloads` |
| Executables | lowercase with `.exe` | `shell.exe`, `notepad.exe` |
| Libraries | lowercase with `.dll` | `kernel32.dll`, `msvcrt.dll` |
| Config files | PascalCase with `.ini`/`.cfg` | `Theme.ini`, `Boot.cfg` |

## Boot-Time Directory Creation

During registry init, the system creates the default folder hierarchy:

```c
/* Create Impossible OS system directory structure */
vfs_create("C:\\Impossible", VFS_DIRECTORY);
vfs_create("C:\\Impossible\\System", VFS_DIRECTORY);
vfs_create("C:\\Impossible\\System\\Config", VFS_DIRECTORY);
vfs_create("C:\\Impossible\\System\\Config\\Registry", VFS_DIRECTORY);
vfs_create("C:\\Impossible\\System\\Cursors", VFS_DIRECTORY);
vfs_create("C:\\Impossible\\System\\Logs", VFS_DIRECTORY);
vfs_create("C:\\Impossible\\System32", VFS_DIRECTORY);
vfs_create("C:\\Impossible\\Fonts", VFS_DIRECTORY);
vfs_create("C:\\Impossible\\Icons", VFS_DIRECTORY);
vfs_create("C:\\Impossible\\Media", VFS_DIRECTORY);
vfs_create("C:\\Impossible\\Web\\Wallpaper", VFS_DIRECTORY);
vfs_create("C:\\Impossible\\Themes", VFS_DIRECTORY);
vfs_create("C:\\Impossible\\Temp", VFS_DIRECTORY);
vfs_create("C:\\Program Files", VFS_DIRECTORY);
vfs_create("C:\\Users", VFS_DIRECTORY);
vfs_create("C:\\Recycle", VFS_DIRECTORY);
```

## Relationship to Build-Time Resources

Files from the workspace `resources/` directory at build time map to the OS filesystem at runtime:

| Build-time path | Sysroot path | OS runtime path |
|----------------|-------------|-----------------|
| `resources/backgrounds/background.jpg` | `sysroot/Impossible/Web/Wallpaper/default.jpg` | `C:\Impossible\Web\Wallpaper\default.jpg` |
| `resources/cursors/*` | `sysroot/Impossible/System/Cursors/*` | `C:\Impossible\System\Cursors\*` |
| `resources/fonts/*.ttf` | `sysroot/Impossible/Fonts/*.ttf` | `C:\Impossible\Fonts\*.ttf` |
| `resources/system/icons.ires` | `sysroot/Impossible/System/icons.ires` | `C:\Impossible\System\icons.ires` |

## OS Architecture Summary

| Component | Description |
|-----------|-------------|
| **Kernel** | ELF binary, loaded by custom UEFI bootloader (`bootx64.c`) |
| **Boot chain** | UEFI firmware → BOOTX64.EFI → kernel.exe (via `boot_info`) |
| **Native programs** | ELF format, compiled with `clang-19 --target=x86_64-elf` |
| **Windows compat** | PE format support via kernel PE loader |
| **Syscall ABI** | `INT 0x80`, number in RAX, args in RDI/RSI/RDX |
| **Filesystem** | IXFS (in-memory) + FAT32 (disk), VFS layer with drive letters A:-Z: |
| **Path separator** | Backslash `\` (Windows convention) |
| **Display** | Framebuffer at 1280×720, 32bpp BGRA (UEFI GOP) |
| **Compositor** | Software 2D compositing with dirty rectangle tracking |
