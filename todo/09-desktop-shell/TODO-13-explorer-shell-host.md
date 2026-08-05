---
schema_version: 1
id: explorer-shell-host
domain: 09-desktop-shell
status: active
title: "TODO-13 -- Explorer Shell Host (`explorer.exe`)"
---

# TODO-13 -- Explorer Shell Host (`explorer.exe`)

> **Goal:** Ship a **Windows-named shell host** at `C:\Windows\explorer.exe` that boots after install (unless [`../10-platform-services/TODO-11-installer-iso.md`](../10-platform-services/TODO-11-installer-iso.md) `InstallerMode=1` selects `installer.exe`). The host owns **desktop + taskbar integration**, **shell folder launch** (`ShellExecute` / `open` verb), and minimal **Run / RunOnce** startup hooks. It does **not** replace the full File Manager UX epic in [`TODO-09-file-manager.md`](TODO-09-file-manager.md); that TODO stays the deep four-zone browser. Explorer here is the **small PE** that wires shell32 exports from [`../10-platform-services/TODO-C-shell32-export-master-table.md`](../10-platform-services/TODO-C-shell32-export-master-table.md) Tier 1 into the existing compositor and WM.

> [!IMPORTANT]
> **COM-less MVP:** First milestones avoid `IShellWindows`, `WebView`, and shell namespace COM. Use direct calls into `shell32` stubs + WM APIs from [`../10-platform-services/TODO-A-user32-export-master-table.md`](../10-platform-services/TODO-A-user32-export-master-table.md). Add COM rows later under a new section with explicit XREF to a future shell COM TODO if needed.

## Inputs

| Path / TODO | Purpose |
|-------------|---------|
| [`../10-platform-services/TODO-C-shell32-export-master-table.md`](../10-platform-services/TODO-C-shell32-export-master-table.md) | Required `shell32` exports for boot shell |
| [`../10-platform-services/TODO-A-user32-export-master-table.md`](../10-platform-services/TODO-A-user32-export-master-table.md) | Message pump + HWND wiring |
| [`../10-platform-services/TODO-08-win32-api-surface.md`](../10-platform-services/TODO-08-win32-api-surface.md) Sections 12 and 14 | `shell32.c` and `ShellExecute` |
| [`../08-graphics-ui/TODO-10-taskbar.md`](../08-graphics-ui/TODO-10-taskbar.md) | Taskbar SYS hooks and progress APIs |
| [`TODO-09-file-manager.md`](TODO-09-file-manager.md) | Optional host for `explore` verb; keep scope split |
| [`../10-platform-services/TODO-11-installer-iso.md`](../10-platform-services/TODO-11-installer-iso.md) Section 2 | `InstallerMode` vs `explorer.exe` boot |
| [`TODO-07-win32-pe-loader.md`](../10-platform-services/TODO-07-win32-pe-loader.md) | `pe_exec` ring-3 launch |

## Outcome

- `C:\Windows\explorer.exe` exists on the system image and launches the shell host PE.
- Desktop + taskbar show; `ShellExecuteA("open", ...)` launches File Manager or apps per file assoc.
- Registry `HKLM\...\InstallerMode` still switches to `installer.exe` when set (installer TODO).

## Implementation Order

| Order | Deliverable | Depends On | Status |
| :---: | ----------- | ---------- | :----: |
| 1 | Minimal `explorer.exe` PE (message loop, hidden or minimal main HWND) | D10 T07 §7; D10 T08 §10; D10 TODO-A Tier 1 | [ ] |
| 2 | Wire `SHGetFolderPath` subset for Desktop / Startup paths | D10 T08 §12; D10 TODO-C Tier 1 | [ ] |
| 3 | Taskbar heartbeat: integrate with [`../08-graphics-ui/TODO-10-taskbar.md`](../08-graphics-ui/TODO-10-taskbar.md) window list | D08 TODO-08-taskbar §5 | [ ] |
| 4 | `ShellExecute` open verb to filemgr or assoc target | D10 T08 §14; D09 T09 | [ ] |
| 5 | Boot selection: `explorer.exe` default; installer override unchanged | D10 T11 §2 | [ ] |

