/* ============================================================================
 * pku.c -- Protection Keys for User-mode (PKU) kernel API
 *
 * Manages the 16 protection keys (0-15) available via Intel PKU.
 * Key 0 is always reserved as the default (full access).
 * Keys 1-15 are allocated/freed by pku_alloc_key()/pku_free_key().
 *
 * WRPKRU/RDPKRU are ring-0/3 instructions; pku_set_permissions()
 * works from both kernel and user mode without a syscall.
 *
 * XREF: 02-kernel-core/TODO-09-x86-64-architecture.md
 * ============================================================================ */

#include "kernel/security/pku.h"
#include "kernel/cpuid.h"
#include "kernel/sched/spinlock.h"
#include "kernel/klog.h"

/* Global flag: 1 only when CR4.PKE is set on EVERY online CPU.
 * Gates all RDPKRU/WRPKRU operations -- both instructions #GP when CR4.PKE is
 * clear on the executing CPU, so a flag set by whichever CPU happened to enable
 * PKU would fault a thread scheduled onto a PKU-less AP (TODO-09 S11).
 * Published by cpu_enable_pku() on the BSP and narrowed to the online-CPU
 * intersection by cpu_features_finalize_global(); both writers release-store,
 * so every read here is an acquire load. */
int pku_enabled = 0;

/* Bitmap of allocated keys: bit N = 1 means key N is in use.
 * Key 0 is always reserved (set at init). */
static uint16_t s_key_bitmap;
static DEFINE_SPINLOCK(s_key_lock);

void pku_init(void)
{
    s_key_bitmap = 1;   /* key 0 is reserved (default, always full access) */
    klog(LOG_INFO, "pku", "PKU key allocator initialized (15 keys available)");
}

int pku_alloc_key(void)
{
    uint64_t irq_flags;
    int key;

    if (!__atomic_load_n(&pku_enabled, __ATOMIC_ACQUIRE))
        return -1;

    spin_lock_irqsave(&s_key_lock, &irq_flags);

    /* Find first free key (skip key 0) */
    for (key = 1; key < PKU_KEY_COUNT; key++) {
        if (!(s_key_bitmap & (1u << key))) {
            s_key_bitmap |= (1u << key);
            spin_unlock_irqrestore(&s_key_lock, irq_flags);
            return key;
        }
    }

    spin_unlock_irqrestore(&s_key_lock, irq_flags);
    return -1;  /* all keys in use */
}

void pku_free_key(int key)
{
    uint64_t irq_flags;

    if (key < 1 || key > PKU_KEY_MAX)
        return;

    /* Revoke access before releasing the key to prevent stale PKRU
     * state from granting access after key reuse. */
    pku_set_permissions(key, PKU_ACCESS_DISABLE);

    spin_lock_irqsave(&s_key_lock, &irq_flags);
    s_key_bitmap &= ~(1u << key);
    spin_unlock_irqrestore(&s_key_lock, irq_flags);
}

void pku_set_permissions(int key, uint32_t flags)
{
    uint32_t pkru;

    if (key < 0 || key >= PKU_KEY_COUNT)
        return;
    if (!__atomic_load_n(&pku_enabled, __ATOMIC_ACQUIRE))
        return;

    /* Read current PKRU, modify the 2-bit field for this key, write back.
     * RDPKRU: EAX = PKRU, ECX must be 0.
     * WRPKRU: EAX = new PKRU, ECX = 0, EDX = 0. */
    pkru = pku_read();

    /* Clear the 2-bit field for this key, then set the new flags */
    pkru &= ~(3u << (key * 2));
    pkru |= (flags & 3u) << (key * 2);

    __asm__ volatile (
        "wrpkru"
        :
        : "a"(pkru), "c"((uint32_t)0), "d"((uint32_t)0)
        : "memory"
    );
}

uint32_t pku_read(void)
{
    uint32_t pkru;

    if (!__atomic_load_n(&pku_enabled, __ATOMIC_ACQUIRE))
        return 0;

    __asm__ volatile (
        "rdpkru"
        : "=a"(pkru)
        : "c"((uint32_t)0)
        : "edx"
    );
    return pkru;
}
