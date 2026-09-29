<!-- docs: covers=todo/09-desktop-shell/TODO-06-security-accounts.md sources=include/kernel/csprng.h,src/kernel/csprng.c,include/kernel/security/token.h,src/kernel/security/token.c,src/kernel/security/privileges.c,include/kernel/fs/ixfs.h,src/kernel/fs/ixfs/ixfs_format.c,src/libs/monocypher/monocypher.h,src/kernel/registry.c,src/desktop/desktop.c reviewed=2026-09-29 order=12 -->
# Security and User Accounts

## What is it?

This roadmap turns Impossible OS from a single all-powerful user into a multi-user system: user accounts with hashed passwords, a sign-in screen, a Win+L lock screen, user switching, per-user file permissions and a consent prompt before a program gains administrator rights. The kernel security primitives it builds on have shipped (random numbers, password-hashing code, access tokens and privileges), but nothing a user would see has: there are no accounts, no sign-in and no lock screen. The first section is in progress because its only remaining item is to confirm the kernel random number generator is the one consumed.

## How does it work?

**Today.** Everything runs as one user, and nothing asks who you are.

- **Random numbers.** The kernel CSPRNG is ChaCha20 with fast key erasure, seeded at boot from RDRAND, the TSC, the ACPI timer, jitter, the TPM and the loader's seed ([`csprng.c`](../../src/kernel/csprng.c)). `csprng_fill()` and `csprng_u64()` serve kernel callers, `csprng_crypto_ok()` says whether it is seeded well enough to make keys, and `NtGetRandom` serves user programs ([`csprng.h`](../../include/kernel/csprng.h)). The seeding is described in [Early Entropy and Random Seed](../boot/early-entropy-random-seed.md).
- **Password hashing.** Monocypher 4.0.2 is vendored and built, including `crypto_argon2()` with the Argon2d, Argon2i and Argon2id variants ([`monocypher.h`](../../src/libs/monocypher/monocypher.h)). Only a kernel self-test calls it today.
- **Tokens and privileges.** Every task carries an access token. Task 0 gets the SYSTEM token with all privileges, and new tasks copy their creator's, so every program runs as SYSTEM ([`token.c`](../../src/kernel/security/token.c)). `SeCreateUserToken()` can build a standard or administrator user token but has no caller outside the tests. 25 privileges are defined ([`privileges.c`](../../src/kernel/security/privileges.c)), and some are enforced: shutdown and reboot need `SeShutdownPrivilege`, setting the clock needs `SeSystemtimePrivilege`. The whole model is described in [Security Reference Monitor](../kernel/security-reference-monitor.md).
- **Registry.** `HKU\Default` holds the one profile (home folder `C:\Users\Default`, shell `C:\cmd.exe`, wallpaper), and `HKEY_CURRENT_USER` always resolves to it because nothing calls `reg_set_current_user()` ([`registry.c`](../../src/kernel/registry.c)).
- **File permissions.** IXFS stores an owner, a group and Unix-style read, write and execute bits on each inode, and `ixfs_check_perm()` checks read and write for a user and group ([`ixfs_format.c`](../../src/kernel/fs/ixfs/ixfs_format.c), [`ixfs.h`](../../include/kernel/fs/ixfs.h)). Nothing calls it, and new files are owned by user 0.
- **Shutdown.** The Start menu's power button calls `acpi_shutdown()` directly instead of the privilege-checked shutdown call ([`desktop.c`](../../src/desktop/desktop.c)); rerouting it is an open item in the login screen section.

**Planned design.**

