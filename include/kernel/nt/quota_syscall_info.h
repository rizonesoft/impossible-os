/* quota_syscall_info.h -- QUOTA_LIMITS ABI for ProcessQuotaLimits
 *
 * The wire contract behind NtQueryInformationProcess / NtSetInformationProcess
 * information class ProcessQuotaLimits (class 1, the Windows value). There is
 * deliberately NO dedicated NtQueryQuotaInformationProcess / NtSetQuota-
 * InformationProcess pair: Windows exposes process quotas through the two
 * general process-information syscalls, and a second entry point onto the same
 * mutable state would carry its own probe rules, length rules, privilege check
 * and transaction, so a caller could pick whichever surface is weaker. One
 * external surface, one authorization path.
 *
 * ABI: the structures below are byte-for-byte the Win32 `QUOTA_LIMITS` and
 * `QUOTA_LIMITS_EX` (winnt.h) on x86-64, where SIZE_T is 8 bytes. Every field
 * offset and both sizes are pinned by _Static_assert -- a silent layout skew
 * here mis-reads a ring-3 buffer, so the assertions are the ABI, not a comment.
 *
 * Which structure a call means is decided by the caller-supplied LENGTH, never
 * by reading a suffix field out of a short buffer:
 *   length >= QUOTA_LIMITS_EX_SIZE  -> QUOTA_LIMITS_EX (88 bytes)
 *   length >= QUOTA_LIMITS_SIZE     -> QUOTA_LIMITS    (48 bytes)
 *   anything shorter                -> STATUS_BUFFER_TOO_SMALL
 *
 * ZERO SEMANTICS (this kernel's contract, stated because Windows leaves it
 * loose): a limit field of 0 means UNLIMITED on BOTH the query and the set
 * path, matching QUOTA_LIMIT_UNLIMITED in quota/quota.h so one value means one
 * thing across the whole kernel. A set therefore applies EVERY field it carries
 * -- there is no "leave this one alone" encoding -- and because 0 removes a
 * cap, writing it is a RAISE and needs SeIncreaseQuotaPrivilege like any other.
 * CARRIES is the operative word: a 48-byte request carries no suffix at all, so
 * the EX-only fields are preserved unchanged rather than zeroed. A legacy
 * caller cannot destroy a policy it has no way to express.
 *
 * MinimumWorkingSetSize inverts the direction rule: it is a FLOOR, so 0 means
 * "no floor" and RAISING it (which reserves more resident memory) is the
 * privileged move, not lowering it.
 *
 * Owner: TODO-25-kernel-resource-accounting-quotas.md section 8. Projection + transaction live in
 * include/kernel/quota/quota_policy.h; the syscall marshalling lives in
 * src/kernel/nt/nt_process.c.
 */
#ifndef _KERNEL_NT_QUOTA_SYSCALL_INFO_H
#define _KERNEL_NT_QUOTA_SYSCALL_INFO_H

#include "kernel/types.h"

/* ProcessQuotaLimits is PROCESSINFOCLASS value 1 on Windows. Defined here
 * rather than in nt_process.h so the class constant and its wire structures
 * cannot drift apart; nt_process.h includes this header. */
#define ProcessQuotaLimits          1

/* --- RATE_QUOTA_LIMIT ----------------------------------------------------- *
 * A 32-bit word: the whole word as RateData, or the CPU rate as a 7-bit
 * percentage in the low bits. Modelled as a plain uint32_t plus accessor
 * macros rather than a C bitfield -- bitfield allocation order is
 * implementation-defined, and this word crosses the ring boundary. Same 4-byte
 * ABI either way. */
typedef uint32_t RATE_QUOTA_LIMIT;

#define RATE_QUOTA_PERCENT_MASK     0x0000007FU  /* RatePercent : 7  */
#define RATE_QUOTA_RESERVED_MASK    0xFFFFFF80U  /* Reserved0   : 25 */
#define RATE_QUOTA_PERCENT(rate)    ((rate) & RATE_QUOTA_PERCENT_MASK)
#define RATE_QUOTA_PERCENT_MAX      100U

/* --- QUOTA_LIMITS flags (winnt.h) ----------------------------------------- */
#define QUOTA_LIMITS_HARDWS_MIN_ENABLE   0x00000001U
#define QUOTA_LIMITS_HARDWS_MIN_DISABLE  0x00000002U
#define QUOTA_LIMITS_HARDWS_MAX_ENABLE   0x00000004U
#define QUOTA_LIMITS_HARDWS_MAX_DISABLE  0x00000008U
#define QUOTA_LIMITS_USE_DEFAULT_LIMITS  0x00000010U

