---
schema_version: 1
id: obbrowse-namespace-browser
domain: 13-tools-accessories
status: active
title: "TODO-01 -- ObBrowse: Object Namespace Browser"
---

# TODO-01 -- ObBrowse: Object Namespace Browser

> **Goal:** A user-mode GUI tool (`obbrowse.exe`) that displays the kernel object namespace as a tree -- like Windows WinObj or Process Explorer's handle viewer. Shows directory objects (`\`, `\Device`, `\BaseNamedObjects`, `\DosDevices`, `\Sessions`), object types, reference counts, and security descriptors. Essential for kernel development debugging and verifying Object Manager correctness.

> [!IMPORTANT]
> **Current state:** The kernel Object Manager namespace is fully functional (11 built-in types, 6 root directories). `NtOpenDirectoryObject` + `NtQueryDirectoryObject` syscalls exist (`SYS_OPENDIROBJ`, `SYS_QUERYDIROBJ`). No user-mode tool to browse it -- only visible via serial log (`ob: Namespace: \, \Device, \KernelObjects, ...`).

---

## Inputs

- `include/kernel/ob/ob.h` -- `NtOpenDirectoryObject`, `NtQueryDirectoryObject`, `OBJECT_DIRECTORY_INFORMATION`
- `include/kernel/ob/ob_ns.h` -- `ObLookupObjectByName`
- `src/kernel/sched/syscall.c` -- `SYS_OPENDIROBJ` (line 381), `SYS_QUERYDIROBJ` (line 392)
- `src/kernel/ob/ob_ns.c` -- namespace root directories (line 350)
- → XREF: `02-kernel-core/TODO-05-object-manager.md` -- OB implementation (was TODO-03, deferred this tool)
- → XREF: `00-infrastructure/TODO-03-kernel-test-harness.md` -- unit test wiring for test_register_obbrowse()

---

## Outcome