1. **CSPRNG.** Consume the kernel generator; the roadmap's own pool was retired.
2. **Password hashing.** `auth_hash_password()` and `auth_verify_password()` on Argon2 with a random salt.
3. **Accounts.** A user record in the Registry, with an Administrator and a Guest created on first boot.
4. **Authentication.** `auth_login()` and `auth_logout()`, loading the user's `HKU\{name}` hive, with a 30-second lockout after five failures.
5. **Home folders.** `C:\Users\{name}` with Desktop, Documents, Pictures, Music and Downloads, owned by the user.
6. **Sign-in screen** and 7. **Sign-in flow.** A full-screen acrylic screen with a 192 pixel avatar and a masked password box, per the [lock and sign-in design](../design/shell.md#lock-and-sign-in-screens), then avatars for each user, a shake on a wrong password and optional automatic sign-in.
8. **Lock screen.** Win+L blurs the desktop and asks for the password to resume.
9. **User switching.** Sign out and switch user from the Start menu.
10. **File permissions.** `ixfs_check_perm()` enforced on open, write and execute.
11. **Elevation.** A consent dialog that grants a program administrator rights for the life of that process.

## What are its interfaces?

| Interface | Status |
| --- | --- |
| `csprng_fill()`, `csprng_u64()`, `csprng_crypto_ok()`, `NtGetRandom` | Shipped |
| `crypto_argon2()` | Shipped (vendored Monocypher) |
| Access tokens, `SeCreateUserToken()`, privilege checks, token syscalls | Shipped kernel primitives; no user logon path |
| `ixfs_check_perm()` | Shipped, not called |
| `auth_*`, sign-in and lock screens, elevation dialog | Planned |

## How do I use it?

It cannot be used yet. The system starts straight to the desktop as the default user.

## What is not implemented yet?

- [CSPRNG](../../todo/09-desktop-shell/TODO-06-security-accounts.md#1-csprng-opus) (in progress) and [Password Hashing](../../todo/09-desktop-shell/TODO-06-security-accounts.md#2-password-hashing-opus)
- [User Account System](../../todo/09-desktop-shell/TODO-06-security-accounts.md#3-user-account-system-sonnet), [Authentication API](../../todo/09-desktop-shell/TODO-06-security-accounts.md#4-authentication-api-sonnet) and [User Home Directories](../../todo/09-desktop-shell/TODO-06-security-accounts.md#5-user-home-directories-sonnet)
- [Login Screen UI](../../todo/09-desktop-shell/TODO-06-security-accounts.md#6-login-screen-ui-sonnet) and [Login Flow](../../todo/09-desktop-shell/TODO-06-security-accounts.md#7-login-flow-sonnet)
- [Lock Screen](../../todo/09-desktop-shell/TODO-06-security-accounts.md#8-lock-screen-sonnet) and [User Switching](../../todo/09-desktop-shell/TODO-06-security-accounts.md#9-user-switching-sonnet)
- [File Permissions](../../todo/09-desktop-shell/TODO-06-security-accounts.md#10-file-permissions-opus)
- [UAC-Equivalent Elevation](../../todo/09-desktop-shell/TODO-06-security-accounts.md#11-uac-equivalent-elevation-opus), which may use the kernel's filtered-token elevation once [UAC and token filtering](../../todo/02-kernel-core/TODO-15-security-reference-monitor.md) ships

## How does it compare with Windows 11 and Linux?

Windows 11 has SAM and domain accounts with SIDs and full access control lists, Windows Hello and PIN sign-in, the Win+L lock screen, NTFS permissions and the UAC prompt with integrity levels. Linux uses `/etc/passwd` and `/etc/shadow` with yescrypt or SHA-512 hashes through PAM, display managers such as GDM and SDDM, `loginctl lock-session`, rwx bits plus ACLs, and `sudo` or PolicyKit for elevation. Impossible OS has the kernel token model already but only one user. The plan hashes passwords with Argon2, stores accounts in the Registry, and reuses the IXFS permission bits without changing the inode format.

## See also

- [Security and User Accounts roadmap](../../todo/09-desktop-shell/TODO-06-security-accounts.md)
- [Security Reference Monitor](../kernel/security-reference-monitor.md)
- [Early Entropy and Random Seed](../boot/early-entropy-random-seed.md)
- [Kernel Libraries](../kernel/kernel-libraries.md)
- [IXFS Core](../storage/ixfs-core.md)
- [CNG Crypto and Certificate Store](cng-crypto.md)
