# 83 — Network Shares (SMB)

## Overview
Access shared folders on other computers over the network. Like Windows "Map network drive" or `\\server\share`.

## Protocol: SMB2/3 (Server Message Block) over TCP port 445
## Features
- Browse network computers
- Mount remote folders as drive letters (e.g., `Z:\`)
- Read/write files on remote shares

## Complexity: SMB is very complex (~5000+ lines for basic client)
## Alternative: NFS (simpler, ~1000 lines for basic client) or custom protocol

## API
```c
int  smb_connect(const char *server, const char *share, const char *user, const char *pass);
int  smb_list(int session, const char *path, char *listing, uint32_t max);
int  smb_read(int session, const char *path, void *buf, uint32_t max);
int  smb_write(int session, const char *path, const void *buf, uint32_t len);
```

## Depends on: TCP (`22`)
## Files: `src/kernel/net/smb.c` (~1000 lines for minimal client) | 4-8 weeks
## Priority: 🔴 Long-term
