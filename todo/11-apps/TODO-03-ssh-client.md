---
schema_version: 1
id: ssh-client
domain: 11-apps
status: active
title: "TODO-03 -- SSH Client"
---

# TODO-03 -- SSH Client

> **Goal:** Build `ssh.exe` -- a complete SSH2 client for Impossible OS, delivering secure remote
> shell access, file transfer (SCP/SFTP), TOFU host verification, and a Windows-style SSH config
> file. All sections build on the monocypher crypto primitives and TCP sockets established in
> lower layers.

> [!IMPORTANT]
> `06-networking/TODO-08-ssh-ftp-clients.md §2–§9` is the canonical SSH **protocol** implementation
> spec (transport, auth, channel, shell command, SSH agent, SFTP subsystem). This TODO extends
> that with the richer wire-format details from the prompt spec, Registry-backed TOFU/known-hosts,
> Windows-style SSH config file parsing, SCP protocol flow, and sftp interactive UI.
> Implement §1–§3 here in parallel with or after TODO-08 §2–§6; do not re-implement what
> TODO-08 already specifies -- use XREFs to stay aligned.
>
> Monocypher (Curve25519 / ChaCha20-Poly1305 / Ed25519) **must** be ported before any crypto
> work begins -- `→ XREF: 11-user-platform-sdk/TODO-01 §4`.

---

## Inputs

- `include/desktop/terminal.h` -- `terminal_open()`, `terminal_puts()`, `terminal_trygetchar()`, `TERM_COLS`, `TERM_ROWS`
- `06-networking/TODO-08-ssh-ftp-clients.md §2–§9` -- SSH protocol reference spec
- `06-networking/TODO-02-dns-sockets.md §3` -- `kern_socket`, `kern_connect`, `kern_send`, `kern_recv`, `kern_close`, `dns_resolve`
- `11-user-platform-sdk/TODO-03-kernel-libraries.md §6` -- monocypher: `crypto_x25519_*`, `crypto_chacha20_*`, `crypto_poly1305_*`, `crypto_ed25519_*`, `crypto_blake2b_*`, `csprng_fill`
- `include/registry.h` -- `reg_set_string`, `reg_get_string`, `reg_create_key`
- `10-apps/TODO-02-ftp-wget-wifi.md §2` -- `progress_bar_print(done, total, elapsed_ms)` helper

---

## Outcome

`ssh user@host` opens a full ANSI interactive remote shell rendered in the Terminal app. `scp` uploads/downloads files over the encrypted SSH exec channel with a progress bar. `sftp user@host` starts an interactive file-transfer session. Host fingerprints are stored in Registry and users are warned on mismatch. A Windows-style SSH config file at `C:\Users\{name}\AppData\ssh\config` lets developers use short aliases.

---

## Implementation Order

| Step | Section                                | 💎/⭐ | Dependency                           |
| ---- | -------------------------------------- | ----- | ------------------------------------ |
| 1    | SSH2 Transport Layer                   | 💎    | monocypher §4, `kern_connect` §5     |
| 2    | SSH Authentication                     | 💎    | §1 complete                          |
| 3    | Interactive Channel + PTY Relay        | 💎    | §2 complete, `terminal.h`            |
| 4    | Shell Integration + TOFU + Known Hosts | ⭐    | §3 complete, Registry                |
| 5    | SCP File Transfer                      | 💎    | §2 complete, VFS, progress_bar_print |
| 6    | SSH Config File                        | ⭐    | §4 complete, Registry                |
| 7    | sftp Command (Stretch)                 | 💎    | §3 complete                          |

---

## 1. SSH2 Transport Layer `[Opus]`

> → XREF: `06-networking/TODO-08-ssh-ftp-clients.md §2` -- base implementation spec.
> This section specifies the HKDF-SHA256 key derivation, ChaCha20-Poly1305 packet framing, and
> sequence-number tracking details that §4 of TODO-08 leaves implicit.

**Source file:** `src/apps/ssh/ssh_transport.c`; header `include/apps/ssh/ssh.h`

