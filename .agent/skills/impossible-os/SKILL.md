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
│   │   └── Config\              ← System configuration files
│   ├── Fonts\                   ← System fonts (.ttf, .otf)
│   ├── Cursors\                 ← Cursor theme files
│   ├── Icons\                   ← System icons
│   ├── Sounds\                  ← System sound files
│   ├── Wallpapers\              ← Default wallpaper images
│   ├── Themes\                  ← UI theme definitions
│   ├── Bin\                     ← System executables (shell, tools)
│   └── Lib\                     ← Shared libraries, DLL stubs
├── Programs\                    ← Installed applications (like C:\Program Files)
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
├── Documents\                   ← Legacy/shared documents (existing)
│   └── backgrounds\             ← Desktop backgrounds (existing)
└── Temp\                        ← Temporary files
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
| **Cursors** | `C:\Impossible\Cursors\` | Cursor theme raw images |
| **System icons** | `C:\Impossible\Icons\` | App icons, file type icons |
| **System sounds** | `C:\Impossible\Sounds\` | Startup, click, error sounds |
| **Wallpapers** | `C:\Impossible\Wallpapers\` | Default wallpapers |
| **Themes** | `C:\Impossible\Themes\` | Color schemes, UI settings |
| **System binaries** | `C:\Impossible\Bin\` | Shell, system tools |
| **DLL stubs** | `C:\Impossible\Lib\` | kernel32.dll, msvcrt.dll stubs |
| **Drivers** | `C:\Impossible\System\Drivers\` | Device driver files |
| **Config files** | `C:\Impossible\System\Config\` | Boot config, registry-like settings |
| **User files** | `C:\Users\{name}\Documents\` | Per-user documents |
| **Installed apps** | `C:\Programs\{AppName}\` | Third-party applications |
| **Temp files** | `C:\Temp\` | Temporary/scratch files |

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

During kernel init, the system should create the default folder hierarchy:

```c
/* Create Impossible OS system directory structure */
vfs_create("C:\\Impossible", VFS_DIRECTORY);
vfs_create("C:\\Impossible\\System", VFS_DIRECTORY);
vfs_create("C:\\Impossible\\System\\Drivers", VFS_DIRECTORY);
vfs_create("C:\\Impossible\\System\\Config", VFS_DIRECTORY);
vfs_create("C:\\Impossible\\Fonts", VFS_DIRECTORY);
vfs_create("C:\\Impossible\\Cursors", VFS_DIRECTORY);
vfs_create("C:\\Impossible\\Icons", VFS_DIRECTORY);
vfs_create("C:\\Impossible\\Sounds", VFS_DIRECTORY);
vfs_create("C:\\Impossible\\Wallpapers", VFS_DIRECTORY);
vfs_create("C:\\Impossible\\Themes", VFS_DIRECTORY);
vfs_create("C:\\Impossible\\Bin", VFS_DIRECTORY);
vfs_create("C:\\Impossible\\Lib", VFS_DIRECTORY);
vfs_create("C:\\Programs", VFS_DIRECTORY);
vfs_create("C:\\Users", VFS_DIRECTORY);
vfs_create("C:\\Temp", VFS_DIRECTORY);
```

## Relationship to Build-Time Resources

Files from the workspace `resources/` directory at build time map to the OS filesystem at runtime:

| Build-time path | Initrd file | OS runtime path |
|----------------|-------------|-----------------|
| `resources/backgrounds/background.jpg` | `wallpaper.raw` | `C:\Impossible\Wallpapers\default.raw` |
| `resources/start/icon_32.png` | `start_icon.raw` | `C:\Impossible\Icons\start.raw` |
| `resources/cursors/*.png` | `cursor_*.raw` | `C:\Impossible\Cursors\*.raw` |
| `resources/fonts/*.ttf` | `*.ttf` | `C:\Impossible\Fonts\*.ttf` |

## OS Architecture Summary

| Component | Description |
|-----------|-------------|
| **Kernel** | ELF binary, loaded by GRUB (Multiboot2) at boot |
| **Native programs** | ELF format, compiled with `x86_64-elf-gcc` |
| **Windows compat** | PE format support via kernel PE loader |
| **Syscall ABI** | `INT 0x80`, number in RAX, args in RDI/RSI/RDX |
| **Filesystem** | IXFS (in-memory), VFS layer with drive letters A:-Z: |
| **Path separator** | Backslash `\` (Windows convention) |
| **Display** | Framebuffer at 1280×720, 32bpp BGRA |
| **Compositor** | Software 2D compositing (planned: gfx library) |
