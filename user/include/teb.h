/* ============================================================================
 * teb.h -- User-mode view of the Thread Environment Block (TEB)
 *
 * User binaries access a subset of TEB fields via `gs:<offset>` segment
 * reads after the kernel has programmed `MSR_IA32_KERNEL_GS_BASE` with
 * the thread's TEB address and the ring-3-bound iretq has issued
 * `swapgs` so user-mode `gs:` points at the TEB. Only the fields the
 * fast-path probe + user/lib/win32.c touch are modeled here; the full
 * struct lives in include/kernel/ob/teb.h and is kernel-internal.
 *
 * The `_Static_assert` block at the bottom is the LOAD-BEARING part of
 * this file. Every offset the user-mode fast path hard-codes in inline
 * asm (`movq %gs:0x40, %rax` etc.) is mirrored here as a compile-time
 * offsetof() == literal check. A kernel-side TEB layout bump that
 * forgets to update user code fails compilation in the user tree with a
 * named message naming the drifted field -- no silent runtime corruption.
 *
 * MUST be kept bitwise-identical to the fields declared in
 * include/kernel/ob/teb.h through offset 0x70. Anything past that point
 * is reserved padding; user code MUST NOT reach into it.
 * ============================================================================ */

#pragma once

#include "types.h"

/* CLIENT_ID: kernel ob_types.h mirrors Windows CLIENT_ID layout.
 * UniqueProcess at offset 0x00, UniqueThread at 0x08 within the struct.
 * TEB.ClientId sits at TEB offset 0x40 so TEB.ClientId.UniqueProcess is
 * reachable via `movq %gs:0x40, %rax` and UniqueThread via `gs:0x48`. */
typedef struct user_client_id {
    uint64_t UniqueProcess;   /* offset 0x00 within CLIENT_ID (TEB+0x40) */
    uint64_t UniqueThread;    /* offset 0x08 within CLIENT_ID (TEB+0x48) */
} USER_CLIENT_ID;

/* NT_TIB subset: only the Self pointer at 0x30 is used by the fast path
 * (probe 1). Prior fields are padding the user doesn't read. */
typedef struct user_nt_tib {
    uint8_t  _pad_0000[0x30];
    struct user_teb *Self;    /* offset 0x30 (gs:0x30) */
} USER_NT_TIB;

/* Minimal user-visible TEB. Fields beyond CountOfOwnedCriticalSections
 * are not modeled -- if the user-mode fast path ever touches them, add
 * the field here with a matching _Static_assert so a future kernel
 * layout bump fails compilation at the user-mode callsite, not the
 * kernel read. */
typedef struct user_teb {
    /* 0x0000 */ USER_NT_TIB    NtTib;
    /* 0x0038 */ void          *EnvironmentPointer;
    /* 0x0040 */ USER_CLIENT_ID ClientId;
    /* 0x0050 */ void          *ActiveRpcHandle;
    /* 0x0058 */ void          *ThreadLocalStoragePointer;
    /* 0x0060 */ void          *ProcessEnvironmentBlock;
    /* 0x0068 */ uint32_t       LastErrorValue;
    /* 0x006C */ uint32_t       CountOfOwnedCriticalSections;
} USER_TEB;

/* Compile-time offset mirrors of include/kernel/ob/teb.h.
 * Fail-closed drift detection for every field the user-mode fast path
 * reads by segment override. Bytes-level offsets (ClientId.UniqueProcess
 * == 0x40) matter because the inline asm uses literals. */
_Static_assert(__builtin_offsetof(USER_TEB, NtTib.Self) == 0x30,
               "USER_TEB.NtTib.Self must match kernel TEB at gs:0x30");
_Static_assert(__builtin_offsetof(USER_TEB, EnvironmentPointer) == 0x38,
               "USER_TEB.EnvironmentPointer must match kernel TEB at 0x38");
_Static_assert(__builtin_offsetof(USER_TEB, ClientId) == 0x40,
               "USER_TEB.ClientId must match kernel TEB at 0x40");
_Static_assert(__builtin_offsetof(USER_TEB, ClientId.UniqueProcess) == 0x40,
               "USER_TEB.ClientId.UniqueProcess must match kernel TEB at gs:0x40 "
               "(load-bearing for gs:0x40 fast-path probe + GetCurrentProcessId)");
_Static_assert(__builtin_offsetof(USER_TEB, ClientId.UniqueThread) == 0x48,
               "USER_TEB.ClientId.UniqueThread must match kernel TEB at gs:0x48");
_Static_assert(__builtin_offsetof(USER_TEB, ProcessEnvironmentBlock) == 0x60,
               "USER_TEB.ProcessEnvironmentBlock must match kernel TEB at gs:0x60");
_Static_assert(__builtin_offsetof(USER_TEB, LastErrorValue) == 0x68,
               "USER_TEB.LastErrorValue must match kernel TEB at gs:0x68");
