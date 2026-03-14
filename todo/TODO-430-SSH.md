# P1104 — SSH Client

> **Goal:** Secure remote shell access via SSH2 protocol.

> [!CAUTION]
> **Memory Rule:** Use `pmm_alloc_contiguous()` for ALL buffers > 4 KB. `kmalloc` is ONLY for small kernel structs (≤ 4 KB).

---

## 4. SSH Client

### 4.1 SSH Protocol Implementation

**Prompt:** Choose: port dropbear (MIT, ~15K lines) or build custom minimal SSH2 client (~2K lines). SSH2 transport: TCP connect port 22, version exchange `SSH-2.0-ImpossibleOS`, key exchange (Curve25519 via monocypher), symmetric encryption (ChaCha20), MAC (HMAC-SHA256). Auth: password auth via SSH_MSG_USERAUTH_REQUEST. Channel: open interactive session, PTY request, forward stdin/stdout. After all items, create `docs/user/ssh.md`, mark `[x]`, run `bash scripts/build.sh clean`, commit `"apps: SSH client"`.


- [ ] Create `src/apps/ssh/ssh.c`
- [ ] Choose approach:
  - [ ] Port **dropbear** (MIT, ~15K lines, lightweight) — or —
  - [ ] Custom minimal SSH2 client (~2K lines, transport + auth only)
- [ ] SSH2 transport layer:
  - [ ] TCP connect to server (port 22)
  - [ ] Version exchange: `SSH-2.0-ImpossibleOS`
  - [ ] Key exchange (Diffie-Hellman or Curve25519 via monocypher)
  - [ ] Symmetric encryption (ChaCha20 or AES-CTR)
  - [ ] MAC (HMAC-SHA256)
- [ ] Authentication:
  - [ ] Password auth: `SSH_MSG_USERAUTH_REQUEST` with password
  - [ ] *(Stretch)* Public key auth: Ed25519 key pair
- [ ] Channel:
  - [ ] Open interactive session channel
  - [ ] PTY request
  - [ ] Forward stdin/stdout between terminal and remote shell
- [ ] Commit: `"apps: SSH client"`

### 4.2 SSH Shell Integration

**Prompt:** Shell command: `ssh user@host` connects and starts interactive session inside Terminal app. `ssh user@host -p 2222` for custom port. Terminal handles rendering, SSH handles network transport. Stretch: `scp user@host:file local_file` for file transfer. After all items, mark `[x]`, run `bash scripts/build.sh clean`, commit `"shell: ssh command"`.


- [ ] Shell command: `ssh user@host` → connect and start interactive session
- [ ] Shell command: `ssh user@host -p 2222` → custom port
- [ ] Runs inside Terminal app (Terminal handles rendering, SSH handles network)
- [ ] *(Stretch)* `scp user@host:file local_file` — file transfer
- [ ] Commit: `"shell: ssh command"`

