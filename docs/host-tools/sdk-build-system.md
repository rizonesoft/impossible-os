<!-- docs: covers=todo/14-host-tools/TODO-01-sdk-build-system.md sources=tools/build-host.sh,tools/ixfs-mount/Makefile,.gitignore reviewed=2026-11-23 order=1 -->
# Host Tools Build

## What is it?

`tools/build-host.sh` builds the host tools: programs that run on the developer's Linux machine and work with Impossible OS formats, such as the IXFS mount tool. It is separate from the kernel build on purpose. The kernel uses the `clang-19 --target=x86_64-elf` cross toolchain through [`scripts/build.sh`](../../scripts/build.sh); host tools use the host's own C compiler and host libraries. The first three sections of the roadmap have shipped; section 4, honest skip reporting and a CI build, is open.

## How does it work?

The script runs in four steps, all in [`tools/build-host.sh`](../../tools/build-host.sh):

1. **Compiler.** It uses `gcc` when present and falls back to `clang`. With neither, it prints `FAIL No C compiler found` and ends with `=== HOST TOOLS BUILD FAILED ===`.
2. **Dependencies.** It looks for libfuse3 with `pkg-config --exists fuse3`, then `dpkg -s libfuse3-dev`. A missing library is a warning with the install command, not an error.
3. **Discovery.** Every directory directly under `tools/` that contains a `Makefile` is a tool. Directories without one are skipped silently. The list is sorted so the build order is deterministic.
4. **Build.** Each tool runs `make -C <dir> CC=<compiler> OUTDIR=<repository>/build/host-tools` (an absolute path), timed individually. On failure the script prints only the lines matching `error:`, `undefined reference`, `fatal error` or `cannot find`, at most 20 of them.

```mermaid
flowchart LR
    A[tools/build-host.sh] --> B{gcc or clang?}
    B -- none --> F[HOST TOOLS BUILD FAILED]
    B -- found --> C[check libfuse3]
    C --> D["discover tools/*/Makefile"]
    D --> E["make -C each tool, OUTDIR=build/host-tools"]
    E --> G[HOST TOOLS BUILD OK or FAILED]
```

Binaries land in `build/host-tools/`, which is gitignored ([`.gitignore`](../../.gitignore)) and created by the script when missing. Each tool's Makefile decides for itself what to do about a missing dependency: [`tools/ixfs-mount/Makefile`](../../tools/ixfs-mount/Makefile) builds nothing and prints `NOTE: libfuse3-dev not found -- skipping ixfs-mount` when `pkg-config` cannot find fuse3.

## What are its interfaces?

| Interface | Meaning |
| --- | --- |
| `bash tools/build-host.sh` | Build every discovered tool (incremental, because each Makefile is) |
| `bash tools/build-host.sh clean` | Run `make clean` in every tool directory |
| Last line `=== HOST TOOLS BUILD OK ===` | Every tool's `make` exited 0 |
| Last line `=== HOST TOOLS BUILD FAILED ===` | No compiler, or at least one tool failed; exit status 1 |
| `tools/<tool>/Makefile` | The contract for a new tool: honour `CC` and `OUTDIR`, provide `clean` |

## How do I use it?

On the WSL2 development host without libfuse3 installed, a build prints this (colour codes removed):

```text
  Compiler: gcc
  WARN  MISSING: libfuse3-dev -- install with: sudo apt install libfuse3-dev
        Tools requiring libfuse3 may fail to build
  Found 1 host tools: ixfs-mount
  [1/1] Building ixfs-mount...  OK  (0.0s)
   HOST TOOLS BUILD OK  -- 1 tools built in 0.1s
=== HOST TOOLS BUILD OK ===
```

Install the library with `sudo apt install libfuse3-dev` and run it again to get a real `build/host-tools/ixfs-mount`.

To add a tool, create `tools/<name>/Makefile` with an `all` target that writes `$(OUTDIR)/<name>` and a `clean` target. The next `bash tools/build-host.sh` finds it without any edit to the script.

## What is not implemented yet?

- **A skipped tool is reported as built.** The output above says `OK` and `1 tools built` although the Makefile skipped `ixfs-mount` and `build/host-tools/` is still empty, because the script trusts `make`'s exit status. Tracked in [Host Tools in CI and Honest Skip Reporting](../../todo/14-host-tools/TODO-01-sdk-build-system.md#4-host-tools-in-ci-and-honest-skip-reporting).
- **Nothing builds or tests the host tools automatically.** No workflow in `.github/workflows/`, no lint check and no tooling suite runs `tools/build-host.sh`, which is how the IXFS mount tool drifted out of step with the kernel's on-disk format unnoticed (see [IXFS Mount](ixfs-mount.md)). Tracked in the same section.
- **Linux only.** There is no Windows build path; the BlackBox extractor roadmap plans a MinGW cross-compile for its own tool in [section 8](../../todo/14-host-tools/TODO-08-blackbox-log-extractor.md#8-windows-build).
- **Only one tool exists.** The other six roadmap tools in this folder are planned, not built.

## How does it compare with Windows 11 and Linux?

Windows SDK tools are built with MSBuild or CMake and Linux tools with make or CMake; both print a compiler log and leave dependency installation to the developer. The host tools script is deliberately smaller: one command, per-tool timing, filtered error lines and an install hint for a missing library, in the same visual style as the kernel build.

## See also

- [Host Tools Build roadmap](../../todo/14-host-tools/TODO-01-sdk-build-system.md)
- [IXFS Mount](ixfs-mount.md)
- [Development Tooling](../infrastructure/development-tooling.md)
- [SDK Distribution](../sdk/sdk-distribution.md), the application SDK for programs that run on Impossible OS
