/* ============================================================================
 * kusd.h -- User-mode view of KUSER_SHARED_DATA
 *
 * The kernel maps a physical page at the fixed VA 0x7FFE0000 as user
 * read-only. Every user process sees the same page; the kernel updates
 * time/tick fields from the LAPIC tick ISR. Programs that need low-latency
 * time-of-day / tick / cookie can dereference offsets directly instead of
 * paying a ring transition on every read.
 *
 * Only the fields the user-mode fast-path probe + user libc consume are
 * modeled here. The full struct lives in include/kernel/nt/kusd.h and is
 * kernel-owned; anything past this minimal view is reserved padding from
 * the user's perspective.
 *
 * Layout rule: any `_Static_assert(__builtin_offsetof(USER_KUSD, ...) ==
 * <offset>)` below MUST agree with the matching assert in the kernel
 * header. A silent kernel-side KUSD layout bump shifts later fields and
 * breaks every fast-path reader; the static assert pair fails compilation
 * on BOTH sides before the drift can leak to runtime.
 * ============================================================================ */

#pragma once

#include "types.h"

/* KSYSTEM_TIME (12 bytes): Windows shared time representation used for
 * SystemTime + TickCount + InterruptTime fields.
 *   LowPart   = uint32_t  (0x00)
 *   High1Time = int32_t   (0x04)
 *   High2Time = int32_t   (0x08)   // updated-last; read twice for tear check
 * User code that needs a torn-read-free value reads High1Time, then
 * LowPart, then High2Time and retries while High1Time != High2Time.
 * For the fastpath probe we read only LowPart (low 32 bits) which is
 * always monotonically non-decreasing in the common case.
 */
typedef struct user_ksystem_time {
    uint32_t LowPart;
    int32_t  High1Time;
    int32_t  High2Time;
} USER_KSYSTEM_TIME;

/* Fixed user-mode VA (Windows standard since NT 3.51). The kernel maps
 * the shared page here user-RO; deref is direct. Kept as a macro so
 * inline asm and C code share a single source of truth. */
#define USER_KUSD_VA  0x7FFE0000ULL

/* Minimal user-visible KUSER_SHARED_DATA subset -- only the fields the
 * fast-path probe + user libc consume. Padded up to offset 0x334 which
 * is past Cookie; anything beyond that is reserved from user code's
 * perspective. If a new user-mode fast-path consumer lands, add the
 * field here with a matching _Static_assert so drift becomes a compile
 * error at the user-side callsite. */
typedef struct user_kusd {
    /* 0x0000 */ uint32_t          TickCountLowDeprecated;
    /* 0x0004 */ uint32_t          TickCountMultiplier;
    /* 0x0008 */ USER_KSYSTEM_TIME InterruptTime;
    /* 0x0014 */ USER_KSYSTEM_TIME SystemTime;
    /* 0x0020 */ uint8_t           _pad_0020[0x320 - 0x020];
    /* 0x0320 */ USER_KSYSTEM_TIME TickCount;
    /* 0x032C */ uint8_t           _pad_032C[0x330 - 0x32C];
    /* 0x0330 */ uint32_t          Cookie;
} USER_KUSD;

_Static_assert(__builtin_offsetof(USER_KUSD, TickCountLowDeprecated) == 0x000,
               "USER_KUSD.TickCountLowDeprecated must match kernel KUSD at 0x000 "
               "(load-bearing for the fast-path probe + low-latency GetTickCount)");
_Static_assert(__builtin_offsetof(USER_KUSD, TickCountMultiplier) == 0x004,
               "USER_KUSD.TickCountMultiplier must match kernel KUSD at 0x004");
_Static_assert(__builtin_offsetof(USER_KUSD, InterruptTime) == 0x008,
               "USER_KUSD.InterruptTime must match kernel KUSD at 0x008");
_Static_assert(__builtin_offsetof(USER_KUSD, SystemTime) == 0x014,
               "USER_KUSD.SystemTime must match kernel KUSD at 0x014");
_Static_assert(__builtin_offsetof(USER_KUSD, TickCount) == 0x320,
               "USER_KUSD.TickCount must match kernel KUSD at 0x320");
_Static_assert(__builtin_offsetof(USER_KUSD, Cookie) == 0x330,
               "USER_KUSD.Cookie must match kernel KUSD at 0x330 "
               "(load-bearing for stack-cookie seed derivation)");
