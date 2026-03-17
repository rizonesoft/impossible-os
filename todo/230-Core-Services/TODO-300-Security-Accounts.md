# P2301 — Security & User Accounts

> **Goal:** Transform Impossible OS from a single-user, unprotected system into a
> secure multi-user operating system with login authentication, file permissions,
> privilege separation, password hashing, data encryption, and a UAC-like
> elevation prompt — protecting user data and system integrity.

> [!CAUTION]
> **Memory Rule:** Use `pmm_alloc_contiguous()` for ALL buffers > 4 KB (fonts, images, file data). `kmalloc` is ONLY for small kernel structs (≤ 4 KB). Violating this crashes the 2 MiB heap silently. See `rules.md` Known Gotchas and `/add-asset` workflow.

---

## 1. User Accounts & Authentication

### 1.1 User Account System

**Prompt:** The user account system is the foundation for all security features. Define `struct user_account` with uid, username, display_name, password_hash (never plaintext), privilege_level (ADMIN/USER/GUEST), avatar_path, home_dir, and auto_login flag. Privilege levels control access: ADMIN has full access, USER can read/write own files and read system files, GUEST is read-only with a temporary session. Store user data in Registry under `HKU\{name}\*`. Create a default Admin account on first boot with a configurable password. After completing all items,sh clean`, and commit as `"kernel: user account system"`.


- [ ] Create `src/kernel/auth.c` and `include/auth.h`
- [ ] Define `struct user_account` (uid, username, display_name, password_hash, privilege_level, avatar_path, home_dir, auto_login)
- [ ] Privilege levels:
  - [ ] `PRIV_ADMIN` (0) — full access to all files and system settings
  - [ ] `PRIV_USER` (1) — own files + read system files
  - [ ] `PRIV_GUEST` (2) — read-only access, temporary session
- [ ] User storage in Registry: `HKU\{name}\PasswordHash`, `HKU\{name}\Privilege`, `HKU\{name}\Avatar`, `HKU\{name}\AutoLogin`
- [ ] Create default admin account on first boot: "Admin" with configurable password
- [ ] Create optional Guest account (no password, read-only)
- [ ] Commit: `"kernel: user account system"`

### 1.2 Password Hashing

**Prompt:** Use the monocypher crypto library for password hashing. Argon2id is the recommended algorithm (memory-hard, resistant to GPU attacks). Generate a random 16-byte salt per user using RDRAND (or the entropy pool from §6.4). `auth_hash_password(password, salt, hash_out)` produces a fixed-size hash. `auth_verify_password(password, stored_hash)` re-derives the hash with the stored salt and compares in constant time. Never store plaintext passwords — only the salted hash. After completing all items,sh clean`, and commit as `"kernel: password hashing (Argon2/BLAKE2b)"`.


- [ ] Implement password hashing using **monocypher** (Argon2id or BLAKE2b-based)
- [ ] `auth_hash_password(password, salt, hash_out)` — generate hash
- [ ] `auth_verify_password(password, stored_hash)` — compare
- [ ] Generate random salt (16 bytes) per user via RDRAND or entropy pool
- [ ] **Never** store plaintext passwords — only salted hashes
- [ ] Commit: `"kernel: password hashing (Argon2/BLAKE2b)"`

### 1.3 Authentication API

**Prompt:** `auth_login(username, password)` verifies the password hash, and on success sets the current user in kernel state. `auth_logout()` clears the current user and returns to the login screen. `auth_get_current_user()` returns the active user struct (used by permission checks, file creation, etc.). Admin-only functions: `auth_create_user()` and `auth_delete_user()` (cannot delete self). `auth_change_password()` requires verifying the old password first (or admin override). Track the current user globally so all kernel operations can check permissions. After completing all items,sh clean`, and commit as `"kernel: authentication API"`.


- [ ] Implement `auth_login(username, password)` — verify password, set current user
- [ ] Implement `auth_logout()` — end session, return to login screen
- [ ] Implement `auth_get_current_user()` — return currently logged-in user
- [ ] Implement `auth_create_user(username, password, privilege)` — admin only
- [ ] Implement `auth_delete_user(username)` — admin only, cannot delete self
- [ ] Implement `auth_change_password(username, old_pw, new_pw)` — verify old first
- [ ] Track current user globally (stored in kernel state)
- [ ] Commit: `"kernel: authentication API"`

### 1.4 User Home Directories

**Prompt:** When a new user is created, automatically create their home directory tree under `C:\Users\{name}\` with subdirectories: Desktop, Documents, Downloads, Pictures, AppData. Set the directory owner to the new user (using the permission model from §3.1). The shell's working directory starts at the user's home directory. Set `%USERPROFILE%` environment variable to `C:\Users\{name}`. After completing all items,sh clean`, and commit as `"kernel: user home directories"`.


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

