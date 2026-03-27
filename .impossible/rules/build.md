# Safety & Build Rules

> **Applies to:** All source files, scripts, CI workflows, and project docs.
> **Canonical location:** `.impossible/rules/build.md`

## Workspace Boundaries

- Stay inside the repository workspace. Do not access `/mnt/c/`, modify host Windows settings, or work outside the repo unless the user explicitly asks.
- Do not install system-wide packages or make external network requests unless the user explicitly asks.
- Never store, log, or auto-type passwords, tokens, or credentials. If `sudo` is required, pause and let the user enter it manually.

## Build Commands

- **Never run raw `make`**. Always use the build script:
  - `bash scripts/build.sh` — incremental build
  - `bash scripts/build.sh clean` — full clean build
  - `bash scripts/build.sh run` — build + launch QEMU
  - `bash scripts/build.sh clean run` — clean build + QEMU

- Treat `tail -1 build/build.log` as the authoritative build result. `command_status` can falsely report `RUNNING` on builds — do not poll it.
- Builds are background-safe. Poll every ~10–20s; stop when `BUILD OK` or `BUILD FAILED` appears.

## Git & Device Safety

- Do not treat silent Git output as a hang. Check exit status before retrying or terminating.
- Never run real-device disk commands (`dd`, `mkfs`, writes to `/dev/sd*` or `/dev/nvme*`) without explicit user approval.
- If repo `.githooks/` are installed, expect pre-commit staged C/H linting and post-commit `COUNT.md` regeneration.

## Mandatory Human-Review Triggers

Stop and get explicit user approval before:

- **Security-sensitive**: authentication, encryption, key handling, Secure Boot, TPM, access control.
- **Destructive operations**: deleting files, wiping partitions, overwriting disk images, resetting registry hives.
- **Public API/ABI changes**: syscall numbers, struct layouts visible to user-mode, ELF/PE load contract.
- **Dependency additions**: new compiler flags, external libraries, toolchain components, or MCP servers.
- **Build/release tooling**: Makefile, linker scripts, `scripts/build.sh`, GitHub Actions, disk image layout.
- **Large refactors**: renaming subsystems, moving source trees, or changing conventions across more than one domain.

## Stop / Ask / Escalate

Stop work and ask when:

- Requirements are ambiguous and proceeding has irreversible consequences.
- Two tracked docs directly contradict each other.
- The repo is in unexpected state before a write operation.
- Verification fails or cannot be reproduced — do not claim success; report the failure.

Do not silently skip, paper over, or assume it is fine to proceed.

## Headless QEMU

Headless QEMU with serial output is a supported automation and debugging path. Serial output at terminal (`-serial stdio`) and captured log files both count as valid evidence.
