/* ============================================================================
 * teb.h -- Thread Environment Block (TEB)
 *
 * Windows x64-compatible TEB layout at exact offsets so ntdll inline macros
 * (NtCurrentTeb() = mov rax, gs:[0x30]) work without patching.
 *
 * The TEB is mapped in user address space and accessed via GS segment:
 *   Ring 3: GS points to TEB (via IA32_KERNEL_GS_BASE + swapgs)
 *   Ring 0: GS points to per-CPU data (IA32_GS_BASE)
 *
 * Reference: Windows x64 TEB offsets from winternl.h / NtInternals
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* Forward-declare PEB (full definition in peb.h) */
struct peb;

/* ---- CLIENT_ID ---------------------------------------------------------- */
/* Offset 0x40 in TEB. Two 64-bit fields on x64. */

typedef struct client_id {
    uint64_t UniqueProcess;                /* offset 0x00 -- PID */
    uint64_t UniqueThread;                 /* offset 0x08 -- TID */
} CLIENT_ID;

/* ---- NT_TIB (Thread Information Block) ---------------------------------- */
/* Offset 0x00 in TEB. 56 bytes (7 pointers) on x64. */

typedef struct nt_tib {
    void     *ExceptionList;               /* offset 0x00 -- SEH chain head */
    void     *StackBase;                   /* offset 0x08 -- top of stack */
    void     *StackLimit;                  /* offset 0x10 -- bottom of stack */
    void     *SubSystemTib;                /* offset 0x18 -- subsystem-specific */
    union {
        void     *FiberData;               /* offset 0x20 -- fiber context */
        uint32_t  Version;                 /* offset 0x20 -- version (alt) */
    };
    void     *ArbitraryUserPointer;        /* offset 0x28 -- user-settable */
    struct nt_tib *Self;                   /* offset 0x30 -- TEB self-pointer */
} NT_TIB;                                  /* sizeof = 0x38 (56 bytes) */

/* ---- TEB ---------------------------------------------------------------- */
/* Full Thread Environment Block. Fields at exact Windows x64 offsets.
 * Only commonly-used fields are named; gaps are reserved padding.
 *
 * NtCurrentTeb() = gs:[0x30] → NT_TIB.Self → points back to TEB start.
 * GetLastError() = gs:[0x68] → LastErrorValue.
 * NtCurrentPeb() = gs:[0x60] → ProcessEnvironmentBlock.
 * TlsGetValue(i) = gs:[0x1480 + i*8] → TlsSlots[i].
 */

typedef struct teb {
    /* 0x0000 */ NT_TIB      NtTib;
    /* 0x0038 */ void       *EnvironmentPointer;
    /* 0x0040 */ CLIENT_ID   ClientId;
    /* 0x0050 */ void       *ActiveRpcHandle;
    /* 0x0058 */ void       *ThreadLocalStoragePointer;
    /* 0x0060 */ struct peb *ProcessEnvironmentBlock;
    /* 0x0068 */ uint32_t    LastErrorValue;
    /* 0x006C */ uint32_t    CountOfOwnedCriticalSections;

    /* 0x0070 – 0x147F: reserved fields (not yet needed)
     * This 5136-byte gap contains CsrClientThread, Win32ThreadInfo,
     * GdiTebBatch, RealClientId, GdiCachedProcessHandle, and many more
     * Windows-internal fields. We pad to preserve correct offsets. */
    uint8_t  _reserved_0070[0x1480 - 0x0070];

    /* 0x1480 */ uint64_t    TlsSlots[64];

    /* 0x1680 – 0x177F: reserved */
    uint8_t  _reserved_1680[0x1780 - 0x1680];

    /* 0x1780 */ void       *TlsExpansionSlots;

    /* 0x1788+: additional reserved fields.
     * Pad to 4 KB page boundary for clean VMM allocation. */
    uint8_t  _reserved_1788[0x1000 - (0x1788 & 0xFFF)];
} TEB;

/* Compile-time offset verification */
_Static_assert(__builtin_offsetof(TEB, NtTib)                    == 0x0000, "TEB.NtTib offset");
_Static_assert(__builtin_offsetof(TEB, NtTib.Self)               == 0x0030, "TEB.NtTib.Self (gs:[0x30])");
_Static_assert(__builtin_offsetof(TEB, EnvironmentPointer)       == 0x0038, "TEB.EnvironmentPointer offset");
_Static_assert(__builtin_offsetof(TEB, ClientId)                 == 0x0040, "TEB.ClientId offset");
_Static_assert(__builtin_offsetof(TEB, ActiveRpcHandle)          == 0x0050, "TEB.ActiveRpcHandle offset");
_Static_assert(__builtin_offsetof(TEB, ThreadLocalStoragePointer)== 0x0058, "TEB.ThreadLocalStoragePointer offset");
_Static_assert(__builtin_offsetof(TEB, ProcessEnvironmentBlock)  == 0x0060, "TEB.ProcessEnvironmentBlock (gs:[0x60])");
_Static_assert(__builtin_offsetof(TEB, LastErrorValue)           == 0x0068, "TEB.LastErrorValue (gs:[0x68])");
_Static_assert(__builtin_offsetof(TEB, CountOfOwnedCriticalSections) == 0x006C,
               "TEB.CountOfOwnedCriticalSections offset");
_Static_assert(__builtin_offsetof(TEB, TlsSlots)                == 0x1480, "TEB.TlsSlots (gs:[0x1480])");
_Static_assert(__builtin_offsetof(TEB, TlsExpansionSlots)       == 0x1780, "TEB.TlsExpansionSlots offset");
