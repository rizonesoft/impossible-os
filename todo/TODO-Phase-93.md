# Phase 09 — Security & Access Control

> **Goal:** Transform Impossible OS from a single-user, unprotected system into a
> secure multi-user operating system with login authentication, file permissions,
> privilege separation, password hashing, data encryption, and a UAC-like
> elevation prompt — protecting user data and system integrity.
> [!CAUTION]
> **Memory Rule:** Use `pmm_alloc_contiguous()` for ALL buffers > 4 KB (fonts, images, file data). `kmalloc` is ONLY for small kernel structs (≤ 4 KB). Violating this crashes the 2 MiB heap silently. See `rules.md` Known Gotchas and `/add-asset` workflow.


---

## 1. User Accounts & Authentication
> *Research: [01_user_accounts_login.md](research/phase_09_security/01_user_accounts_login.md)*

### 1.1 User Account System

**Prompt:** The user account system is the foundation for all security features. Define `struct user_account` with uid, username, display_name, password_hash (never plaintext), privilege_level (ADMIN/USER/GUEST), avatar_path, home_dir, and auto_login flag. Privilege levels control access: ADMIN has full access, USER can read/write own files and read system files, GUEST is read-only with a temporary session. Store user data in Codex under `User\{name}\*`. Create a default Admin account on first boot with a configurable password. After completing all items, create `docs/architecture/security.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"kernel: user account system"`.


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

**Prompt:** Use the monocypher crypto library (already available from Phase 03) for password hashing. Argon2id is the recommended algorithm (memory-hard, resistant to GPU attacks). Generate a random 16-byte salt per user using RDRAND (or the entropy pool from §6.4). `auth_hash_password(password, salt, hash_out)` produces a fixed-size hash. `auth_verify_password(password, stored_hash)` re-derives the hash with the stored salt and compares in constant time. Never store plaintext passwords — only the salted hash. After completing all items, update `docs/architecture/security.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"kernel: password hashing (Argon2/BLAKE2b)"`.


- [ ] Implement password hashing using **monocypher** (Argon2id or BLAKE2b-based)
- [ ] `auth_hash_password(password, salt, hash_out)` — generate hash
- [ ] `auth_verify_password(password, stored_hash)` — compare
- [ ] Generate random salt (16 bytes) per user via RDRAND or entropy pool
- [ ] **Never** store plaintext passwords — only salted hashes
- [ ] Commit: `"kernel: password hashing (Argon2/BLAKE2b)"`

### 1.3 Authentication API

**Prompt:** `auth_login(username, password)` verifies the password hash, and on success sets the current user in kernel state. `auth_logout()` clears the current user and returns to the login screen. `auth_get_current_user()` returns the active user struct (used by permission checks, file creation, etc.). Admin-only functions: `auth_create_user()` and `auth_delete_user()` (cannot delete self). `auth_change_password()` requires verifying the old password first (or admin override). Track the current user globally so all kernel operations can check permissions. After completing all items, update `docs/architecture/security.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"kernel: authentication API"`.


- [ ] Implement `auth_login(username, password)` — verify password, set current user
- [ ] Implement `auth_logout()` — end session, return to login screen
- [ ] Implement `auth_get_current_user()` — return currently logged-in user
- [ ] Implement `auth_create_user(username, password, privilege)` — admin only
- [ ] Implement `auth_delete_user(username)` — admin only, cannot delete self
- [ ] Implement `auth_change_password(username, old_pw, new_pw)` — verify old first
- [ ] Track current user globally (stored in kernel state)
- [ ] Commit: `"kernel: authentication API"`

### 1.4 User Home Directories