### 2.1 Login Screen UI

**Prompt:** The login screen is displayed full-screen before the desktop loads, after the boot splash. Background: blurred wallpaper or solid gradient. Center the OS logo, user avatar (circular image or default icon), username display, password input field (masked with bullet characters), and a [Sign in] button. Bottom-left: Power button (shutdown/restart), Network status icon, Accessibility icon. Use TrueType fonts for crisp text rendering. After completing all items,sh clean`, and commit as `"desktop: login screen UI"`.


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

**Prompt:** The login flow runs after the boot splash and before the desktop compositor starts. If multiple accounts exist, show user avatars side-by-side (click to select). Type password and press Enter or click Sign in. On success: call `auth_login()`, load the user's profile (wallpaper, pinned apps, Registry settings), and start the desktop. On failure: animate a horizontal shake on the password field, show "Incorrect password" text in red, clear the input. Lock out after 5 consecutive failed attempts with a 30-second cooldown timer displayed on screen. Auto-login: if only one account and `AutoLogin = 1` in Registry, skip the login screen entirely. After completing all items,sh clean`, and commit as `"desktop: login flow"`.


- [ ] Pre-boot: show login screen after boot splash
- [ ] Select user (if multiple accounts): click avatar to switch
- [ ] Type password → click Sign in (or press Enter)
- [ ] On success: load desktop with user's profile (wallpaper, pinned apps, Registry settings)
- [ ] On failure: shake password field, show "Incorrect password", clear input
- [ ] Lock out after 5 failed attempts (30-second cooldown)
- [ ] Auto-login option: skip login for single user (Registry `HKU\{name}\AutoLogin = 1`)
- [ ] Commit: `"desktop: login flow"`

### 2.3 User Switching

**Prompt:** User switching from the Start menu: click the user avatar → "Switch user" or "Sign out". Sign out saves the user's state, calls `auth_logout()`, and returns to the login screen. Win+L locks the screen — shows a lock screen with clock and "Enter password to unlock" that resumes the same session (no sign-out). Stretch goal: fast user switching keeps the current session alive. After completing all items,sh clean`, and commit as `"desktop: user switching"`.


- [ ] Start menu → user avatar → "Switch user" or "Sign out"
- [ ] Sign out: save state → return to login screen
- [ ] *(Stretch)* Fast user switching: keep user session alive, switch without closing apps
- [ ] Win+L → lock screen (different from login: shows clock, resume to same session)
- [ ] Commit: `"desktop: user switching"`

---

## 3. File Permissions

### 3.1 Permission Model

**Prompt:** File permissions use a simple owner-based model: each file has an owner_uid, owner_perms (read/write/exec bits), and other_perms. Store permissions in the IXFS inode (add owner_uid, owner_perms, other_perms fields to `struct ixfs_inode`). For FAT32 (which has no permission support), use a fallback: all files owned by current user, full permissions. Define permission bit constants: `PERM_READ` (0x04), `PERM_WRITE` (0x02), `PERM_EXEC` (0x01). After completing all items,sh clean`, and commit as `"kernel: file permission data structures"`.


- [ ] Create `src/kernel/security.c` and `include/permissions.h`
- [ ] Define permission bits: `PERM_READ` (0x04), `PERM_WRITE` (0x02), `PERM_EXEC` (0x01)
- [ ] Define `struct file_permissions` (owner_perms, other_perms, owner_uid)
- [ ] Store permissions in filesystem inode (IXFS) or extended attribute (FAT32 fallback)
- [ ] Commit: `"kernel: file permission data structures"`

### 3.2 Permission Enforcement

**Prompt:** `security_check_access(path, user, requested_perms)` is the central access check function. Logic: admin always allowed, owner checks owner_perms, other users check other_perms, guest always denied write regardless of bits. Hook this check into VFS operations: `vfs_open()`, `vfs_create()`, `vfs_delete()`, `vfs_rename()`. Return `ERR_ACCESS_DENIED` (-13) on failure. After completing all items,sh clean`, and commit as `"kernel: permission enforcement in VFS"`.


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

