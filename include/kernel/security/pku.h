/* ============================================================================
 * pku.h -- Protection Keys for User-mode (PKU)
 *
 * Intel PKU assigns a 4-bit key (0-15) to each page via PTE bits 62:59.
 * The 32-bit PKRU register controls access per key: 2 bits per key,
 * bit 0 = access-disable (AD), bit 1 = write-disable (WD).
 *
 * Key 0 is the default: all pages start with key 0 and PKRU always
 * allows full access to key 0. Keys 1-15 are disabled by default
 * (PKRU initial value 0x55555554).
 *
 * PKU only applies to user-mode (CPL=3) data accesses when CR4.PKE=1.
 * Kernel-mode accesses are unaffected (PKS is a separate feature).
 *
 * WRPKRU is a ring-0/3 instruction (~1 ns); user-mode code can switch
 * protection zones without a syscall, unlike mprotect (~1 us).
 *
 * XREF: 02-kernel-core/TODO-09-x86-64-architecture.md
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* ---- PKRU permission flags ---- */

#define PKU_ACCESS_DISABLE  0x1   /* AD: prevent all access (read + write) */
#define PKU_WRITE_DISABLE   0x2   /* WD: prevent writes, allow reads */

/* ---- PKRU initial value ----
 *
 * Key 0 (bits 1:0) = 0b00  -> full access (default for all pages)
 * Keys 1-15 (bits 3:2 .. 31:30) = 0b01 each -> access disabled
 *
 * This prevents user-mode from accessing pages tagged with keys 1-15
 * until the task explicitly enables a key via pku_set_permissions().
 *
 * 0x55555554 = 01 01 01 01 01 01 01 01 01 01 01 01 01 01 01 00 (binary) */
#define PKU_INITIAL_PKRU    0x55555554u

/* Maximum usable protection keys (0 is reserved as default) */
#define PKU_KEY_MAX         15
#define PKU_KEY_COUNT       16   /* 0 through 15 */

/* ---- Kernel API ---- */

/* Allocate a protection key (1-15). Returns the key number, or -1 if
 * all keys are in use. Thread-safe (uses spinlock). */
int pku_alloc_key(void);

/* Free a previously allocated protection key. No-op for key 0 or
 * out-of-range keys. Thread-safe. */
void pku_free_key(int key);

/* Set PKRU permissions for a specific key on the CURRENT CPU.
 * flags: PKU_ACCESS_DISABLE, PKU_WRITE_DISABLE, or 0 (full access).
 * Uses WRPKRU instruction (no syscall overhead). */
void pku_set_permissions(int key, uint32_t flags);

/* Read current PKRU value from the CPU. */
uint32_t pku_read(void);

/* Global flag: 1 only when CR4.PKE is set on EVERY online CPU.
 * Gates all RDPKRU/WRPKRU operations -- both instructions #GP when CR4.PKE is
 * clear on the executing CPU, and PKU is AP-optional, so the flag tracks the
 * ONLINE-CPU INTERSECTION rather than any single CPU (TODO-09 S11).
 * cpu_has(CPU_FEATURE_PKU) alone is not sufficient because CR4.PKE may have
 * been skipped (e.g., XCR0 bit 9 not available on that CPU).
 * Read it with an acquire load; only cpu_enable_pku() (BSP publication) and
 * cpu_features_finalize_global() (authoritative narrowing) write it.
 *
 * SCOPE OF THE GUARANTEE: the intersection is computed over the per-CPU
 * cr4_at_boot snapshots, so it is exactly as trustworthy as single-writer
 * ownership of those slots. A misidentified AP can still write a reassigned
 * slot before the LAPIC-identity park (src/kernel/smp/smp.c:160 vs :176), which
 * would let an impostor's CR4 stand in for the slot's real AP. That window is
 * pre-existing and shared with the S6 feature mask; closing it is the AP_DATA
 * consume-ack handshake owned by 01-boot-platform/TODO-09 S10. */
extern int pku_enabled;

/* Initialize PKU subsystem. Call after cpu_configure_xcr0(). */
void pku_init(void);
