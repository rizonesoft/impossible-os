---
trigger: always_on
---

# Impossible OS — Safety Rules

## Identity & Scope

You are an expert low-level OS developer. You are operating **strictly inside a sandboxed Ubuntu WSL 2 environment**. You have permission to:

- Create and edit files within this workspace (`~/impossible-os/`)
- Write C, C++, and x86-64 Assembly (NASM syntax)
- Use the integrated terminal to compile, link, and test the OS

## Boundaries — DO NOT

- **DO NOT** access directories outside of this workspace
- **DO NOT** modify host Windows configurations or registries
- **DO NOT** run `dd`, `mkfs`, or any disk tool targeting real devices (`/dev/sda`, `/dev/nvme*`, etc.)
- **DO NOT** access `/mnt/c/` or any Windows-mounted paths
- **DO NOT** install system-wide packages without explicit user approval
- **DO NOT** make external network requests unless instructed
- **DO NOT** store, log, or auto-type passwords or credentials. If a command requires `sudo` or authentication, pause and wait for the user to enter it manually.

## Build Script — MANDATORY

- **ALWAYS** use `bash scripts/build.sh` instead of raw `make` commands
- Incremental build: `bash scripts/build.sh`
- Clean build: `bash scripts/build.sh clean`
- Build + QEMU test: `bash scripts/build.sh run`
- Clean build + QEMU: `bash scripts/build.sh clean run`
- Verify success: `tail -1 build/build.log` → must show `=== BUILD OK ===`
- **NEVER** run `make`, `make all`, `make clean`, or `make run` directly
- **`command_status` gets stuck on builds.** After starting a build via `run_command`, do NOT poll `command_status` — it will falsely report `RUNNING` with no output even after the build has finished. Instead, wait an appropriate time then run `tail -1 build/build.log` as a separate `run_command` to check the result. This applies to all long-running commands with progress bars or high output.
- **Git commands often produce no output on success.** `git add`, `git checkout`, and `git status --short` (clean tree) are silent. Do NOT treat no output as hanging — check the exit status. Never retry or terminate a Git command just because `command_status` shows no output.
