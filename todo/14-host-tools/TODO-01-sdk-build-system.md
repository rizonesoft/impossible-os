# TODO-01 — SDK Build System

> **Goal:** Create a build system for SDK tools that mirrors the kernel's `scripts/build.sh` experience — progress bars, colored output, error extraction, and dependency detection. SDK tools build independently from the kernel using the host's native compiler. Works on both Linux (bash) and Windows (PowerShell) with zero manual setup.

> [!IMPORTANT]
> SDK tools are NOT built by `scripts/build.sh` (kernel build). They have their own build scripts:
> - **Linux:** `bash sdk/build.sh` — uses system gcc/clang
> - **Windows:** `sdk\build.bat` → `sdk\build.ps1` — downloads MinGW + WinFsp SDK into `sdk/build/` automatically
> - Both discover tools in `sdk/src/*/`, show progress, and output to `sdk/tools/`

## Inputs

- [`scripts/build.sh`](../../scripts/build.sh) — kernel build script (reference for progress/output style)
- `sdk/src/` — SDK tool source directories

> [!NOTE]
> `sdk/src/ixfs-mount/` is created by TODO-02. Until TODO-02 §1 scaffolds the first tool, `sdk/build.sh` will discover zero tools. Test with a minimal stub `Makefile` in `sdk/src/test-tool/` if TODO-02 hasn't landed yet.

## Outcome

- `bash sdk/build.sh` (Linux) or `sdk\build.bat` (Windows) — builds all SDK tools
- Progress bar, colored output, timing — same feel as kernel build
- Auto-detects dependencies and reports missing ones clearly
- `sdk/build.sh clean` / `sdk\build.bat clean` — clean build
- Each tool in `sdk/src/*/` is discovered and built automatically
- **Windows:** build tools (MinGW, WinFsp SDK) downloaded into `sdk/build/` — no system-wide install, no environment variables, fully self-contained

## Source Layout

```
sdk/
  build.sh              # Linux build script (bash)
  build.bat             # Windows build launcher (calls build.ps1)
  build.ps1             # Windows build script (PowerShell)
  build/                # Downloaded build tools (gitignored)
    mingw64/            # MinGW-w64 toolchain (auto-downloaded)
    winfsp/             # WinFsp SDK headers + libs (auto-downloaded)
  src/
    ixfs-mount/         # First SDK tool (TODO-02)
    (future tools...)
  tools/                # Compiled output (gitignored)
  scripts/              # Mount/utility scripts
```

## Implementation Order

| ⭐  | Order | Deliverable                                  | Depends On | Status |
| --- | :---: | -------------------------------------------- | ---------- | :----: |
| 💎  |   1   | Linux build script (bash)                    | —          |  [x]   |
| 💎  |   2   | Dependency detection and reporting           | §1         |  [x]   |
| 💎  |   3   | Auto-discovery of SDK tool dirs              | §1         |  [x]   |
| 💎  |   4   | Windows build script (PowerShell + bat)      | —          |  [x]   |
| 💎  |   5   | Windows toolchain auto-download              | §4         |  [x]   |

---

## 1. Linux Build Script
Build all SDK tools on Linux with progress output.

**Files:** `sdk/build.sh`

- [x] Detect host compiler: prefer `gcc`, fall back to `clang`
- [x] Check for `libfuse3-dev` (pkg-config or header check) — warn if missing
- [x] Discover tool directories: `for dir in sdk/src/*/; do ...`
- [x] Each tool directory must have a `Makefile` — run `make -C $dir`
- [x] Progress bar: `[1/N] Building ixfs-mount...`
- [x] Timing: report per-tool and total build time
- [x] Color output: green OK, red FAILED, yellow WARN
- [x] `sdk/build.sh clean` — run `make -C $dir clean` for each tool
- [x] Error extraction: show relevant compiler errors on failure
- [x] Commit: `"sdk: Linux build script with progress and dependency detection"`

**Test checkpoint (Linux):**
- `bash sdk/build.sh` completes without error
- Output contains `[1/1] Building test-tool...` (or ixfs-mount if TODO-02 landed)
- Output ends with `SDK BUILD OK` and total time
- `ls sdk/tools/` shows compiled binary

## 2. Dependency Detection and Reporting
Clear messages when required dependencies are missing.

**Files:** `sdk/build.sh`

- [x] Linux: check `pkg-config --exists fuse3` or `dpkg -s libfuse3-dev`
- [x] Missing dependency: print install instructions, not just "not found"
- [x] Example: `MISSING: libfuse3-dev — install with: sudo apt install libfuse3-dev`
- [x] Build continues for tools that don't need the missing dep (graceful skip)
- [x] Commit: `"sdk: dependency detection with install instructions"`

