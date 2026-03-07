# 40 — Security & File Permissions

## Overview

File-level read/write/execute permissions, privilege levels (admin vs standard), and basic access control.

---

## Permission model (simplified ACL)

```c
/* permissions.h */
#define PERM_READ    0x04
#define PERM_WRITE   0x02
#define PERM_EXEC    0x01

struct file_permissions {
    uint8_t  owner_perms;    /* rwx for file owner */
    uint8_t  other_perms;    /* rwx for everyone else */
    uint32_t owner_uid;      /* user ID of owner */
};

/* Privilege levels */
#define PRIV_ADMIN    0   /* full access */
#define PRIV_USER     1   /* own files + read system */
#define PRIV_GUEST    2   /* read-only */
```

## System folder protections
| Path | Owner | Others |
|------|-------|--------|
| `C:\Impossible\System\` | Admin (rwx) | User (r-x) |
| `C:\Users\Derick\` | Derick (rwx) | Others (---) |
| `C:\Programs\` | Admin (rwx) | User (r-x) |
| `C:\Temp\` | All (rwx) | All (rwx) |

## UAC-like prompt: "This action requires administrator permission. [Allow] [Deny]"

## Files: `src/kernel/security.c` (~300 lines), `include/permissions.h`
## Implementation: 1-2 weeks