- [ ] TCP connect: `kern_socket(AF_INET, SOCK_STREAM, 0)` → `kern_connect(fd, ip, port)` (default port 22)
- [ ] Version exchange: send `"SSH-2.0-ImpossibleOS_1.0\r\n"`; read server banner (terminated by `\r\n`); verify prefix `"SSH-2.0"` or `"SSH-1.99"`; store server banner string
- [ ] `SSH_MSG_KEXINIT` (byte 20): send 16-byte random cookie, name-list `"curve25519-sha256"` preferred for kex, `"ssh-ed25519"` for host key, `"chacha20-poly1305@openssh.com"` for encryption, `"hmac-sha2-256"` for MAC, `"none"` for compression; recv server KEXINIT; intersect name-lists and select algorithms
- [ ] `SSH_MSG_KEX_ECDH_INIT` (byte 30): generate ephemeral Curve25519 keypair via `crypto_x25519_public_key(client_pub, client_priv)` using `csprng_fill(client_priv, 32)`; send `client_pub[32]`
- [ ] Recv `SSH_MSG_KEX_ECDH_REPLY` (byte 31): parse server host key blob (type + key bytes); parse server ephemeral pubkey `server_pub[32]`; parse server signature over exchange hash H
- [ ] Compute shared secret: `crypto_x25519(shared, client_priv, server_pub)` → `shared[32]`
- [ ] Derive session keys via HKDF-SHA256:
  - `H = Blake2b(V_C || V_S || I_C || I_S || K_S || Q_C || Q_S || K)` -- exchange hash
  - `session_id = H` (first key exchange only)
  - `enc_key_c2s[32] = HKDF-SHA256(K || H, "C", session_id)`; `enc_key_s2c[32]` with "D"
  - `mac_key_c2s[32]` with "E"; `mac_key_s2c[32]` with "F"
  - `iv_c2s[12]` with "A"; `iv_s2c[12]` with "B"
- [ ] Verify server host key signature: `crypto_ed25519_check(sig, server_pubkey, H)` → reject on mismatch
- [ ] Store server host key fingerprint: `crypto_blake2b(fingerprint, 16, host_key, host_key_len)` → hex string for TOFU (§4)
- [ ] `SSH_MSG_NEWKEYS` (byte 21): send then recv; activate ChaCha20-Poly1305 AEAD for all subsequent packets:
  - Packet format: 4-byte encrypted length (ChaCha20 block 0, key `enc_key_c2s`) + encrypted payload (ChaCha20 blocks 1+) + 16-byte Poly1305 tag
  - Sequence numbers: `uint32_t seq_send = 0`, `seq_recv = 0`; increment after each packet sent/received; used as ChaCha20 counter

---

## 2. SSH Authentication `[Opus]`

> → XREF: `06-networking/TODO-08-ssh-ftp-clients.md §3`

**Source file:** `src/apps/ssh/ssh_auth.c`

- [ ] `SSH_MSG_SERVICE_REQUEST "ssh-userauth"` → recv `SSH_MSG_SERVICE_ACCEPT`
- [ ] `SSH_MSG_USERAUTH_REQUEST` with method `"none"` → recv `SSH_MSG_USERAUTH_FAILURE` with allowed-auth-methods list
- [ ] Password auth:
  - [ ] Prompt `"{user}@{host}'s password: "` (no echo: use terminal raw mode)
  - [ ] Send `SSH_MSG_USERAUTH_REQUEST`: service `"ssh-connection"`, method `"password"`, `FALSE`, password string (all encrypted inside established AEAD channel)
  - [ ] On `SSH_MSG_USERAUTH_FAILURE`: retry up to 3 times, then print `"Permission denied (password)."` and exit
  - [ ] On `SSH_MSG_USERAUTH_SUCCESS`: proceed to §3
- [ ] Stretch -- public key auth (Ed25519):
  - [ ] Keypair path: `C:\Users\{name}\AppData\ssh\id_ed25519` (private, 64 bytes) + `id_ed25519.pub` (public, 32 bytes)
  - [ ] Auto-generate on first use: `csprng_fill(seed, 32)` → `crypto_ed25519_key_pair(pub, priv, seed)` → write both files; print `"Generating new Ed25519 key pair... done."`
  - [ ] `SSH_MSG_USERAUTH_REQUEST` with method `"publickey"`, algorithm `"ssh-ed25519"`, public key blob; query only (no sig) first
  - [ ] On `SSH_MSG_USERAUTH_PK_OK`: send request again with `TRUE` + signature: `crypto_ed25519_sign(sig, priv, session_id || USERAUTH_REQUEST_blob)`

