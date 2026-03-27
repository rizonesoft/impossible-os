# API Surface & Design Direction

> **Applies to:** All source, docs, SDKs, and OS-visible interfaces.
> **Canonical location:** `.impossible/rules/api-surface.md`

## Production Standard

- Impossible OS is **production-grade**, not a prototype or hobby OS. Difficulty is never a reason to lower standards or accept hacky long-term direction.
- If an API or interface feels like a workaround, **redesign it** instead of normalizing the workaround.

## Win32-First API Surface

- **Win32 is the native API surface.** POSIX-style APIs belong only to the Linux compatibility layer.
- Do not expose POSIX-style interfaces on the native OS-visible surface.

## Path Conventions

- **Windows-style paths are canonical**: use backslashes and drive letters.
  - ✅ `C:\Impossible\System32\notepad.exe`
  - ✅ `C:\Program Files\MyApp\`
  - ❌ `/usr/bin/`, `/etc/`, `/home/`
- In C code, double backslashes: `vfs_open("C:\\Impossible\\Fonts\\Inter.ttf", ...)`

## Product Conventions

- Use Windows-consistent conventions for OS-visible surfaces:
  - Control Panel applets use `.cpl` extension
  - System executables follow Windows naming (`explorer.exe`, `cmd.exe`, `diskpart.exe`)
  - System root is `C:\Impossible\` (equivalent to `C:\Windows\`)

## Example

```c
// CORRECT
vfs_open("C:\\Impossible\\System32\\notepad.exe", VFS_READ);

// WRONG — POSIX path, not valid in Impossible OS
vfs_open("/usr/bin/notepad", VFS_READ);
```
