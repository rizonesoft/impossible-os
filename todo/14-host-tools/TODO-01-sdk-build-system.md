---
schema_version: 1
id: sdk-build-system
domain: 14-host-tools
status: active
title: "TODO-01 -- SDK Build System"
---

# TODO-01 -- SDK Build System

> **Goal:** Create a build system for SDK tools that mirrors the kernel's `scripts/build.sh` experience -- progress bars, colored output, error extraction, and dependency detection. SDK tools build independently from the kernel using the host's native compiler.

> [!IMPORTANT]
> SDK tools are NOT built by `scripts/build.sh` (kernel build). They have their own build script:
> - `bash sdk/build.sh` -- uses system gcc/clang
> - Discovers tools in `sdk/src/*/`, shows progress, outputs to `sdk/tools/`

## Inputs

- [`scripts/build.sh`](../../scripts/build.sh) -- kernel build script (reference for progress/output style)
- `sdk/src/` -- SDK tool source directories
- → XREF: `D00 T01 §1-§3` -- repo-local developer tooling owns the top-level setup, host-profile, and wrapper contract this SDK flow complements

## Outcome

- `bash sdk/build.sh` -- builds all SDK tools
- Progress bar, colored output, timing -- same feel as kernel build
- Auto-detects dependencies and reports missing ones clearly
- `sdk/build.sh clean` -- clean build
- Each tool in `sdk/src/*/` is discovered and built automatically

## Source Layout

```
sdk/
  build.sh              # Linux build script (bash)
  src/
    ixfs-mount/         # First SDK tool (TODO-02)
    (future tools...)
  tools/                # Compiled output (gitignored)
  scripts/              # Mount/utility scripts
```

## Implementation Order

| ⭐  | Order | Deliverable                         | Depends On     | Status |
| --- | :---: | ----------------------------------- | -------------- | :----: |
| 💎  |   1   | 🐧 Linux build script (bash)        | --             |  [x]   |
| 💎  |   2   | Dependency detection and reporting  | §1             |  [x]   |
| 💎  |   3   | Auto-discovery of SDK tool dirs     | §1             |  [x]   |
| 💎  |   4   | SDK tools in CI, honest skip report | §1, §2, T02 §6 |  [ ]   |

---

## 1. Linux Build Script
Build all SDK tools on Linux with progress output.

**Files:** `sdk/build.sh`

- [x] Detect host compiler: prefer `gcc`, fall back to `clang`
- [x] Check for `libfuse3-dev` (pkg-config or header check) -- warn if missing
- [x] Discover tool directories: `for dir in sdk/src/*/; do ...`
- [x] Each tool directory must have a `Makefile` -- run `make -C $dir`
- [x] Progress bar: `[1/N] Building ixfs-mount...`
- [x] Timing: report per-tool and total build time
- [x] Color output: green OK, red FAILED, yellow WARN
- [x] `sdk/build.sh clean` -- run `make -C $dir clean` for each tool
- [x] Error extraction: show relevant compiler errors on failure
- [x] Commit: `"sdk: Linux build script with progress and dependency detection"`

**Test checkpoint (Linux):**
- `bash sdk/build.sh` completes without error
- Output ends with `SDK BUILD OK` and total time

## 2. Dependency Detection and Reporting
Clear messages when required dependencies are missing.

**Files:** `sdk/build.sh`

- [x] Check `pkg-config --exists fuse3` or `dpkg -s libfuse3-dev`
- [x] Missing dependency: print install instructions, not just "not found"
- [x] Build continues for tools that don't need the missing dep (graceful skip)
- [x] Commit: `"sdk: dependency detection with install instructions"`

**Test checkpoint (Linux):**
- `MISSING: libfuse3-dev -- install with: sudo apt install libfuse3-dev` shown when missing

## 3. Auto-Discovery of SDK Tool Directories
Build discovers new tools automatically -- add a directory to `sdk/src/`, it gets built.

**Files:** `sdk/build.sh`

- [x] Scan `sdk/src/*/` for directories containing `Makefile`
- [x] Skip directories without build files (no error, just skip)
- [x] Report: `Found N SDK tools: ixfs-mount, ...`
- [x] Build in alphabetical order (deterministic)
- [x] Commit: `"sdk: auto-discover SDK tool directories"`

**Test checkpoint (Linux):**
- Dir without Makefile is silently skipped
- Tools built in alphabetical order

---

## 4. SDK Tools in CI and Honest Skip Reporting

> **Spawned-by:** root

Found while writing the host-tools docs pages (`00-infrastructure/TODO-10` §26): the build reports success for tools it never built, and nothing runs it, so the SDK drifted from the kernel unnoticed.

**Files:** `sdk/build.sh`, `sdk/src/ixfs-mount/Makefile`, a CI workflow or `scripts/test-tooling.sh`

- [ ] Report a tool its Makefile skipped (missing libfuse3) as `SKIPPED`, not `OK`, and count it apart from built tools
  - Measured 2026-09-30 on WSL2 without libfuse3: `[1/1] Building ixfs-mount...  OK` and `1 tools built`, while `sdk/tools/` stayed empty.
  - Needs a Makefile-to-script signal (an exit code or a marker file), not a grep of make's output.
- [ ] Build the SDK and run `test_ixfs_core` against a freshly built image on every push that touches `sdk/` or the IXFS headers, with libfuse3 installed so `ixfs-mount` really compiles
  - Today no workflow, lint check or tooling suite runs `sdk/build.sh`; the IXFS inode-size drift in TODO-02 §6 went unseen because of it. -> XREF: `14-host-tools/TODO-02-ixfs-mount.md` §6 (item: "Add a regression net")
- [ ] Commit: `"sdk: report skipped tools honestly and build the SDK in CI"`

**Test checkpoint:** on a host without libfuse3 the summary reads `0 built, 1 skipped`; with it, CI builds `ixfs-mount` and `test_ixfs_core` lists a non-empty root directory. That last check needs the parser fix in `14-host-tools/TODO-02` §6 first; the skip reporting and the CI build do not.

---

## OS Comparison

| ⭐  | Feature         | 🪟 Win11           | 🐧 Linux          | 🚀 Impossible OS           |
| --- | --------------- | ------------------ | ----------------- | -------------------------- |
| 💎  | Build system    | ✅ MSBuild / CMake | ✅ make / CMake   | ✅ §1 bash build script    |
| ⭐  | Progress output | ❌ Verbose only    | ❌ Verbose only   | ✅ §1 colored progress bar |
| ⭐  | Dep detection   | ❌ Manual install  | ❌ Manual install | ✅ §2 auto-detect + guide  |

## Verification

- [x] Linux: `bash sdk/build.sh` -- builds all tools, progress shown, timing reported
- [x] Missing deps: clear install instructions printed
- [x] New tool: add directory, auto-discovered on next build
