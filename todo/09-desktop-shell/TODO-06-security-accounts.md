---
schema_version: 1
id: security-accounts
domain: 09-desktop-shell
status: active
title: "TODO-06 -- Security & User Accounts"
---

# TODO-06 -- Security & User Accounts

> **Goal:** Add proper multi-user authentication, cryptographic password hashing, file permissions enforcement, a graphical login/lock screen, and UAC-equivalent privilege elevation -- transitioning from a single root-equivalent user to a production-grade security model that rivals Windows 11 and Linux.

> [!IMPORTANT]
> **Already exists**: `IXFS_S_IRUSR/IWUSR/IXUSR` permission constants + `i_uid/i_gid` inode fields + `ixfs_check_perm(inode, uid, ...)` in `ixfs.h` -- §9 VFS enforcement layer wires these up; do not redefine. `CPU_FEATURE_RDRAND` flag in `cpuid.h`. `vfs_mkdir/create/open/read/write`. `RegGetValue/SetValueEx/RegCreateKeyEx` for `HKU\{name}\` hive. `task_create_user()` for launching processes. `gfx_blur_rect()` (compositor) for blurred backgrounds. `theme_get(THEME_ACCENT)` for accent color. `SYS_CLIPBOARD_GET=57`, `SYS_SEARCH=58` highest assigned → `SYS_PRIVILEGE_REQUEST=59`. **Missing**: `auth_*`, login/lock screen UI, privilege token in task struct, UAC consent dialog (`csprng_*` + Monocypher SHIPPED 2026-06-12 via `02-kernel-core/TODO-03` §5 -- consume, do not re-implement). **Scope note**: §1 (user account stub) from TODO-03 §1 was a placeholder -- the full implementation lives here; TODO-03 §1 becomes a forward reference to this TODO. Complete sections in order: CSPRNG → password hashing → user accounts → auth API → home dirs → login screen UI → login flow → lock screen → user switching → file permissions → UAC elevation.

## Inputs

- `include/kernel/fs/ixfs.h` -- `i_uid`, `i_gid`, `IXFS_S_IRUSR/IWUSR/IXUSR`, `ixfs_check_perm()` -- §9 VFS permission enforcement wires these
- `include/kernel/fs/vfs.h` -- `vfs_open/read/write/create/mkdir` -- §3 hive load, §4 home dir creation, §9 access check hook
- `include/kernel/cpuid.h` -- `CPU_FEATURE_RDRAND` -- §11 CSPRNG entropy source
- `include/registry.h` -- `RegOpenKeyEx/SetValueEx/GetValue/CreateKeyEx` -- §1 account storage in `HKU\{name}\`
- `include/kernel/sched/task.h` -- `task_t` struct -- §10 adds `priv_level` + `owner_uid` to task; `task_create_user()`
- `include/kernel/sched/syscall.h` -- syscall table -- `SYS_PRIVILEGE_REQUEST=59` added in §10
- `include/kernel/klog.h` -- `klog()` -- throughout
- `include/gfx.h` -- `gfx_blur_rect()`, `gfx_fill_rect()`, `gfx_blit_alpha()` -- §5/§7 login + lock screen backgrounds
- `include/desktop/controls.h` (TODO-05) -- `CTRL_TEXTBOX`, `CTRL_BUTTON`, `ctrl_textbox_set_masked()` -- §5 password field
- `include/desktop/wm.h` -- `wm_create_window()`, `wm_set_z_order()` -- §5 full-screen login window
- `include/desktop/startmenu.h` (TODO-09) -- Start Menu user avatar area -- §9 user switching entry point
- `include/kernel/boot_splash.h` -- `boot_splash_progress()` -- §5 login screen replaces boot splash at handoff
- → XREF: `09-desktop-shell/TODO-03-service-manager.md §1` -- user account stub there is superseded by §1/§5 here; TODO-03 §1 is a forward reference to this TODO
- → XREF: `08-graphics-ui/TODO-09-desktop-shell-features.md §1` -- wallpaper engine provides `wallpaper_set()` called in §7 after successful login
- → XREF: `08-graphics-ui/TODO-04-animation-engine.md` -- `anim_mgr_add()` used in §6 shake animation and §3/§7 fade transitions
- -> XREF: `01-boot-platform/TODO-02-uefi-hardening-secureboot.md §6` -- Secure Boot shim chain ends at kernel entry; CSPRNG / runtime entropy is separate (see this file and kernel crypto TODOs, not a TODO-01 section)
- → XREF: `02-kernel-core/TODO-15-security-reference-monitor.md §9` -- full NT `NtFilterToken` + linked-token UAC kernel machinery; when TODO-11 §3 is implemented, §11 `privilege_request()` consent dialog should signal `NtRequestTokenElevation` rather than using `SYS_PRIVILEGE_REQUEST=59` directly

## Outcome

- `csprng_read(buf, len)` fills cryptographically secure random bytes from RDRAND + PIT jitter entropy pool.
- monocypher vendored freestanding; `auth_hash_password()` using `crypto_argon2i` + 16-byte salt; constant-time verify.
- `user_account_t` Registry-backed; `auth_create/delete/change_password`; Admin + optional Guest on first boot.
- `auth_login(user, pass)` → verify → load `HKU\{name}` hive → set kernel current-user state.
- Full-screen login screen (blurred wallpaper, avatar, bullet-masked password, Power/Network/Accessibility icons).
- Multi-user avatar strip; incorrect-password shake animation; 5-attempt lockout; auto-login bypass.
- Win+L lock screen overlay; resume same session on correct password.
- Start Menu user switching; session keep-alive stub for fast user switching.
- IXFS `i_uid` permission checks wired into `vfs_open/write/exec`; FAT32 fallback permits all.
- `privilege_request(reason)` UAC-style consent dialog; `SYS_PRIVILEGE_REQUEST=59`.

## Implementation Order

| ⭐  | Order | Deliverable                                                                                           | Depends On                                                                            | Status |
| --- | :---: | ----------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------- | :----: |
| ⭐  |   1   | §1 CSPRNG -- consume kernel `csprng_fill()` (shipped in D02T03 §5; ad-hoc pool retired)               | `02-kernel-core/TODO-03` §5 (shipped)                                                 |  [/]   |
| ⭐  |   2   | §2 Password hashing -- monocypher vendor, `crypto_argon2i`, `auth_hash/verify_password`               | §11 CSPRNG (salt generation); `kmalloc` (argon2i work-area)                          |  [ ]   |
| 💎  |   3   | §1 User account system -- `user_account_t`, Registry CRUD, Admin + Guest first boot                   | §2 password hashing; Registry (exists)                                                |  [ ]   |
| 💎  |   4   | §3 Authentication API -- `auth_login/logout/get_current_user`, `HKU\{name}` hive load/unload          | §1 account system; Registry hive load                                                 |  [ ]   |
| 💎  |   5   | §4 User home directories -- `vfs_mkdir` on `auth_create_user()`, `%USERPROFILE%`, shell CWD            | §1 account system; `vfs_mkdir` (exists)                                               |  [ ]   |
| 💎  |   6   | §5 Login screen UI -- full-screen blurred bg, OS logo, avatar, bullet-masked textbox, sign-in button   | §3 auth API; TODO-05 `CTRL_TEXTBOX`; `gfx_blur_rect()` (exists); WM z-order         |  [ ]   |
| 💎  |   7   | §6 Login flow -- multi-avatar strip, shake animation, lockout, auto-login, profile load                | §5 login UI; §3 auth; TODO-07 `wallpaper_set()`; TODO-02 animation engine            |  [ ]   |
| 💎  |   8   | §7 Lock screen -- Win+L overlay, blurred desktop capture, clock, password resume same session          | §6 login flow; compositor back-buffer blur                                            |  [ ]   |
| 💎  |   9   | §8 User switching -- Start Menu avatar → Switch/Sign out, fast-user-switch session stub                | §6 login flow; TODO-09 Start Menu user area                                           |  [ ]   |
| ⭐  |  10   | §9 File permissions -- wire `ixfs_check_perm()` into `vfs_open/write/exec`; FAT32 fallback            | §1 user accounts (uid available); `ixfs_check_perm()` (exists in ixfs.h)            |  [ ]   |
| ⭐  |  11   | §10 UAC elevation -- `privilege_request(reason)` consent dialog, process token, `SYS_PRIVILEGE_REQUEST=59` | §3 auth API; §9 permissions; TODO-05 dialog; `task_t` priv token                |  [ ]   |

---

## 1. CSPRNG `[Opus]`

The kernel CSPRNG SHIPPED 2026-06-12 in `02-kernel-core/TODO-03` §5: `src/kernel/csprng.c` (ChaCha20 fast-key-erasure, Blake2b conditioner, seeded at boot Phase 1, `NtGetRandom` syscall). This section is now a thin consumer: auth code calls `csprng_fill()`/`csprng_u64()` directly for §2 password salts and nonces.

**Files:** consume `include/kernel/csprng.h` (exists; no desktop-side files)

> [!NOTE]
> The previously drafted ad-hoc 256-byte XOR pool (RDRAND + PIT jitter, `csprng_read()`) is OBSOLETE and must NOT be implemented -- it was the fallback for the case where this domain shipped before the kernel library section; the real CSPRNG won the race.

- [x] OWNERSHIP RESOLVED: kernel CSPRNG is `02-kernel-core/TODO-03` §5 `csprng_fill()` (shipped; first-seed hardening continues in `01-boot-platform/TODO-12` §8); the ad-hoc pool design is retired
- [ ] §2 password hashing and any nonce generation in this TODO call `csprng_fill()` directly (verify when §2 is implemented; no `csprng_read` shim)

## 2. Password Hashing `[Opus]`

Vendor monocypher (public domain, ~1.5 K lines) at `src/libs/monocypher/`. `crypto_argon2i` password hashing (memory-hard, GPU-resistant). 16-byte random salt via `csprng_read()`. Constant-time compare.

**Files:** `src/libs/monocypher/monocypher.c` (vendor), `include/libs/monocypher.h` (vendor), `src/kernel/auth.c` (new), `include/kernel/auth.h` (new)

> [!NOTE]
> `[Opus]` due to: security-critical cryptography (password hashing), constant-time compare (timing side-channel prevention), and memory-hard KDF (argon2i work-area allocation). monocypher is available at https://monocypher.org (C99, public domain, ~1500 lines, single `.c` file, no libc dependencies except `<stdint.h>` and `<stddef.h>`). Use same `libc_shim.h` + freestanding compile strategy as miniz (TODO-04 §3). **Argon2i params**: `m_cost=4096` (4 MiB work-area via `kmalloc`), `passes=3`, `lane_width=1`, `tag_length=64`. Work-area: `kmalloc(4 * 1024 * 1024)` -- this is within the 2 MiB heap limit risk; use `pmm_alloc_contiguous()` instead for the 4 MiB argon2i work-area. Salt: `csprng_read(salt, 16)`. Stored hash format (Registry, base64-like hex): `{salt_hex32}{hash_hex128}` = 160-char string. **Constant-time compare**: `crypto_verify64(a, b)` from monocypher -- use this instead of `memcmp` for hash comparison.

- [ ] Vendor monocypher: `src/libs/monocypher/monocypher.c` + `include/libs/monocypher.h`; Makefile freestanding compile rule
- [ ] `void auth_hash_password(const char *password, const uint8_t salt[16], char hash_out[160])` -- argon2i; PMM work-area; hex-encode result
- [ ] `int auth_verify_password(const char *password, const char *stored_hash)` -- decode stored salt; rehash; `crypto_verify64()` constant-time compare; return 1 on match
- [ ] `krealloc` / `libc_shim.h` pattern from TODO-04 §3 reused for monocypher build
- [ ] Makefile: monocypher compile rule with `-ffreestanding -O2 -include include/kernel/libc_shim.h`
- [ ] Boot log: `klog(LOG_OK, "auth", "monocypher argon2i ready (m=%u passes=%u)", 4096, 3)`
- [ ] Commit: `"security: password hashing -- monocypher argon2i, 16-byte salt, constant-time verify"`

## 3. User Account System `[Sonnet]`

`user_account_t` (uid, username, display_name, password_hash, privilege_level, avatar_path, home_dir, auto_login). Registry at `HKU\{name}\`. `auth_create/delete/change_password`. Admin + optional Guest on first boot.

**Files:** `src/kernel/auth.c` (extend), `include/kernel/auth.h` (extend)

> [!NOTE]
> `typedef struct { uint16_t uid; char username[32]; char display_name[64]; char password_hash[160]; uint8_t privilege; char avatar_path[128]; char home_dir[128]; uint8_t auto_login; } user_account_t;` Privilege: `#define PRIV_ADMIN 2`, `PRIV_USER 1`, `PRIV_GUEST 0`. Registry layout: `HKU\{name}\PasswordHash`, `HKU\{name}\Privilege`, `HKU\{name}\Avatar`, `HKU\{name}\AutoLogin`, `HKU\{name}\DisplayName`, `HKU\{name}\HomeDir`, `HKU\{name}\UID`. Global: `HKLM\SOFTWARE\Impossible\Accounts\LastUID` for auto-increment UID. First-boot detection: `HKLM\SOFTWARE\Impossible\Accounts\Initialized` flag. On first boot: create Admin user (uid=1, privilege=ADMIN, password="") + Guest (uid=2, privilege=GUEST, no password, `AutoLogin=0`). Supersedes the stub in TODO-03 §1.

