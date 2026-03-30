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

> [!NOTE]
> `sdk/src/ixfs-mount/` is created by TODO-02. Until TODO-02 §1 scaffolds the first tool, `sdk/build.sh` will discover zero tools. Test §1–§4 with a minimal stub `Makefile` in `sdk/src/test-tool/` if TODO-02 hasn't landed yet.

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

| ⭐  | Order | Deliverable                                     | Depends On | Status |
| --- | :---: | ----------------------------------------------- | ---------- | :----: |
| 💎  |   1   | Linux build script (bash)                       | —          |  [ ]   |
| 💎  |   2   | Windows build script (PowerShell + bat)         | —          |  [ ]   |
| 💎  |   3   | Dependency detection and reporting              | §1, §2     |  [ ]   |
| 💎  |   4   | Auto-discovery of SDK tool directories          | §1, §2     |  [ ]   |

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
- [ ] Commit: `"sdk: Linux build script with progress and dependency detection"`

**Test checkpoint (Linux):**
- `bash sdk/build.sh` completes without error
- Output contains `[1/1] Building test-tool...` (or ixfs-mount if TODO-02 landed)
- Output ends with `SDK BUILD OK` and total time
- `ls sdk/tools/` shows compiled binary

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

**Test checkpoint (Windows):**
- `sdk\build.bat` completes without error
- Output contains `[1/1] Building test-tool...`
- Output ends with `SDK BUILD OK` and total time
- `dir sdk\tools\` shows compiled binary

## 3. Dependency Detection and Reporting
Clear messages when required dependencies are missing.

**Files:** `sdk/build.sh`, `sdk/build.ps1`

- [ ] Linux: check `pkg-config --exists fuse3` or `dpkg -l libfuse3-dev`
- [ ] Windows: check `Test-Path "C:\Program Files (x86)\WinFsp\inc\winfsp\winfsp.h"`
- [ ] Missing dependency: print install instructions, not just "not found"
- [ ] Example: `MISSING: libfuse3-dev — install with: sudo apt install libfuse3-dev`
- [ ] Example: `MISSING: WinFsp — install from: https://winfsp.dev/`
- [ ] Commit: `"sdk: dependency detection with install instructions"`

**Test checkpoint (Linux + Windows):**
- Linux: uninstall `libfuse3-dev`, run `bash sdk/build.sh` — output contains `MISSING: libfuse3-dev — install with: sudo apt install libfuse3-dev`
- Windows: rename WinFsp dir, run `sdk\build.bat` — output contains `MISSING: WinFsp — install from: https://winfsp.dev/`
- Build continues for tools that don't need the missing dep (graceful skip)

## 4. Auto-Discovery of SDK Tool Directories
Build discovers new tools automatically — add a directory to `sdk/src/`, it gets built.

**Files:** `sdk/build.sh`, `sdk/build.ps1`

- [ ] Scan `sdk/src/*/` for directories containing `Makefile` (Linux) or `build-win.bat` (Windows)
- [ ] Skip directories without build files (no error, just skip)
- [ ] Report: `Found N SDK tools: ixfs-mount, disk-inspector, ...`
- [ ] Build in alphabetical order (deterministic)
- [ ] Commit: `"sdk: auto-discover SDK tool directories"`

**Test checkpoint (Linux + Windows):**
- Create `sdk/src/test-tool/Makefile` with a trivial target, run `bash sdk/build.sh`
- Output contains `Found 2 SDK tools: ixfs-mount, test-tool` (or `Found 1 SDK tools: test-tool` if no ixfs-mount yet)
- Dir without Makefile is silently skipped (no error)
- Tools built in alphabetical order

---

## OS Comparison

| ⭐ | Feature            | Win11                | Linux                | Impossible OS               |
|----|--------------------|-----------------------|----------------------|-----------------------------|
| 💎 | Build system       | ✅ MSBuild / CMake   | ✅ make / CMake      | ⬜ §1+§2 bash + PowerShell |
| ⭐ | Progress output    | ❌ Verbose only      | ❌ Verbose only      | ⬜ §1 colored progress bar |
| ⭐ | Dep detection      | ❌ Manual install    | ❌ Manual install    | ⬜ §3 auto-detect + guide  |
| ⭐ | Watch mode         | ❌ None built-in     | ❌ None built-in     | ⬜ Planned `--watch` flag  |
| ⭐ | Cross-host build   | ❌ Manual toolchain  | ❌ Manual toolchain  | ⬜ Planned one-flag cross  |

## Verification

- [ ] Linux: `bash sdk/build.sh` — builds all tools, progress shown, timing reported
- [ ] Windows: `sdk\build.bat` — same experience
- [ ] Missing deps: clear install instructions printed
- [ ] New tool: add directory, auto-discovered on next build
