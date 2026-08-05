---
schema_version: 1
id: ssh-ftp-clients
domain: 07-networking
status: active
title: "TODO-08 -- SSH & FTP Clients"
---

# TODO-08 -- SSH & FTP Clients

> **Goal:** Implement the two most-used remote access protocols: FTP (plain, FTPS/FTPES, interactive shell, `wget ftp://`, File Manager `ftp://` URL), and SSH2 (transport with Curve25519/ChaCha20-Poly1305 via **monocypher**, password + Ed25519 public-key auth, interactive channel + PTY relay, `ssh`/`scp`/`ssh-keygen` shell commands, SSH agent, and SFTP subsystem with `sftp://` URL). Together these make Impossible OS viable for system administration and remote file management.

> [!IMPORTANT]
> TCP + TLS (Mbed TLS from TODO-03) must be complete before any work here begins. SSH uses **monocypher** (MIT, ~2K lines of portable C) for Curve25519 ECDH, ChaCha20-Poly1305 AEAD, and Ed25519 signing -- port it the same way Mbed TLS was ported (redirect `malloc`/`free` → `kmalloc`/`kfree`, compile with `-ffreestanding`). FTP plain-text builds first (§1–§9) as it has no crypto dependency; FTPS/FTPES (§5) adds Mbed TLS after FTP is proven. SSH builds on monocypher and existing TCP sockets (TODO-02 §3). The `terminal_open()`/`terminal_puts()`/`terminal_trygetchar()` API from `include/desktop/terminal.h` is the PTY relay bridge for the SSH interactive session. SFTP (§9) requires SSH channels (§6) to be complete -- do not start §9 until §6 passes the verification test.

## Inputs

- `src/kernel/net/socket.c` + TCP API (TODO-01/02) -- `kern_connect(fd, ip, port)`, `kern_send(fd, ...)`, `kern_recv(fd, ...)`, `kern_close(fd)` for all TCP connections
- `src/kernel/net/tls.c` + Mbed TLS (TODO-03) -- `tls_connect(fd, hostname)`, `tls_send()`, `tls_recv()`, `tls_close()` for FTPS/FTPES
- `lib/monocypher/` (new) -- monocypher 4.x (MIT, ~2 K lines): `crypto_x25519()`, `crypto_chacha20_poly1305_*()`, `crypto_eddsa_sign()`, `crypto_eddsa_check()`, `crypto_sha512()`; ported with `kmalloc`/`kfree` shim
- `include/desktop/terminal.h` -- `terminal_open()`, `terminal_puts()`, `terminal_trygetchar()` for SSH PTY relay
- `src/kernel/fs/vfs.c` -- `vfs_open()`/`vfs_read()`/`vfs_write()` for SCP/SFTP file transfers and known_hosts file
- `include/kernel/net/dns.h` + `dns_resolve()` -- resolve hostnames for SSH/FTP connections
- → XREF: `06-networking/TODO-03-http-tls.md` -- Mbed TLS (`tls_connect`) used by §5 FTPS/FTPES; monocypher port mirrors the same `-ffreestanding` compile approach as Mbed TLS
- → XREF: `06-networking/TODO-02-dns-sockets.md` -- BSD socket layer (`kern_connect/send/recv/close`) used by both SSH and FTP
- → XREF: `07-networking/TODO-07-web-browser.md` -- `wget ftp://` (§8) extends the `wget` command from TODO-06 §4; `sftp://` File Manager URL support shares VFS path conventions

## Outcome

- FTP: `ftp_connect/list/download/upload/disconnect` API; interactive `ftp host` shell; `wget ftp://` anonymous download; PASV + binary/ASCII mode.
- FTPS/FTPES: `AUTH TLS` upgrade on control connection; `PBSZ 0` + `PROT P` encrypted data channel.
- SSH2 transport: Curve25519 KEX, ChaCha20-Poly1305 AEAD, `known_hosts` host-key verify.
- SSH auth: password + Ed25519 public-key; `ssh-keygen -t ed25519` generates key pair.
- SSH channel + PTY: interactive terminal relay; `SSH_MSG_CHANNEL_WINDOW_ADJUST`.
- `ssh user@host`, `scp user@host:src dst`, `ssh-keygen` shell commands.
- SSH agent: `SSH_AUTH_SOCK`, in-process key store, `ssh-add keyfile`.
- SFTP subsystem: `SSH_FXP_OPEN/READ/WRITE/CLOSE/STAT/OPENDIR/READDIR`; `sftp user@host` session; `sftp://` File Manager URL.

## Implementation Order

