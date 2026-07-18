/* ============================================================================
 * zw.h -- ZwXxx Kernel-Mode Aliases for NtXxx SSDT Entry Points
 *
 * In user mode, NtXxx and ZwXxx are identical (both enter via SYSCALL).
 * In kernel mode (CPL 0), ZwXxx calls go through ssdt_dispatch() directly,
 * bypassing the user-buffer probe. This is the convention Windows' own
 * drivers and executive components use.
 *
 * Usage:
 *   - Kernel code calls ZwClose(), ZwCreateFile(), etc.
 *   - These resolve to ssdt_dispatch(SSDT_NtClose, ...) which invokes the
 *     registered handler with kernel privileges.
 *   - The handler skips ProbeForRead/ProbeForWrite when the caller is CPL 0
 *     (checked via ssdt_previous_mode()).
 *
 * Convention:
 *   - Kernel components call Zw variants (trusted pointers, no probe).
 *   - User mode calls Nt variants via SYSCALL (probed pointers).
 *   - Both resolve to the same SSDT handler implementation.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"
#include "kernel/nt/ntstatus.h"
#include "kernel/nt/ssdt.h"
#include "kernel/nt/service_numbers.h"

/* ---- Previous-mode tracking ------------------------------------------------
 *
 * ssdt_set_previous_mode() is called by the syscall entry path to indicate
 * the caller was ring 3. Kernel callers (ZwXxx) leave the mode at 0 (kernel).
 * Handlers call ssdt_previous_mode() to decide whether to probe buffers. */

/* Previous mode: 0 = KernelMode, 1 = UserMode */
#define SSDT_KERNEL_MODE   0
#define SSDT_USER_MODE     1

/* Set/get previous mode. Stored per-thread (struct thread.previous_mode),
 * resolved through thread_current(); see ssdt.c for the SMP-closure note. */
void ssdt_set_previous_mode(uint32_t mode);
uint32_t ssdt_previous_mode(void);

/* Enter/leave a user-originated system service: bracket the true ring-3 -> ring-0
 * syscall dispatch (SYSCALL fast path + INT 0x2E). One thread_current() resolution
 * sets BOTH previous_mode (SSDT probe gating) and in_system_service (so
 * ki_kernel_bugcheck_code() tells a fault inside a system service, STOP 0x3B, from
 * a pure kernel-thread fault, STOP 0x1E). Unlike previous_mode, in_system_service
 * is NOT touched by zw_dispatch, so a nested Zw within a user syscall keeps it set.
 * Same per-thread / thread_current() SMP-closure caveat as previous_mode (ssdt.c). */
void ssdt_syscall_enter(void);
void ssdt_syscall_leave(void);

/* ---- User-buffer probing ---------------------------------------------------
 *
 * ProbeForRead/ProbeForWrite validate that a user-mode buffer is entirely
 * within the user address range. Called by NtXxx handlers when previous
 * mode is UserMode. Kernel-mode callers (ZwXxx) skip probing. */

/* Highest valid user-mode address. Any address above this is kernel-only.
 * KUSD is at 0x7FFE0000 (one 4 KiB page); one page above is the boundary. */
#define MM_USER_PROBE_ADDRESS   0x7FFF0000ULL

/* Validate that [Address, Address+Length) is within user-mode range and
 * the start address is aligned to Alignment (must be power of 2).
 * Returns STATUS_SUCCESS or STATUS_ACCESS_VIOLATION. */
NTSTATUS ProbeForRead(const void *Address, uint64_t Length, uint32_t Alignment);
NTSTATUS ProbeForWrite(void *Address, uint64_t Length, uint32_t Alignment);

/* Convenience: probe only if previous mode is UserMode. No-op for kernel. */
static inline NTSTATUS ProbeForReadIfUser(const void *Address,
                                          uint64_t Length, uint32_t Alignment)
{
    if (ssdt_previous_mode() == SSDT_USER_MODE)
        return ProbeForRead(Address, Length, Alignment);
    return STATUS_SUCCESS;
}

static inline NTSTATUS ProbeForWriteIfUser(void *Address,
                                           uint64_t Length, uint32_t Alignment)
{
    if (ssdt_previous_mode() == SSDT_USER_MODE)
        return ProbeForWrite(Address, Length, Alignment);
    return STATUS_SUCCESS;
}

/* ---- Kernel-only API guard ------------------------------------------------ */

/* Returns STATUS_PRIVILEGE_NOT_HELD if current previous mode is UserMode.
 * Use at the top of handlers that must only be called from kernel mode. */
#define ASSERT_KERNEL_CALLER() \
    do { \
        if (ssdt_previous_mode() != SSDT_KERNEL_MODE) \
            return STATUS_PRIVILEGE_NOT_HELD; \
    } while (0)

/* ---- ZwXxx aliases --------------------------------------------------------
 *
 * Each ZwXxx is a static inline that saves the current previous mode,
 * forces KernelMode, dispatches via ssdt_dispatch(), then restores the
 * saved mode. This ensures nested Zw calls from within a UserMode syscall
 * handler correctly bypass probes.
 *
 * SMP note: previous mode is stored per-thread (struct thread.previous_mode),
 * resolved through thread_current() -- see the SMP-closure note in ssdt.c.
 * It is NOT yet fully CPU-local (thread_current() reads the global
 * current-thread cursor); that closure is the per-CPU current-thread work
 * in the SMP phase-2 run-queue TODO. */

