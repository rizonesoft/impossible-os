---
name: command-completion-workaround
description: Workaround for command_status tool not reporting output for completed commands
---

# Command Completion Workaround

The `command_status` tool often fails to report output or completion for commands
that have finished — especially builds with progress bars, `tee`, or high output.

## ⚠️ CRITICAL RULE: Never Use `command_status` for Builds

**DO NOT** run the build and then poll `command_status`. It will report `RUNNING`
with `No output` even after the build has finished. This wastes time and confuses
the workflow.

## The Correct Pattern: Single Compound Command

Combine the build + sentinel check into **one synchronous `run_command`** call.
Set `WaitMsBeforeAsync` high enough for the build to finish (120000ms = 2 min).

### Build Commands

```bash
# Clean build — single command, blocks until done, prints result
bash scripts/build.sh clean 2>&1; echo "---SENTINEL---"; tail -3 build/build.log
```

```bash
# Incremental build
bash scripts/build.sh 2>&1; echo "---SENTINEL---"; tail -3 build/build.log
```

```bash
# Build + QEMU (use WaitMsBeforeAsync=500 since QEMU stays open)
bash scripts/build.sh run
```

### run_command Settings

| Build type       | WaitMsBeforeAsync | Why                                      |
|------------------|-------------------|------------------------------------------|
| `clean`          | 500               | Runs async, then check log after ~90s    |
| incremental      | 500               | Runs async, then check log after ~30s    |
| `run` (QEMU)     | 500               | QEMU stays open, needs async             |

### After Async Build: Check the Log

If the build runs asynchronously (background), **do NOT use `command_status`**.
Instead, wait a reasonable time and then run:

```bash
tail -1 build/build.log
```

Expected: `=== BUILD OK ===` or `=== BUILD FAILED ===`

If the sentinel is not yet present, wait 30 more seconds and check again.
**Maximum 2 checks.** If still not present, check `build/build.log` for errors.

### General Long-Running Commands

For any non-build command that may take a while:

```bash
some-command 2>&1 | tee /tmp/cmd.log && echo "=== OK ===" >> /tmp/cmd.log || echo "=== FAIL ===" >> /tmp/cmd.log
```

Then verify: `tail -1 /tmp/cmd.log`

## Rules

1. **NEVER use `command_status`** for builds. Always check `build/build.log`.
2. **NEVER re-run a build** just because `command_status` reported no output.
3. After async build, wait then run `tail -1 build/build.log` — max 2 checks.
4. For builds, **always use `scripts/build.sh`** — never raw `make`.