| ⭐  | Order | Deliverable                                                                                       | Depends On                                                             | Status |
| --- | :---: | ------------------------------------------------------------------------------------------------- | ---------------------------------------------------------------------- | :----: |
| 💎  |   1   | §6 FTP protocol -- TCP 21, USER/PASS, PASV, LIST/RETR/STOR, directory commands                   | TCP socket layer (TODO-02); `dns_resolve()`                            |  [ ]   |
| 💎  |   2   | §8 FTP shell + GUI -- interactive `ftp host`, subcommands, `wget ftp://`, File Manager `ftp://`  | §1 FTP protocol API                                                    |  [ ]   |
| 💎  |   3   | §7 FTPS/FTPES -- `AUTH TLS`, `PBSZ 0 PROT P`, encrypted data channel                            | §1 FTP + Mbed TLS from TODO-03                                         |  [ ]   |
| 💎  |   4   | §1 SSH2 transport -- monocypher port, version exchange, packet framing, Curve25519 KEX, ChaCha20 | monocypher compiled to `lib/libmonocypher.a`; TCP socket layer         |  [ ]   |
| 💎  |   5   | §2 SSH authentication -- password auth, Ed25519 public key auth, `known_hosts` verify            | §4 transport (MAC/cipher must work before auth can be sent securely)   |  [ ]   |
| 💎  |   6   | §3 SSH channel + PTY -- channel open, pty-req, shell, stdin/stdout relay, window adjust          | §5 auth (must be authenticated before channel can be opened)           |  [ ]   |
| 💎  |   7   | §4 SSH shell integration -- `ssh user@host`, `scp`, `ssh-keygen` shell commands                  | §6 channel + PTY (must relay I/O before shell command is usable)       |  [ ]   |
| 💎  |   8   | §5 SSH agent stub -- `SSH_AUTH_SOCK`, in-process key store, `ssh-add`                            | §7 shell (agent is invoked by `ssh` command during auth)               |  [ ]   |
| 💎  |   9   | §9 SFTP subsystem -- SSH "sftp" subsystem, SFTPv3, `sftp` interactive, `sftp://` File Manager    | §6 channel (SFTP runs over an SSH exec channel); §1 FTP for comparison |  [ ]   |

---

## 1. FTP Protocol `[Sonnet]`

TCP port 21 control connection. `ftp_connect(host, user, pass)` sends USER/PASS. PASV passive data port negotiation. `ftp_list()` (LIST → parse directory listing). `ftp_download(remote, local)` (RETR → stream to VFS file). `ftp_upload(local, remote)` (STOR). Commands: CWD, PWD, MKD, RMD, SIZE, DELE, RENAME, TYPE I/A.

**Files:** `src/apps/ftp/ftp.c` (new), `include/apps/ftp/ftp.h` (new)

> [!NOTE]
> FTP response parsing: every server response is `NNN text\r\n` (3-digit code + space + text). Multi-line responses use `NNN-` prefix until `NNN ` (same code + space) terminates. Read response via `kern_recv()` line-by-line (scan for `\r\n`). PASV response format: `227 Entering Passive Mode (h1,h2,h3,h4,p1,p2)` -- data port = `p1 * 256 + p2`; IP = `h1.h2.h3.h4`. Data connection: open a *second* TCP connection to the PASV IP:port **immediately before** LIST/RETR/STOR; close after transfer completes. `ftp_connect()` sends `USER name\r\nPASS pass\r\n`; if anonymous: `USER anonymous\r\nPASS guest@\r\n`. `TYPE I` (binary) is set by default; `TYPE A` for ASCII mode (line ending conversion). Transfer via RETR: send `RETR filename\r\n` on control; accept data on data connection; write to VFS file in 4 KiB chunks; on data close: read `226 Transfer complete` on control.

- [ ] `ftp_session_t { int ctrl_fd; uint32_t server_ip; uint16_t ctrl_port; char cwd[1024]; uint8_t type_binary; }` in `ftp.h`
- [ ] `ftp_readline(fd, buf, max)` → len: recv bytes until `\r\n`; handle multi-line `NNN-` prefix
- [ ] `ftp_send_cmd(fd, cmd)` → response code: write `cmd\r\n`; call `ftp_readline()`; return 3-digit code
- [ ] `ftp_connect(host, user, pass, &session)` → 0 or -errno: `dns_resolve` → `kern_connect(fd, ip, 21)`; wait for `220` greeting; send USER; send PASS; expect `230 Login successful`
- [ ] `ftp_pasv(session, &data_ip, &data_port)` → 0 or -errno: send `PASV\r\n`; parse `227` response
- [ ] `ftp_list(session, buf, max)` → len: `ftp_pasv()`; open data fd; send `LIST\r\n`; recv until data fd close; append `\0`
- [ ] `ftp_download(session, remote, local_path)` → bytes: `ftp_pasv()`; open data fd; `TYPE I`; send `RETR remote\r\n`; `vfs_open(local_path, CREATE)` + write 4 KiB chunks; close data fd
- [ ] `ftp_upload(session, local_path, remote)` → bytes: `ftp_pasv()`; open data fd; `STOR remote\r\n`; `vfs_read()` + send; close data fd
- [ ] `ftp_cwd(session, path)`, `ftp_pwd(session, buf)`, `ftp_mkd(session, dir)`, `ftp_rmd(session, dir)`, `ftp_size(session, file)`, `ftp_dele(session, file)`, `ftp_rename(session, from, to)` → all via `ftp_send_cmd()`
- [ ] `ftp_disconnect(session)`: send `QUIT\r\n`; `kern_close(ctrl_fd)`
- [ ] Commit: `"apps/ftp: FTP protocol -- TCP 21, USER/PASS, PASV, LIST/RETR/STOR, CWD/PWD/MKD/RMD/SIZE/DELE"`

## 2. FTP Shell + GUI Integration `[Sonnet]`

Interactive `ftp host` with subcommands (ls, cd, get, put, mget, mput, pwd, mkdir, rmdir, delete, bye). `wget ftp://host/path` anonymous download. `ftp://` URL support in File Manager address bar.