/* Every flag bit this kernel accepts. Fail-closed: an unknown bit is rejected
 * rather than stored, so a future flag cannot be silently swallowed by an old
 * kernel and read back as if it had been honoured. */
#define QUOTA_LIMITS_SUPPORTED_FLAGS \
    (QUOTA_LIMITS_HARDWS_MIN_ENABLE  | QUOTA_LIMITS_HARDWS_MIN_DISABLE | \
     QUOTA_LIMITS_HARDWS_MAX_ENABLE  | QUOTA_LIMITS_HARDWS_MAX_DISABLE | \
     QUOTA_LIMITS_USE_DEFAULT_LIMITS)

/* The two ENABLE bits turn a working-set bound from advisory into enforced,
 * and because the bounds point in opposite directions so do their privilege
 * rules: DROPPING _MAX_ENABLE loosens a ceiling, while ADDING _MIN_ENABLE pins
 * a reservation. Each is the privileged move in its own direction -- there is
 * deliberately no combined "enforcing" mask, because one would invite treating
 * them symmetrically. */

/* The mutually exclusive ENABLE/DISABLE pairs: setting both halves of a pair
 * is a contradiction, not a preference. */
#define QUOTA_LIMITS_HARDWS_MIN_PAIR \
    (QUOTA_LIMITS_HARDWS_MIN_ENABLE | QUOTA_LIMITS_HARDWS_MIN_DISABLE)
#define QUOTA_LIMITS_HARDWS_MAX_PAIR \
    (QUOTA_LIMITS_HARDWS_MAX_ENABLE | QUOTA_LIMITS_HARDWS_MAX_DISABLE)

/* --- QUOTA_LIMITS (48 bytes) ---------------------------------------------- *
 * TimeLimit is a signed LARGE_INTEGER in 100-ns units, projected from
 * RLIMIT_CPU seconds. QUOTA_TIME_LIMIT_NONE is the "no CPU time limit"
 * encoding, matching RLIM_INFINITY on the rlimit side. */
typedef struct _QUOTA_LIMITS {
    uint64_t PagedPoolLimit;         /* bytes; 0 = unlimited        */
    uint64_t NonPagedPoolLimit;      /* bytes; 0 = unlimited        */
    uint64_t MinimumWorkingSetSize;  /* bytes; 0 = unlimited        */
    uint64_t MaximumWorkingSetSize;  /* bytes; 0 = unlimited        */
    uint64_t PagefileLimit;          /* bytes; 0 = unlimited        */
    int64_t  TimeLimit;              /* 100-ns units; 0 = unlimited */
} QUOTA_LIMITS;

#define QUOTA_TIME_LIMIT_NONE       0
/* The smallest representable REAL limit (one 100-ns tick). A zero-second
 * RLIMIT_CPU is projected onto this rather than onto QUOTA_TIME_LIMIT_NONE:
 * the two encodings mean opposite things, and reporting the tightest possible
 * cap as "unlimited" is the dangerous direction of that collision. */
#define QUOTA_TIME_LIMIT_MIN        1

/* --- QUOTA_LIMITS_EX (88 bytes) ------------------------------------------- *
 * The Windows 11 extended form. Reserved2..Reserved4 are written as zero and
 * must be supplied as zero: they are reserved by the ABI, and accepting junk
 * there would make a future field impossible to introduce compatibly. */
typedef struct _QUOTA_LIMITS_EX {
    uint64_t         PagedPoolLimit;
    uint64_t         NonPagedPoolLimit;
    uint64_t         MinimumWorkingSetSize;
    uint64_t         MaximumWorkingSetSize;
    uint64_t         PagefileLimit;
    int64_t          TimeLimit;
    uint64_t         WorkingSetLimit;
    uint64_t         Reserved2;
    uint64_t         Reserved3;
    uint64_t         Reserved4;
    uint32_t         Flags;
    RATE_QUOTA_LIMIT CpuRateLimit;
} QUOTA_LIMITS_EX;

#define QUOTA_LIMITS_SIZE           48U
#define QUOTA_LIMITS_EX_SIZE        88U

/* Layer 1 of the 5-layer defense: size + EVERY field offset. These pin the
 * ring-3 wire format; quota_policy_abi_verify() is the runtime layer and
 * test_quota_syscall.c is the test layer. */
