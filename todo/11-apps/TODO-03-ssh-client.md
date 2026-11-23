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
> `07-networking/TODO-08-ssh-ftp-clients.md §4–§9` is the canonical SSH **protocol** implementation
> spec (transport, auth, channel, shell command, SSH agent, SFTP subsystem). This TODO extends
> that with the richer wire-format details from the prompt spec, Registry-backed TOFU/known-hosts,
> Windows-style SSH config file parsing, SCP protocol flow, and sftp interactive UI.
> Implement §1–§3 here in parallel with or after TODO-08 §4–§6; do not re-implement what
> TODO-08 already specifies -- use XREFs to stay aligned.
>
> Monocypher (Curve25519 / ChaCha20-Poly1305 / Ed25519) **must** be ported before any crypto
> work begins -- `→ XREF: 12-user-platform-sdk/TODO-01 §4`.

---

## Inputs

- `include/desktop/terminal.h` -- `terminal_open()`, `terminal_puts()`, `terminal_trygetchar()`, `TERM_COLS`, `TERM_ROWS`
- `07-networking/TODO-08-ssh-ftp-clients.md §4–§9` -- SSH protocol reference spec
- `07-networking/TODO-02-dns-sockets.md §5` (sockets) and §1-§3 (`dns_resolve`) -- `kern_socket`, `kern_connect`, `kern_send`, `kern_recv`, `kern_close`, `dns_resolve`
- `02-kernel-core/TODO-03-kernel-libraries.md §5` -- monocypher: `crypto_x25519_*`, `crypto_chacha20_*`, `crypto_poly1305_*`, `crypto_ed25519_*`, `crypto_blake2b_*`, `csprng_fill`
- `include/registry.h` -- `RegSetString`, `RegGetString`, `RegCreateKeyEx` (the `reg_*` calls below are shorthand for these)
- `11-apps/TODO-02-ftp-wget-wifi.md §2` -- `progress_bar_print(done, total, elapsed_ms)` helper

---

## Outcome

`ssh user@host` opens a full ANSI interactive remote shell rendered in the Terminal app. `scp` uploads/downloads files over the encrypted SSH exec channel with a progress bar. `sftp user@host` starts an interactive file-transfer session. Host fingerprints are stored in Registry and users are warned on mismatch. A Windows-style SSH config file at `C:\Users\{name}\AppData\ssh\config` lets developers use short aliases.

---

## Implementation Order

| Step | Section                                | 💎/⭐ | Dependency                                                                             | Status |
| ---- | -------------------------------------- | ----- | -------------------------------------------------------------------------------------- | :----: |
| 1    | SSH2 Transport Layer                   | 💎    | §8                                                                                     |  [ ]   |
| 2    | SSH Authentication                     | 💎    | §8, §1                                                                                 |  [ ]   |
| 3    | Interactive Channel + PTY Relay        | 💎    | §8, §2                                                                                 |  [ ]   |
| 4    | Shell Integration + TOFU + Known Hosts | ⭐    | §8, §3                                                                                 |  [ ]   |
| 5    | SCP File Transfer                      | 💎    | §8, §2                                                                                 |  [ ]   |
| 6    | SSH Config File                        | ⭐    | §8, §4                                                                                 |  [ ]   |
| 7    | sftp Command (Stretch)                 | 💎    | §8, §3                                                                                 |  [ ]   |
| 8    | Import SSH                             | 💎    | `10-platform-services/TODO-07` §7 (PE programs run), `10-platform-services/TODO-13` §5 |  [ ]   |

---

## 1. SSH2 Transport Layer

**Moved:** ssh

Built in SSH, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until §8 has brought it and it is shown to work.

- [ ] Shown to work here on the imported copy, as §8 brings it
- [ ] Commit: `"imports: ssh brings SSH2 Transport Layer"`

---

## 2. SSH Authentication

**Moved:** ssh

Built in SSH, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until §8 has brought it and it is shown to work.

- [ ] Shown to work here on the imported copy, as §8 brings it
- [ ] Commit: `"imports: ssh brings SSH Authentication"`

---

## 3. Interactive Channel + PTY Relay

**Moved:** ssh

Built in SSH, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until §8 has brought it and it is shown to work.

- [ ] Shown to work here on the imported copy, as §8 brings it
- [ ] Commit: `"imports: ssh brings Interactive Channel + PTY Relay"`

