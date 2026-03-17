# P1105 — FTP Client

> **Goal:** File Transfer Protocol client with shell and GUI interfaces.

> [!CAUTION]
> **Memory Rule:** Use `pmm_alloc_contiguous()` for ALL buffers > 4 KB. `kmalloc` is ONLY for small kernel structs (≤ 4 KB).

---

## 5. FTP Client

### 5.1 FTP Protocol

**Prompt:** FTP control connection on TCP port 21. `ftp_connect(host, user, pass)` authenticates with USER/PASS commands. `ftp_list()` sends LIST via data connection. `ftp_download()` uses RETR, `ftp_upload()` uses STOR. Data connection: use PASV (passive mode) to get data port from server. Additional: CWD, PWD, MKD, RMD, SIZE, DELE. After all items,sh clean`, commit `"apps: FTP client protocol"`.


- [ ] Create `src/apps/ftp/ftp.c`
- [ ] Implement FTP control connection (TCP port 21):
  - [ ] `ftp_connect(host, user, pass)` — connect + authenticate (USER, PASS)
  - [ ] `ftp_list(session, listing, max)` — LIST command (directory listing)
  - [ ] `ftp_download(session, remote, local)` — RETR command (download file)
  - [ ] `ftp_upload(session, local, remote)` — STOR command (upload file)
  - [ ] `ftp_disconnect(session)` — QUIT
- [ ] Data connection: passive mode (PASV) — server sends data connection details
- [ ] Additional commands: CWD (cd), PWD, MKD (mkdir), RMD (rmdir), SIZE, DELE
- [ ] Commit: `"apps: FTP client protocol"`

### 5.2 FTP Shell Commands

**Prompt:** Interactive `ftp ftp.example.com` with subcommands: ls, cd, get, put, pwd, bye. Also support `wget ftp://host/path` for anonymous single-file download. After all items, mark `[x]`, run `bash scripts/build.sh clean`, commit `"shell: ftp command"`.


- [ ] Shell command: `ftp ftp.example.com` → interactive FTP session
  - [ ] Subcommands: `ls`, `cd`, `get <file>`, `put <file>`, `pwd`, `bye`
- [ ] Shell command: `wget ftp://host/path` → anonymous download
- [ ] Commit: `"shell: ftp command"`

### 5.3 FTP GUI (Future)

**Prompt:** Stretch: standalone FTP app with dual-pane view (local ↔ remote). Also support ftp:// URLs in File Manager address bar. After all items, mark `[x]`, run `bash scripts/build.sh clean`, commit `"apps: FTP GUI client"`.


- [ ] *(Stretch)* Standalone FTP app with dual-pane view (local ↔ remote)
- [ ] *(Stretch)* `ftp://` URL support in File Manager address bar