**Files:** `src/shell/cmd_ftp.c` (new), `src/apps/browser/browser.c` (extend for ftp:// stub)

> [!NOTE]
> Interactive `ftp` mode: display `ftp> ` prompt; read commands via `terminal_trygetchar()` loop; parse subcommand + arguments; dispatch to FTP API; print responses. `mget pattern` / `mput pattern`: `ftp_list()` → filter by glob pattern; loop `ftp_download()`/`ftp_upload()` for each matching filename. `get remote [local]`: if no local filename specified, use basename of remote. Progress for `get`/`put`: print `remote → N KB @ KB/s` using `uptime_ms()` delta. `wget ftp://host/path` extension: parse `ftp://[user:pass@]host/path`; if user:pass absent → anonymous; call `ftp_connect` + `ftp_download`; this extends the `wget` command from TODO-06 §4 by adding `ftp://` scheme detection alongside `http://`/`https://`. `ftp://` in File Manager: stub that prints `[FTP browser not yet implemented -- use 'ftp' command]` (full dual-pane GUI is a stretch goal).

- [ ] `cmd_ftp(argc, argv)`: parse `ftp host` or `ftp user@host`; call `ftp_connect()`; enter interactive prompt loop
- [ ] Interactive subcommands: `ls [dir]`, `cd <dir>`, `pwd`, `get <remote> [local]`, `put <local> [remote]`, `mget <pattern>`, `mput <pattern>`, `mkdir <dir>`, `rmdir <dir>`, `delete <file>`, `rename <from> <to>`, `binary`/`ascii`, `size <file>`, `bye`/`quit`
- [ ] `ftp_glob_match(pattern, filename)` → bool: `*` matches any sequence; `?` matches one char
- [ ] Extend `cmd_wget()` from TODO-06: if `strncmp(url, "ftp://", 6) == 0`: `ftp_parse_url()` → extract user/pass/host/path; `ftp_connect()` + `ftp_download()`; print progress
- [ ] Register `ftp` in shell command table
- [ ] Commit: `"shell/ftp: interactive ftp subcommands, mget/mput glob, wget ftp:// anonymous download"`

## 3. FTPS / FTPES `[Sonnet]`

`AUTH TLS` upgrades the control connection to TLS (Explicit FTPS / FTPES). `PBSZ 0` declares no protection buffer. `PROT P` enables the encrypted data channel. Reuses Mbed TLS from TODO-03.

**Files:** `src/apps/ftp/ftp.c` (extend), `include/apps/ftp/ftp.h` (extend)

> [!NOTE]
> Explicit FTPS (FTPES): after plain TCP connect and `220` greeting, send `AUTH TLS\r\n`; expect `234` response; call `tls_connect(ctrl_fd, hostname)` from TODO-03 §8 -- from this point all control channel I/O goes through `tls_send()`/`tls_recv()` instead of `kern_send()`/`kern_recv()`. Then send `PBSZ 0\r\n` (protection buffer size) and `PROT P\r\n` (data channel protection = private = encrypted). Encrypted data channel: `ftp_pasv()` still opens a second TCP fd; wrap it with `tls_connect(data_fd, hostname)` before RETR/STOR. The `ftp_session_t` gains `tls_conn_t *ctrl_tls; tls_conn_t *data_tls;` fields; the `ftp_send_cmd()` and `ftp_readline()` functions use `tls_send()`/`tls_recv()` when `ctrl_tls != NULL`. Implicit FTPS (port 990) is not supported -- log a warning if port 990 is specified.

- [ ] Extend `ftp_session_t`: add `tls_conn_t *ctrl_tls; tls_conn_t *data_tls; uint8_t use_tls;`
- [ ] `ftp_connect_tls(host, user, pass, &session)` → 0 or -errno: plain `ftp_connect()` first; send `AUTH TLS`; on `234`: `tls_connect(ctrl_fd, host)` → store in `session->ctrl_tls`; send `PBSZ 0` + `PROT P` via `tls_send()`
- [ ] Update `ftp_readline()` + `ftp_send_cmd()`: if `session->use_tls`: use `tls_recv()`/`tls_send()` instead of `kern_recv()`/`kern_send()`
- [ ] Update `ftp_download()`/`ftp_upload()` + `ftp_pasv()`: if `session->data_tls != NULL`: wrap data fd with `tls_connect(data_fd, host)`; use `tls_send()`/`tls_recv()` for data transfer
- [ ] `ftp_disconnect()` extension: call `tls_close(ctrl_tls)` + `tls_close(data_tls)` before `kern_close()`
- [ ] `cmd_ftp` extension: add `ftps` alias that calls `ftp_connect_tls()` instead of `ftp_connect()`; log `[FTP] TLS secured` on success
- [ ] Commit: `"apps/ftp: FTPS/FTPES -- AUTH TLS, PBSZ 0 PROT P, Mbed TLS control+data channel wrap"`

## 4. SSH2 Transport Layer `[Opus]`

Port **monocypher** 4.x to a freestanding kernel static library. Version exchange `SSH-2.0-ImpossibleOS`. SSH binary packet protocol (length + padding + payload + MAC). Key exchange: `KEXINIT` → Curve25519 ECDH (`crypto_x25519()`) → derive session keys. ChaCha20-Poly1305 AEAD bulk cipher. Host key verification against `C:\Users\Default\AppData\SSH\known_hosts`.

**Files:** `lib/monocypher/` (new), `src/apps/ssh/ssh_transport.c` (new), `include/apps/ssh/ssh.h` (new)

> [!NOTE]
> This is `[Opus]` -- the SSH transport is security-critical with a novel cryptographic architecture. **Monocypher port**: download monocypher 4.x (MIT, ~2K lines, pure C, no stdlib); compile with kernel flags (`-target x86_64-elf -ffreestanding -nostdlib -nostdinc`); monocypher has zero heap allocations internally -- no malloc redirect needed; archive to `lib/libmonocypher.a`. **Packet framing** (RFC 4253 §6): each packet: `uint32_t packet_length | uint8_t padding_length | payload[n] | padding[pad_len] | MAC[mac_len]`. All multi-byte integers big-endian. **KEXINIT** (RFC 4253 §7): send `SSH_MSG_KEXINIT` with algorithm lists; agree on `curve25519-sha256`, `chacha20-poly1305@openssh.com`, `ssh-ed25519` host key. **Curve25519 KEX**: generate ephemeral key pair `crypto_x25519_public_key(eph_pub, eph_priv)`; send `SSH_MSG_KEXECDH_INIT`; receive server's ephemeral pub + host key + signature; `crypto_x25519(shared, eph_priv, server_eph_pub)`; derive `H = SHA256(V_C || V_S || I_C || I_S || K_S || Q_C || Q_S || K)` (RFC exchange hash); verify server's Ed25519 signature over `H` with `crypto_eddsa_check()`. **Session keys**: derive IV + encryption + integrity keys from HKDF(shared_secret, H) as per RFC 4253 §7.2. **ChaCha20-Poly1305**: use `crypto_chacha20_poly1305_lock()`/`unlock()` for encrypt/decrypt. **known_hosts**: `C:\Users\Default\AppData\SSH\known_hosts` -- one line per host: `hostname ssh-ed25519 BASE64_PUBKEY`; on first connect to unknown host: prompt `"Add host key for <host>? [y/n]"`.

- [ ] Vendor monocypher 4.x into `lib/monocypher/`; add to `scripts/build.sh` (archive to `lib/libmonocypher.a`); add to kernel linker command
- [ ] `ssh_conn_t { int fd; uint8_t session_id[32]; uint8_t enc_key_c2s[32]; uint8_t enc_key_s2c[32]; uint8_t iv_c2s[12]; uint8_t iv_s2c[12]; uint64_t seq_c2s; uint64_t seq_s2c; uint8_t phase; }` in `ssh.h`
- [ ] `ssh_send_packet(conn, payload, len)`: compute `padding_len` (min 4, pad to 8-byte boundary); append random padding; encrypt with ChaCha20-Poly1305 (`seq_c2s` as nonce); send
- [ ] `ssh_recv_packet(conn, buf, max)` → payload_len: recv 4-byte length; recv full packet; decrypt + verify MAC; strip padding; return payload; increment `seq_s2c`
- [ ] `ssh_version_exchange(conn)`: send `"SSH-2.0-ImpossibleOS\r\n"`; recv server version; validate `SSH-2.0` prefix
- [ ] `ssh_kex(conn)`: send KEXINIT; recv server KEXINIT; negotiate algorithms; send `SSH_MSG_KEXECDH_INIT` with ephemeral Curve25519 public key; recv `SSH_MSG_KEXECDH_REPLY`; verify server Ed25519 host key signature; derive session keys; send `SSH_MSG_NEWKEYS`; recv `SSH_MSG_NEWKEYS`; activate encryption
- [ ] `ssh_known_hosts_check(hostname, pubkey, pubkey_len)` → 0 (ok), 1 (unknown→prompt), -1 (mismatch→abort): `vfs_open` known_hosts; scan lines; base64-compare pubkey
- [ ] `ssh_known_hosts_add(hostname, pubkey, pubkey_len)`: append `hostname ssh-ed25519 BASE64\n` to known_hosts
- [ ] Log: `[SSH] KEX complete: Curve25519-SHA256; session_id=%08x...`, `[SSH] Host key OK: %s`, `[SSH] WARNING: Host key mismatch for %s -- connection aborted`
- [ ] Commit: `"apps/ssh: transport -- monocypher port, version exchange, Curve25519 KEX, ChaCha20-Poly1305, known_hosts"`

## 5. SSH Authentication `[Opus]`

`SSH_MSG_SERVICE_REQUEST "ssh-userauth"`. Password auth (`SSH_MSG_USERAUTH_REQUEST` method "password"). Ed25519 public-key auth: sign challenge with `crypto_eddsa_sign()`. `ssh-keygen -t ed25519` generates key pair in `C:\Users\Default\AppData\SSH\id_ed25519`.

**Files:** `src/apps/ssh/ssh_auth.c` (new)

> [!NOTE]
> This is `[Opus]` -- SSH authentication is security-critical: an incorrect Ed25519 signing implementation or improper challenge response leaks credentials. **Password auth** (RFC 4252 §8): send `SSH_MSG_USERAUTH_REQUEST` with service="ssh-connection", method="password", password string (plaintext -- safe because the transport is already encrypted by §4). **Public key auth** (RFC 4252 §7): send probe with method="publickey", `want_reply=0` and algorithm + public key; if server responds `SSH_MSG_USERAUTH_PK_OK`: construct the signing blob `session_id || SSH_MSG_USERAUTH_REQUEST || ...`; sign with `crypto_eddsa_sign(sig, priv_key, message, message_len)`; send `SSH_MSG_USERAUTH_REQUEST` with signature. **Key file format**: store private key as raw 64-byte Ed25519 private key in PEM-like format (`-----BEGIN OPENSSH PRIVATE KEY-----` with base64-encoded `[pub_key (32 bytes)][priv_key (64 bytes)]`). **ssh-keygen**: generate 32-byte random seed via `RDRAND`; `crypto_eddsa_key_pair(pub, priv, seed)`; write files; chmod 600 equivalent (VFS permission bit).

- [ ] `ssh_service_request(conn, service)`: send `SSH_MSG_SERVICE_REQUEST`; recv `SSH_MSG_SERVICE_ACCEPT`; verify service name matches
- [ ] `ssh_auth_password(conn, username, password)` → 0 or -EACCES: build and send `SSH_MSG_USERAUTH_REQUEST` (method="password"); recv `SSH_MSG_USERAUTH_SUCCESS` or `FAILURE`
- [ ] `ssh_auth_pubkey(conn, username, priv_key, pub_key)` → 0 or -EACCES: send probe; on `PK_OK`: sign blob; send full auth request with signature; recv success/failure
- [ ] `ssh_load_privkey(path, priv_out, pub_out)` → 0 or -errno: `vfs_open(path)`; parse PEM header; base64-decode body; extract 32-byte pub + 64-byte priv
- [ ] `ssh_save_keypair(privkey_path, pubkey_path, pub, priv)`: write PEM-wrapped base64 to privkey_path; write `ssh-ed25519 BASE64 comment\n` to pubkey_path
- [ ] `cmd_ssh_keygen(argc, argv)`: parse `-t ed25519` flag (only type supported); prompt for passphrase (empty = unencrypted); call `crypto_eddsa_key_pair()`; save to `C:\Users\Default\AppData\SSH\id_ed25519`/`.pub`; print fingerprint
- [ ] `ssh_connect_and_auth(host, port, user, auth_method, &conn)`: wraps §4 transport + service request + auth; auth_method = password or pubkey; tries pubkey first (load `id_ed25519`) then falls back to password prompt
- [ ] Log: `[SSH] Auth: password OK` or `[SSH] Auth: pubkey OK (Ed25519)` or `[SSH] Auth FAILED: %s`
- [ ] Commit: `"apps/ssh: auth -- password + Ed25519 pubkey, ssh_connect_and_auth, ssh-keygen -t ed25519"`

## 6. SSH Channel + PTY `[Sonnet]`

`SSH_MSG_CHANNEL_OPEN "session"`. `SSH_MSG_CHANNEL_REQUEST "pty-req"` (term=xterm, cols/rows). `SSH_MSG_CHANNEL_REQUEST "shell"`. Forward terminal stdin → channel data → terminal stdout. Handle `SSH_MSG_CHANNEL_WINDOW_ADJUST`. `SSH_MSG_DISCONNECT` on close.

**Files:** `src/apps/ssh/ssh_channel.c` (new)

> [!NOTE]
> SSH channel multiplexing (RFC 4254): each channel has `local_channel_id`, `remote_channel_id`, `local_window`, `remote_window`, `max_packet`. Initial window: 128 KiB. **Open**: send `SSH_MSG_CHANNEL_OPEN` with type="session", initial window, max packet; recv `SSH_MSG_CHANNEL_OPEN_CONFIRMATION` → store `remote_channel_id` + remote window. **PTY request**: send `SSH_MSG_CHANNEL_REQUEST` type="pty-req", term="xterm", `TERMINAL_COLS × TERMINAL_ROWS` (from `include/desktop/terminal.h` constants), mode string (empty). **Shell request**: send `SSH_MSG_CHANNEL_REQUEST` type="shell", `want_reply=1`; recv `SSH_MSG_CHANNEL_SUCCESS`. **Data relay loop**: `terminal_trygetchar()` → if char available → send `SSH_MSG_CHANNEL_DATA`; `ssh_recv_packet()` → if `SSH_MSG_CHANNEL_DATA` → `terminal_puts(data)`; if `SSH_MSG_CHANNEL_WINDOW_ADJUST` → increase `remote_window`; if `SSH_MSG_CHANNEL_EOF`/`CLOSE` → close channel; loop uses `ksleep(1)` to avoid busy-wait. **Window control**: track `local_window`; when < 32 KiB: send `SSH_MSG_CHANNEL_WINDOW_ADJUST` to add 128 KiB.

- [ ] `ssh_channel_t { uint32_t local_id; uint32_t remote_id; uint32_t local_window; uint32_t remote_window; uint32_t max_packet; uint8_t open; uint8_t eof; }`
- [ ] `ssh_channel_open(conn, type, &ch)` → 0 or -errno: send `CHANNEL_OPEN`; recv `OPEN_CONFIRMATION` or `OPEN_FAILURE`
- [ ] `ssh_channel_request(conn, ch, type, want_reply, extra_data, extra_len)` → 0 or -errno: send `CHANNEL_REQUEST`; if `want_reply`: recv `SUCCESS` or `FAILURE`
- [ ] `ssh_channel_send_data(conn, ch, data, len)` → bytes: check `remote_window >= len`; send `SSH_MSG_CHANNEL_DATA`; decrement `remote_window`
- [ ] `ssh_channel_recv_data(conn, ch, buf, max)` → bytes or 0 (no data) or -1 (closed): `ssh_recv_packet()` non-blocking (returns 0 if no data ready); dispatch `CHANNEL_DATA`/`WINDOW_ADJUST`/`EOF`/`CLOSE`
- [ ] `ssh_channel_window_adjust(conn, ch, add_bytes)`: send `SSH_MSG_CHANNEL_WINDOW_ADJUST`; increment `local_window`
- [ ] `ssh_open_shell(conn)` → 0 or -errno: `ssh_channel_open(conn, "session", &ch)`; `ssh_channel_request(pty-req)`; `ssh_channel_request(shell)`; enter relay loop
- [ ] Relay loop: `terminal_trygetchar()` → `ssh_channel_send_data()`; `ssh_channel_recv_data()` → `terminal_puts()`; check `local_window` → send window adjust; check `ch.eof` → break; `ksleep(1)`
- [ ] `ssh_disconnect(conn, reason)`: send `SSH_MSG_DISCONNECT`; `kern_close(conn->fd)`
- [ ] Commit: `"apps/ssh: channel+PTY -- channel open, pty-req xterm, shell relay, window adjust, disconnect"`

## 7. SSH Shell Integration `[Sonnet]`

`ssh user@host [-p port] [-i keyfile]` shell command running inside Terminal app. `scp user@host:src dst` file copy via `SSH_MSG_CHANNEL_REQUEST "exec"`. `ssh-keygen` command (already in §5).

**Files:** `src/shell/cmd_ssh.c` (new), `src/shell/cmd_scp.c` (new)

> [!NOTE]
> `ssh` command: parse `[user@]host [-p port] [-i keyfile]`; extract user (default = current process user); extract host; port defaults to 22; call `dns_resolve(host, &ip)`; call `ssh_connect_and_auth()`; call `ssh_open_shell()` which enters the relay loop; on exit: return to shell prompt. The Terminal app keeps rendering normally -- the SSH relay loop takes over stdin/stdout by calling `terminal_puts()`/`terminal_trygetchar()` directly. `scp` via SSH exec channel: `ssh_channel_open("session")`; `ssh_channel_request("exec", "scp -f remote_path")`; implement the scp protocol (scp `C mode size filename\n` + data + `\0` control flow); write received bytes to local VFS file. `ssh-keygen` is already registered from §5.

- [ ] `cmd_ssh(argc, argv)`: parse user/host/port/-i; `dns_resolve()`; `ssh_connect_and_auth()`; `terminal_open()` if not already open; `ssh_open_shell()` relay loop; print `[SSH] Connection closed` on exit
- [ ] `cmd_scp(argc, argv)`: parse `user@host:remote_path local_path`; `ssh_connect_and_auth()`; `ssh_channel_open("session")`; `ssh_channel_request("exec", "scp -f ...")` → receive scp protocol stream; write to `local_path` via `vfs_open`/`vfs_write`
- [ ] `scp_recv_file(ch, conn, local_path)`: parse `C0644 SIZE FILENAME\n`; allocate `pmm_alloc_contiguous()` for buffer; recv SIZE bytes; send `\0` ack; `vfs_write` to local path
- [ ] `scp_send_file(ch, conn, local_path, remote_path)`: `vfs_open` local; read size; send `C0644 SIZE BASENAME\n`; send data; send `\0`
- [ ] Register `ssh`, `scp` in shell command table
- [ ] Commit: `"shell/ssh: ssh user@host -p/-i, scp file copy via exec channel, Terminal relay loop"`

## 8. SSH Agent Stub `[Sonnet]`

`SSH_AUTH_SOCK` environment variable. In-process key store (loaded keys survive for current session). `ssh-add keyfile` loads key into store. `ssh` command queries agent for authentication before prompting for password.

**Files:** `src/apps/ssh/ssh_agent.c` (new), `include/apps/ssh/ssh_agent.h` (new)

> [!NOTE]
> Full Unix domain socket `SSH_AUTH_SOCK` is not implemented -- use a simpler in-process global store. `ssh_agent_t { ssh_key_entry_t keys[8]; int key_count; }` global singleton. `ssh-add keyfile`: load key via `ssh_load_privkey()`; add to `ssh_agent.keys`; log `[SSH] Identity added: %s`. `ssh` command auth flow: before prompting for password: call `ssh_agent_sign(conn, username, session_id, &sig)` -- if agent has a key: try each key with `ssh_auth_pubkey()`; if server accepts: done; if none work: fall back to password prompt. `SSH_AUTH_SOCK` environment variable: set to `"(in-process)"` string -- visible in `env` output; allows future replacement with a real socket-based agent without changing the `ssh` command.

- [ ] `ssh_key_entry_t { uint8_t pub[32]; uint8_t priv[64]; char comment[128]; }` + `ssh_agent_t { ssh_key_entry_t keys[8]; int key_count; }`
- [ ] `ssh_agent_add(path)` → 0 or -errno: `ssh_load_privkey(path, priv, pub)`; append to `ssh_agent.keys`; print confirmation
- [ ] `ssh_agent_sign(pub, message, msg_len, sig_out)` → 0 or -1: find key by pub; `crypto_eddsa_sign(sig_out, priv, message, msg_len)`; return 0; return -1 if no matching key
- [ ] `ssh_agent_list()`: print all loaded key fingerprints (`SHA256:BASE64`)
- [ ] `cmd_ssh_add(argc, argv)`: parse `ssh-add [-D] [keyfile]`; `-D` = clear all keys; no arg = add default `id_ed25519`; call `ssh_agent_add()`
- [ ] Update `ssh_connect_and_auth()`: before password prompt: if `ssh_agent.key_count > 0`: iterate keys; try `ssh_auth_pubkey()` for each; on success: skip password
- [ ] Set `SSH_AUTH_SOCK=(in-process)` env var in `ssh_agent_init()` (called during kernel init)
- [ ] Register `ssh-add` in shell command table; register `ssh-agent` as alias for `ssh-add -l` (list)
- [ ] Commit: `"apps/ssh: agent stub -- in-process key store, ssh-add, auto-use in ssh auth, SSH_AUTH_SOCK"`

## 9. SFTP Subsystem `[Opus]`

SSH channel with `SSH_MSG_CHANNEL_REQUEST "subsystem" "sftp"`. SFTP v3 protocol: `SSH_FXP_INIT`/`VERSION`, `SSH_FXP_OPEN`/`READ`/`WRITE`/`CLOSE`/`STAT`/`OPENDIR`/`READDIR`/`REALPATH`. Interactive `sftp user@host` session. `sftp://` URL in File Manager address bar.

**Files:** `src/apps/ssh/sftp.c` (new), `include/apps/ssh/sftp.h` (new)

> [!NOTE]
> This is `[Opus]` -- SFTP over SSH channels involves a novel bidirectional multiplexed RPC protocol with no prior implementation. SFTP packets flow through `ssh_channel_send_data()` / `ssh_channel_recv_data()` -- the channel is the transport, not raw TCP. **SFTP packet format** (SSH file transfer protocol v3): `uint32_t length | uint8_t type | uint32_t request_id | data`. `SSH_FXP_INIT (1)`: send version=3; recv `SSH_FXP_VERSION`. Handle types: `SSH_FXP_OPEN (3)`, `SSH_FXP_CLOSE (4)`, `SSH_FXP_READ (5)`, `SSH_FXP_WRITE (6)`, `SSH_FXP_STAT (17)`, `SSH_FXP_OPENDIR (11)`, `SSH_FXP_READDIR (12)`, `SSH_FXP_REALPATH (16)`. All requests are asynchronous by request_id -- implement a simple synchronous mode (send request, wait for matching response_id). **SFTP string encoding**: `uint32_t length` + bytes (no NUL terminator). Interactive `sftp` session: same subcommands as FTP (`ls`/`cd`/`get`/`put`/`pwd`/`mkdir`/`rmdir`/`rm`/`bye`). `sftp://` URL: parse `sftp://user@host/path`; `ssh_connect_and_auth()` + SFTP subsystem open; read directory listing via `OPENDIR`/`READDIR`.

- [ ] `sftp_session_t { ssh_conn_t *conn; ssh_channel_t ch; uint32_t next_req_id; }` in `sftp.h`
- [ ] `sftp_open_subsystem(conn, &sftp)` → 0 or -errno: `ssh_channel_open("session")`; `ssh_channel_request("subsystem", "sftp")`; send `SSH_FXP_INIT(version=3)`; recv `SSH_FXP_VERSION`
- [ ] `sftp_send(sftp, type, payload, len)`: build SFTP packet; `ssh_channel_send_data()`
- [ ] `sftp_recv(sftp, &type, buf, max)` → payload_len: `ssh_channel_recv_data()` loop until complete SFTP packet; validate request_id; return type + payload
- [ ] `sftp_open(sftp, path, flags)` → handle_str: send `SSH_FXP_OPEN`; recv `SSH_FXP_HANDLE` or `SSH_FXP_STATUS`
- [ ] `sftp_read(sftp, handle, offset, len, buf)` → bytes: send `SSH_FXP_READ`; recv `SSH_FXP_DATA`
- [ ] `sftp_write(sftp, handle, offset, data, len)` → 0 or -errno: `SSH_FXP_WRITE`; recv `SSH_FXP_STATUS`
- [ ] `sftp_close(sftp, handle)`: `SSH_FXP_CLOSE`; recv status
- [ ] `sftp_stat(sftp, path, &attrs)`: `SSH_FXP_STAT`; recv `SSH_FXP_ATTRS { size, uid, gid, perms, atime, mtime }`
- [ ] `sftp_opendir(sftp, path)` + `sftp_readdir(sftp, handle, &name, &attrs)` → 0 or -ENOENT: `OPENDIR`/`READDIR` loop until status=EOF
- [ ] `sftp_realpath(sftp, path, resolved_out)`: `SSH_FXP_REALPATH`; recv `SSH_FXP_NAME`
- [ ] `cmd_sftp(argc, argv)`: interactive session analogous to `cmd_ftp()`; subcommands via `sftp_opendir/readdir/open/read/write`
- [ ] `sftp://` File Manager URL stub: parse `sftp://user@host/path`; list directory; return entries (full dual-pane GUI is stretch)
- [ ] Commit: `"apps/ssh: SFTP subsystem -- SFTPv3 OPEN/READ/WRITE/STAT/OPENDIR/READDIR, sftp interactive, sftp:// URL"`

---

## OS Comparison


| ⭐   | Feature                                  | 🪟 Win11                                  | 🐧 Linux                                  | 🚀 Impossible OS                          |
| --- | ---------------------------------------- | ---------------------------------------- | ---------------------------------------- | ---------------------------------------- |
| 💎   | FTP client                               | ✅ `ftp.exe` CLI; File Explorer `ftp://`; | ✅ `ftp`/`lftp`; File Manager `ftp://` via | ⬜ §1 -- `ftp_session_t` API; PASV mode; binary/ASCII |
| 💎   | FTP shell + GUI                          | ✅ `ftp.exe` interactive; Windows Explorer drag-drop | ✅ `ftp`/`lftp` interactive; GNOME/KDE file manager | ⬜ §2 -- glob matching; `wget ftp://` anonymous |
| 💎   | FTPS/FTPES -- AUTH TLS, PBSZ 0 PROT P    | ✅ WinSCP/FileZilla FTPS; not built into  | ✅ `lftp` FTPS; `curl --ftp-ssl`          | ⬜ §3 -- Mbed TLS control + data          |
| 💎   | SSH2 transport                           | ✅ OpenSSH bundled since Win10 1809;      | ✅ OpenSSH built-in; all algorithms supported | ⬜ §4 -- monocypher port; host key fingerprint |
| 💎   | SSH auth -- password + Ed25519 public key | ✅ OpenSSH: password + RSA/Ed25519/ECDSA; Windows | ✅ OpenSSH: full key type support         | ⬜ §5 -- Ed25519 via monocypher; `ssh-keygen -t |
| 💎   | SSH channel + PTY                        | ✅ OpenSSH: full PTY via ConPTY;          | ✅ OpenSSH: full PTY; pseudo-tty via      | ⬜ §6 -- relay via `terminal_puts`/`terminal_trygetchar`; window adjust |
| 💎   | `ssh user@host`, `scp` file copy         | ✅ `ssh.exe`, `scp.exe` in System32       | ✅ OpenSSH `ssh`/`scp` standard on all    | ⬜ §7 -- `ssh` + `scp` shell commands     |
| 💎   | SSH agent -- key store, `ssh-add`, auto-use | ✅ `ssh-agent` service; `ssh-add`; Pageant (PuTTY) | ✅ `ssh-agent`; `ssh-add`; gnome-keyring  | ⬜ §8 -- in-process key store (8 keys)    |
| 💎   | SFTP subsystem                           | ✅ OpenSSH `sftp.exe`; WinSCP SFTP; no    | ✅ OpenSSH `sftp`; GNOME Files `sftp://`  | ⬜ §9 -- SFTP over SSH channel; `sftp`    |

> **After §1–§9:** Impossible OS can act as a full remote administration client -- FTP (plain + TLS), SSH interactive sessions, SCP/SFTP file transfer, and key-based authentication. All implemented as kernel-native code using monocypher (analogous to the Mbed TLS approach), with no userspace daemon boundary. The monocypher port brings Curve25519 + Ed25519 + ChaCha20-Poly1305 at ~2K lines, dramatically smaller than OpenSSH's ~100K lines.

## Verification

- [ ] `bash scripts/build.sh clean` → `tail -1 build/build.log` → `=== BUILD OK ===` (monocypher must compile cleanly under kernel flags)
- [ ] FTP: `ftp ftp.kernel.org` → connects; `ls` shows directory listing; `get README` downloads file to CWD; `bye` disconnects
- [ ] `wget ftp://ftp.gnu.org/pub/gnu/README` → file downloaded anonymously; progress shown
- [ ] FTPS: `ftps ftps-test-server` → serial log shows `[FTP] TLS secured`; data transfer encrypted (verified via QEMU packet capture)
- [ ] Monocypher: smoke test in boot: `crypto_x25519()` + `crypto_chacha20_poly1305_lock()` round-trip produces correct known-vector output; logged to serial
- [ ] SSH transport: `ssh user@localhost` first connect → `"Add host key for localhost? [y/n]"` prompt; `y` → key saved to known_hosts; connect again → no prompt
- [ ] SSH auth: password auth succeeds with correct password; `ssh-keygen -t ed25519` generates files; `ssh -i id_ed25519 user@host` authenticates with key
- [ ] SSH PTY: interactive session renders remote shell output in Terminal; typing works; resize notification sent on window resize; `exit` returns to local shell
- [ ] `scp user@host:~/testfile /tmp/local` → file downloaded; local size matches remote
- [ ] SSH agent: `ssh-add id_ed25519`; `ssh user@host` → no password prompt (agent key used); `ssh-add -l` lists key fingerprint
- [ ] SFTP: `sftp user@host`; `ls /tmp` → remote directory listed; `get /tmp/testfile local` → file downloaded; `put localfile /tmp/uploaded` → file on server
- [ ] Commit: `"apps: complete SSH+FTP clients -- FTP/FTPS, SSH2/SCP/SFTP, Ed25519, monocypher, agent"`