**Prompt:** When a new user is created, automatically create their home directory tree under `C:\Users\{name}\` with subdirectories: Desktop, Documents, Downloads, Pictures, AppData. Set the directory owner to the new user (using the permission model from §3.1). The shell's working directory starts at the user's home directory. Set `%USERPROFILE%` environment variable (Phase 01 §9) to `C:\Users\{name}`. After completing all items, update `docs/architecture/security.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"kernel: user home directories"`.


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

**Prompt:** The login screen is displayed full-screen before the desktop loads, after the boot splash (Phase 04 §10). Background: blurred wallpaper or solid gradient using the graphics primitives from Phase 02 §1. Center the OS logo, user avatar (circular image or default icon), username display, password input field (masked with bullet characters), and a [Sign in] button. Bottom-left: Power button (shutdown/restart via Phase 03 §2.2), Network status icon, Accessibility icon. Use TrueType fonts (Phase 02 §2) for crisp text rendering. After completing all items, create `docs/architecture/login.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"desktop: login screen UI"`.


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

**Prompt:** The login flow runs after the boot splash and before the desktop compositor starts. If multiple accounts exist, show user avatars side-by-side (click to select). Type password and press Enter or click Sign in. On success: call `auth_login()`, load the user's profile (wallpaper, pinned apps, Codex settings), and start the desktop. On failure: animate a horizontal shake on the password field, show "Incorrect password" text in red, clear the input. Lock out after 5 consecutive failed attempts with a 30-second cooldown timer displayed on screen. Auto-login: if only one account and `AutoLogin = 1` in Codex, skip the login screen entirely. After completing all items, update `docs/architecture/login.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"desktop: login flow"`.


- [ ] Pre-boot: show login screen after boot splash
- [ ] Select user (if multiple accounts): click avatar to switch
- [ ] Type password → click Sign in (or press Enter)
- [ ] On success: load desktop with user's profile (wallpaper, pinned apps, Codex settings)
- [ ] On failure: shake password field, show "Incorrect password", clear input
- [ ] Lock out after 5 failed attempts (30-second cooldown)
- [ ] Auto-login option: skip login for single user (Codex `User\{name}\AutoLogin = 1`)
- [ ] Commit: `"desktop: login flow"`

### 2.3 User Switching

**Prompt:** User switching from the Start menu (Phase 04 §2): click the user avatar → "Switch user" or "Sign out". Sign out saves the user's state, calls `auth_logout()`, and returns to the login screen. Win+L locks the screen (Phase 04 §12) — shows a lock screen with clock and "Enter password to unlock" that resumes the same session (no sign-out). Stretch goal: fast user switching keeps the current session alive in memory while loading a different user's profile. After completing all items, update `docs/architecture/login.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"desktop: user switching"`.


- [ ] Start menu → user avatar → "Switch user" or "Sign out"
- [ ] Sign out: save state → return to login screen
- [ ] *(Stretch)* Fast user switching: keep user session alive, switch without closing apps
- [ ] Win+L → lock screen (different from login: shows clock, resume to same session)
- [ ] Commit: `"desktop: user switching"`

---

## 3. File Permissions
> *Research: [02_security_permissions.md](research/phase_09_security/02_security_permissions.md)*

### 3.1 Permission Model

**Prompt:** File permissions use a simple owner-based model: each file has an owner_uid, owner_perms (read/write/exec bits), and other_perms. Store permissions in the IXFS inode (add owner_uid, owner_perms, other_perms fields to `struct ixfs_inode`). For FAT32 (which has no permission support), use a fallback: all files owned by current user, full permissions. Define permission bit constants: `PERM_READ` (0x04), `PERM_WRITE` (0x02), `PERM_EXEC` (0x01). After completing all items, create `docs/architecture/permissions.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"kernel: file permission data structures"`.


- [ ] Create `src/kernel/security.c` and `include/permissions.h`
- [ ] Define permission bits: `PERM_READ` (0x04), `PERM_WRITE` (0x02), `PERM_EXEC` (0x01)
- [ ] Define `struct file_permissions` (owner_perms, other_perms, owner_uid)
- [ ] Store permissions in filesystem inode (IXFS) or extended attribute (FAT32 fallback)
- [ ] Commit: `"kernel: file permission data structures"`

### 3.2 Permission Enforcement

**Prompt:** `security_check_access(path, user, requested_perms)` is the central access check function. Logic: admin always allowed, owner checks owner_perms, other users check other_perms, guest always denied write regardless of bits. Hook this check into VFS operations: `vfs_open()` (check read or write based on flags), `vfs_create()` (check write on parent directory), `vfs_delete()` (check write), `vfs_rename()` (check write on both source and destination directories). Return `ERR_ACCESS_DENIED` (-13) on failure — the shell and file manager should display appropriate error messages. After completing all items, update `docs/architecture/permissions.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"kernel: permission enforcement in VFS"`.


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

**Prompt:** Set default permissions during first-boot directory creation (Phase 06 §8.14): system directories (`C:\Impossible\System\`, `C:\Impossible\Bin\`, `C:\Programs\`) owned by Admin with rwx for owner and r-x for others. User home directories (`C:\Users\{name}\`) owned by that user with rwx for owner and no access for others. Temp and Recycle directories have rwx for everyone. This prevents standard users from modifying system files and prevents users from reading each other's files. After completing all items, update `docs/architecture/permissions.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"kernel: default system folder permissions"`.


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

**Prompt:** `security_set_owner(path, uid)` changes file ownership (admin only). `security_set_perms(path, owner_perms, other_perms)` changes permission bits (owner or admin). New files inherit their owner from the creating process's current user. Shell commands: `chmod <perms> <file>` sets permissions (e.g., `chmod rw- file.txt`), `chown <user> <file>` changes owner, `ls -l` shows permissions and owner in the detail listing. These commands check that the calling user has permission to make the change. After completing all items, update `docs/architecture/permissions.md` and `docs/user/shell-commands.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"kernel: ownership and permission management"`.


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

**Prompt:** The UAC-like elevation prompt appears when a standard user attempts an admin-restricted action. Dim the entire screen with a semi-transparent overlay (similar to the screenshot region select from Phase 05 §12.2). Show a modal dialog: icon + "This action requires administrator permission" + description of the action + [Allow]/[Deny] buttons. If the current user is admin, clicking Allow proceeds immediately. If the current user is a standard user, show a password field for the admin account. The overlay acts as a "secure desktop" — no other input is processed while the UAC dialog is shown. After completing all items, update `docs/architecture/security.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"desktop: UAC elevation prompt"`.


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

**Prompt:** Define which actions trigger the elevation prompt: installing/uninstalling programs (Phase 05 §18.2), modifying files in `C:\Impossible\System\`, changing system-wide settings (network, security, users), modifying other users' files, formatting/partitioning disks (Phase 06 §7-8), and enabling/disabling the firewall (Phase 07 §6). Each action checks `auth_get_current_user()->privilege_level` — if not ADMIN, trigger the elevation prompt before proceeding. After completing all items, update `docs/architecture/security.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"kernel: define admin-required actions"`.


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

**Prompt:** Verify that monocypher (from Phase 03) provides all needed crypto primitives: `crypto_argon2()` for key derivation from passwords, ChaCha20-Poly1305 for authenticated encryption (encrypt + MAC in one operation — prevents tampering), and a random nonce generator (12 bytes per encryption operation, never reused). These primitives are used by both file-level encryption (§5.2) and full disk encryption (§5.3). After completing all items, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"kernel: crypto primitives for encryption"`.