---

## 3. Interactive Channel + PTY Relay `[Sonnet]`

> → XREF: `06-networking/TODO-08-ssh-ftp-clients.md §6`

**Source file:** `src/apps/ssh/ssh_channel.c`

- [ ] Channel open: `SSH_MSG_CHANNEL_OPEN` type=`"session"`, `local_channel=0`, `initial_window=131072`, `max_packet=32768`; recv `SSH_MSG_CHANNEL_OPEN_CONFIRMATION` → store `remote_channel_id`, `remote_window`, `remote_max_packet`
- [ ] PTY request: `SSH_MSG_CHANNEL_REQUEST` type=`"pty-req"`, `TERM="xterm-256color"`, cols=`TERM_COLS`, rows=`TERM_ROWS`, pixel-width=0, pixel-height=0, empty mode string; `want_reply=1` → recv `SSH_MSG_CHANNEL_SUCCESS`
- [ ] Shell request: `SSH_MSG_CHANNEL_REQUEST` type=`"shell"`, `want_reply=1` → recv `SSH_MSG_CHANNEL_SUCCESS`
- [ ] Relay loop (runs until channel closes):
  - [ ] `ch = terminal_trygetchar()` -- if char available: send `SSH_MSG_CHANNEL_DATA` with that byte; decrement `remote_window` by 1; if `remote_window < 1024`: send `SSH_MSG_CHANNEL_WINDOW_ADJUST` requesting 65536 bytes
  - [ ] `ssh_recv_packet_nonblock()` -- if `SSH_MSG_CHANNEL_DATA`: call `terminal_puts(data, data_len)`; increment `local_window` consumed; if consumed > 32768: send `SSH_MSG_CHANNEL_WINDOW_ADJUST` +131072
  - [ ] If `SSH_MSG_CHANNEL_WINDOW_ADJUST`: add `bytes_to_add` to `remote_window`
  - [ ] If `SSH_MSG_CHANNEL_EOF` or `SSH_MSG_CHANNEL_CLOSE`: send `SSH_MSG_CHANNEL_CLOSE`; break
  - [ ] `ksleep(1)` to avoid busy-wait
- [ ] Terminal resize: detect `TERM_COLS`/`TERM_ROWS` change → send `SSH_MSG_CHANNEL_REQUEST` type=`"window-change"`, new cols/rows/0/0

---

## 4. Shell Integration + TOFU + Known Hosts `[Sonnet]`

> → XREF: `06-networking/TODO-08-ssh-ftp-clients.md §7`

**Source file:** `src/apps/ssh/ssh_main.c`; shell command registration in `src/shell/cmd_ssh.c`

- [ ] `ssh [user@]host [-p port] [-i keyfile]` -- parse args: extract user (before `@`), host (after `@`), optional `-p port`, optional `-i keyfile`
- [ ] `dns_resolve(host, &ip)` → fail with `"ssh: could not resolve host: {host}"`
- [ ] Opens new terminal window for the SSH session (or reuses current if launched from terminal)
- [ ] Default user: if no `user@`, check `HKCU\Software\Impossible\SSH\DefaultUser`; fallback to current session username
- [ ] TOFU fingerprint verification (called after §1 key exchange):
  - [ ] Registry key: `HKCU\Software\Impossible\SSH\KnownHosts\{host}` stores hex fingerprint string
  - [ ] First connect: print `"The authenticity of host '{host}' can't be established.\nEd25519 key fingerprint: {fingerprint}\nAre you sure you want to continue connecting (yes/no)? "` → on `"yes"`: write fingerprint to Registry; on `"no"`: disconnect
  - [ ] Subsequent connects: `reg_get_string(..., &stored)` → compare with current fingerprint → if mismatch: print `"WARNING: REMOTE HOST IDENTIFICATION HAS CHANGED!\nOffending key for {host} in HKCU\\...\\KnownHosts\nExpected: {stored}\nGot: {current}\nConnection refused."` → disconnect
  - [ ] `ssh-keyscan` stretch: `ssh-keyscan host` prints host key fingerprint without connecting

---

## 5. SCP File Transfer `[Sonnet]`

**Source file:** `src/apps/ssh/scp.c`; shell command `src/shell/cmd_scp.c`