_Static_assert(sizeof(QUOTA_LIMITS) == QUOTA_LIMITS_SIZE,
    "QUOTA_LIMITS must be 48 bytes on x86-64 (Win32 ABI)");
_Static_assert(__builtin_offsetof(QUOTA_LIMITS, PagedPoolLimit) == 0,
    "QUOTA_LIMITS.PagedPoolLimit offset");
_Static_assert(__builtin_offsetof(QUOTA_LIMITS, NonPagedPoolLimit) == 8,
    "QUOTA_LIMITS.NonPagedPoolLimit offset");
_Static_assert(__builtin_offsetof(QUOTA_LIMITS, MinimumWorkingSetSize) == 16,
    "QUOTA_LIMITS.MinimumWorkingSetSize offset");
_Static_assert(__builtin_offsetof(QUOTA_LIMITS, MaximumWorkingSetSize) == 24,
    "QUOTA_LIMITS.MaximumWorkingSetSize offset");
_Static_assert(__builtin_offsetof(QUOTA_LIMITS, PagefileLimit) == 32,
    "QUOTA_LIMITS.PagefileLimit offset");
_Static_assert(__builtin_offsetof(QUOTA_LIMITS, TimeLimit) == 40,
    "QUOTA_LIMITS.TimeLimit offset");

_Static_assert(sizeof(QUOTA_LIMITS_EX) == QUOTA_LIMITS_EX_SIZE,
    "QUOTA_LIMITS_EX must be 88 bytes on x86-64 (Win32 ABI)");
_Static_assert(__builtin_offsetof(QUOTA_LIMITS_EX, PagedPoolLimit) == 0,
    "QUOTA_LIMITS_EX.PagedPoolLimit offset");
_Static_assert(__builtin_offsetof(QUOTA_LIMITS_EX, NonPagedPoolLimit) == 8,
    "QUOTA_LIMITS_EX.NonPagedPoolLimit offset");
_Static_assert(__builtin_offsetof(QUOTA_LIMITS_EX, MinimumWorkingSetSize) == 16,
    "QUOTA_LIMITS_EX.MinimumWorkingSetSize offset");
_Static_assert(__builtin_offsetof(QUOTA_LIMITS_EX, MaximumWorkingSetSize) == 24,
    "QUOTA_LIMITS_EX.MaximumWorkingSetSize offset");
_Static_assert(__builtin_offsetof(QUOTA_LIMITS_EX, PagefileLimit) == 32,
    "QUOTA_LIMITS_EX.PagefileLimit offset");
_Static_assert(__builtin_offsetof(QUOTA_LIMITS_EX, TimeLimit) == 40,
    "QUOTA_LIMITS_EX.TimeLimit offset");
_Static_assert(__builtin_offsetof(QUOTA_LIMITS_EX, WorkingSetLimit) == 48,
    "QUOTA_LIMITS_EX.WorkingSetLimit offset");
_Static_assert(__builtin_offsetof(QUOTA_LIMITS_EX, Reserved2) == 56,
    "QUOTA_LIMITS_EX.Reserved2 offset");
_Static_assert(__builtin_offsetof(QUOTA_LIMITS_EX, Reserved3) == 64,
    "QUOTA_LIMITS_EX.Reserved3 offset");
_Static_assert(__builtin_offsetof(QUOTA_LIMITS_EX, Reserved4) == 72,
    "QUOTA_LIMITS_EX.Reserved4 offset");
_Static_assert(__builtin_offsetof(QUOTA_LIMITS_EX, Flags) == 80,
    "QUOTA_LIMITS_EX.Flags offset");
_Static_assert(__builtin_offsetof(QUOTA_LIMITS_EX, CpuRateLimit) == 84,
    "QUOTA_LIMITS_EX.CpuRateLimit offset");

/* The EX form is a strict prefix-extension of the base form: the first 48
 * bytes must be interchangeable, or size-based dispatch would hand a QUOTA_-
 * LIMITS caller a differently-laid-out record. */
_Static_assert(__builtin_offsetof(QUOTA_LIMITS_EX, TimeLimit) ==
               __builtin_offsetof(QUOTA_LIMITS, TimeLimit),
    "QUOTA_LIMITS_EX must extend QUOTA_LIMITS, not re-lay it out");

#endif /* _KERNEL_NT_QUOTA_SYSCALL_INFO_H */
