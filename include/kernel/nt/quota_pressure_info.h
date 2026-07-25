/* ============================================================================
 * quota_pressure_info.h -- SYSTEM_RESOURCE_PRESSURE_INFORMATION ABI
 *
 * Read-only PSI-shaped stall telemetry, exposed through
 * NtQuerySystemInformation(SystemResourcePressureInformation). Both the kernel
 * marshaller (nt_syscall.c) and the unit tests consume this single definition so
 * the on-the-wire layout cannot drift. Impossible OS extension class value
 * 0x1003 sits above the Windows SYSTEM_INFORMATION_CLASS range (0x1000 kernel
 * config, 0x1001 NLS, 0x1002 notification).
 *
 * Version/Size lead each struct and Reserved[] trails it so future counters can
 * be added WITHOUT an ABI break: a reader checks Version/Size and treats
 * trailing bytes it does not know as reserved. DomainCount and DomainSize are
 * published explicitly so a reader can stride the row array without hardcoding
 * either -- adding a fourth stall domain must not require a new class.
 *
 * THE VALID FLAG IS THE POINT. Every one of the three seams that would feed this
 * data lives outside the quota module and is blocked on work owned elsewhere, so
 * today all three rows report VALID clear. A consumer must read a clear VALID as
 * "this resource is not measured", never as "this resource is not under
 * pressure" -- the two are opposite conclusions and only the flag distinguishes
 * them. Linux PSI has no such flag and cannot make that distinction.
 * ========================================================================== */
#ifndef KERNEL_NT_QUOTA_PRESSURE_INFO_H
#define KERNEL_NT_QUOTA_PRESSURE_INFO_H

#include "kernel/types.h"

/* SYSTEM_INFORMATION_CLASS extension for NtQuerySystemInformation. */
#define SystemResourcePressureInformation  0x1003

#define SYSTEM_RESOURCE_PRESSURE_INFORMATION_VERSION  1

/* Row count: cpu / mem / io, matching quota_stall_domain_t. Pinned here as well
 * as in the kernel header so a mismatch is a compile error in the marshaller. */
#define SYSTEM_RESOURCE_PRESSURE_DOMAIN_COUNT  3

/* Averaging windows per row, matching quota_stall_window_t (10/60/300 s). */
#define SYSTEM_RESOURCE_PRESSURE_WINDOW_COUNT  3

/* Per-row flags. */
#define SYSTEM_RESOURCE_PRESSURE_FLAG_VALID          0x1u  /* numbers are meaningful */
#define SYSTEM_RESOURCE_PRESSURE_FLAG_INSTRUMENTED   0x2u  /* a seam declared itself */
#define SYSTEM_RESOURCE_PRESSURE_FLAG_FULL_UNDEFINED 0x4u  /* Full* are 0 by contract */

/* Whole-class flags. */
#define SYSTEM_RESOURCE_PRESSURE_FLAG_ARMED          0x1u  /* aggregator has run    */
/* SystemLevel below is a MEASUREMENT, not a default. Clear means no domain has
 * reported yet, so SystemLevel reads 0 for want of anything better -- exactly the
 * "unmeasured is not calm" distinction the per-row FLAG_VALID makes one level
 * down. A reader that ignores this bit sees the previous behavior unchanged;
 * SystemLevel keeps its documented 0..3 value space either way, so no existing
 * decode breaks. (The kernel-internal composite has a fifth sentinel for this,
 * QUOTA_PRESSURE_UNKNOWN; it is deliberately NOT marshalled -- widening a live
 * info class's value space is an ABI change, adding a flag bit is not.) */
#define SYSTEM_RESOURCE_PRESSURE_FLAG_SYSTEM_LEVEL_VALID 0x2u

typedef struct _SYSTEM_RESOURCE_PRESSURE_DOMAIN {
    uint32_t Domain;        /* 0 = cpu, 1 = mem, 2 = io                       */
    uint32_t Flags;         /* SYSTEM_RESOURCE_PRESSURE_FLAG_* row bits       */
    /* Cumulative CPU-nanoseconds, NOT wall-nanoseconds: the sum over CPUs of
     * each CPU's stalled time. Two cores stalled for one second adds two
     * seconds. The AVERAGES below are the wall-clock-comparable view; the
     * totals are a conserved counter, and the two answer different questions on
     * purpose -- a per-window weighted mean cannot be accumulated without the
     * running total depending on where the sampler happened to tick. */
    uint64_t SomeTotalNs;   /* cumulative CPU-ns with at least one task stalled */
    uint64_t FullTotalNs;   /* cumulative CPU-ns fully stalled; 0 when
                             * the row carries FULL_UNDEFINED                  */
    uint16_t SomeAvg10;     /* permille, 0..1000                              */
    uint16_t SomeAvg60;
    uint16_t SomeAvg300;
    uint16_t FullAvg10;
    uint16_t FullAvg60;
    uint16_t FullAvg300;
    uint16_t Level;         /* 0 normal, 1 watch, 2 warning, 3 critical       */
    uint16_t Reserved0;     /* zeroed before copy_to_user                     */
    uint64_t Reserved[2];   /* zeroed; future per-row counters                */
} __attribute__((packed)) SYSTEM_RESOURCE_PRESSURE_DOMAIN;