## 1. Binary layout and boot

- [ ] Place built `explorer.exe` on the image at `C:\Windows\explorer.exe` (same leaf name as Windows; parent may be `Impossible` vs `Windows` only where the image policy already standardizes, document in commit if parent path differs).
- [ ] Desktop init: if not installer mode, `pe_exec("C:\\Windows\\explorer.exe")` (or documented canonical path) after compositor ready.
- [ ] Commit: `"desktop: explorer shell host PE on disk + boot wire"`

## 2. Shell32 dependency gate

- [ ] Enumerate required exports from [`../10-platform-services/TODO-C-shell32-export-master-table.md`](../10-platform-services/TODO-C-shell32-export-master-table.md) Tier 1 and Tier 1b; fail boot with klog if any row still `[ ]` when enabling strict gate (optional debug flag).
- [ ] Commit: `"desktop: explorer shell32 export gate"`

## 3. Taskbar and desktop integration

- [ ] Register shell host window class; create invisible or minimal root window for message pump if required by WM.
- [ ] Subscribe to WM events needed by taskbar (`TODO-08-taskbar` XREF) without duplicating filemgr UI.
- [ ] Commit: `"desktop: explorer taskbar integration"`

## 4. ShellExecute open-verb wiring

Wires `ShellExecute` through the file-manager open verb and the association store so double-click opens files via the registered handler app. Extends `02-kernel-core/TODO-14-registry-completion.md` HKCR defaults.

- [ ] `ShellExecute(op='open', path)` dispatches to registered handler via HKCR.
- [ ] Commit: `"shell: wire ShellExecute open verb through explorer.exe"`

**Test checkpoint:** `ShellExecute('open', 'C:\\hello.txt')` opens notepad; registered association table matches HKCR file-class entries.

---

## 5. Boot-time explorer default

Sets `explorer.exe` as the default shell on boot unless the installer override flag is present. Owned by the service-manager init path.

- [ ] Wire default-shell lookup through `HKLM\SYSTEM\CurrentControlSet\Control\WinLogon` Shell value; fall back to `cmd.exe` if missing.
- [ ] Commit: `"shell: explorer.exe boot default + installer override"`

**Test checkpoint:** Clean boot lands on desktop with `explorer.exe` running; installer mode boot lands in the installer without starting explorer.

---

## OS Comparison

| ⭐  | Feature            | 🪟 Win11                 | 🐧 Linux                                | 🚀 Impossible OS                         |
| --- | ------------------ | ------------------------ | --------------------------------------- | ---------------------------------------- |
| 💎  | Shell host process | explorer.exe + DWM stack | DE-specific entry (`gnome-shell`, etc.) | explorer.exe PE + TODO-C Tier 1 export gates |

## Unit Tests

**Note:** Headless or QEMU serial tests: boot with `InstallerMode=0`, assert `explorer.exe` task appears in task list or serial marker from host. No live `boot_progress` calls in kernel unit tests per project rules; user-mode tier tests belong under compat matrix when wired.

## Verification

- `bash scripts/build.sh` after wiring `explorer.exe` into the desktop boot path.
- QEMU headless: boot with `InstallerMode=0`; serial shows shell host started (marker TBD when implemented).
- Flip `InstallerMode=1`; confirm `installer.exe` still replaces shell per [`../10-platform-services/TODO-11-installer-iso.md`](../10-platform-services/TODO-11-installer-iso.md).

**Test runner:** `scripts/debug/kernel/run-boot-tests.bat` (SUITE=boot)

## History

| Date | Action | Summary |
|------|--------|---------|
| 2026-04-14 | Created TODO-13 explorer shell host plan | Initial scope: COM-less MVP, XREF TODO-A/C + installer boot. |
| 2026-04-14 | gap-analysis | Shell host split from TODO-09 file manager; export truth in D10 TODO-C. |
| 2026-04-14 | validate | OS Comparison 5-column template; stripped model tags from headings; Verification + boot test runner; Depends On uses DNN TNN notation. |
