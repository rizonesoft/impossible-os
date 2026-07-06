/* ============================================================================
 * knf_syscall_info.h -- SYSTEM_NOTIFICATION_INFORMATION ABI
 *
 * Read-only diagnostics for the Kernel Notification Facility, exposed through
 * NtQuerySystemInformation(SystemNotificationInformation). Both the kernel
 * marshaller (nt_syscall.c) and the unit tests consume this single definition
 * so the on-the-wire layout cannot drift. Impossible OS extension class value
 * 0x1002 sits above the Windows SYSTEM_INFORMATION_CLASS range (0x1000 kernel
 * config, 0x1001 NLS).
 *
 * Version/Size lead the struct and Reserved[] trails it so future counters can
 * be added WITHOUT an ABI break: a reader checks Version/Size and treats trailing
 * bytes it does not know as reserved. All cumulative counters are uint64 (KNF
 * sequences and per-subscriber missed counts are already uint64, so a hot state
 * would wrap a 32-bit tally).
 * ============================================================================ */
#ifndef KERNEL_NT_KNF_SYSCALL_INFO_H
#define KERNEL_NT_KNF_SYSCALL_INFO_H

#include "kernel/types.h"

/* SYSTEM_INFORMATION_CLASS extension for NtQuerySystemInformation. */
#define SystemNotificationInformation  0x1002

#define SYSTEM_NOTIFICATION_INFORMATION_VERSION  1

/* Flags: KNF init health (from the subsystem-readiness oracle). Exactly one of
 * READY / UNAVAILABLE is set; DEGRADED is an additional qualifier on READY.
 * OK = READY; DEGRADED = READY|DEGRADED; FATAL = UNAVAILABLE. */
#define SYSTEM_NOTIFICATION_FLAG_READY        0x1u  /* namespace + type initialized (usable) */
#define SYSTEM_NOTIFICATION_FLAG_DEGRADED     0x2u  /* usable but partial (missing category) */
#define SYSTEM_NOTIFICATION_FLAG_UNAVAILABLE  0x4u  /* fatal init: no type/root, cannot create states */

typedef struct _SYSTEM_NOTIFICATION_INFORMATION {
    uint16_t Version;          /* SYSTEM_NOTIFICATION_INFORMATION_VERSION */
    uint16_t Size;             /* sizeof(SYSTEM_NOTIFICATION_INFORMATION) */
    uint32_t Flags;            /* SYSTEM_NOTIFICATION_FLAG_* readiness bits */
    uint64_t LiveStateCount;   /* live (referenced) KNF states -- inc on fresh
                                * create, dec when the body is destroyed; counts
                                * still-pinned states that outlived namespace delete */
    uint64_t SubscriberCount;  /* live in-kernel subscribers across all states */
    uint64_t PublishCount;     /* cumulative successful knf_publish calls */
    uint64_t CoalescedCount;   /* cumulative coalesced (missed) updates */
    uint64_t SecurityDenials;  /* cumulative create/publish denials (SeAccessCheck
                                * policy denials add here when that engine lands) */
    uint64_t DropsAtDispatch;  /* cumulative trace publishes dropped at >= DISPATCH */
    uint64_t TraceGuardSkips;  /* cumulative trace publishes skipped by the recursion guard */
    uint64_t Reserved[4];      /* zeroed before copy_to_user; future counters */
} __attribute__((packed)) SYSTEM_NOTIFICATION_INFORMATION;

_Static_assert(sizeof(SYSTEM_NOTIFICATION_INFORMATION) == 96,
    "SYSTEM_NOTIFICATION_INFORMATION ABI size pinned at 96 bytes");
_Static_assert(__builtin_offsetof(SYSTEM_NOTIFICATION_INFORMATION, Flags) == 4,
    "SYSTEM_NOTIFICATION_INFORMATION.Flags ABI offset pinned at 4");
_Static_assert(__builtin_offsetof(SYSTEM_NOTIFICATION_INFORMATION, LiveStateCount) == 8,
    "SYSTEM_NOTIFICATION_INFORMATION.LiveStateCount ABI offset pinned at 8");
_Static_assert(__builtin_offsetof(SYSTEM_NOTIFICATION_INFORMATION, PublishCount) == 24,
    "SYSTEM_NOTIFICATION_INFORMATION.PublishCount ABI offset pinned at 24");
_Static_assert(__builtin_offsetof(SYSTEM_NOTIFICATION_INFORMATION, Reserved) == 64,
    "SYSTEM_NOTIFICATION_INFORMATION.Reserved ABI offset pinned at 64");

#endif /* KERNEL_NT_KNF_SYSCALL_INFO_H */
