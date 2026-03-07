# 71 — Disk Encryption

## Overview
Encrypt partitions or individual files to protect data. Like BitLocker (simplified).

## Approach
- Use **monocypher** (already researched in `31_general_utilities.md`) for AES-256 + ChaCha20
- Full-disk encryption (FDE): encrypts entire partition, decrypts at boot with password
- File-level encryption: right-click → Encrypt, enter password

## API
```c
int crypto_encrypt_file(const char *path, const char *password);
int crypto_decrypt_file(const char *path, const char *password);
```

## FDE architecture: encryption layer between VFS and disk driver
## Codex: `System\Security\EncryptionEnabled`, never store passwords

## Files: `src/kernel/crypto.c` (~300 lines) | 1-2 weeks
## Priority: 🔴 Long-term
