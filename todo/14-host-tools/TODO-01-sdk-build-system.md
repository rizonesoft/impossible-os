# TODO-01 — SDK Build System

> **Goal:** Create a build system for SDK tools that mirrors the kernel's `scripts/build.sh` experience — progress bars, colored output, error extraction, and cross-platform support. SDK tools build independently from the kernel using the host's native compiler.

> [!IMPORTANT]
> SDK tools are NOT built by `scripts/build.sh` (kernel build). They have their own build script that:
> - Detects the host OS (Windows/Linux)
> - Finds the correct compiler (MSVC, MinGW, gcc, clang)
> - Finds SDK dependencies (WinFsp on Windows, libfuse3 on Linux)
> - Builds all SDK tools with progress output
> - Outputs binaries to `sdk/tools/`

## Inputs

- [`scripts/build.sh`](../../scripts/build.sh) — kernel build script (reference for progress/output style)
- `sdk/src/` — SDK tool source directories

## Outcome

- `bash sdk/build.sh` (Linux) or `sdk\build.bat` (Windows) — builds all SDK tools
- Progress bar, colored output, timing — same feel as kernel build
- Auto-detects dependencies and reports missing ones clearly
- `sdk/build.sh clean` — clean build
- Each tool in `sdk/src/*/` is discovered and built automatically

## Source Layout

```
sdk/
  build.sh              # Linux build script (bash)
  build.bat              # Windows build launcher (calls build.ps1)
  build.ps1              # Windows build script (PowerShell)
  src/
    ixfs-mount/          # First SDK tool (TODO-02)
    (future tools...)
  tools/                 # Compiled output (gitignored)
  scripts/               # Mount/utility scripts
```

## Implementation Order

| ⭐  | Order | Deliverable                                    | Depends On | Status |
| --- | :---: | ---------------------------------------------- | ---------- | :----: |
| 💎  |   1   | Linux build script (bash)                      | —          |  [ ]   |
| 💎  |   2   | Windows build script (PowerShell + bat)         | —          |  [ ]   |
| 💎  |   3   | Dependency detection and reporting              | §1, §2     |  [ ]   |
| 💎  |   4   | Auto-discovery of SDK tool directories          | §1, §2     |  [ ]   |

---

## 1. Linux Build Script
Build all SDK tools on Linux with progress output.

**Files:** `sdk/build.sh`

- [ ] Detect host compiler: prefer `gcc`, fall back to `clang`
- [ ] Check for `libfuse3-dev` (pkg-config or header check) — warn if missing
- [ ] Discover tool directories: `for dir in sdk/src/*/; do ...`
- [ ] Each tool directory must have a `Makefile` — run `make -C $dir`
- [ ] Progress bar: `[1/N] Building ixfs-mount...`
- [ ] Timing: report per-tool and total build time
- [ ] Color output: green OK, red FAILED, yellow WARN
- [ ] `sdk/build.sh clean` — run `make -C $dir clean` for each tool
- [ ] Error extraction: show relevant compiler errors on failure
- [ ] Commit: `"sdk: Linux build script with progress and dependency detection"`

**Test checkpoint:** `bash sdk/build.sh` — builds ixfs-mount, shows progress, reports time.

## 2. Windows Build Script
Build all SDK tools on Windows with progress output.

**Files:** `sdk/build.bat`, `sdk/build.ps1`

- [ ] `build.bat` — launcher that calls `build.ps1` via PowerShell
- [ ] Detect compiler: check for `cl.exe` (MSVC), fall back to `gcc` (MinGW)
- [ ] Check for WinFsp SDK at `C:\Program Files (x86)\WinFsp\`
- [ ] Discover tool directories: `Get-ChildItem sdk\src\*`
- [ ] Each tool directory must have a `build-win.bat` — invoke it
- [ ] Progress: `[1/N] Building ixfs-mount...`
- [ ] Timing and color output matching Linux script
- [ ] `sdk\build.bat clean` — clean build
- [ ] Commit: `"sdk: Windows build script with progress and dependency detection"`

**Test checkpoint:** `sdk\build.bat` — builds ixfs-mount.exe, shows progress.

## 3. Dependency Detection and Reporting
Clear messages when required dependencies are missing.

**Files:** `sdk/build.sh`, `sdk/build.ps1`

- [ ] Linux: check `pkg-config --exists fuse3` or `dpkg -l libfuse3-dev`
- [ ] Windows: check `Test-Path "C:\Program Files (x86)\WinFsp\inc\winfsp\winfsp.h"`
- [ ] Missing dependency: print install instructions, not just "not found"
- [ ] Example: `MISSING: libfuse3-dev — install with: sudo apt install libfuse3-dev`
- [ ] Example: `MISSING: WinFsp — install from: https://winfsp.dev/`
- [ ] Commit: `"sdk: dependency detection with install instructions"`

**Test checkpoint:** Uninstall libfuse3-dev, run build, see clear install instructions.

## 4. Auto-Discovery of SDK Tool Directories
Build discovers new tools automatically — add a directory to `sdk/src/`, it gets built.

**Files:** `sdk/build.sh`, `sdk/build.ps1`

- [ ] Scan `sdk/src/*/` for directories containing `Makefile` (Linux) or `build-win.bat` (Windows)
- [ ] Skip directories without build files (no error, just skip)
- [ ] Report: `Found N SDK tools: ixfs-mount, disk-inspector, ...`
- [ ] Build in alphabetical order (deterministic)
- [ ] Commit: `"sdk: auto-discover SDK tool directories"`

**Test checkpoint:** Add empty `sdk/src/test-tool/Makefile`, run build, see it discovered.

---

## OS Comparison

| ⭐ | Feature                 | Win11 SDK                   | Linux SDK                    | Impossible OS SDK            |
|----|-------------------------|-----------------------------|------------------------------|------------------------------|
| 💎 | Build system            | ✅ MSBuild / CMake           | ✅ make / CMake              | ⬜ §1+§2 — bash + PowerShell |
| ⭐ | Progress output         | ❌ MSBuild verbose only      | ❌ make verbose only         | ⬜ §1 — colored progress bar |
| ⭐ | Dep detection           | ❌ Manual install            | ❌ Manual install            | ⬜ §3 — auto-detect + guide  |

## Verification

- [ ] Linux: `bash sdk/build.sh` — builds all tools, progress shown, timing reported
- [ ] Windows: `sdk\build.bat` — same experience
- [ ] Missing deps: clear install instructions printed
- [ ] New tool: add directory, auto-discovered on next build