_Static_assert(sizeof(SYSTEM_RESOURCE_PRESSURE_DOMAIN) == 56,
    "SYSTEM_RESOURCE_PRESSURE_DOMAIN ABI size pinned at 56 bytes");
_Static_assert(__builtin_offsetof(SYSTEM_RESOURCE_PRESSURE_DOMAIN, Flags) == 4,
    "SYSTEM_RESOURCE_PRESSURE_DOMAIN.Flags ABI offset pinned at 4");
_Static_assert(__builtin_offsetof(SYSTEM_RESOURCE_PRESSURE_DOMAIN, SomeTotalNs) == 8,
    "SYSTEM_RESOURCE_PRESSURE_DOMAIN.SomeTotalNs ABI offset pinned at 8");
_Static_assert(__builtin_offsetof(SYSTEM_RESOURCE_PRESSURE_DOMAIN, FullTotalNs) == 16,
    "SYSTEM_RESOURCE_PRESSURE_DOMAIN.FullTotalNs ABI offset pinned at 16");
/* Pin every interior average too, not just the boundaries: these six are the
 * same width, so a reorder among them would keep sizeof at 56 yet silently
 * relabel which window a reader decodes -- the same standard the sibling
 * SYSTEM_NOTIFICATION_INFORMATION counters are held to. */
_Static_assert(__builtin_offsetof(SYSTEM_RESOURCE_PRESSURE_DOMAIN, SomeAvg10) == 24,
    "SYSTEM_RESOURCE_PRESSURE_DOMAIN.SomeAvg10 ABI offset pinned at 24");
_Static_assert(__builtin_offsetof(SYSTEM_RESOURCE_PRESSURE_DOMAIN, SomeAvg60) == 26,
    "SYSTEM_RESOURCE_PRESSURE_DOMAIN.SomeAvg60 ABI offset pinned at 26");
_Static_assert(__builtin_offsetof(SYSTEM_RESOURCE_PRESSURE_DOMAIN, SomeAvg300) == 28,
    "SYSTEM_RESOURCE_PRESSURE_DOMAIN.SomeAvg300 ABI offset pinned at 28");
_Static_assert(__builtin_offsetof(SYSTEM_RESOURCE_PRESSURE_DOMAIN, FullAvg10) == 30,
    "SYSTEM_RESOURCE_PRESSURE_DOMAIN.FullAvg10 ABI offset pinned at 30");
_Static_assert(__builtin_offsetof(SYSTEM_RESOURCE_PRESSURE_DOMAIN, FullAvg60) == 32,
    "SYSTEM_RESOURCE_PRESSURE_DOMAIN.FullAvg60 ABI offset pinned at 32");
_Static_assert(__builtin_offsetof(SYSTEM_RESOURCE_PRESSURE_DOMAIN, FullAvg300) == 34,
    "SYSTEM_RESOURCE_PRESSURE_DOMAIN.FullAvg300 ABI offset pinned at 34");
_Static_assert(__builtin_offsetof(SYSTEM_RESOURCE_PRESSURE_DOMAIN, Level) == 36,
    "SYSTEM_RESOURCE_PRESSURE_DOMAIN.Level ABI offset pinned at 36");
_Static_assert(__builtin_offsetof(SYSTEM_RESOURCE_PRESSURE_DOMAIN, Reserved) == 40,
    "SYSTEM_RESOURCE_PRESSURE_DOMAIN.Reserved ABI offset pinned at 40");

