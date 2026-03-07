---
description: How to use Git/GitHub commands — handling no-output completions
---

# Git / GitHub Commands

## Key Behavior: No Output ≠ Hanging

Many Git commands produce **no output** on success. This is normal and expected — **do not treat "no output" as "hanging" or "stuck"**.

### Commands That Commonly Produce No Output on Success

| Command | No output means |
|---------|----------------|
| `git add <files>` | Files staged successfully |
| `git status --short` | Working tree is clean (nothing to report) |
| `git diff --name-only` | No modified files |
| `git diff --cached --name-only` | No staged changes |
| `git ls-files --others --exclude-standard` | No untracked files |
| `git push` (when up to date) | Already pushed, nothing to do |
| `git checkout <branch>` | Switched silently |

### Commands That Always Produce Output

| Command | Expected output |
|---------|----------------|
| `git status` (long form) | Always prints branch info + status |
| `git log --oneline -N` | Prints N commit lines |
| `git commit -m "..."` | Prints commit hash + summary |
| `git push` (with new commits) | Prints upload progress |

## Rules

1. **Never retry** a Git command just because `command_status` shows "No output". Check the status field — if it shows completed, it succeeded.
2. **Never terminate** a Git command because you assume it's hanging. Wait for the status to show "done" or "completed".
3. When `command_status` returns `Status: RUNNING` with no output, **wait longer** (use `WaitDurationSeconds: 60` or more) rather than terminating and retrying.
4. If a command genuinely needs credentials (e.g., `git push` to a private repo), it will show a prompt in the output — only then should you ask the user for help.

## Push Workflow

```bash
# Stage + commit + push in sequence
git add <files>
git commit -m "scope: description"
git push
```

- `git push` may take 10–30 seconds depending on network speed. Do not assume it is hanging.
- If `git push` requires authentication, the output will contain a credential prompt or an error message.
