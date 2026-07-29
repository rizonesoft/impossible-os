/* ============================================================================
 * abi_contract.h -- Kernel/ring-3 ABI contract (GENERATED, SINGLE SOURCE)
 *
 * Regenerate via: python3 scripts/gen-user-abi.py
 * Verified via:   make check-abi
 *
 * DO NOT EDIT BY HAND. This header is machine-derived from:
 *   include/kernel/sched/syscall.h       (INT 0x80 SYS_* numbers)
 *   include/kernel/nt/service_numbers.h  (SSDT_* service numbers)
 *   include/kernel/nt/ntstatus.h         (NTSTATUS return codes)
 *   include/kernel/sched/task.h          (TASK_EXIT_* exit statuses)
 *
 * Any kernel-side renumber that forgets to regenerate this header will
 * fail `make check-abi` before the drift leaks to runtime. User-mode
 * tests + user/lib MUST consume these constants rather than duplicating
 * the kernel values by hand -- the hand-copy pattern is exactly what
 * TODO-04 -17 was filed to eliminate.
 *
 * SINGLE COMMIT POINT. This file is the ONLY generated ABI artifact. It
 * used to be a PAIR -- user/include/abi_numbers.h plus
 * include/kernel/abi_hash.h -- published by two back-to-back os.replace
 * calls. POSIX rename is atomic for ONE path, so a SIGKILL between those
 * calls left a mismatched pair on disk, and crt0 aborts every ring-3
 * binary at SYS_ABI_HANDSHAKE when the two disagree. One generated file
 * means one rename: no reader can observe half a generation. Both former
 * destinations are now STATIC committed shims over this file; a shim
 * carries no generated data, so it cannot desynchronize from the source.
 *
 * TWO INDEPENDENT GUARDS, deliberately. The fingerprint is always visible;
 * the number tables appear only under ABI_CONTRACT_WANT_NUMBERS, which the
 * ring-3 shim defines and the kernel shim does not -- the kernel already
 * defines SYS_* in include/kernel/sched/syscall.h and would collide. The
 * guards are separate rather than one `#pragma once` so INCLUSION ORDER
 * cannot matter: a translation unit that already saw this header without
 * the macro still gets the tables when a later include defines it.
 *
 * Layout contract: flat `#define` block per category. No enums (user libc
 * is freestanding and may be compiled without -std=c11+), no function-like
 * macros (keeps parse surface minimal for future tooling).
 * ============================================================================ */

#ifndef IMPOSSIBLE_OS_ABI_CONTRACT_H
#define IMPOSSIBLE_OS_ABI_CONTRACT_H

/* ---- ABI fingerprint (FNV-1a 64-bit) --------------------------------------------------------- */
/* Hash over the sorted tuple of (SYS_*, SSDT_*, FAULT_*, exported
 * TASK_EXIT_* statuses, TEB offsets, KUSD offsets). The STATUS_* codes
 * in the number block are NOT fingerprinted -- they mirror a stable external
 * contract rather than a number this kernel assigns. Changing an
 * exported exit status therefore invalidates existing user binaries at
 * crt0, which is deliberate: a stale binary would otherwise compare a
 * waitpid result against a number the kernel no longer produces.
 * Kernel emits the same hash via SYS_ABI_HANDSHAKE; user
 * crt0 calls that syscall and aborts with EX_ABI_MISMATCH = 0x42 on
 * disagreement. A kernel-side renumber that slipped through review
 * but skipped this generator surfaces at process start, not at the
 * first syscall with corrupted semantics. */
#define IMPOSSIBLE_OS_ABI_HASH   0x448A9D27775243B3ULL

#endif /* IMPOSSIBLE_OS_ABI_CONTRACT_H */

#if defined(ABI_CONTRACT_WANT_NUMBERS) && !defined(IMPOSSIBLE_OS_ABI_NUMBERS_H)
#define IMPOSSIBLE_OS_ABI_NUMBERS_H