---

## 4. Shell Integration + TOFU + Known Hosts

**Moved:** ssh

Built in SSH, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until §8 has brought it and it is shown to work.

What is this OS's own work stays here:

- [ ] `ssh` starts from the shell and from `cmd.exe`
- [ ] Shown to work here on the imported copy, as §8 brings it
- [ ] Commit: `"imports: ssh brings Shell Integration + TOFU + Known Hosts"`

---

## 5. SCP File Transfer

**Moved:** ssh

Built in SSH, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until §8 has brought it and it is shown to work.

What is this OS's own work stays here:

- [ ] `scp` starts from the shell and from `cmd.exe`
- [ ] Shown to work here on the imported copy, as §8 brings it
- [ ] Commit: `"imports: ssh brings SCP File Transfer"`

---

## 6. SSH Config File

**Moved:** ssh

Built in SSH, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until §8 has brought it and it is shown to work.

- [ ] Shown to work here on the imported copy, as §8 brings it
- [ ] Commit: `"imports: ssh brings SSH Config File"`

---

## 7. sftp Command (Stretch)

**Moved:** ssh

Built in SSH, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until §8 has brought it and it is shown to work.

What is this OS's own work stays here:

- [ ] `sftp` starts from the shell and from `cmd.exe`
- [ ] Shown to work here on the imported copy, as §8 brings it
- [ ] Commit: `"imports: ssh brings sftp Command (Stretch)"`

## 8. Import SSH

> **Spawned-by:** root

**Design:** n/a -- imports a program, it designs nothing

**Import:** ssh

SSH is developed in its own repository and proved on Windows. This section brings its program and tests here and runs them on the OS.

- [ ] Copy the pinned program with `python3 scripts/imports.py --pull ssh` into `src/apps/ssh/`
- [ ] Build it in the OS build and run its engine tests there
- [ ] Open SSH on the desktop and walk it by pointer and by keyboard
- [ ] Propose what had to change here with `python3 scripts/imports.py --propose ssh`
- [ ] Commit: `"apps: SSH imported"`

---

## OS Comparison


| ⭐  | Feature                                            | 🪟 Win11                                         | 🐧 Linux                  | 🚀 Impossible OS                                                |
| --- | -------------------------------------------------- | ------------------------------------------------ | ------------------------- | --------------------------------------------------------------- |
| 💎  | SSH2 transport -- ChaCha20-Poly1305 + RFC 4253 KDF | ✅ OpenSSH via `ssh.exe`                         | ✅ OpenSSH                | ⬜ §1 -- monocypher crypto; RFC 4253 section 7.2 key derivation |
| 💎  | Password + pubkey (Ed25519) auth                   | ✅ OpenSSH                                       | ✅ OpenSSH                | ⬜ §2 -- Ed25519 auto-generate on first use                     |
| 💎  | Interactive PTY relay + ANSI rendering             | ✅ OpenSSH + Windows Terminal                    | ✅ OpenSSH + any terminal | ⬜ §3 -- relay via `terminal_puts()`/`terminal_trygetchar()`    |
| ⭐  | TOFU fingerprint stored in Registry                | ✅ OpenSSH uses `%USERPROFILE%\.ssh\known_hosts` | ✅ `~/.ssh/known_hosts`   | ⬜ §4 -- `HKCU\Software\Impossible\SSH\KnownHosts\{host}`       |
| 💎  | SCP file transfer with progress                    | ✅ OpenSSH `scp.exe`                             | ✅ OpenSSH `scp`          | ⬜ §5 -- SCP C-mode protocol, 64 KiB                            |
| ⭐  | Windows-style SSH config                           | ✅ `%USERPROFILE%\.ssh\config`                   | ✅ `~/.ssh/config`        | ⬜ §6 -- IxUI Registry fallback + auto-create                   |
| 💎  | Interactive SFTP subsystem                         | ✅ OpenSSH `sftp.exe`                            | ✅ OpenSSH `sftp`         | ⬜ §7 -- (Stretch) -- ; `SSH2_FXP_*` protocol                   |

Impossible OS stores known hosts natively in the Registry (no hidden dotfiles), auto-generates Ed25519 keys on first use, and routes PTY I/O directly through the native terminal API -- no POSIX pty layer, no ConPTY shim.

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