- [ ] Ensure **monocypher** crypto library is integrated (from Phase 03)
- [ ] Key derivation: `crypto_argon2(password, salt)` → 256-bit encryption key
- [ ] Symmetric encryption: ChaCha20-Poly1305 (authenticated encryption)
- [ ] Random nonce generation (12 bytes) per encryption operation
- [ ] Commit: `"kernel: crypto primitives for encryption"`

### 5.2 File-Level Encryption

**Prompt:** `crypto_encrypt_file(path, password)` derives a 256-bit key from the password + random salt via Argon2id, generates a random 12-byte nonce, encrypts the file data with ChaCha20-Poly1305 (which also produces a 16-byte authentication tag), writes the output as: salt(16) + nonce(12) + ciphertext + tag(16), and renames the file to `filename.enc`. `crypto_decrypt_file(path, password)` reverses the process, verifying the auth tag to detect tampering. Add right-click context menu options: "Encrypt..." (prompts for password) and "Decrypt..." (prompts for password). The file manager shows a lock icon for `.enc` files. After completing all items, create `docs/user/encryption.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"kernel: file encryption/decryption"`.


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

**Prompt:** Full disk encryption adds a transparent encrypt/decrypt layer between the VFS and the block device driver. Every sector read is decrypted, every sector write is encrypted, using a per-sector nonce derived from the LBA. At boot, before mounting the encrypted partition, prompt for the encryption password (pre-desktop, text-mode or minimal graphics). The encryption key is derived from the password and stored in a LUKS-like header on the first sectors of the partition. This is a stretch goal — it requires careful integration with the boot sequence and IXFS mount process. After completing all items, update `docs/user/encryption.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"kernel: full disk encryption"`.


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