**Test checkpoint (Linux):**
- Uninstall `libfuse3-dev`, run `bash sdk/build.sh` — output contains `MISSING: libfuse3-dev — install with: sudo apt install libfuse3-dev`
- Build continues and succeeds for tools that don't need libfuse3

## 3. Auto-Discovery of SDK Tool Directories
Build discovers new tools automatically — add a directory to `sdk/src/`, it gets built.

**Files:** `sdk/build.sh`

- [x] Scan `sdk/src/*/` for directories containing `Makefile`
- [x] Skip directories without build files (no error, just skip)
- [x] Report: `Found N SDK tools: ixfs-mount, disk-inspector, ...`
- [x] Build in alphabetical order (deterministic)
- [x] Commit: `"sdk: auto-discover SDK tool directories"`

**Test checkpoint (Linux):**
- Create `sdk/src/test-tool/Makefile` with a trivial target, run `bash sdk/build.sh`
- Output contains `Found 1 SDK tools: test-tool`
- Dir without Makefile is silently skipped (no error)
- Tools built in alphabetical order

## 4. Windows Build Script
Build all SDK tools on Windows with progress output. Self-contained — downloads its own toolchain.

**Files:** `sdk/build.bat`, `sdk/build.ps1`

- [x] `build.bat` — launcher that calls `build.ps1` via PowerShell (bypass execution policy)
- [x] Discover tool directories: `Get-ChildItem sdk\src\*` with `Makefile` check
- [x] Each tool directory built via `mingw32-make -C $dir` using local MinGW
- [x] Progress: `[1/N] Building ixfs-mount...` with color (`Write-Host -ForegroundColor`)
- [x] Timing: per-tool and total build time via `[System.Diagnostics.Stopwatch]`
- [x] `sdk\build.bat clean` — clean build
- [x] Error extraction: capture stderr, show relevant compiler errors on failure
- [x] Commit: `"sdk: Windows build script with progress"`

**Test checkpoint (Windows):**
- `sdk\build.bat` completes without error
- Output contains `[1/1] Building test-tool...`
- Output ends with `SDK BUILD OK` and total time
- `dir sdk\tools\` shows compiled binary

## 5. Windows Toolchain Auto-Download
The PowerShell build script downloads MinGW-w64 and WinFsp SDK into `sdk/build/` on first run. No system-wide install, no environment variables, no manual setup.

**Files:** `sdk/build.ps1`

- [x] On first run: check if `sdk/build/mingw64/bin/gcc.exe` exists
- [x] If missing: download MinGW-w64 release .7z from GitHub (x86_64-posix-seh-ucrt)
- [x] Extract to `sdk/build/mingw64/` — use 7z or tar
- [x] Check if `sdk/build/winfsp/inc/winfsp/winfsp.h` exists
- [x] If missing: download WinFsp MSI, extract headers + libs via `msiexec /a`
- [x] All paths resolved relative to `sdk/build/` — no `$env:PATH` modification beyond the build script
- [x] `sdk\build.bat clean` also removes `sdk/build/` for a full reset
- [x] Add `sdk/build/` to `.gitignore`
- [x] Commit: `"sdk: Windows auto-download MinGW + WinFsp SDK into sdk/build/"`

**Test checkpoint (Windows):**
- Delete `sdk/build/`, run `sdk\build.bat` — downloads MinGW + WinFsp, then builds all tools
- Second run: no download, uses cached toolchain, builds immediately
- `sdk\build.bat clean` removes `sdk/build/` and `sdk/tools/`
- `sdk/build/mingw64/bin/gcc.exe --version` shows MinGW gcc
- `dir sdk\build\winfsp\inc\winfsp\winfsp.h` exists

---

## OS Comparison

| ⭐ | Feature            | Win11              | Linux              | Impossible OS              |
|----|--------------------|--------------------|--------------------|-----------------------------|
| 💎 | Build system       | ✅ MSBuild / CMake | ✅ make / CMake    | ✅ §1 bash + §4 PowerShell |
| ⭐ | Progress output    | ❌ Verbose only    | ❌ Verbose only    | ✅ §1 colored progress bar |
| ⭐ | Dep detection      | ❌ Manual install  | ❌ Manual install  | ✅ §2 auto-detect + guide  |
| ⭐ | Zero-setup Windows | ❌ Install VS/CMake| N/A                | ✅ §5 auto-download tools  |
| ⭐ | Watch mode         | ❌ None built-in   | ❌ None built-in   | ⬜ Planned `--watch` flag  |

## Verification

- [x] Linux: `bash sdk/build.sh` — builds all tools, progress shown, timing reported
- [x] Missing deps: clear install instructions printed
- [x] New tool: add directory, auto-discovered on next build
- [ ] Windows: `sdk\build.bat` — downloads tools on first run, builds all tools (test on Windows)
- [ ] Windows: clean run after deleting `sdk/build/` re-downloads and builds (test on Windows)
