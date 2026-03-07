# Phase 09 — Security & Access Control

> **Goal:** Transform Impossible OS from a single-user, unprotected system into a
> secure multi-user operating system with login authentication, file permissions,
> privilege separation, password hashing, data encryption, and a UAC-like
> elevation prompt — protecting user data and system integrity.

---

## 1. User Accounts & Authentication
> *Research: [01_user_accounts_login.md](research/phase_09_security/01_user_accounts_login.md)*

### 1.1 User Account System

- [ ] Create `src/kernel/auth.c` and `include/auth.h`
- [ ] Define `struct user_account` (uid, username, display_name, password_hash, privilege_level, avatar_path, home_dir, auto_login)
- [ ] Privilege levels:
  - [ ] `PRIV_ADMIN` (0) — full access to all files and system settings
  - [ ] `PRIV_USER` (1) — own files + read system files
  - [ ] `PRIV_GUEST` (2) — read-only access, temporary session
- [ ] User storage in Codex: `User\{name}\PasswordHash`, `User\{name}\Privilege`, `User\{name}\Avatar`, `User\{name}\AutoLogin`
- [ ] Create default admin account on first boot: "Admin" with configurable password
- [ ] Create optional Guest account (no password, read-only)
- [ ] Commit: `"kernel: user account system"`

### 1.2 Password Hashing

- [ ] Implement password hashing using **monocypher** (Argon2id or BLAKE2b-based)
- [ ] `auth_hash_password(password, salt, hash_out)` — generate hash
- [ ] `auth_verify_password(password, stored_hash)` — compare
- [ ] Generate random salt (16 bytes) per user via RDRAND or entropy pool
- [ ] **Never** store plaintext passwords — only salted hashes
- [ ] Commit: `"kernel: password hashing (Argon2/BLAKE2b)"`

### 1.3 Authentication API

- [ ] Implement `auth_login(username, password)` — verify password, set current user
- [ ] Implement `auth_logout()` — end session, return to login screen
- [ ] Implement `auth_get_current_user()` — return currently logged-in user
- [ ] Implement `auth_create_user(username, password, privilege)` — admin only
- [ ] Implement `auth_delete_user(username)` — admin only, cannot delete self
- [ ] Implement `auth_change_password(username, old_pw, new_pw)` — verify old first
- [ ] Track current user globally (stored in kernel state)
- [ ] Commit: `"kernel: authentication API"`

### 1.4 User Home Directories

- [ ] On user creation, create home directory structure:
  ```
  C:\Users\{name}\
  ├── Desktop\
  ├── Documents\
  ├── Downloads\
  ├── Pictures\
  └── AppData\
  ```