**Prompt:** Track login sessions: each login creates a session with user reference, login timestamp, and unique session ID. Configurable idle timeout: auto-lock the screen (Phase 04 §12) after N minutes of no keyboard/mouse input. Track failed login attempts with timestamps for the lockout feature. Maintain an audit log in Codex `System\Security\AuditLog` recording login/logout events with timestamps and usernames. Shell commands: `who` shows current user and session info, `last` shows login history. After completing all items, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"kernel: session management and audit log"`.


- [ ] Track login sessions: user, login time, session ID
- [ ] Session timeout: auto-lock after configurable idle period
- [ ] Track failed login attempts with timestamps
- [ ] Audit log: store login/logout events in Codex `System\Security\AuditLog`
- [ ] Shell command: `who` — show current logged-in user + session info
- [ ] Shell command: `last` — show login history
- [ ] Commit: `"kernel: session management and audit log"`

### 6.2 Process Ownership

**Prompt:** Every running process is associated with the user who launched it. Add `owner_uid` to the process/task struct (Phase 01 §1 scheduler). When a process spawns a child, the child inherits the parent's owner_uid. A process can only send signals to (or kill) processes it owns — admin can kill any process. The Task Manager (Phase 05 §8) shows the process owner as an additional column. This prevents standard users from killing system processes or other users' applications. After completing all items, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"kernel: process ownership"`.


- [ ] Associate each running process with the user who launched it
- [ ] Store `owner_uid` in process/task struct
- [ ] Process inherits owner from parent process
- [ ] Process can only signal (kill) processes it owns (or admin can kill all)
- [ ] Task Manager: show process owner column
- [ ] Commit: `"kernel: process ownership"`

### 6.3 Secure Credential Storage

**Prompt:** The credential store securely saves passwords for services (WiFi passwords, network share credentials, email accounts). Encrypt credentials with a master key derived from the user's login password. `credential_store_save(service, username, password)` encrypts with ChaCha20-Poly1305 and stores in `C:\Users\{name}\AppData\Credentials\`. `credential_store_get(service, username, password_out)` decrypts and returns. The store is locked (inaccessible) when the user logs out. After completing all items, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"kernel: secure credential storage"`.