- [ ] `scp [user@]host:/remote/path C:\local\path` -- download mode
- [ ] `scp C:\local\file [user@]host:/remote/path` -- upload mode
- [ ] `scp [-r] [-P port] [-i keyfile] source dest` -- optional flags
- [ ] Establish SSH connection and authenticate (reuse §1–§2)
- [ ] Download: `ssh_channel_open("session")` → `ssh_channel_exec("scp -f /remote/path")`
  - [ ] Recv: `C{mode} {size} {filename}\n` -- parse mode (e.g. `0644`), size (uint64_t), filename
  - [ ] Send `\0` ACK byte
  - [ ] Read `{size}` bytes from channel in 64 KiB chunks → write to VFS file via `vfs_open`/`vfs_write`
  - [ ] Send `\0` ACK byte → recv `\0` final ACK
- [ ] Upload: `ssh_channel_exec("scp -t /remote/path")`
  - [ ] Recv `\0` ACK byte
  - [ ] Send `C0644 {size} {filename}\n`
  - [ ] Send file bytes in 64 KiB chunks from VFS → `kern_send()`
  - [ ] Send `\0` ACK byte → recv `\0` final ACK
- [ ] Progress: `progress_bar_print(bytes_done, file_size, elapsed_ms)` with `\r` in-place update (KB/s + ETA)
- [ ] Stretch: `scp -r` recursive directory copy -- `D{mode} 0 {dirname}\n` for dirs, `E\n` for end-of-dir

---

## 6. SSH Config File `[Sonnet]`

> → XREF: `10-platform-services/TODO-08 §4` -- `GetEnvironmentVariableA` for username resolution

**Source file:** `src/apps/ssh/ssh_config.c`

- [ ] Config path: `C:\Users\{name}\AppData\ssh\config` (open with VFS `vfs_open`)
- [ ] Format: INI-style `Host <alias>` stanzas, one key-value per line, e.g.:
  ```
  Host dev
      HostName 192.168.1.100
      User admin
      Port 22
      IdentityFile C:\Users\derick\AppData\ssh\id_ed25519

  Host bastion
      HostName bastion.example.com
      User ec2-user
      Port 2222
  ```
- [ ] `ssh_config_load()`: `vfs_open` → parse line-by-line, store up to 16 `struct ssh_config_entry { alias[64], hostname[256], user[64], port, identity_file[256] }`; comments (`#`) and blank lines skipped
- [ ] `ssh_config_resolve(alias, &entry)`: lookup by alias; copy matched fields into resolved config; unset fields get defaults (port 22, no identity file, DefaultUser)
- [ ] `ssh alias` → `ssh_config_resolve()` expands to full HostName/User/Port/IdentityFile before connect
- [ ] Auto-create empty config with header comment `# Impossible OS SSH config\n# Host alias\n#     HostName ...\n` if file missing
- [ ] Registry fallback: `HKCU\Software\Impossible\SSH\DefaultUser` → used when `Host` stanza has no `User` key

---

## 7. sftp Command (Stretch) `[Sonnet]`

> → XREF: `06-networking/TODO-08-ssh-ftp-clients.md §9` -- SFTP subsystem protocol spec

**Source file:** `src/apps/ssh/sftp.c`; shell command `src/shell/cmd_sftp.c`

- [ ] `sftp [user@]host` -- establish SSH connection + auth (§1–§2)
- [ ] `SSH_MSG_CHANNEL_REQUEST "subsystem" "sftp"` → `SSH2_FXP_INIT` (version=3) → recv `SSH2_FXP_VERSION`
- [ ] Interactive prompt: `sftp> ` -- read line from terminal, parse subcommand
- [ ] Subcommands:
  - [ ] `ls [path]` → `SSH2_FXP_OPENDIR` + `SSH2_FXP_READDIR` → format and print entries with permissions, size, date
  - [ ] `cd <dir>` → `SSH2_FXP_REALPATH` → update remote `cwd`
  - [ ] `lcd <local-dir>` → update local cwd (VFS `vfs_chdir`)
  - [ ] `get <remote> [local]` → `SSH2_FXP_OPEN` (read) + `SSH2_FXP_READ` chunks → `vfs_write`; progress bar
  - [ ] `put <local> [remote]` → `vfs_read` → `SSH2_FXP_OPEN` (write/create) + `SSH2_FXP_WRITE` chunks; progress bar
  - [ ] `rm <file>` → `SSH2_FXP_REMOVE`
  - [ ] `mkdir <dir>` → `SSH2_FXP_MKDIR`
  - [ ] `bye` / `exit` → `SSH2_FXP_CLOSE` all handles → close channel