- [ ] Set directory owner to the created user
- [ ] Home directory path: `C:\Users\{name}\`
- [ ] Environment variable: `%USERPROFILE%` → `C:\Users\{name}`
- [ ] Shell working directory starts at `C:\Users\{name}\`
- [ ] Commit: `"kernel: user home directories"`

---

## 2. Login Screen
> *Research: [01_user_accounts_login.md](research/phase_09_security/01_user_accounts_login.md)*

### 2.1 Login Screen UI

- [ ] Create `src/desktop/login.c`
- [ ] Full-screen login displayed before desktop loads:
  - [ ] Background: blurred wallpaper or solid gradient
  - [ ] OS logo: "Impossible OS" centered
  - [ ] User avatar (circle, centered)
  - [ ] Username display
  - [ ] Password input field (masked with bullets: ••••••)
  - [ ] [Sign in →] button
  - [ ] Bottom-left: Power button (⏻ shutdown/restart), Network status (🌐), Accessibility (♿)
- [ ] Commit: `"desktop: login screen UI"`

### 2.2 Login Flow

- [ ] Pre-boot: show login screen after boot splash
- [ ] Select user (if multiple accounts): click avatar to switch
- [ ] Type password → click Sign in (or press Enter)
- [ ] On success: load desktop with user's profile (wallpaper, pinned apps, Codex settings)
- [ ] On failure: shake password field, show "Incorrect password", clear input
- [ ] Lock out after 5 failed attempts (30-second cooldown)
- [ ] Auto-login option: skip login for single user (Codex `User\{name}\AutoLogin = 1`)
- [ ] Commit: `"desktop: login flow"`

### 2.3 User Switching

- [ ] Start menu → user avatar → "Switch user" or "Sign out"
- [ ] Sign out: save state → return to login screen
- [ ] *(Stretch)* Fast user switching: keep user session alive, switch without closing apps
- [ ] Win+L → lock screen (different from login: shows clock, resume to same session)
- [ ] Commit: `"desktop: user switching"`

---

## 3. File Permissions
> *Research: [02_security_permissions.md](research/phase_09_security/02_security_permissions.md)*

### 3.1 Permission Model

- [ ] Create `src/kernel/security.c` and `include/permissions.h`
- [ ] Define permission bits: `PERM_READ` (0x04), `PERM_WRITE` (0x02), `PERM_EXEC` (0x01)
- [ ] Define `struct file_permissions` (owner_perms, other_perms, owner_uid)
- [ ] Store permissions in filesystem inode (IXFS) or extended attribute (FAT32 fallback)
- [ ] Commit: `"kernel: file permission data structures"`

### 3.2 Permission Enforcement

- [ ] `security_check_access(path, user, requested_perms)` — check if user can read/write/exec
- [ ] Logic:
  - [ ] If user is admin → always allow
  - [ ] If user is owner → check `owner_perms`
  - [ ] Otherwise → check `other_perms`
  - [ ] Guest → always deny write (regardless of permission bits)
- [ ] Hook into VFS: check on `vfs_open()`, `vfs_create()`, `vfs_delete()`, `vfs_rename()`
- [ ] Return `ERR_ACCESS_DENIED` (-13) on permission failure
- [ ] Commit: `"kernel: permission enforcement in VFS"`

### 3.3 System Folder Protections

- [ ] Set default permissions on system directories:
  | Path | Owner | Owner Perms | Other Perms |
  |------|-------|-------------|-------------|
  | `C:\Impossible\System\` | Admin | rwx | r-x |
  | `C:\Impossible\Bin\` | Admin | rwx | r-x |
  | `C:\Programs\` | Admin | rwx | r-x |
  | `C:\Users\{name}\` | {name} | rwx | --- |
  | `C:\Temp\` | System | rwx | rwx |
  | `C:\Recycle\` | System | rwx | rwx |
- [ ] Apply during first-boot directory creation
- [ ] Commit: `"kernel: default system folder permissions"`

### 3.4 Ownership Management

- [ ] `security_set_owner(path, uid)` — change file owner (admin only)
- [ ] `security_set_perms(path, owner_perms, other_perms)` — change permissions (owner or admin)
- [ ] New files inherit owner from creating process's current user
- [ ] Shell command: `chmod <perms> <file>` — set permissions
- [ ] Shell command: `chown <user> <file>` — change owner
- [ ] Shell command: `ls -l` — show permissions + owner in detail view
- [ ] Commit: `"kernel: ownership and permission management"`

---

## 4. Privilege Elevation (UAC-like)
> *Research: [02_security_permissions.md](research/phase_09_security/02_security_permissions.md)*

### 4.1 Elevation Prompt

- [ ] Create `src/desktop/uac.c`
- [ ] When a standard user attempts an admin-only action:
  - [ ] Dim the screen (semi-transparent overlay)
  - [ ] Show modal dialog: "This action requires administrator permission"
  - [ ] Show the action description
  - [ ] [Allow] button — if current user is admin, proceed; otherwise prompt for admin password
  - [ ] [Deny] button — cancel the action
- [ ] Secure desktop: input only goes to UAC dialog while shown
- [ ] Commit: `"desktop: UAC elevation prompt"`

### 4.2 Actions Requiring Elevation

- [ ] Installing/uninstalling programs
- [ ] Modifying system files (`C:\Impossible\System\`)
- [ ] Changing system settings (network, security, users)
- [ ] Modifying other users' files
- [ ] Formatting/partitioning disks
- [ ] Enabling/disabling firewall
- [ ] Commit: `"kernel: define admin-required actions"`

---

## 5. Disk Encryption
> *Research: [03_disk_encryption.md](research/phase_09_security/03_disk_encryption.md)*

### 5.1 Cryptographic Primitives

- [ ] Ensure **monocypher** crypto library is integrated (from Phase 03)
- [ ] Key derivation: `crypto_argon2(password, salt)` → 256-bit encryption key
- [ ] Symmetric encryption: ChaCha20-Poly1305 (authenticated encryption)
- [ ] Random nonce generation (12 bytes) per encryption operation
- [ ] Commit: `"kernel: crypto primitives for encryption"`

### 5.2 File-Level Encryption

- [ ] Create `src/kernel/crypto.c` and `include/crypto.h`
- [ ] Implement `crypto_encrypt_file(path, password)`:
  - [ ] Derive key from password (Argon2id)
  - [ ] Read file data
  - [ ] Encrypt with ChaCha20-Poly1305
  - [ ] Write: salt + nonce + ciphertext + auth tag
  - [ ] Rename to `filename.enc`
- [ ] Implement `crypto_decrypt_file(path, password)`:
  - [ ] Read salt, nonce, ciphertext, auth tag
  - [ ] Derive key from password + salt
  - [ ] Decrypt and verify auth tag
  - [ ] Write original file, remove `.enc`
- [ ] Right-click context menu: "Encrypt..." → prompt for password
- [ ] Right-click `.enc` file: "Decrypt..." → prompt for password
- [ ] File manager: show 🔒 icon for encrypted files
- [ ] Commit: `"kernel: file encryption/decryption"`

### 5.3 Full Disk Encryption (Future)

- [ ] *(Stretch)* Encryption layer between VFS and block device driver
- [ ] *(Stretch)* Boot-time password prompt before mounting encrypted partition
- [ ] *(Stretch)* Encrypt/decrypt sectors transparently
- [ ] *(Stretch)* Key stored in LUKS-like header on disk (never plaintext)
- [ ] *(Stretch)* Codex: `System\Security\EncryptionEnabled`
- [ ] Commit: `"kernel: full disk encryption"`

---

## 6. Agent-Recommended Additions

> Items not in the research files but critical for a secure operating system.

### 6.1 Session Management

- [ ] Track login sessions: user, login time, session ID
- [ ] Session timeout: auto-lock after configurable idle period
- [ ] Track failed login attempts with timestamps
- [ ] Audit log: store login/logout events in Codex `System\Security\AuditLog`
- [ ] Shell command: `who` — show current logged-in user + session info
- [ ] Shell command: `last` — show login history
- [ ] Commit: `"kernel: session management and audit log"`

### 6.2 Process Ownership

- [ ] Associate each running process with the user who launched it
- [ ] Store `owner_uid` in process/task struct
- [ ] Process inherits owner from parent process
- [ ] Process can only signal (kill) processes it owns (or admin can kill all)
- [ ] Task Manager: show process owner column
- [ ] Commit: `"kernel: process ownership"`

### 6.3 Secure Credential Storage

- [ ] Encrypted credential store for saved passwords (WiFi, network shares)
- [ ] Master key derived from user's login password
- [ ] `credential_store_save(service, username, password)` — encrypt and store
- [ ] `credential_store_get(service, username, password_out)` — decrypt and return
- [ ] Stored in `C:\Users\{name}\AppData\Credentials\` (encrypted)
- [ ] Locked when user logs out
- [ ] Commit: `"kernel: secure credential storage"`

### 6.4 Entropy Pool / Random Number Generator

- [ ] Collect entropy from: keyboard timings, mouse movements, PIT jitter, RDRAND
- [ ] Maintain entropy pool (256-byte ring buffer with mixing)
- [ ] `random_bytes(buf, len)` — produce cryptographically secure random bytes
- [ ] Used by: password salt generation, TLS, nonces, session IDs
- [ ] Seed at boot from RDRAND (if available) + RTC
- [ ] Commit: `"kernel: entropy pool and CSPRNG"`

### 6.5 Accounts Settings Applet

- [ ] `accounts.spl` in Settings Panel:
  - [ ] View all user accounts (name, privilege level, avatar)
  - [ ] Create new account (admin only)
  - [ ] Delete account (admin only, confirm dialog)
  - [ ] Change password (own account or admin for any)
  - [ ] Change avatar (select from built-in or browse file)
  - [ ] Set privilege level (admin only)
  - [ ] Enable/disable auto-login
- [ ] Commit: `"apps: accounts settings applet"`

### 6.6 Security Settings Applet

- [ ] `security.spl` in Settings Panel:
  - [ ] Firewall: enable/disable, view rules
  - [ ] Encryption: encrypt/decrypt drives
  - [ ] Password policy: minimum length, complexity requirements
  - [ ] Screen lock timeout setting
  - [ ] Login attempt lockout threshold
- [ ] Commit: `"apps: security settings applet"`

### 6.7 Sudo / Run-As

- [ ] Shell command: `sudo <command>` — run command with admin privileges
- [ ] Prompts for admin password before executing
- [ ] Cache credentials for 5 minutes (configurable)
- [ ] *(Stretch)* GUI "Run as administrator" option in context menus
- [ ] Commit: `"shell: sudo command"`

### 6.8 Executable Signing (Future)

- [ ] *(Stretch)* Sign OS binaries with Ed25519 signature (monocypher)
- [ ] *(Stretch)* Verify signature before executing system binaries
- [ ] *(Stretch)* Warn user when running unsigned executables
- [ ] *(Stretch)* Trust store for known publisher keys

---

## Priority Order

| Priority | Section | Reason |
|----------|---------|--------|
| 🔴 P0 | 1.1 User Account System | Foundation for all security |
| 🔴 P0 | 1.2 Password Hashing | Secure authentication |
| 🔴 P0 | 1.3 Authentication API | Login/logout mechanics |
| 🔴 P0 | 3.1–3.2 File Permissions | Protect user data + system files |
| 🟠 P1 | 2.1–2.2 Login Screen | User-facing authentication |
| 🟠 P1 | 1.4 Home Directories | Per-user file isolation |
| 🟠 P1 | 3.3 System Folder Protections | Lock down system directories |
| 🟠 P1 | 6.2 Process Ownership | Limit process control |
| 🟠 P1 | 6.4 Entropy Pool | Secure randomness for crypto |
| 🟡 P2 | 4.1–4.2 UAC Elevation | Prevent unauthorized system changes |
| 🟡 P2 | 3.4 Ownership Management | chmod/chown tools |
| 🟡 P2 | 6.1 Session Management | Login tracking + auto-lock |
| 🟡 P2 | 6.7 Sudo | Admin commands from shell |
| 🟡 P2 | 2.3 User Switching | Multi-user convenience |
| 🟢 P3 | 5.1–5.2 File Encryption | Protect sensitive files |
| 🟢 P3 | 6.5 Accounts Applet | GUI user management |
| 🟢 P3 | 6.6 Security Applet | GUI security settings |
| 🟢 P3 | 6.3 Credential Storage | Saved passwords |
| 🔵 P4 | 5.3 Full Disk Encryption | Whole-partition crypto |
| 🔵 P4 | 6.8 Executable Signing | Code trust (long-term) |