- `obbrowse.exe` deployed to `C:\Impossible\System32\`
- Launchable from shell: `C:\> obbrowse` or from Start Menu
- Tree view of the entire object namespace
- Click a directory → expand children via `NtQueryDirectoryObject`
- Click an object → show type, name, ref count in detail panel
- Useful for kernel debugging: see all registered devices, named events, mutexes, symlinks

---

## Implementation Order

| ⭐  | Order | Deliverable                                             | Depends On | Status |
| --- | :---: | ------------------------------------------------------- | ---------- | :----: |
| 💎  |   1   | User-mode syscall wrappers for OB directory enumeration | --         |  [ ]   |
| 💎  |   2   | Console-mode ObBrowse (text tree dump to stdout)        | §1         |  [ ]   |
| ⭐  |   3   | GUI ObBrowse with tree view + detail panel              | §2         |  [ ]   |
| ⭐  |   4   | Object type icons and security descriptor display       | §3         |  [ ]   |

> 💎 = parity -- Windows ships WinObj (Sysinternals) and Process Explorer handle view.
> ⭐ = exclusive -- integrated into the OS itself, not a third-party download.

---

## 1. User-Mode Syscall Wrappers

Wrap the existing kernel syscalls for use from user-mode code.

- [ ] Create `user/include/ob_browse.h` with:
  ```c
  int ob_open_directory(const char *path);     /* returns handle */
  int ob_query_directory(int handle, OB_DIR_INFO *buf, int max, int *count);
  void ob_close(int handle);
  ```
- [ ] Implement wrappers using `SYS_OPENDIROBJ`, `SYS_QUERYDIROBJ`, `SYS_CLOSEHANDLE`
- [ ] Test: open `\` → enumerate → expect `Device`, `KernelObjects`, `BaseNamedObjects`
- [ ] Commit: `"tools: user-mode OB directory enumeration syscall wrappers"`

---

## 2. Console-Mode ObBrowse

Text-based namespace tree -- works before GUI is needed.

- [ ] Create `user/apps/obbrowse.c`:
  - Open root `\` via `ob_open_directory("\\")`
  - Recursive enumeration: for each entry, if type is `Directory`, recurse
  - Print indented tree: `\Device\`, `\Device\Ahci0`, `\BaseNamedObjects\TestEvent`, etc.
  - Print type name next to each entry: `[Directory]`, `[Event]`, `[Mutant]`, `[File]`
- [ ] Add to Makefile: compile as `obbrowse.exe`, deploy to sysroot
- [ ] Test: `C:\> obbrowse` → prints namespace tree to terminal
- [ ] Commit: `"tools: console-mode obbrowse.exe -- text namespace tree"`

---

## 3. GUI ObBrowse with Tree View

Graphical version with split-pane layout.

- [ ] Create `src/apps/obbrowse/obbrowse_gui.c`:
  - 640×480 window: left pane (tree), right pane (detail)
  - Left pane: expandable tree of directories (click `+` to expand)
  - Right pane: list of objects in selected directory (name, type, handle count)
  - Populate root on launch: `\Device`, `\KernelObjects`, `\BaseNamedObjects`, `\DosDevices`, `\Sessions`
  - Lazy expansion: only enumerate children when user clicks to expand
- [ ] Toolbar: Refresh button, address bar showing current path
- [ ] Status bar: object count in current directory
- [ ] Commit: `"tools: GUI obbrowse.exe -- tree view namespace browser"`

---

## 4. Object Detail and Type Icons

Enhanced display with per-type icons and security info.

- [ ] Icon per object type: Directory (folder), Event (flag), Mutant (lock), File (document), Process (gear), Thread (arrow), SymbolicLink (shortcut arrow)
- [ ] Detail panel when object selected: Name, Type, Ref Count, Handle Count, Flags
- [ ] Security tab (if security descriptor present): Owner SID, DACL ACEs
- [ ] Commit: `"tools: obbrowse.exe type icons + security display"`

---

## OS Comparison

| ⭐  | Feature                  | 🪟 Win11                 | 🐧 Linux             | 🚀 Impossible OS          |
| --- | ------------------------ | ------------------------ | -------------------- | ------------------------- |
| 💎  | Object namespace browser | ✅ WinObj (Sysinternals) | ⚠️ /proc + /sys      | ⬜ §2–§3                  |
| 💎  | Handle viewer            | ✅ Process Explorer      | ⚠️ lsof              | ⬜ §3 detail panel        |
| ⭐  | Built-in (not 3rd party) | ❌ WinObj is download    | ✅ /proc is built-in | ⬜ §2–§3 -- ships with OS |
| ⭐  | Security descriptor view | ✅ WinObj shows SD       | ❌ Not in /proc      | ⬜ §4                     |

---

## Unit Tests

> Wire into `test_runner_init()` via `test_register_obbrowse()` (XREF: `00-infrastructure/TODO-03-kernel-test-harness.md`).
> Boot tests run with `debug=1` or `test=1` in boot.conf.

- [ ] Create `src/kernel/test/test_obbrowse.c` with:
  - `SYS_OPENDIROBJ("\\")` returns valid handle from user mode
  - `SYS_QUERYDIROBJ` returns entries with non-empty names
  - Root directory contains at least 3 entries (Device, KernelObjects, BaseNamedObjects)
  - Opening nonexistent directory returns error
- [ ] Register in `test_runner_init()`: `test_register_obbrowse()`
- [ ] Commit: `"test: add obbrowse syscall test suite"`

---

## Verification

- [ ] `bash scripts/build.sh clean` → `=== BUILD OK ===`
- [ ] QEMU WHPX: `C:\> obbrowse` prints namespace tree with `\Device`, `\BaseNamedObjects`
- [ ] QEMU WHPX: GUI version shows tree with expandable directories
- [ ] Object count matches `ob: Registered 11 built-in types` from boot log
- [ ] Bare metal: obbrowse.exe runs correctly on real hardware
- [ ] Commit: `"tools: obbrowse.exe verified -- namespace browser complete"`