- [ ] Stretch: `sftp -b <batchfile>` -- read commands from VFS file, execute non-interactively

---

## OS Comparison


| ⭐  | Feature                                  | 🪟 Win11                                 | 🐧 Linux                  | 🚀 Impossible OS                         |
| --- | ---------------------------------------- | ---------------------------------------- | ------------------------- | ---------------------------------------- |
| 💎  | SSH2 transport -- ChaCha20-Poly1305 + HKDF-SHA256 | ✅ OpenSSH via `ssh.exe`                 | ✅ OpenSSH                | ⬜ §1 -- monocypher crypto; HKDF-SHA256 key derivation |
| 💎  | Password + pubkey (Ed25519) auth         | ✅ OpenSSH                               | ✅ OpenSSH                | ⬜ §2 -- Ed25519 auto-generate on first use |
| 💎  | Interactive PTY relay + ANSI rendering   | ✅ OpenSSH + Windows Terminal            | ✅ OpenSSH + any terminal | ⬜ §3 -- relay via `terminal_puts()`/`terminal_trygetchar()` |
| ⭐  | TOFU fingerprint stored in Registry      | ✅ OpenSSH uses `%USERPROFILE%\.ssh\known_hosts` | ✅ `~/.ssh/known_hosts`   | ⬜ §4 -- `HKCU\Software\Impossible\SSH\KnownHosts\{host}` |
| 💎  | SCP file transfer with progress          | ✅ OpenSSH `scp.exe`                     | ✅ OpenSSH `scp`          | ⬜ §5 -- SCP C-mode protocol, 64 KiB     |
| ⭐  | Windows-style SSH config                 | ✅ `%USERPROFILE%\.ssh\config`           | ✅ `~/.ssh/config`        | ⬜ §6 -- IxUI Registry fallback + auto-create |
| 💎  | Interactive SFTP subsystem               | ✅ OpenSSH `sftp.exe`                    | ✅ OpenSSH `sftp`         | ⬜ §7 -- (Stretch) -- ; `SSH2_FXP_*` protocol |

Impossible OS stores known hosts natively in the Registry (no hidden dotfiles), auto-generates
Ed25519 keys on first use, and routes PTY I/O directly through the native terminal API -- no POSIX
pty layer, no ConPTY shim.

---

## Verification

Run `bash scripts/build.sh run` for each verification step.

- [ ] **Transport:** QEMU running sshd (OpenSSH) on host side; connect `ssh root@10.0.2.2`; serial log shows `SSH2 transport OK`, `KEXINIT negotiated curve25519-sha256`, `NEWKEYS sent/received`
- [ ] **Auth (password):** `ssh user@host`; enter correct password → `SSH_MSG_USERAUTH_SUCCESS` in log; wrong password × 3 → `Permission denied` printed
- [ ] **Auth (pubkey stretch):** first run auto-generates key files at `C:\Users\…\AppData\ssh\id_ed25519`; subsequent run authenticates without password prompt
- [ ] **PTY relay:** run `ls -la`, `top`, `vim` on remote; ANSI escape codes render correctly in local terminal; terminal resize updates remote cols/rows
- [ ] **TOFU:** first connect stores fingerprint in Registry; second connect succeeds silently; manually corrupt stored fingerprint → warning message + connection refused
- [ ] **SCP download:** `scp root@10.0.2.2:/etc/os-release C:\Temp\os-release` → file appears on `C:\Temp\`; progress bar printed; byte count matches
- [ ] **SCP upload:** `scp C:\Temp\hello.txt root@10.0.2.2:/tmp/hello.txt` → `ssh root@host "cat /tmp/hello.txt"` shows correct content
- [ ] **SSH config:** create `C:\Users\default\AppData\ssh\config` with `Host dev` stanza; `ssh dev` resolves and connects
- [ ] **sftp (stretch):** `sftp root@10.0.2.2` → `ls /etc` lists files; `get /etc/hostname C:\Temp\hostname` downloads correctly; `bye` exits cleanly
- [ ] Commit: `"apps: SSH2 client -- transport, auth, PTY relay, TOFU, SCP, config, sftp"`