- [ ] `typedef user_account_t` struct; `#define PRIV_ADMIN/USER/GUEST` in `auth.h`
- [ ] `int auth_create_user(const char *name, const char *display_name, const char *password, uint8_t privilege)` -- uid = ++LastUID; `auth_hash_password()`; write Registry `HKU\{name}\*`; call `user_home_create(name)` (§4)
- [ ] `int auth_delete_user(const char *name)` -- admin-only check; delete `HKU\{name}\*` subtree; leave home dir (Windows behavior: home dir preserved)
- [ ] `int auth_change_password(const char *name, const char *old_pass, const char *new_pass)` -- verify old; hash new; write `PasswordHash`
- [ ] `int auth_load_user(const char *name, user_account_t *out)` -- read all fields from `HKU\{name}\`
- [ ] `int auth_list_users(user_account_t *out, int max)` -- enumerate `HKU\*` subkeys
- [ ] `void auth_first_boot_setup(void)` -- check `Initialized` flag; create Admin + Guest; set flag
- [ ] `auth_first_boot_setup()` called from `kernel_main()` after Registry is live; after `csprng_init()`
- [ ] Commit: `"auth: user account system -- user_account_t, Registry CRUD, Admin+Guest first boot"`

## 4. Authentication API `[Sonnet]`

`auth_login(username, password)` verifies hash, sets current user, loads `HKU\{username}\` hive. `auth_logout()` clears user, returns to login screen. `auth_get_current_user()`.

**Files:** `src/kernel/auth.c` (extend), `include/kernel/auth.h` (extend)

> [!NOTE]
> Kernel global: `static user_account_t g_current_user` + `static uint8_t g_logged_in`. `auth_login()`: `auth_load_user(name, &account)` → `auth_verify_password(pass, account.password_hash)` → if ok: `g_current_user = account`; `g_logged_in = 1`; `registry_load_hive("HKU\\{name}\\", hive_path)` (loads per-user Registry from `C:\Users\{name}\AppData\registry.hive`); update `%USERPROFILE%` env; kick off desktop init (call `desktop_start()`). `auth_logout()`: `g_logged_in = 0`; unload `HKU\{name}` hive; call `desktop_stop()` + `login_screen_show()`. **Lockout**: `auth_login()` increments `g_login_failures[uid]`; if >= 5 and last-failure within 30 s: return `AUTH_ERR_LOCKED`; `g_lockout_until_tick` set to `system_get_ticks() + 30 * PIT_TARGET_FREQ`.

- [ ] `int auth_login(const char *username, const char *password)` → `AUTH_ERR_INVALID / AUTH_ERR_LOCKED / AUTH_OK`
- [ ] `void auth_logout(void)` -- clear `g_current_user`, unload hive, return to login screen
- [ ] `const user_account_t *auth_get_current_user(void)` -- returns `&g_current_user` or NULL if not logged in
- [ ] `uint8_t auth_is_logged_in(void)` + `uint8_t auth_is_admin(void)` helpers
- [ ] Lockout state: per-uid failure counter array + `g_lockout_until_tick`; reset on success
- [ ] `AUTH_ERR_INVALID = -1`, `AUTH_ERR_LOCKED = -2`, `AUTH_OK = 0` in `auth.h`
- [ ] Commit: `"auth: auth_login/logout/get_current_user, HKU hive load/unload, lockout logic"`

## 5. User Home Directories `[Sonnet]`

On `auth_create_user()`: create `C:\Users\{name}\{Desktop,Documents,Downloads,Pictures,AppData}`. Set inode `i_uid`. `%USERPROFILE%` env var. Shell CWD starts at home.

**Files:** `src/kernel/auth.c` (extend)

> [!NOTE]
> `user_home_create(name)` called internally from `auth_create_user()`. Subdirs to create: `Desktop`, `Documents`, `Downloads`, `Pictures`, `AppData`, `AppData\Startup`. Use `vfs_mkdir()` for each. After creating each dir: open node → set `i_uid = user.uid` via an IXFS direct call (`ixfs_set_owner(node, uid)`). `%USERPROFILE%` env: store in a simple global `g_userprofile_path[256]`; `auth_login()` sets it. Shell `cmd_cd_default()`: start CWD at `g_userprofile_path` on login.

- [ ] `void user_home_create(const char *username, uint16_t uid)` -- `vfs_mkdir` for each standard dir; set inode owner
- [ ] `int ixfs_set_owner(struct vfs_node *node, uint16_t uid)` -- write `i_uid` to inode (new helper in `ixfs.c`)
- [ ] `g_userprofile_path[256]` global; set by `auth_login()`; `%USERPROFILE%` exposed via `auth_get_userprofile()`
- [ ] Shell: update `cmd.c` to default CWD to `g_userprofile_path` at startup if logged in
- [ ] Commit: `"auth: user home dirs -- vfs_mkdir Desktop/Documents/Downloads/Pictures/AppData, i_uid owner"`

## 6. Login Screen UI `[Sonnet]`

Full-screen window before desktop loads: blurred wallpaper background, OS logo, user avatar (circular), username label, password field (bullet-masked), [Sign in →] button. Bottom-left: Power (⏻), Network (🌐), Accessibility (♿) icons.

**Files:** `src/desktop/login_screen.c` (new), `include/desktop/login_screen.h` (new)

> [!NOTE]
> Displayed before desktop init: `login_screen_show()` called from `kernel_main()` after `auth_first_boot_setup()`. For single user with `auto_login=1`: skip to desktop immediately (`desktop_start()`). Full-screen window: `wm_create_window(0, 0, screen_w, screen_h, "login", WM_FLAG_FULLSCREEN | WM_FLAG_NO_DECORATIONS)`; `wm_set_z_order(win, 32767)` (topmost). Background: capture framebuffer; `gfx_blur_rect(fb, 0, 0, screen_w, screen_h, 20)`. OS logo: `gfx_blit(logo_surface, center_x - 64, center_y - 200)`. Avatar: circular `gfx_draw_circle_clip(avatar_surface, ax, ay, 64)` -- if no avatar image: draw colored circle with initial letter. Password `CTRL_TEXTBOX`: `ctrl_textbox_set_masked(textbox, 1)` -- renders `•` per char. Sign-in button: `CTRL_BUTTON` with callback → `auth_login(username, password_buf)`. Bottom icons: 3 × 40 px icon buttons at `(16, screen_h - 56)`.

- [ ] `void login_screen_show(void)` -- create full-screen window; blur background; render layout; event loop
- [ ] `void login_screen_hide(void)` -- destroy login window; transfer focus to desktop
- [ ] Background blur: `gfx_blur_rect()` on a captured background; rendered once at show
- [ ] Avatar: circular clip; fallback initial-letter colored circle
- [ ] Password textbox: `ctrl_textbox_set_masked(textbox, 1)` -- bullet substitution
- [ ] Sign-in button callback: call `auth_login()`; on `AUTH_OK`: `login_screen_hide()` + `desktop_start()`; on fail: trigger shake (§7)
- [ ] Power icon: `sys_shutdown()` / `sys_reboot()` via existing syscalls
- [ ] Route the Start Menu power button (`desktop.c` `acpi_shutdown()` direct call) through the SeShutdownPrivilege-gated `sys_shutdown()` syscall, not the raw ACPI primitive -> XREF: 02-kernel-core/TODO-15 §8
- [ ] Network icon: show network status flyout stub (forward ref to TODO-09 tray)
- [ ] Commit: `"login: login screen UI -- blurred bg, avatar, bullet password, sign-in button, power/network icons"`

## 7. Login Flow `[Sonnet]`

Multi-user avatar strip. On failure: shake animation + "Incorrect password" in red. Lockout after 5 failures (30 s cooldown). Auto-login bypass. On success: load wallpaper, pinned apps, `HKU\{name}` Registry.

**Files:** `src/desktop/login_screen.c` (extend)

> [!NOTE]
> **Multi-user**: `auth_list_users(users, 8)` → render avatar strip horizontally (click to select active user); selected user highlighted with accent underline. Selected username populates the username label; password field focused. **Shake animation**: on `AUTH_ERR_INVALID`: `anim_mgr_add()` tween on `x_offset` of password row; easing = `EASE_BACK` (overshoot); amplitude ±8 px; duration 400 ms; clear password field. **"Incorrect password"**: red label below textbox, auto-hide after 3 s. **Lockout**: on `AUTH_ERR_LOCKED`: disable sign-in button; show countdown "Try again in Xs" (update every second via `sched_task_add("login_unlock", ..., 1, 1)`). **Auto-login**: `auth_first_boot_setup()` or `auth_login_check_autologin()` → single user with `AutoLogin=1`: call `auth_login(name, "")` with empty password directly. **Profile load**: `auth_login()` loads `HKU\{name}` hive → `wallpaper_set()` reads `HKU\{name}\Software\Impossible\Wallpaper` → `desktop_load_pinned_apps()`.

- [ ] `login_screen_show()` multi-user: `auth_list_users()` → avatar strip; click handler sets active user
- [ ] Shake animation: `anim_mgr_add()` on `password_row_x_offset`; `EASE_BACK`; 400 ms; clear password field
- [ ] `login_screen_tick()` -- called from desktop tick; updates lockout countdown label
- [ ] Lockout display: disabled sign-in button + countdown label; re-enable when lockout expires
- [ ] `auth_login_check_autologin()` -- called at boot; single `AutoLogin=1` user → skip screen
- [ ] On success: `login_screen_hide()` → `desktop_start()` → `wallpaper_set()` → `desktop_load_pinned_apps()`
- [ ] Commit: `"login: login flow -- multi-user avatars, shake animation, lockout countdown, auto-login, profile load"`

## 8. Lock Screen `[Sonnet]`

Win+L: overlay (clock, blurred desktop behind, "Enter password"). Resume same session on correct password. Distinct from sign-out -- no app restart.

**Files:** `src/desktop/lock_screen.c` (new), `include/desktop/lock_screen.h` (new)

> [!NOTE]
> Lock screen is different from login screen: it overlays the running desktop (blurs current back-buffer in place) and restores the same session on unlock -- no `desktop_stop()` call. `lock_screen_show()`: create full-screen overlay window with `z_order=32767`; `gfx_blur_rect(g_compositor_backbuf, 0, 0, screen_w, screen_h, 20)` → use as background; render clock (HH:MM, updates every second); username + avatar (circular); password textbox. Win+L hotkey: registered in global hotkey table (TODO-06 §8) → `lock_screen_show()`. On correct password (`auth_verify_password()`): `lock_screen_hide()` → desktop immediately visible (no reload). On failure: shake animation (reuse login_screen shake pattern). Auto-lock: `HKLM\SOFTWARE\Impossible\Screen\LockAfterSeconds` (default 300 s idle) → `lock_screen_show()` via scheduler task.

- [ ] `void lock_screen_show(void)` -- overlay; blur current back-buffer; render clock + avatar + password field
  - Also the actuator for power-button action `4` (lock), which resolves and is refused today because nothing can lock -> XREF: `02-kernel-core/TODO-26-power-management.md` §7 (item: "`4` = lock screen")
- [ ] `void lock_screen_hide(void)` -- destroy overlay; compositor back-buffer restored
- [ ] Win+L: global hotkey dispatch entry → `lock_screen_show()`
- [ ] Clock update: `sched_task_add("lock_clock", lock_screen_update_clock, 1, 1)` while locked
- [ ] Auto-lock: scheduler task checks idle ticks; `lock_screen_show()` if exceeded `LockAfterSeconds`
- [ ] Incorrect password: shake animation + "Incorrect password" label
- [ ] Commit: `"security: lock screen -- Win+L, blur overlay, clock, password resume session, auto-lock"`

## 9. User Switching `[Sonnet]`

Start Menu user avatar → "Switch user" or "Sign out". Session keep-alive stub for fast user switching.

**Files:** `src/desktop/startmenu.c` (extend)

> [!NOTE]
> Start Menu bottom-left: current user avatar (32 px circular) + display name. Click → popup: "Switch user", "Sign out", "Lock" (Win+L). **Sign out**: `auth_logout()` → `login_screen_show()`. **Switch user**: `auth_logout()` → `login_screen_show()` (all existing apps close; full session teardown -- fast user switching is stretch). **Fast user switch stretch**: keep current `task_t` tree in suspended state; `auth_login()` spawns new desktop session in separate task group; Win+Ctrl+Left/Right toggles between sessions. Keep as stub: `fast_user_switch_suspend()` / `fast_user_switch_resume()` declared but unimplemented.

- [ ] Start Menu: add user avatar (32 px) + display name at bottom-left; `auth_get_current_user()`
- [ ] Click → context menu: "Switch user", "Sign out", "Lock"
- [ ] "Sign out" → `auth_logout()`; "Lock" → `lock_screen_show()`; "Switch user" → `auth_logout()` + `login_screen_show()`
- [ ] `fast_user_switch_suspend(uint16_t uid)` + `fast_user_switch_resume(uint16_t uid)` stubs in `auth.h` (not yet implemented)
- [ ] Commit: `"desktop: user switching -- Start Menu avatar, sign out, switch user, lock entry points"`

## 10. File Permissions `[Opus]`

Wire `ixfs_check_perm(inode, uid, access_type)` into `vfs_open/write/exec` paths. FAT32 fallback: all files owned by current user, full permissions. `security_check_access(path, uid, access_type)` enforced.

**Files:** `src/kernel/fs/vfs.c` (extend), `src/kernel/fs/ixfs.c` (extend), `include/kernel/security.h` (new)

> [!NOTE]
> `[Opus]` due to: security-critical permission enforcement in hot paths (every VFS open/write/exec must check), correct uid propagation from current user state, and FAT32 filesystem that has no uid concept needing graceful fallback. `security_check_access(path, uid, access_flags)`: open VFS node → check filesystem type; if IXFS: call `ixfs_check_perm(inode, uid, access_flags)` -- returns 0 on allow; if FAT32: return 0 (always allow); if uid == 0 (Admin): return 0 (always allow). Integrate in `vfs_open()`: after finding node, call `security_check_access(path, auth_get_current_uid(), requested_flags)` → if deny: return `VFS_ERR_PERMISSION`. Same in `vfs_write()` and exec path (`task_create_user()`). `access_flags`: `#define ACCESS_READ 0x04`, `ACCESS_WRITE 0x02`, `ACCESS_EXEC 0x01` (mirrors IXFS permission bits). Admin bypass: `auth_get_current_user()->privilege == PRIV_ADMIN` skips check (mirrors Windows admin elevation).

