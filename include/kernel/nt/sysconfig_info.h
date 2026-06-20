/* ============================================================================
 * sysconfig_info.h -- SYSTEM_KERNEL_CONFIG_INFORMATION ABI
 *
 * Shared contract for the read-only kernel configuration query exposed through
 * NtQuerySystemInformation(SystemKernelConfigInformation). Both the kernel
 * marshaller (nt_syscall.c) and the unit tests consume this single definition
 * so the on-the-wire layout cannot drift. Impossible OS extension class value
 * 0x1000 sits above the Windows SYSTEM_INFORMATION_CLASS range.
 *
 * The configuration SET path is owned by NtSetSystemInformation and gated on
 * the security reference monitor's SeSinglePrivilegeCheck + per-token lock.
 * ============================================================================ */
#ifndef KERNEL_NT_SYSCONFIG_INFO_H
#define KERNEL_NT_SYSCONFIG_INFO_H

#include "kernel/types.h"

/* SYSTEM_INFORMATION_CLASS extension for NtQuerySystemInformation. */
#define SystemKernelConfigInformation  0x1000

/* Read-only summary of the immutable kernel_config_t snapshot plus the runtime
 * tunable / feature counts and the policy lock phase. */
typedef struct _SYSTEM_KERNEL_CONFIG_INFORMATION {
    uint16_t Version;          /* kernel_config_t.version */
    uint16_t Size;             /* kernel_config_t.size */
    uint8_t  BootMode;         /* 0=normal, 1=safe, 2=recovery */
    uint8_t  SafeMode;         /* safe_mode_t */
    uint8_t  SafeModeReason;   /* safe_mode_reason_t */
    uint8_t  DebugEnabled;
    uint8_t  TestMode;
    uint8_t  LockPhase;        /* tunable_phase_t */
    uint8_t  LockdownLevel;    /* kernel_lockdown_level_t (policy_lock.c) */
    uint8_t  Reserved[1];      /* zeroed before copy_to_user */
    uint32_t BootReason;       /* boot_reason_code */
    uint32_t SelectionReason;  /* boot_selection_reason */
    uint32_t TunableCount;
    uint32_t FeatureCount;
} __attribute__((packed)) SYSTEM_KERNEL_CONFIG_INFORMATION;

_Static_assert(sizeof(SYSTEM_KERNEL_CONFIG_INFORMATION) == 28,
    "SYSTEM_KERNEL_CONFIG_INFORMATION ABI size pinned at 28 bytes");
_Static_assert(__builtin_offsetof(SYSTEM_KERNEL_CONFIG_INFORMATION, LockdownLevel) == 10,
    "SYSTEM_KERNEL_CONFIG_INFORMATION.LockdownLevel ABI offset pinned at 10");
_Static_assert(__builtin_offsetof(SYSTEM_KERNEL_CONFIG_INFORMATION, BootReason) == 12,
    "SYSTEM_KERNEL_CONFIG_INFORMATION.BootReason ABI offset pinned at 12");
_Static_assert(__builtin_offsetof(SYSTEM_KERNEL_CONFIG_INFORMATION, TunableCount) == 20,
    "SYSTEM_KERNEL_CONFIG_INFORMATION.TunableCount ABI offset pinned at 20");

#endif /* KERNEL_NT_SYSCONFIG_INFO_H */