**Prompt:** Set default permissions during first-boot directory creation: system directories owned by Admin with rwx for owner and r-x for others. User home directories owned by that user with rwx for owner and no access for others. After completing all items,sh clean`, and commit as `"kernel: default system folder permissions"`.


- [ ] Set default permissions on system directories:
| Path                        | Owner  | Owner Perms | Other Perms |
|----------------------------|--------|-------------|-------------|
| `C:\Impossible\System32\`  | Admin  | rwx         | r-x         |
| `C:\Program Files\`        | Admin  | rwx         | r-x         |
| `C:\Users\{name}\`         | {name} | rwx         |-------------|
| `C:\Impossible\Temp\`      | System | rwx         | rwx         |
| `$Recycle.Bin\`            | System | rwx         | rwx         |
- [ ] Apply during first-boot directory creation
- [ ] Commit: `"kernel: default system folder permissions"`

### 3.4 Ownership Management

**Prompt:** `security_set_owner(path, uid)` changes file ownership (admin only). `security_set_perms(path, owner_perms, other_perms)` changes permission bits (owner or admin). New files inherit their owner from the creating process's current user. Shell commands: `chmod`, `chown`, `ls -l`. After completing all items,sh clean`, and commit as `"kernel: ownership and permission management"`.


- [ ] `security_set_owner(path, uid)` — change file owner (admin only)
- [ ] `security_set_perms(path, owner_perms, other_perms)` — change permissions (owner or admin)
- [ ] New files inherit owner from creating process's current user
- [ ] Shell command: `chmod <perms> <file>` — set permissions
- [ ] Shell command: `chown <user> <file>` — change owner
- [ ] Shell command: `ls -l` — show permissions + owner in detail view
- [ ] Commit: `"kernel: ownership and permission management"`

---

## 4. Privilege Elevation (UAC-like)

### 4.1 Elevation Prompt

**Prompt:** The UAC-like elevation prompt appears when a standard user attempts an admin-restricted action. Dim the entire screen with a semi-transparent overlay. Show a modal dialog: icon + "This action requires administrator permission" + description + [Allow]/[Deny] buttons. If the current user is admin, clicking Allow proceeds immediately. If standard user, show a password field for the admin account. After completing all items,sh clean`, and commit as `"desktop: UAC elevation prompt"`.


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
- [ ] Modifying system files (`C:\Impossible\System32\`)
- [ ] Changing system settings (network, security, users)
- [ ] Modifying other users' files
- [ ] Formatting/partitioning disks
- [ ] Enabling/disabling firewall
- [ ] Commit: `"kernel: define admin-required actions"`

---

## 5. Disk Encryption

### 5.1 Cryptographic Primitives

**Prompt:** Verify that monocypher provides all needed crypto primitives: `crypto_argon2()` for key derivation, ChaCha20-Poly1305 for authenticated encryption, and a random nonce generator. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"kernel: crypto primitives for encryption"`.


- [ ] Ensure **monocypher** crypto library is integrated
- [ ] Key derivation: `crypto_argon2(password, salt)` → 256-bit encryption key
- [ ] Symmetric encryption: ChaCha20-Poly1305 (authenticated encryption)
- [ ] Random nonce generation (12 bytes) per encryption operation
- [ ] Commit: `"kernel: crypto primitives for encryption"`

### 5.2 File-Level Encryption

**Prompt:** `crypto_encrypt_file(path, password)` derives a 256-bit key from the password + random salt via Argon2id, generates a random 12-byte nonce, encrypts the file data with ChaCha20-Poly1305, writes output as: salt(16) + nonce(12) + ciphertext + tag(16), and renames to `filename.enc`. `crypto_decrypt_file(path, password)` reverses the process, verifying the auth tag. Right-click context menu: "Encrypt..." and "Decrypt...". File manager shows 🔒 for `.enc` files. After completing all items,sh clean`, and commit as `"kernel: file encryption/decryption"`.


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

### 5.3 Full Disk Encryption *(Stretch)*

- [ ] *(Stretch)* Encryption layer between VFS and block device driver
- [ ] *(Stretch)* Boot-time password prompt before mounting encrypted partition
- [ ] *(Stretch)* Encrypt/decrypt sectors transparently
- [ ] *(Stretch)* Key stored in LUKS-like header on disk (never plaintext)
- [ ] *(Stretch)* Registry: `HKLM\SYSTEM\Security\EncryptionEnabled`
- [ ] Commit: `"kernel: full disk encryption"`

---

## 6. Session Management & System Hardening

### 6.1 Session Management

**Prompt:** Track login sessions: each login creates a session with user reference, login timestamp, and unique session ID. Configurable idle timeout: auto-lock the screen after N minutes. Track failed login attempts. Maintain audit log in Registry `HKLM\SYSTEM\Security\AuditLog`. Shell commands: `who` shows current user, `last` shows login history. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"kernel: session management and audit log"`.