- [ ] `int security_check_access(const char *path, uint16_t uid, uint8_t access_flags)` in `security.h/c`
- [ ] `uint16_t auth_get_current_uid(void)` -- returns `g_current_user.uid` or 0 if no user
- [ ] Hook `vfs_open()`: after node lookup, call `security_check_access()`; return `-EPERM` on deny
- [ ] Hook `vfs_write()`: check `ACCESS_WRITE` before write
- [ ] Hook `task_create_user()`: check `ACCESS_EXEC` before launching ELF
- [ ] FAT32: `security_check_access()` returns 0 (permit all) for non-IXFS nodes
- [ ] Admin bypass: `PRIV_ADMIN` → skip permission check (log `klog(LOG_DEBUG, "sec", "admin bypass %s")`)
- [ ] `#define ACCESS_READ/WRITE/EXEC` in `security.h`; use `VFS_ERR_PERMISSION = -13` (errno 13 = EACCES)
- [ ] Commit: `"security: file permissions -- vfs_open/write/exec enforcement, ixfs_check_perm, FAT32 bypass, admin skip"`

## 11. UAC-Equivalent Elevation `[Opus]`

`privilege_request(reason)` shows consent dialog ("Allow this app to make changes?", reason text, Yes/No). Non-admin processes get `PRIV_USER` token. Admin operations blocked without consent. `SYS_PRIVILEGE_REQUEST=59`.

