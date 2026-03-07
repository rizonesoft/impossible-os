# 58 — Environment Variables

## Overview

System-wide and per-user environment variables (PATH, HOME, TEMP, etc.).

---

## Default variables
| Variable | Value | Scope |
|----------|-------|-------|
| `PATH` | `C:\Impossible\Bin;C:\Programs\` | System |
| `SYSTEMROOT` | `C:\Impossible\` | System |
| `TEMP` | `C:\Temp\` | System |
| `HOME` | `C:\Users\{name}\` | User |
| `USERNAME` | `Derick` | User |
| `COMPUTERNAME` | `IMPOSSIBLE-PC` | System |

## API
```c
const char *env_get(const char *name);
void env_set(const char *name, const char *value);
void env_expand(const char *input, char *output, uint32_t max);
/* env_expand("%HOME%\\Documents", buf, 256) → "C:\Users\Derick\Documents" */
```

## Stored in Codex: `System\Environment\PATH`, `User\{name}\Environment\HOME`
## Used by: shell (PATH for command lookup), apps (TEMP for scratch files)

## Files: `src/kernel/env.c` (~100 lines)
## Implementation: 1-2 days