- [ ] Track login sessions: user, login time, session ID
- [ ] Session timeout: auto-lock after configurable idle period
- [ ] Track failed login attempts with timestamps
- [ ] Audit log: store login/logout events in Registry `HKLM\SYSTEM\Security\AuditLog`
- [ ] Shell command: `who` — show current logged-in user + session info
- [ ] Shell command: `last` — show login history
- [ ] Commit: `"kernel: session management and audit log"`

### 6.2 Process Ownership

**Prompt:** Every running process is associated with the user who launched it. Add `owner_uid` to the process/task struct. When a process spawns a child, the child inherits the parent's owner_uid. A process can only kill processes it owns — admin can kill any. Task Manager shows process owner column. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"kernel: process ownership"`.


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

**Prompt:** Collect entropy from multiple sources: keyboard inter-keystroke timings, mouse movement deltas, PIT timer jitter, and RDRAND instruction. Maintain a 256-byte entropy pool with cryptographic mixing (XOR + BLAKE2b hash). `random_bytes(buf, len)` produces cryptographically secure random bytes. This CSPRNG is used by: password salts, TLS nonces, session IDs, encryption nonces. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"kernel: entropy pool and CSPRNG"`.


- [ ] Collect entropy from: keyboard timings, mouse movements, PIT jitter, RDRAND
- [ ] Maintain entropy pool (256-byte ring buffer with mixing)
- [ ] `random_bytes(buf, len)` — produce cryptographically secure random bytes
- [ ] Used by: password salt generation, TLS, nonces, session IDs
- [ ] Seed at boot from RDRAND (if available) + RTC
- [ ] Commit: `"kernel: entropy pool and CSPRNG"`

### 6.5 Sudo / Run-As

- [ ] Shell command: `sudo <command>` — run command with admin privileges
- [ ] Prompts for admin password before executing
- [ ] Cache credentials for 5 minutes (configurable)
- [ ] *(Stretch)* GUI "Run as administrator" option in context menus
- [ ] Commit: `"shell: sudo command"`

### 6.6 Executable Signing *(Stretch)*

- [ ] *(Stretch)* Sign OS binaries with Ed25519 signature (monocypher)
- [ ] *(Stretch)* Verify signature before executing system binaries
- [ ] *(Stretch)* Warn user when running unsigned executables
- [ ] *(Stretch)* Trust store for known publisher keys

---

## 7. Control Panel Applets

### 7.1 User Accounts Applet

- [ ] `nusrmgr.cpl` in Control Panel:
  - [ ] View all user accounts (name, privilege level, avatar)
  - [ ] Create new account (admin only)
  - [ ] Delete account (admin only, confirm dialog)
  - [ ] Change password (own account or admin for any)
  - [ ] Change avatar (select from built-in or browse file)
  - [ ] Set privilege level (admin only)
  - [ ] Enable/disable auto-login
- [ ] Commit: `"apps: user accounts control panel applet"`

### 7.2 Security and Privacy Applet

- [ ] `secpol.cpl` in Control Panel:
  - [ ] Firewall: enable/disable, view rules
  - [ ] Encryption: encrypt/decrypt drives
  - [ ] Password policy: minimum length, complexity requirements
  - [ ] Screen lock timeout setting
  - [ ] Login attempt lockout threshold
- [ ] Commit: `"apps: security control panel applet"`

---

## Priority Order

| Priority | Section                        | Reason                              |
|----------|--------------------------------|-------------------------------------|
| 🔴 P0     | §1.1 User Account System       | Foundation for all security         |
| 🔴 P0     | §1.2 Password Hashing          | Secure authentication               |
| 🔴 P0     | §1.3 Authentication API        | Login/logout mechanics              |
| 🔴 P0     | §3.1–3.2 File Permissions      | Protect user data + system files    |
| 🟠 P1     | §2.1–2.2 Login Screen          | User-facing authentication          |
| 🟠 P1     | §1.4 Home Directories          | Per-user file isolation             |
| 🟠 P1     | §3.3 System Folder Protections | Lock down system directories        |
| 🟠 P1     | §6.2 Process Ownership         | Limit process control               |
| 🟠 P1     | §6.4 Entropy Pool              | Secure randomness for crypto        |
| 🟡 P2     | §4.1–4.2 UAC Elevation         | Prevent unauthorized system changes |
| 🟡 P2     | §3.4 Ownership Management      | chmod/chown tools                   |
| 🟡 P2     | §6.1 Session Management        | Login tracking + auto-lock          |
| 🟡 P2     | §6.5 Sudo                      | Admin commands from shell           |
| 🟡 P2     | §2.3 User Switching            | Multi-user convenience              |
| 🟢 P3     | §5.1–5.2 File Encryption       | Protect sensitive files             |
| 🟢 P3     | §7.1 Accounts Applet           | GUI user management                 |
| 🟢 P3     | §7.2 Security Applet           | GUI security settings               |
| 🟢 P3     | §6.3 Credential Storage        | Saved passwords                     |
| 🔵 P4     | §5.3 Full Disk Encryption      | Whole-partition crypto              |
| 🔵 P4     | §6.6 Executable Signing        | Code trust (long-term)              |

---

## Key Files

| File                            | Purpose                                    |
|---------------------------------|--------------------------------------------|
| `src/kernel/auth.c`             | [NEW] User accounts + authentication       |
| `include/auth.h`                | [NEW] Auth API header                      |
| `src/kernel/security.c`         | [NEW] File permission enforcement          |
| `include/permissions.h`         | [NEW] Permission bits + structs            |
| `src/kernel/crypto.c`           | [NEW] File encryption (ChaCha20/Argon2)    |
| `include/crypto.h`              | [NEW] Crypto API header                    |
| `src/desktop/login.c`           | [NEW] Login screen UI + flow               |
| `src/desktop/uac.c`             | [NEW] UAC elevation prompt                 |
| `src/apps/control/nusrmgr.cpl`  | [NEW] User Accounts control panel applet   |

---

## OS Comparison

| Feature                           | Windows 11 (NT Security)             | Linux (PAM / DAC)                     | Impossible OS                          |
|-----------------------------------|--------------------------------------|---------------------------------------|----------------------------------------|
| Multi-user account system         | ✅ SAM / Active Directory             | ✅ `/etc/passwd` + PAM                 | ⬜ §1.1 P0 — Registry HKU\{user}      |
| Password hashing                  | ✅ NTLM / NTHash (weak) + MS-CHAP2   | ✅ SHA-512 / Argon2 (shadow)           | ⬜ §1.2 P0 — **Argon2id / monocypher** |
| Login screen                      | ✅ winlogon.exe                       | ✅ GDM / SDDM / lightdm                | ⬜ §2.1 P1 — inline kernel login screen |
| User switching / fast switch      | ✅ Win+L / Switch user                | ✅ `chvt` / GDM multi-seat             | ⬜ §2.3 P2                             |
| File permission model             | ✅ NTFS ACL (DACL/SACL)              | ✅ POSIX DAC (rwxrwxrwx)              | ⬜ §3.1 P0 — **owner+others model (simpler than NTFS ACL)** |
| Permission enforcement in VFS     | ✅ SeAccessCheck                      | ✅ `vfs_permission()`                  | ⬜ §3.2 P0 — hook into `vfs_open()`   |
| System folder protections         | ✅ Protected by NTFS ACL              | ✅ Root-owned directories               | ⬜ §3.3 P1 — first-boot default perms |
| UAC / privilege elevation         | ✅ UAC dialog (secure desktop)        | ✅ `sudo` / `pkexec`                   | ⬜ §4.1 P2 — modal prompt + admin pw  |
| File-level encryption             | ✅ EFS (Encrypting File System)       | ✅ ecryptfs / gocryptfs                | ⬜ §5.2 P3 — ChaCha20-Poly1305       |
| Full disk encryption              | ✅ BitLocker                          | ✅ LUKS / dm-crypt                     | ⬜ §5.3 P4 (stretch)                  |
| CSPRNG / entropy pool             | ✅ CNG `BCryptGenRandom`              | ✅ `/dev/urandom` (ChaCha20 pool)      | ⬜ §6.4 P1 — keyboard+PIT+RDRAND pool  |
| `sudo` / run-as admin             | ✅ Run as administrator               | ✅ `sudo` / `su`                       | ⬜ §6.5 P2 — `sudo <cmd>`             |
| Session + audit log               | ✅ Windows Event Log                  | ✅ systemd journal / `/var/log/auth`   | ⬜ §6.1 P2 — Registry audit log       |
| **Argon2id password hashing**     | ❌ Legacy NTLM hashes                 | ⚠️ SHA-512 crypt (not Argon2 by default) | ⬜ **§1.2 — ahead of both: Argon2id GPU-resistant** |
| **Simple owner model (no ACLs)**  | ❌ Complex DACL/SACL ACL lists        | ⚠️ POSIX rwx (no fine-grained)        | ⬜ **§3.1 — clean owner+other, no ACL complexity** |