typedef struct _SYSTEM_RESOURCE_PRESSURE_INFORMATION {
    uint16_t Version;           /* SYSTEM_RESOURCE_PRESSURE_INFORMATION_VERSION */
    uint16_t Size;              /* sizeof(SYSTEM_RESOURCE_PRESSURE_INFORMATION) */
    uint32_t Flags;             /* SYSTEM_RESOURCE_PRESSURE_FLAG_ARMED          */
    uint32_t DomainCount;       /* SYSTEM_RESOURCE_PRESSURE_DOMAIN_COUNT        */
    uint32_t DomainSize;        /* sizeof(SYSTEM_RESOURCE_PRESSURE_DOMAIN)      */
    uint64_t UpdateIntervalNs;  /* aggregation cadence                          */
    uint64_t LastUpdateNs;      /* uptime of the most recent closed window      */
    uint64_t WindowsClosed;     /* windows aggregated since boot                */
    uint32_t SystemLevel;       /* worst level across VALID rows; 0 and
                                 * meaningless unless FLAG_SYSTEM_LEVEL_VALID   */
    uint32_t Reserved0;         /* zeroed before copy_to_user                   */
    SYSTEM_RESOURCE_PRESSURE_DOMAIN Domains[SYSTEM_RESOURCE_PRESSURE_DOMAIN_COUNT];
    uint64_t Reserved[4];       /* zeroed; future whole-class counters          */
} __attribute__((packed)) SYSTEM_RESOURCE_PRESSURE_INFORMATION;

_Static_assert(sizeof(SYSTEM_RESOURCE_PRESSURE_INFORMATION) == 248,
    "SYSTEM_RESOURCE_PRESSURE_INFORMATION ABI size pinned at 248 bytes");
_Static_assert(__builtin_offsetof(SYSTEM_RESOURCE_PRESSURE_INFORMATION, Flags) == 4,
    "SYSTEM_RESOURCE_PRESSURE_INFORMATION.Flags ABI offset pinned at 4");
_Static_assert(__builtin_offsetof(SYSTEM_RESOURCE_PRESSURE_INFORMATION, DomainCount) == 8,
    "SYSTEM_RESOURCE_PRESSURE_INFORMATION.DomainCount ABI offset pinned at 8");
_Static_assert(__builtin_offsetof(SYSTEM_RESOURCE_PRESSURE_INFORMATION, DomainSize) == 12,
    "SYSTEM_RESOURCE_PRESSURE_INFORMATION.DomainSize ABI offset pinned at 12");
_Static_assert(__builtin_offsetof(SYSTEM_RESOURCE_PRESSURE_INFORMATION, UpdateIntervalNs) == 16,
    "SYSTEM_RESOURCE_PRESSURE_INFORMATION.UpdateIntervalNs ABI offset pinned at 16");
_Static_assert(__builtin_offsetof(SYSTEM_RESOURCE_PRESSURE_INFORMATION, LastUpdateNs) == 24,
    "SYSTEM_RESOURCE_PRESSURE_INFORMATION.LastUpdateNs ABI offset pinned at 24");
_Static_assert(__builtin_offsetof(SYSTEM_RESOURCE_PRESSURE_INFORMATION, WindowsClosed) == 32,
    "SYSTEM_RESOURCE_PRESSURE_INFORMATION.WindowsClosed ABI offset pinned at 32");
_Static_assert(__builtin_offsetof(SYSTEM_RESOURCE_PRESSURE_INFORMATION, SystemLevel) == 40,
    "SYSTEM_RESOURCE_PRESSURE_INFORMATION.SystemLevel ABI offset pinned at 40");
_Static_assert(__builtin_offsetof(SYSTEM_RESOURCE_PRESSURE_INFORMATION, Domains) == 48,
    "SYSTEM_RESOURCE_PRESSURE_INFORMATION.Domains ABI offset pinned at 48");
_Static_assert(__builtin_offsetof(SYSTEM_RESOURCE_PRESSURE_INFORMATION, Reserved) == 216,
    "SYSTEM_RESOURCE_PRESSURE_INFORMATION.Reserved ABI offset pinned at 216");

/* The row stride a reader computes from DomainSize must be the one the compiler
 * laid out; if these ever disagree, a reader walking the array by DomainSize
 * would decode garbage from row 1 onward. */
_Static_assert(sizeof(((SYSTEM_RESOURCE_PRESSURE_INFORMATION *)0)->Domains) ==
               (uint64_t)SYSTEM_RESOURCE_PRESSURE_DOMAIN_COUNT *
               sizeof(SYSTEM_RESOURCE_PRESSURE_DOMAIN),
    "row array must be exactly DomainCount strides of DomainSize");

#endif /* KERNEL_NT_QUOTA_PRESSURE_INFO_H */