- [ ] Encrypted credential store for saved passwords (WiFi, network shares)
- [ ] Master key derived from user's login password
- [ ] `credential_store_save(service, username, password)` — encrypt and store
- [ ] `credential_store_get(service, username, password_out)` — decrypt and return
- [ ] Stored in `C:\Users\{name}\AppData\Credentials\` (encrypted)
- [ ] Locked when user logs out
- [ ] Commit: `"kernel: secure credential storage"`

### 6.4 Entropy Pool / Random Number Generator

**Prompt:** Collect entropy from multiple sources: keyboard inter-keystroke timings, mouse movement deltas, PIT timer jitter, and RDRAND instruction (if CPU supports it, check CPUID). Maintain a 256-byte entropy pool with cryptographic mixing (XOR + BLAKE2b hash). `random_bytes(buf, len)` produces cryptographically secure random bytes by hashing the pool state. Seed at boot from RDRAND (if available) + RTC value. This CSPRNG is used by: password salt generation, TLS nonces, session IDs, and encryption nonces. After completing all items, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"kernel: entropy pool and CSPRNG"`.


- [ ] Collect entropy from: keyboard timings, mouse movements, PIT jitter, RDRAND
- [ ] Maintain entropy pool (256-byte ring buffer with mixing)
- [ ] `random_bytes(buf, len)` — produce cryptographically secure random bytes
- [ ] Used by: password salt generation, TLS, nonces, session IDs
- [ ] Seed at boot from RDRAND (if available) + RTC
- [ ] Commit: `"kernel: entropy pool and CSPRNG"`

### 6.5 Accounts Settings Applet

**Prompt:** The `accounts.spl` settings applet (Phase 05 §4 SPL framework) provides a GUI for user management. Show all accounts in a list with name, privilege level icon, and avatar. Admin-only buttons: Create New Account (opens a dialog for username, password, privilege level), Delete Account (confirmation dialog, cannot delete self). Any user can: change own password (verify old password first), change own avatar (select from built-in options or browse files). Admin can change any user's password and privilege level. Auto-login toggle for the selected account. After completing all items, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"apps: accounts settings applet"`.


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

**Prompt:** The `security.spl` settings applet consolidates security configuration: firewall enable/disable toggle with rule summary (from Phase 07 §6), drive encryption status (from §5), password policy settings (minimum length, require uppercase/number/symbol), screen lock timeout (minutes of inactivity before auto-lock), and login attempt lockout threshold (number of failed attempts before cooldown). All settings stored in Codex under `System\Security\*`. After completing all items, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"apps: security settings applet"`.


- [ ] `security.spl` in Settings Panel:
  - [ ] Firewall: enable/disable, view rules
  - [ ] Encryption: encrypt/decrypt drives
  - [ ] Password policy: minimum length, complexity requirements
  - [ ] Screen lock timeout setting
  - [ ] Login attempt lockout threshold
- [ ] Commit: `"apps: security settings applet"`

### 6.7 Sudo / Run-As

**Prompt:** The `sudo <command>` shell command runs a command with admin privileges. If the current user is admin, prompt for their password to confirm identity. If standard user, prompt for the admin password. Cache credentials for 5 minutes (configurable in Codex `System\Security\SudoTimeout`) so repeated sudo commands don't re-prompt. Stretch goal: add a "Run as administrator" option to the right-click context menu (Phase 04 §4) for GUI applications. After completing all items, update `docs/user/shell-commands.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"shell: sudo command"`.


- [ ] Shell command: `sudo <command>` — run command with admin privileges
- [ ] Prompts for admin password before executing
- [ ] Cache credentials for 5 minutes (configurable)
- [ ] *(Stretch)* GUI "Run as administrator" option in context menus
- [ ] Commit: `"shell: sudo command"`

### 6.8 Executable Signing (Future)

**Prompt:** Stretch goal: sign OS binaries with Ed25519 signatures (monocypher) at build time. Embed the signature in the binary or store alongside it (.sig file). Before executing system binaries, verify the signature against a built-in public key. Warn the user when running unsigned executables (UAC-like dialog: "This program is from an unknown publisher"). Maintain a trust store of known publisher public keys. This prevents execution of tampered system files. After completing all items, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"kernel: executable signing"`.


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
