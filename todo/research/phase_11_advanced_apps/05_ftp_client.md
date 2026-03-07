# 74 — FTP Client

## Overview
File Transfer Protocol client for uploading/downloading files from FTP servers.

## Protocol: FTP (port 21 control, port 20 data) over TCP
## Commands: USER, PASS, LIST, RETR (download), STOR (upload), CWD, PWD, QUIT

## UI options
1. **Shell command**: `ftp ftp.example.com` (interactive text mode)
2. **File Manager integration**: `ftp://server/path` in address bar
3. **Standalone FTP app** with dual-pane view (local ↔ remote)

## API
```c
int ftp_connect(const char *host, const char *user, const char *pass);
int ftp_list(int session, char *listing, uint32_t max);
int ftp_download(int session, const char *remote, const char *local);
int ftp_upload(int session, const char *local, const char *remote);
void ftp_disconnect(int session);
```

## Depends on: TCP (`22`)
## Files: `src/apps/ftp/ftp.c` (~300 lines) | 3-5 days
## Priority: 🟡 Medium-term
