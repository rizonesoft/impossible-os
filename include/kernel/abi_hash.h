/* ============================================================================
 * abi_hash.h -- Kernel ABI fingerprint (GENERATED)
 *
 * Regenerate via: python3 scripts/gen-user-abi.py
 * Verified via:   make check-abi
 *
 * DO NOT EDIT BY HAND. Derived from the same sources as
 * user/include/abi_numbers.h -- the kernel-side copy is needed so
 * SYS_ABI_HANDSHAKE can return the same 64-bit hash the user libc
 * has compiled in. A drift between the two headers is a build error,
 * not a runtime surprise.
 * ============================================================================ */

#pragma once

#define IMPOSSIBLE_OS_ABI_HASH   0x9B8D6CD1D01DB251ULL
