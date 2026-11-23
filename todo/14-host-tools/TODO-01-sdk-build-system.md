---
schema_version: 1
id: sdk-build-system
domain: 14-host-tools
status: active
title: "TODO-01 -- Host Tools Build"
---

# TODO-01 -- Host Tools Build

> **Goal:** Create a build system for host tools that mirrors the kernel's `scripts/build.sh` experience -- progress bars, colored output, error extraction, and dependency detection. host tools build independently from the kernel using the host's native compiler.

> [!IMPORTANT]
> Host tools are NOT built by `scripts/build.sh` (kernel build). They have their own build script:
> - `bash tools/build-host.sh` -- uses system gcc/clang
> - Discovers tools in `tools/*/`, shows progress, outputs to `build/host-tools/`

## Inputs

- [`scripts/build.sh`](../../scripts/build.sh) -- kernel build script (reference for progress/output style)
- `tools/` -- host tool source directories
- → XREF: `D00 T01 §1-§3` -- repo-local developer tooling owns the top-level setup, host-profile, and wrapper contract this flow complements

## Outcome

- `bash tools/build-host.sh` -- builds all host tools
- Progress bar, colored output, timing -- same feel as kernel build
- Auto-detects dependencies and reports missing ones clearly
- `tools/build-host.sh clean` -- clean build
- Each tool in `tools/*/` is discovered and built automatically

## Source Layout

```
tools/
  build-host.sh         # Linux build script (bash)
  ixfs-mount/           # First host tool (TODO-02), with its mount scripts
  (future tools...)
build/host-tools/       # Compiled output (gitignored)
```

## Implementation Order

| ⭐  | Order | Deliverable                          | Depends On     | Status |
| --- | :---: | ------------------------------------ | -------------- | :----: |
| 💎  |   1   | 🐧 Linux build script (bash)         | --             |  [x]   |
| 💎  |   2   | Dependency detection and reporting   | §1             |  [x]   |
| 💎  |   3   | Auto-discovery of host tool dirs     | §1             |  [x]   |
| 💎  |   4   | host tools in CI, honest skip report | §1, §2, T02 §6 |  [ ]   |

---

## 1. Linux Build Script
Build all host tools on Linux with progress output.

**Files:** `tools/build-host.sh`

- [x] Detect host compiler: prefer `gcc`, fall back to `clang`
- [x] Check for `libfuse3-dev` (pkg-config or header check) -- warn if missing
- [x] Discover tool directories: `for dir in tools/*/; do ...`
- [x] Each tool directory must have a `Makefile` -- run `make -C $dir`
- [x] Progress bar: `[1/N] Building ixfs-mount...`
- [x] Timing: report per-tool and total build time
- [x] Color output: green OK, red FAILED, yellow WARN
- [x] `tools/build-host.sh clean` -- run `make -C $dir clean` for each tool
- [x] Error extraction: show relevant compiler errors on failure
- [x] Commit: `"sdk: Linux build script with progress and dependency detection"`

**Test checkpoint (Linux):**
- `bash tools/build-host.sh` completes without error
- Output ends with `HOST TOOLS BUILD OK` and total time

## 2. Dependency Detection and Reporting
Clear messages when required dependencies are missing.

**Files:** `tools/build-host.sh`

- [x] Check `pkg-config --exists fuse3` or `dpkg -s libfuse3-dev`
- [x] Missing dependency: print install instructions, not just "not found"
- [x] Build continues for tools that don't need the missing dep (graceful skip)
- [x] Commit: `"sdk: dependency detection with install instructions"`

**Test checkpoint (Linux):**
- `MISSING: libfuse3-dev -- install with: sudo apt install libfuse3-dev` shown when missing

## 3. Auto-Discovery of Host Tool Directories

> **Spawned-by:** root

Build discovers new tools automatically -- add a directory to `tools/`, it gets built.

**Files:** `tools/build-host.sh`

- [x] Scan `tools/*/` for directories containing `Makefile`
- [x] Skip directories without build files (no error, just skip)
- [x] Report: `Found N host tools: ixfs-mount, ...`
- [x] Build in alphabetical order (deterministic)
- [x] Commit: `"sdk: auto-discover SDK tool directories"`

**Test checkpoint (Linux):**
- Dir without Makefile is silently skipped
- Tools built in alphabetical order

---

## 4. Host Tools in CI and Honest Skip Reporting

> **Spawned-by:** root

Found while writing the host-tools docs pages (`00-infrastructure/TODO-10` §26): the build reports success for tools it never built, and nothing runs it, so the tools drifted from the kernel unnoticed.

**Files:** `tools/build-host.sh`, `tools/ixfs-mount/Makefile`, a CI workflow or `scripts/test-tooling.sh`

- [ ] Report a tool its Makefile skipped (missing libfuse3) as `SKIPPED`, not `OK`, and count it apart from built tools
  - Measured 2026-09-30 on WSL2 without libfuse3: `[1/1] Building ixfs-mount...  OK` and `1 tools built`, while the output folder (then `sdk/tools/`) stayed empty.
  - Needs a Makefile-to-script signal (an exit code or a marker file), not a grep of make's output.
- [ ] Build the host tools and run `test_ixfs_core` against a freshly built image on every push that touches `tools/ixfs-mount/`, `tools/build-host.sh` or the IXFS headers, with libfuse3 installed so `ixfs-mount` really compiles
  - Today no workflow, lint check or tooling suite runs `tools/build-host.sh`; the IXFS inode-size drift in TODO-02 §6 went unseen because of it. -> XREF: `14-host-tools/TODO-02-ixfs-mount.md` §6 (item: "Add a regression net")
- [ ] Commit: `"tools: report skipped host tools honestly and build them in CI"`

**Test checkpoint:** on a host without libfuse3 the summary reads `0 built, 1 skipped`; with it, CI builds `ixfs-mount` and `test_ixfs_core` lists a non-empty root directory. That last check needs the parser fix in `14-host-tools/TODO-02` §6 first; the skip reporting and the CI build do not.

---

## OS Comparison

| ⭐  | Feature         | 🪟 Win11           | 🐧 Linux          | 🚀 Impossible OS           |
| --- | --------------- | ------------------ | ----------------- | -------------------------- |
| 💎  | Build system    | ✅ MSBuild / CMake | ✅ make / CMake   | ✅ §1 bash build script    |
| ⭐  | Progress output | ❌ Verbose only    | ❌ Verbose only   | ✅ §1 colored progress bar |
| ⭐  | Dep detection   | ❌ Manual install  | ❌ Manual install | ✅ §2 auto-detect + guide  |

## Verification

- [x] Linux: `bash tools/build-host.sh` -- builds all tools, progress shown, timing reported
- [x] Missing deps: clear install instructions printed
- [x] New tool: add directory, auto-discovered on next build