/* ---- INT 0x80 syscall numbers (SYS_*, FAULT_*) --------------------------------------------------------- */
#define SYS_WRITE                1
#define SYS_READ                 2
#define SYS_EXIT                 3
#define SYS_YIELD                4
#define SYS_FORK                 5
#define SYS_EXEC                 6
#define SYS_WAITPID              7
#define SYS_READFILE             8
#define SYS_READDIR              9
#define SYS_GETPROCS             10
#define SYS_KILL                 11
#define SYS_UPTIME               12
#define SYS_REBOOT               13
#define SYS_SHUTDOWN             14
#define SYS_PING                 15
#define SYS_NETINFO              16
#define SYS_LOG                  17
#define SYS_GETPID               18
#define SYS_SETPGID              19
#define SYS_GETPGID              20
#define SYS_SETSID               21
#define SYS_GETSID               22
#define SYS_GETPGRP              23
#define SYS_TCSETPGRP            24
#define SYS_TCGETPGRP            25
#define SYS_GENCONSOLECTRL       26
#define SYS_PIPE                 33
#define SYS_SIGNAL               34
#define SYS_SHMEM_CREATE         35
#define SYS_SHMEM_MAP            36
#define SYS_MMAP                 37
#define SYS_MUNMAP               38
#define SYS_OPENFILE             39
#define SYS_CLOSEHANDLE          40
#define SYS_READHANDLE           41
#define SYS_OPENDIROBJ           42
#define SYS_QUERYDIROBJ          43
#define SYS_FAULT_INJECT         44
#define SYS_WRITEHANDLE          45
#define SYS_UNMAPVIEW            46
#define SYS_TEST_REPORT          48
#define SYS_ABI_HANDSHAKE        47
#define FAULT_KMALLOC_NEXT       1
#define FAULT_KMALLOC_COUNTDOWN  2
#define FAULT_PMM_NEXT           3
#define FAULT_VMM_MAP_NEXT       4
#define FAULT_COPY_USER_NEXT     5
#define FAULT_CLEAR_ALL          6
#define FAULT_PMM_COUNTDOWN      7
#define FAULT_KMALLOC_SITE       8
#define FAULT_PMM_SITE           9
#define FAULT_SITE_QUERY         10
#define FAULT_ALLOC_KMALLOC      1
#define FAULT_ALLOC_PMM          2
#define FAULT_SITE_NONE          0
#define FAULT_SITE_EXEC_ARGV_TABLE 1
#define FAULT_SITE_EXEC_PRIVATE_FRAMES 2
#define FAULT_SITE_PEB_FRAMES    3
#define FAULT_SITE_FORK_CHILD_PML4 4
#define FAULT_SITE_MAX           4
#define SYS_NT_WRITE             0x0013
#define SYS_NT_READ              0x0012
#define SYS_NT_EXIT              0x0033
#define SYS_NT_YIELD             0x0044
#define SYS_NT_WAITPID           0x0006
#define SYS_NT_READDIR           0x0017
#define SYS_NT_GETPROCS          0x00D0
#define SYS_NT_SHUTDOWN          0x00D7
#define SYS_NT_PIPE              0x001B
#define SYS_NT_SHMEM_CREATE      0x005C
#define SYS_NT_SHMEM_MAP         0x005E
#define SYS_NT_CLOSE             0x0000

/* ---- SSDT service numbers (SSDT_*) -- fastpath probe --------------------------------------------------------- */
#define SSDT_NtClose                     0x0000
#define SSDT_NtTerminateProcess          0x0033
#define SSDT_NtYieldExecution            0x0044
#define SSDT_NtQuerySystemInformation    0x00D0

/* ---- NTSTATUS return codes used by user probes --------------------------------------------------------- */
#define STATUS_SUCCESS                   0x00000000
#define STATUS_NOT_IMPLEMENTED           0xC0000002
#define STATUS_INVALID_HANDLE            0xC0000008
#define STATUS_ACCESS_DENIED             0xC0000022

/* ---- Kernel exit statuses (TASK_EXIT_*) -- resolved, signed --------------------------------------------------------- */
#define TASK_EXIT_EXEC_IMAGE_DESTROYED   (-1001)

/* ---- crt0 handshake failure exit status --------------------------------------------------- */
#define EX_ABI_MISMATCH          0x42

#endif /* ABI_CONTRACT_WANT_NUMBERS */
