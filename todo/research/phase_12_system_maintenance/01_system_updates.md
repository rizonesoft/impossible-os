# 50 — System Updates

## Overview

Download and install OS updates from a server. Version checking, delta updates, and rollback.

---

## Update flow

```
1. Check for updates:
   HTTP GET https://impossible-os.dev/api/version
   → Returns: { latest: "0.3.0", url: "...", hash: "sha256:..." }
2. Compare with current version (Codex: System\Version)
3. Download update package (.ipkg format)
4. Verify SHA-256 hash
5. Apply update:
   - Replace system files in C:\Impossible\System\
   - Update Codex version entry
6. Prompt restart
```

## Update types
| Type | What changes | Size |
|------|-------------|------|
| **Hotfix** | Single file patch | <100 KB |
| **Minor update** | Bug fixes, small features | 1-5 MB |
| **Major update** | New features, kernel changes | Full ISO (~10+ MB) |

## API
```c
/* update.h */
struct update_info {
    char version[16];
    char url[256];
    char hash[65];    /* SHA-256 hex */
    uint32_t size;
};

int update_check(struct update_info *info);     /* check server */
int update_download(const struct update_info *info, const char *path);
int update_verify(const char *path, const char *expected_hash);
int update_apply(const char *path);             /* install update */
```

## Requires: TCP/HTTP (from `22_internet_wireless.md`), crypto hashing (monocypher)
## Settings: `update.spl` with "Check for updates" button
## Codex: `System\Update\AutoCheck`, `System\Update\LastCheck`, `System\Update\Channel`

## Files: `src/apps/updater/updater.c` (~300 lines)
## Implementation: 1-2 weeks (after HTTP is working)
