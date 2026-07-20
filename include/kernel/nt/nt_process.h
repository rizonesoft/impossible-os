/* ============================================================================
 * nt_process.h -- NT process and thread lifecycle SSDT handlers
 *
 * NtCreateProcess, NtCreateThread, NtTerminateThread, NtSuspend/Resume,
 * NtQuery/SetInformation, NtDelayExecution, and related process-thread
 * lifecycle syscalls. SSDT indices 0x0030-0x0047.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"
#include "kernel/nt/quota_syscall_info.h"  /* ProcessQuotaLimits + QUOTA_LIMITS ABI */

/* Register all process/thread SSDT handlers.
 * Call once during Phase 3, after ssdt_init(). */
void nt_process_register_ssdt(void);

/* ---- Thread information classes ----------------------------------------- */
#define ThreadBasicInformation      0
#define ThreadTimes                 1
#define ThreadPriority              2
#define ThreadBasePriority          3
#define ThreadAffinityMask          4
#define ThreadIdealProcessor        13

/* ---- Process information classes ---------------------------------------- *
 * ProcessQuotaLimits (1) and its QUOTA_LIMITS / QUOTA_LIMITS_EX wire structs
 * live together in nt/quota_syscall_info.h so the class value and its ABI
 * cannot drift apart. */
#define ProcessBasicInformation     0
#define ProcessTimes                4
#define ProcessDebugPort            7
#define ProcessDefaultHardErrorMode 12
#define ProcessPriorityClass        18
#define ProcessHandleCount          20
#define ProcessSessionInformation   24
#define ProcessWow64Information     26
#define ProcessImageFileName        27
#define ProcessSystemCallFilterPolicy 41  /* per-process SSDT filter (Win: ProcessSystemCallDisablePolicy=0x29) */
#define ProcessMitigationPolicy     52  /* per-process mitigation policy (see nt/mitigation_policy.h) */

/* ---- Thread basic information output ------------------------------------ */
typedef struct {
    int32_t  ExitStatus;
    void    *TebBaseAddress;
    uint32_t UniqueProcessId;
    uint32_t _pad0;
    uint32_t UniqueThreadId;
    uint32_t _pad1;
    uint32_t BasePriority;
    uint32_t Priority;
} THREAD_BASIC_INFORMATION;

/* ---- Process basic information output ----------------------------------- */
typedef struct {
    int32_t  ExitStatus;
    uint32_t _pad0;
    void    *PebBaseAddress;
    uint64_t AffinityMask;
    uint32_t BasePriority;
    uint32_t UniqueProcessId;
    uint32_t InheritedFromUniqueProcessId;
    uint32_t _pad1;
} PROCESS_BASIC_INFORMATION;
