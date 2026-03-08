---
name: command-completion-workaround
description: Workaround for command_status tool not reporting output for completed commands
---

# Command Completion Workaround

The `command_status` tool sometimes fails to report output or completion for commands that have actually finished. This is a known issue with long-running or high-output commands.

## Workaround

When `command_status` keeps returning `RUNNING` with no output, but you believe the command has completed:

1. **Do NOT keep polling** — after 2–3 attempts with no output, assume completion.
2. **Verify results directly** by running a _new_ command that checks the side effects:
   ```bash
   # Instead of polling the original command, check what it produced:
   ls -lh /path/to/expected/output
   file /path/to/expected/file
   wc -c /path/to/expected/file
   ```
3. **Use `echo $?` in a follow-up** to check the exit code if the shell is still available.
4. **Use short sync commands** (`WaitMsBeforeAsync` ≥ expected duration) for commands under ~10 seconds. Only use background (small `WaitMsBeforeAsync`) for truly long-running commands.
5. **Chain verification into the command itself** to get output you can check:
   ```bash
   # Append verification to the command:
   curl -sL <url> -o file.ttf && echo "OK: $(wc -c < file.ttf) bytes" || echo "FAIL"
   ```

## Key Rule

> After 2 failed `command_status` polls, **stop polling and verify the result directly** with a new command.