**Files:** `src/kernel/auth.c` (extend), `include/kernel/sched/task.h` (extend), `include/kernel/sched/syscall.h` (extend), `src/desktop/login_screen.c` (extend)

> [!NOTE]
> `[Opus]` due to: security-critical privilege separation (non-admin token model), kernel-side consent decision (consent dialog must run in desktop context but decision stored in kernel), and cross-process privilege grant (elevate calling task's token after admin consent). **Task token**: add `uint8_t priv_level` + `uint16_t owner_uid` fields to `task_t`; set at `task_create_user()` to `auth_get_current_user()->privilege`. `privilege_request(reason)`: kernel dispatches to desktop `privilege_consent_dialog(reason)` (a modal dialog on top of everything, `z_order=31000`); dialog shows: gray overlay backdrop (dim 40%), OS shield icon (🛡), reason text, "Administrator password" label + password field if current user is non-admin, [Yes]/[No] buttons. If admin: verify admin password; if already admin: just Yes/No. On Yes: elevate calling task's `priv_level` to `PRIV_ADMIN` for the duration of the operation (stored in task, reset on `SYS_EXIT`). `SYS_PRIVILEGE_REQUEST=59`: user-mode syscall → kernel calls `privilege_request(reason_str)` → blocks until dialog responds → returns `1` (granted) or `0` (denied).

- [ ] Add `uint8_t priv_level` + `uint16_t owner_uid` to `task_t` struct in `task.h`
- [ ] `task_create_user()`: set `task->priv_level = auth_get_current_user()->privilege`; `task->owner_uid = auth_get_current_uid()`
- [ ] `int privilege_request(const char *reason)` -- desktop modal dialog; block kernel side with semaphore until response
- [ ] Consent dialog: 480×280 px, `z_order=31000`; gray overlay; shield icon (Unicode or IRES); reason text; password field (if non-admin); Yes/No buttons
- [ ] On Yes + correct password: `current_task->priv_level = PRIV_ADMIN`; return 1
- [ ] On No or wrong password: return 0
- [ ] `#define SYS_PRIVILEGE_REQUEST 59` in `syscall.h`; `sys_privilege_request(reason_ptr)` handler
- [ ] `auth_is_admin()` checks `current_task->priv_level` (runtime check, not just user account level)
- [ ] Commit: `"security: UAC elevation -- privilege_request(), task priv token, consent dialog, SYS_PRIVILEGE_REQUEST=59"`

---

## OS Comparison


| ⭐  | Feature          | 🪟 Win11                                          | 🐧 Linux                                                             | 🚀 Impossible OS                                     |
| --- | ---------------- | ------------------------------------------------- | -------------------------------------------------------------------- | ---------------------------------------------------- |
| ⭐  | CSPRNG           | ✅ `CryptGenRandom` / `BCryptGenRandom`; RDRAND + | ✅ `/dev/urandom`, `/dev/random`; CSPRNG in kernel                   | ⬜ §11 -- `⭐` pool seeded RDRAND +                  |
| ⭐  | Password hashing | ✅ NTLM / Kerberos (not argon2i);                 | ✅ `shadow` with yescrypt/SHA-512/bcrypt; PAM pluggable              | ⬜ §2 -- `⭐` argon2i (2015 Password Hashing         |
| 💎  | User accounts    | ✅ SAM/LDAP; SIDs; full ACL; groups;              | ✅ `/etc/passwd`+`/etc/shadow`; uid/gid; `useradd/passwd`            | ⬜ §1 -- Registry-backed; uid 16-bit; privilege enum |
| 💎  | Login screen     | ✅ Windows Hello, PIN, fingerprint, picture       | ✅ GDM/SDDM/LightDM; user strip; password field;                     | ⬜ §5 -- /§6; blurred bg + circular                  |
| 💎  | Lock screen      | ✅ Win+L; hello/PIN unlock; no session            | ✅ `loginctl lock-session`; gnome-screensaver/i3lock; idle auto-lock | ⬜ §8 -- blur current compositor back-buffer; no     |
| ⭐  | File permissions | ✅ NTFS ACLs (full DACL/SACL); very               | ✅ Unix `rwxrwxrwx` + ACLs (`setfacl`);                              | ⬜ §9 -- `⭐` same Unix 9-bit model                  |
| ⭐  | UAC elevation    | ✅ UAC elevation prompt; integrity levels;        | ✅ `sudo`/`pkexec`/PolicyKit; `setuid`; `capabilities`               | ⬜ §10 -- `⭐` task-local priv token reset           |

> **After §1–§11:** Impossible OS moves from single root-equivalent to a full multi-user security model. The `⭐` differentiators: argon2i password hashing is stronger than Windows NTLM and typical Linux SHA-512; IXFS permission enforcement requires zero new inode format changes (fields were already designed in); and UAC task-local elevation automatically resets on process exit -- no ambient privilege escalation possible.

## Verification

- [ ] `bash scripts/build.sh clean` → `tail -1 build/build.log` → `=== BUILD OK ===`
- [ ] Boot serial: `[csprng] entropy pool seeded (RDRAND+jitter)` or `(jitter only)`
- [ ] `auth_hash_password("password", salt, hash)` → 160-char hex string; `auth_verify_password("password", hash)` → 1; `auth_verify_password("wrong", hash)` → 0
- [ ] First boot: Admin + Guest accounts created; `HKLM\...\Initialized` flag set; second boot: no duplicate creation
- [ ] `auth_login("Admin", "")` → `AUTH_OK`; `auth_get_current_user()` → uid=1, PRIV_ADMIN
- [ ] `auth_login("Admin", "wrong")` five times → `AUTH_ERR_LOCKED` on sixth attempt; after 30 s → `AUTH_ERR_INVALID` again
- [ ] Login screen appears before desktop; password field shows bullets; correct password → desktop loads with user wallpaper
- [ ] Win+L → lock screen overlays desktop; correct password → desktop resumes without restarting apps
- [ ] Start Menu bottom-left shows avatar + "Admin"; click → "Sign out" → login screen shown
- [ ] Create file as Guest user → attempt to write as uid=2 to Admin's `C:\Users\Admin\Documents\` → `VFS_ERR_PERMISSION` returned
- [ ] `privilege_request("Install driver")` → consent dialog shown; click No → returns 0; click Yes + correct admin pass → returns 1
- [ ] `SYS_PRIVILEGE_REQUEST=59` from user-mode app → kernel routes to consent dialog → blocks until response
- [ ] Commit: `"security: user accounts, auth, login/lock screen, permissions, UAC -- all complete"`