/* Helper: dispatch with scoped KernelMode */
static inline NTSTATUS zw_dispatch(uint32_t svc,
                                    uint64_t a1, uint64_t a2, uint64_t a3,
                                    uint64_t a4, uint64_t a5, uint64_t a6)
{
    uint32_t saved = ssdt_previous_mode();
    NTSTATUS result;
    ssdt_set_previous_mode(SSDT_KERNEL_MODE);
    result = ssdt_dispatch(svc, a1, a2, a3, a4, a5, a6);
    ssdt_set_previous_mode(saved);
    return result;
}

static inline NTSTATUS ZwClose(uint64_t handle)
{
    return zw_dispatch(SSDT_NtClose, handle, 0, 0, 0, 0, 0);
}

static inline NTSTATUS ZwCreateFile(uint64_t a1, uint64_t a2, uint64_t a3,
                                     uint64_t a4, uint64_t a5, uint64_t a6)
{
    return zw_dispatch(SSDT_NtCreateFile, a1, a2, a3, a4, a5, a6);
}

static inline NTSTATUS ZwOpenFile(uint64_t a1, uint64_t a2, uint64_t a3,
                                   uint64_t a4, uint64_t a5, uint64_t a6)
{
    return zw_dispatch(SSDT_NtOpenFile, a1, a2, a3, a4, a5, a6);
}

static inline NTSTATUS ZwReadFile(uint64_t a1, uint64_t a2, uint64_t a3,
                                   uint64_t a4, uint64_t a5, uint64_t a6)
{
    return zw_dispatch(SSDT_NtReadFile, a1, a2, a3, a4, a5, a6);
}

static inline NTSTATUS ZwWriteFile(uint64_t a1, uint64_t a2, uint64_t a3,
                                    uint64_t a4, uint64_t a5, uint64_t a6)
{
    return zw_dispatch(SSDT_NtWriteFile, a1, a2, a3, a4, a5, a6);
}

static inline NTSTATUS ZwQueryInformationFile(uint64_t a1, uint64_t a2,
                                               uint64_t a3, uint64_t a4,
                                               uint64_t a5, uint64_t a6)
{
    return zw_dispatch(SSDT_NtQueryInformationFile, a1, a2, a3, a4, a5, a6);
}

static inline NTSTATUS ZwQueryDirectoryFile(uint64_t a1, uint64_t a2,
                                             uint64_t a3, uint64_t a4,
                                             uint64_t a5, uint64_t a6)
{
    return zw_dispatch(SSDT_NtQueryDirectoryFile, a1, a2, a3, a4, a5, a6);
}

static inline NTSTATUS ZwCreateEvent(uint64_t a1, uint64_t a2, uint64_t a3,
                                      uint64_t a4, uint64_t a5, uint64_t a6)
{
    return zw_dispatch(SSDT_NtCreateEvent, a1, a2, a3, a4, a5, a6);
}

static inline NTSTATUS ZwCreateMutant(uint64_t a1, uint64_t a2, uint64_t a3,
                                       uint64_t a4, uint64_t a5, uint64_t a6)
{
    return zw_dispatch(SSDT_NtCreateMutant, a1, a2, a3, a4, a5, a6);
}

static inline NTSTATUS ZwCreateSemaphore(uint64_t a1, uint64_t a2,
                                          uint64_t a3, uint64_t a4,
                                          uint64_t a5, uint64_t a6)
{
    return zw_dispatch(SSDT_NtCreateSemaphore, a1, a2, a3, a4, a5, a6);
}

static inline NTSTATUS ZwQuerySystemInformation(uint64_t a1, uint64_t a2,
                                                 uint64_t a3, uint64_t a4,
                                                 uint64_t a5, uint64_t a6)
{
    return zw_dispatch(SSDT_NtQuerySystemInformation, a1, a2, a3, a4, a5, a6);
}

static inline NTSTATUS ZwYieldExecution(void)
{
    return zw_dispatch(SSDT_NtYieldExecution, 0, 0, 0, 0, 0, 0);
}

/* NOTE: the SSDT_NtDuplicateObject (0x0001) and SSDT_NtQueryObject (0x0002)
 * slots are NOT registered yet -- the NT-ABI wrapper handlers are owned by
 * the Generic Object Management Syscalls work. Until then these two aliases
 * dispatch to unregistered slots, so the dispatcher's not-implemented status
 * is returned (fail closed). The Object Manager bodies (ob.c
 * NtDuplicateObject/NtQueryObject) use the internal handle-table ABI, not
 * the process-handle NT ABI. */
static inline NTSTATUS ZwDuplicateObject(uint64_t a1, uint64_t a2,
                                          uint64_t a3, uint64_t a4,
                                          uint64_t a5, uint64_t a6)
{
    return zw_dispatch(SSDT_NtDuplicateObject, a1, a2, a3, a4, a5, a6);
}

static inline NTSTATUS ZwQueryObject(uint64_t a1, uint64_t a2,
                                      uint64_t a3, uint64_t a4,
                                      uint64_t a5, uint64_t a6)
{
    return zw_dispatch(SSDT_NtQueryObject, a1, a2, a3, a4, a5, a6);
}

static inline NTSTATUS ZwWaitForSingleObject(uint64_t handle,
                                              uint64_t alertable,
                                              uint64_t timeout)
{
    return zw_dispatch(SSDT_NtWaitForSingleObject,
                       handle, alertable, timeout, 0, 0, 0);
}
