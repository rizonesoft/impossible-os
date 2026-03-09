---
name: command-completion-workaround
description: Workaround for command_status tool not reporting output for completed commands
---

# Command Completion Workaround

The `command_status` tool sometimes fails to report output or completion for commands that have actually finished. This is a known issue with long-running or high-output commands.

## Primary Strategy: Log-Based Verification

For any command that produces significant output, **write to a log file and check the log** instead of polling `command_status`.

### Build Commands

Always use the build wrapper instead of raw `make`:
```bash
bash scripts/build.sh          # clean + all (default)
bash scripts/build.sh all      # quick rebuild
bash scripts/build.sh run      # launch QEMU
```

Verify completion:
```bash
tail -1 build/build.log
```
Expected: `=== BUILD OK ===` or `=== BUILD FAILED (exit N) ===`

### General Pattern

For any long-running command, append a sentinel:
```bash
some-command 2>&1 | tee /tmp/cmd.log && echo "=== OK ===" >> /tmp/cmd.log || echo "=== FAIL ===" >> /tmp/cmd.log
```

Then verify with:
```bash
tail -1 /tmp/cmd.log
```

## Fallback Strategy

When `command_status` returns `RUNNING` with no output:

1. **Do NOT keep polling** — after 2 attempts with no output, stop polling.
2. **Verify results directly** by checking side effects:
   ```bash
   ls -lh /path/to/expected/output
   file /path/to/expected/file
   ```
3. **Use short sync commands** (`WaitMsBeforeAsync` ≥ expected duration) for commands under ~10 seconds.

## Rules

1. Never retry a command just because `command_status` shows "No output". Check the status field.
2. Never terminate a command because you assume it's hanging. Wait for the status or verify side effects.
3. After 2 failed `command_status` polls, **stop polling and verify the result directly**.
4. For builds, **always use `scripts/build.sh`** and verify with `tail -1 build/build.log`.
