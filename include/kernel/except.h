/* ============================================================================
 * except.h -- Windows AMD64 exception ABI (EXCEPTION_RECORD, CONTEXT,
 *             EXCEPTION_POINTERS)
 *
 * Canonical owner of the exception-dispatch ABI. A real ntdll must be able to
 * consume these types unpatched, so every load-bearing offset is pinned with a
 * _Static_assert below (Layer 1 of the 5-layer defense).
 *
 * CONTEXT, XMM_SAVE_AREA32 and M128A previously lived in panic.h for crash-dump
 * use; this header is now their home and panic.h includes it. The layout is
 * unchanged, so MDMP files still open in WinDbg.
 *
 * ARCH: x86-64 -- the CONTEXT register file is the AMD64 ABI by definition.
 *
 * Reference: Windows SDK winnt.h (AMD64), Windows Internals 7e ch. 8.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"
#include "kernel/nt/ntstatus.h"
#include "kernel/ob/peb.h"   /* LIST_ENTRY -- VEH node link field */

/* Full definition in kernel/idt.h. Forward-declared so this header stays free
 * of an arch include; except.c includes idt.h for the field access. */
struct interrupt_frame;

/* --- CONTEXT flags (Windows x64) -----------------------------------------
 *
 * Every group carries the CONTEXT_AMD64 architecture bit. A naive
 * `flags & CONTEXT_CONTROL` test is therefore TRUE for any AMD64 group and
 * silently defeats the mask -- use CONTEXT_HAS_GROUP(), never a bare AND. */

#define CONTEXT_AMD64               0x00100000
#define CONTEXT_CONTROL             (CONTEXT_AMD64 | 0x01)
#define CONTEXT_INTEGER             (CONTEXT_AMD64 | 0x02)
#define CONTEXT_SEGMENTS            (CONTEXT_AMD64 | 0x04)
#define CONTEXT_FLOATING_POINT      (CONTEXT_AMD64 | 0x08)
#define CONTEXT_DEBUG_REGISTERS     (CONTEXT_AMD64 | 0x10)
#define CONTEXT_FULL                (CONTEXT_CONTROL | CONTEXT_INTEGER | CONTEXT_FLOATING_POINT)
#define CONTEXT_ALL                 (CONTEXT_CONTROL | CONTEXT_INTEGER | CONTEXT_SEGMENTS | \
                                     CONTEXT_FLOATING_POINT | CONTEXT_DEBUG_REGISTERS)

/* Groups an interrupt_frame physically carries (see converter contract below). */
#define CONTEXT_FRAME_BACKED        (CONTEXT_CONTROL | CONTEXT_INTEGER)

/* Exact-group test: every bit of `group` must be present in `flags`.
 * Required because all groups share CONTEXT_AMD64 (see comment above). */
#define CONTEXT_HAS_GROUP(flags, group) \
    (((uint32_t)(flags) & (uint32_t)(group)) == (uint32_t)(group))

/* Architectural initial FPU state (Intel SDM Vol.1 "Initialization").
 * A zeroed FCW/MXCSR unmasks every FP exception, so a CONTEXT whose FPU group
 * was never captured is filled with these rather than left as raw zeroes. */
#define FPU_FCW_INIT                0x037F
#define FPU_MXCSR_INIT              0x1F80

/* --- XMM_SAVE_AREA32 (FXSAVE format, 512 bytes) -------------------------
 *
 * `aligned(16)` matches the Windows XSAVE_FORMAT ABI (winnt.h) and FXSAVE's
 * hardware requirement that the 512-byte save area be 16-byte aligned. `packed`
 * keeps every field at its exact byte offset; the two compose (offsets fixed,
 * minimum alignment raised to 16). */

typedef struct __attribute__((packed, aligned(16))) _XMM_SAVE_AREA32 {
    uint16_t ControlWord;       /* 0x000 */
    uint16_t StatusWord;        /* 0x002 */
    uint8_t  TagWord;           /* 0x004 */
    uint8_t  _Reserved1;       /* 0x005 */
    uint16_t ErrorOpcode;       /* 0x006 */
    uint32_t ErrorOffset;       /* 0x008 */
    uint16_t ErrorSelector;     /* 0x00C */
    uint16_t _Reserved2;       /* 0x00E */
    uint32_t DataOffset;        /* 0x010 */
    uint16_t DataSelector;      /* 0x014 */
    uint16_t _Reserved3;       /* 0x016 */
    uint32_t MxCsr;             /* 0x018 */
    uint32_t MxCsr_Mask;        /* 0x01C */
    uint8_t  FloatRegisters[128]; /* 0x020: ST0-ST7 (8 x 16) */
    uint8_t  XmmRegisters[256];   /* 0x0A0: XMM0-XMM15 (16 x 16) */
    uint8_t  _Reserved4[96];   /* 0x1A0 */
} XMM_SAVE_AREA32;             /* total: 0x200 = 512 bytes */

/* --- M128A (128-bit value for vector registers) -------------------------- */

typedef struct __attribute__((aligned(16))) _M128A {
    uint64_t Low;
    int64_t  High;
} M128A;

/* --- CONTEXT (Windows x64 layout, 1232 bytes) ----------------------------
 *
 * Matches the Windows 10/11 AMD64 CONTEXT layout byte-for-byte so that a real
 * ntdll consumes it unpatched and MDMP files open in WinDbg.
 *
 * struct _CONTEXT is forward-declared in nt_types.h -- this completes it.
 * aligned(16) mirrors Windows DECLSPEC_ALIGN(16): the embedded FltSave area
 * must be 16-byte aligned for FXSAVE/FXRSTOR, which only holds if CONTEXT
 * itself is 16-aligned (sizeof 1232 is a multiple of 16, so no tail padding
 * is added). */

struct __attribute__((aligned(16))) _CONTEXT {
    /* Home registers -- offset 0x000 */
    uint64_t P1Home;
    uint64_t P2Home;
    uint64_t P3Home;
    uint64_t P4Home;
    uint64_t P5Home;
    uint64_t P6Home;

    /* Control / MXCSR -- offset 0x030 */
    uint32_t ContextFlags;
    uint32_t MxCsr;

    /* Segment registers -- offset 0x038 */
    uint16_t SegCs;
    uint16_t SegDs;
    uint16_t SegEs;
    uint16_t SegFs;
    uint16_t SegGs;
    uint16_t SegSs;

    /* EFLAGS -- offset 0x044 */
    uint32_t EFlags;

    /* Debug registers -- offset 0x048 */
    uint64_t Dr0;
    uint64_t Dr1;
    uint64_t Dr2;
    uint64_t Dr3;
    uint64_t Dr6;
    uint64_t Dr7;

    /* Integer registers -- offset 0x078 */
    uint64_t Rax;
    uint64_t Rcx;
    uint64_t Rdx;
    uint64_t Rbx;
    uint64_t Rsp;
    uint64_t Rbp;
    uint64_t Rsi;
    uint64_t Rdi;
    uint64_t R8;
    uint64_t R9;
    uint64_t R10;
    uint64_t R11;
    uint64_t R12;
    uint64_t R13;
    uint64_t R14;
    uint64_t R15;

    /* Program counter -- offset 0x0F8 */
    uint64_t Rip;

    /* Floating point state (FXSAVE format) -- offset 0x100 */
    XMM_SAVE_AREA32 FltSave;

    /* YMM high halves + reserved vector slots -- offset 0x300 */
    M128A VectorRegister[26];

    /* Vector control -- offset 0x4A0 */
    uint64_t VectorControl;

    /* Last Branch Record -- offset 0x4A8 */
    uint64_t DebugControl;
    uint64_t LastBranchToRip;
    uint64_t LastBranchFromRip;
    uint64_t LastExceptionToRip;
    uint64_t LastExceptionFromRip;
};

#ifndef _CONTEXT_TYPEDEF_DEFINED
#define _CONTEXT_TYPEDEF_DEFINED
typedef struct _CONTEXT CONTEXT;
#endif

/* Layer 1 -- pin the AMD64 CONTEXT ABI. A real ntdll indexes these offsets. */
_Static_assert(sizeof(CONTEXT) == 1232, "CONTEXT must be 1232 bytes (Windows x64)");
_Static_assert(sizeof(XMM_SAVE_AREA32) == 512, "XMM_SAVE_AREA32 must be 512 bytes (FXSAVE)");
_Static_assert(_Alignof(CONTEXT) == 16, "CONTEXT must be 16-byte aligned (Windows DECLSPEC_ALIGN(16))");
_Static_assert(_Alignof(XMM_SAVE_AREA32) == 16, "XMM_SAVE_AREA32 must be 16-byte aligned (FXSAVE)");
/* Pin the FXSAVE-format field offsets. `packed` + a size assert alone would let
 * a field reorder relocate MxCsr/ControlWord while sizeof stays 512 --
 * context_init_fpu_state() writes those two by name and panic.c reads MXCSR at
 * the hardcoded FXSAVE byte offset 0x18, so a silent desync would break both. */
_Static_assert(__builtin_offsetof(XMM_SAVE_AREA32, ControlWord)    == 0x00, "FXSAVE ControlWord at 0x00");
_Static_assert(__builtin_offsetof(XMM_SAVE_AREA32, StatusWord)     == 0x02, "FXSAVE StatusWord at 0x02");
_Static_assert(__builtin_offsetof(XMM_SAVE_AREA32, MxCsr)          == 0x18, "FXSAVE MxCsr at 0x18");
_Static_assert(__builtin_offsetof(XMM_SAVE_AREA32, MxCsr_Mask)     == 0x1C, "FXSAVE MxCsr_Mask at 0x1C");
_Static_assert(__builtin_offsetof(XMM_SAVE_AREA32, FloatRegisters) == 0x20, "FXSAVE FloatRegisters at 0x20");
_Static_assert(__builtin_offsetof(XMM_SAVE_AREA32, XmmRegisters)   == 0xA0, "FXSAVE XmmRegisters at 0xA0");
_Static_assert(__builtin_offsetof(CONTEXT, ContextFlags) == 0x30, "ContextFlags at 0x30");
_Static_assert(__builtin_offsetof(CONTEXT, MxCsr)   == 0x34, "MxCsr at 0x34");
_Static_assert(__builtin_offsetof(CONTEXT, SegCs)   == 0x38, "SegCs at 0x38");
_Static_assert(__builtin_offsetof(CONTEXT, SegSs)   == 0x42, "SegSs at 0x42");
_Static_assert(__builtin_offsetof(CONTEXT, EFlags)  == 0x44, "EFlags at 0x44");
_Static_assert(__builtin_offsetof(CONTEXT, Dr0)     == 0x48, "Dr0 at 0x48");
_Static_assert(__builtin_offsetof(CONTEXT, Rax)     == 0x78, "Rax at 0x78");
_Static_assert(__builtin_offsetof(CONTEXT, Rsp)     == 0x98, "Rsp at 0x98");
_Static_assert(__builtin_offsetof(CONTEXT, R15)     == 0xF0, "R15 at 0xF0");
_Static_assert(__builtin_offsetof(CONTEXT, Rip)     == 0xF8, "Rip at 0xF8");
_Static_assert(__builtin_offsetof(CONTEXT, FltSave) == 0x100, "legacy XMM_SAVE_AREA32 at 0x100");
_Static_assert(__builtin_offsetof(CONTEXT, VectorRegister) == 0x300, "VectorRegister at 0x300");
_Static_assert(__builtin_offsetof(CONTEXT, VectorControl)  == 0x4A0, "VectorControl at 0x4A0");

/* --- EXCEPTION_RECORD ----------------------------------------------------- */

#define EXCEPTION_MAXIMUM_PARAMETERS 15

/* ExceptionFlags bits. EXCEPTION_CONTINUABLE is the ABSENCE of
 * EXCEPTION_NONCONTINUABLE (value 0), matching winnt.h -- test it with
 * EXCEPTION_IS_CONTINUABLE(), never a bitwise AND against zero. */
#define EXCEPTION_CONTINUABLE       0x00000000
#define EXCEPTION_NONCONTINUABLE    0x00000001
#define EXCEPTION_UNWINDING         0x00000002
#define EXCEPTION_EXIT_UNWIND       0x00000004
#define EXCEPTION_STACK_INVALID     0x00000008
#define EXCEPTION_NESTED_CALL       0x00000010
#define EXCEPTION_TARGET_UNWIND     0x00000020
#define EXCEPTION_COLLIDED_UNWIND   0x00000040

#define EXCEPTION_IS_CONTINUABLE(flags) \
    (((uint32_t)(flags) & (uint32_t)EXCEPTION_NONCONTINUABLE) == 0)

/* Exception codes. In the Windows ABI these are aliases of the matching
 * NTSTATUS values -- canonical definitions live in kernel/nt/ntstatus.h. */
#define EXCEPTION_ACCESS_VIOLATION      ((uint32_t)STATUS_ACCESS_VIOLATION)
#define EXCEPTION_ILLEGAL_INSTRUCTION   ((uint32_t)STATUS_ILLEGAL_INSTRUCTION)
#define EXCEPTION_INT_DIVIDE_BY_ZERO    ((uint32_t)STATUS_INTEGER_DIVIDE_BY_ZERO)
#define EXCEPTION_INT_OVERFLOW          ((uint32_t)STATUS_INTEGER_OVERFLOW)
#define EXCEPTION_BREAKPOINT            ((uint32_t)STATUS_BREAKPOINT)
#define EXCEPTION_SINGLE_STEP           ((uint32_t)STATUS_SINGLE_STEP)
#define EXCEPTION_ARRAY_BOUNDS_EXCEEDED ((uint32_t)STATUS_ARRAY_BOUNDS_EXCEEDED)
#define EXCEPTION_DATATYPE_MISALIGNMENT ((uint32_t)STATUS_DATATYPE_MISALIGNMENT)
#define EXCEPTION_PRIV_INSTRUCTION      ((uint32_t)STATUS_PRIVILEGED_INSTRUCTION)
#define EXCEPTION_STACK_OVERFLOW        ((uint32_t)STATUS_STACK_OVERFLOW)
#define EXCEPTION_IN_PAGE_ERROR         ((uint32_t)STATUS_IN_PAGE_ERROR)
#define EXCEPTION_GUARD_PAGE            ((uint32_t)STATUS_GUARD_PAGE_VIOLATION)
#define EXCEPTION_INVALID_DISPOSITION   ((uint32_t)STATUS_INVALID_DISPOSITION)
#define EXCEPTION_FLT_DENORMAL_OPERAND  ((uint32_t)STATUS_FLOAT_DENORMAL_OPERAND)
#define EXCEPTION_FLT_DIVIDE_BY_ZERO    ((uint32_t)STATUS_FLOAT_DIVIDE_BY_ZERO)
#define EXCEPTION_FLT_INEXACT_RESULT    ((uint32_t)STATUS_FLOAT_INEXACT_RESULT)
#define EXCEPTION_FLT_INVALID_OPERATION ((uint32_t)STATUS_FLOAT_INVALID_OPERATION)
#define EXCEPTION_FLT_OVERFLOW          ((uint32_t)STATUS_FLOAT_OVERFLOW)
#define EXCEPTION_FLT_UNDERFLOW         ((uint32_t)STATUS_FLOAT_UNDERFLOW)
#define EXCEPTION_FLT_STACK_CHECK       ((uint32_t)STATUS_FLOAT_STACK_CHECK)
#define EXCEPTION_STACK_BUFFER_OVERRUN  ((uint32_t)STATUS_STACK_BUFFER_OVERRUN)

/* Fast-fail subcodes (winnt.h FAST_FAIL_*). A __fastfail (int 0x29) carries the
 * code in ECX; a CET #CP shadow-stack RET mismatch reports this specific one.
 * Stored in EXCEPTION_RECORD.ExceptionInformation[0] on the noncontinuable
 * STATUS_STACK_BUFFER_OVERRUN so a consumer can tell the fault kind apart. */
#define FAST_FAIL_CONTROL_INVALID_RETURN_ADDRESS  0x39u

/* CET #CP (vector 21) shadow-stack RET mismatch does NOT get a dedicated NTSTATUS
 * in real Windows: it surfaces the SAME EXCEPTION_STACK_BUFFER_OVERRUN code as
 * __fastfail / a GS cookie violation, disambiguated by the fast-fail subcode
 * (FAST_FAIL_CONTROL_INVALID_RETURN_ADDRESS) in ExceptionInformation[0]. The
 * general fault-to-exception mapping owns the #CP handler and uses that pairing;
 * STATUS_CONTROL_STACK_VIOLATION (0xC00001B2) is a DIFFERENT, SetThreadContext-
 * path constant and is deliberately NOT introduced here. */

/* ExceptionInformation[] indices for STATUS_ACCESS_VIOLATION / IN_PAGE_ERROR,
 * per the winnt.h contract: [0] = access type, [1] = faulting address. */
#define EXCEPTION_INFO_ACCESS_TYPE  0
#define EXCEPTION_INFO_FAULT_ADDR   1

/* Access-type values stored in ExceptionInformation[0]. */
#define EXCEPTION_ACCESS_READ       0
#define EXCEPTION_ACCESS_WRITE      1
#define EXCEPTION_ACCESS_EXECUTE    8

typedef struct _EXCEPTION_RECORD {
    NTSTATUS  ExceptionCode;                   /* 0x00 */
    uint32_t  ExceptionFlags;                  /* 0x04 */
    struct _EXCEPTION_RECORD *ExceptionRecord; /* 0x08: nested/chained record */
    void     *ExceptionAddress;                /* 0x10 */
    uint32_t  NumberParameters;                /* 0x18 */
    /* 4 bytes implicit padding at 0x1C -- ExceptionInformation needs 8-align */
    uint64_t  ExceptionInformation[EXCEPTION_MAXIMUM_PARAMETERS]; /* 0x20 */
} EXCEPTION_RECORD;                            /* total: 0x98 = 152 bytes */

/* Layer 1 -- pin the EXCEPTION_RECORD ABI. Size alone would not catch a field
 * reorder across the implicit padding, so every offset is asserted. */
_Static_assert(sizeof(EXCEPTION_RECORD) == 152, "EXCEPTION_RECORD must be 152 bytes (Windows x64)");
_Static_assert(_Alignof(EXCEPTION_RECORD) == 8, "EXCEPTION_RECORD must be 8-byte aligned");
_Static_assert(__builtin_offsetof(EXCEPTION_RECORD, ExceptionCode)    == 0x00, "ExceptionCode at 0x00");
_Static_assert(__builtin_offsetof(EXCEPTION_RECORD, ExceptionFlags)   == 0x04, "ExceptionFlags at 0x04");
_Static_assert(__builtin_offsetof(EXCEPTION_RECORD, ExceptionRecord)  == 0x08, "ExceptionRecord at 0x08");
_Static_assert(__builtin_offsetof(EXCEPTION_RECORD, ExceptionAddress) == 0x10, "ExceptionAddress at 0x10");
_Static_assert(__builtin_offsetof(EXCEPTION_RECORD, NumberParameters) == 0x18, "NumberParameters at 0x18");
_Static_assert(__builtin_offsetof(EXCEPTION_RECORD, ExceptionInformation) == 0x20,
               "ExceptionInformation at 0x20");

/* --- EXCEPTION_POINTERS --------------------------------------------------- */

typedef struct _EXCEPTION_POINTERS {
    EXCEPTION_RECORD *ExceptionRecord;  /* 0x00 */
    CONTEXT          *ContextRecord;    /* 0x08 */
} EXCEPTION_POINTERS;                   /* total: 16 bytes */

_Static_assert(sizeof(EXCEPTION_POINTERS) == 16, "EXCEPTION_POINTERS must be 16 bytes (Windows x64)");
_Static_assert(__builtin_offsetof(EXCEPTION_POINTERS, ExceptionRecord) == 0x00, "ExceptionRecord at 0x00");
_Static_assert(__builtin_offsetof(EXCEPTION_POINTERS, ContextRecord)   == 0x08, "ContextRecord at 0x08");

/* --- Vectored Exception Handler (VEH) shared ABI --------------------------
 *
 * VEH is a RING-3 mechanism. ntdll keeps the handler list as PROCESS-GLOBAL
 * state -- a lock-guarded doubly-linked list, analogous to Windows'
 * LdrpVectorHandlerList under LdrpVehLock -- and walks it from
 * RtlDispatchException BEFORE the frame-based SEH scan. The kernel NEVER reads,
 * walks, or anchors this list; it publishes only the shared node + disposition
 * ABI so the ntdll VEH list (owned by the ntdll user-runtime Vectored Exception
 * Handling work) and any kernel-side introspection agree byte-for-byte.
 *
 * There is deliberately NO kernel / TEB / PEB anchor field. VEH is per-PROCESS:
 * a per-thread TEB anchor would give each thread its own list and break
 * AddVectoredExceptionHandler semantics (a handler registered on one thread
 * would not fire for another thread's fault). This mirrors the unhandled-
 * exception-filter rule that there is no PEB.UnhandledExceptionFilter field --
 * process-wide ring-3 state lives in ntdll, not in a kernel-published
 * per-thread structure. (Design review 2026-07-19: the original TEB.VehListHead
 * plan was dropped for exactly this reason; the ntdll anchor also sidesteps the
 * two-page TEB mapping dependency in the PEB/TEB user-ABI TEB-allocation work.) */

/* VEH/frame handler dispositions (winnt.h). A handler returns one of these
 * signed values; the 32-bit width is carried by the callback RETURN TYPE
 * (PVECTORED_EXCEPTION_HANDLER returns int32_t -- a Windows LONG in EAX), not by
 * the macros. The macros are bare integers exactly as winnt.h defines them so
 * they remain valid in preprocessor `#if` expressions (a `(int32_t)`-cast form
 * expands to invalid preprocessor tokens). EXCEPTION_EXECUTE_HANDLER is the
 * third winnt.h disposition (frame-based __except filters, not VEH); it is
 * published here as the single canonical home for the triad so no consumer
 * redefines a subset. */
#define EXCEPTION_CONTINUE_EXECUTION  (-1)
#define EXCEPTION_CONTINUE_SEARCH     (0)
#define EXCEPTION_EXECUTE_HANDLER     (1)

/* Preprocessor-safety guard: the dispositions must evaluate in `#if` (winnt.h
 * contract). A `(int32_t)`-cast form would make this directive a syntax error,
 * so this line fails the build the moment the macros stop being bare integers. */
#if (EXCEPTION_CONTINUE_EXECUTION != -1) || (EXCEPTION_CONTINUE_SEARCH != 0) || \
    (EXCEPTION_EXECUTE_HANDLER != 1)
#error "VEH disposition macros must be preprocessor-safe integers (-1/0/1)"
#endif

/* A vectored exception handler. CRITICAL ABI: a compiled PE handler uses the
 * MICROSOFT x64 calling convention (RCX = ExceptionInfo + 32-byte home space),
 * NOT the kernel's SysV (x86_64-elf), and returns a 32-bit Windows LONG. The
 * pointer is therefore ms_abi so a call through it marshals the argument the
 * way a real ntdll handler expects, and the return is int32_t -- a bare `long`
 * is 64-bit under LP64 and would widen EXCEPTION_CONTINUE_EXECUTION (-1) to
 * 0xFFFFFFFFFFFFFFFF in RAX, so it would never compare equal. Every synthetic
 * test handler MUST also be ms_abi (see rtl/unwind.h PEXCEPTION_ROUTINE note).
 * Node-layout asserts cannot catch either error, so the ABI test invokes a
 * handler through this typedef. */
typedef int32_t (__attribute__((ms_abi)) *PVECTORED_EXCEPTION_HANDLER)(
    EXCEPTION_POINTERS *ExceptionInfo);

/* VEH / VCH list node -- the IMPOSSIBLE OS shared ABI, agreed between the kernel
 * (publisher) and Impossible OS's own ntdll (consumer). Impossible OS's ntdll
 * allocates these from its process heap and links them into its process-global
 * VEH list.
 *
 * ONE node type, TWO ntdll list heads. The Vectored Continue Handler (VCH) list
 * reuses this EXACT type; VCH is not a new kernel struct and has no second kernel
 * anchor. VEH runs before frame-based (SEH) handlers, VCH after a handler has
 * chosen to continue; both are per-PROCESS ring-3 state whose list heads + locks
 * live in the ntdll user runtime, never in a kernel / TEB / PEB field. The kernel
 * neither anchors, reads, nor walks either list --
 * there is no ki_call_veh_list / ki_call_vch_list in ring 0. The kernel's whole
 * contribution to both mechanisms is this single canonical node layout.
 *
 * This is the CURRENT minimal shape and is NOT byte-for-byte with Microsoft's
 * ntdll (a modern Microsoft entry is a ~0x28 node carrying reference/lock
 * metadata and an EncodePointer-obfuscated handler so it can safely defer
 * freeing a node whose handler is mid-dispatch). Impossible OS does not load
 * Microsoft's ntdll, so the byte layout is a private contract between the kernel
 * (publisher) and Impossible OS's own ntdll (consumer). The kernel defines the
 * canonical layout so the two sides cannot drift; it never allocates or walks a
 * node, and this _Static_assert fires if either side extends the node.
 *
 * OPEN DESIGN (owned by the ntdll VEH implementation, not the kernel): a VEH
 * handler is arbitrary code that may call RemoveVectoredExceptionHandler (self
 * or another node) or fault into nested dispatch, so a re-entrancy-safe dispatch
 * protocol -- deferred reclamation / dispatch-depth tracking, or per-node
 * lifetime state -- is required and unresolved. IF that design adds per-node
 * lifetime state, this node grows and BOTH sides update together. The 24-byte
 * shape is therefore provisional, not a safety guarantee. */
typedef struct _VECTORED_HANDLER_ENTRY {
    LIST_ENTRY                  List;    /* 0x00: links in the ntdll process-global list */
    PVECTORED_EXCEPTION_HANDLER Handler; /* 0x10: the registered callback */
} VECTORED_HANDLER_ENTRY;                /* total: 0x18 = 24 bytes */

/* Layer 1 -- pin the shared VEH node ABI so the kernel and ntdll cannot drift. */
_Static_assert(sizeof(VECTORED_HANDLER_ENTRY) == 24, "VECTORED_HANDLER_ENTRY must be 24 bytes (Impossible OS VEH ABI)");
_Static_assert(_Alignof(VECTORED_HANDLER_ENTRY) == 8, "VECTORED_HANDLER_ENTRY must be 8-byte aligned");
_Static_assert(__builtin_offsetof(VECTORED_HANDLER_ENTRY, List)    == 0x00, "VEH node List at 0x00");
_Static_assert(__builtin_offsetof(VECTORED_HANDLER_ENTRY, Handler) == 0x10, "VEH node Handler at 0x10");

/* --- Frame <-> CONTEXT conversion ----------------------------------------
 *
 * An interrupt_frame physically carries only the CONTROL and INTEGER register
 * groups. It has no DS/ES/FS/GS, no debug registers and no FPU state, so those
 * groups can never be honoured from a frame alone:
 *
 *   - Reading them live would capture the CURRENT cpu, not the frame's thread.
 *   - Executing FXSAVE would, under the lazy-FPU scheme (sched/task.c
 *     nm_handler), save whichever task last touched the FPU -- a cross-task
 *     data leak.
 *
 * Both converters therefore serve the frame-backed groups only, and report what
 * they actually did instead of echoing the caller's requested mask.
 * Per-thread FPU/segment/debug capture belongs to NtGetContextThread
 * (the process/thread native-API work). The panic path keeps its own live-capture
 * route (panic_build_context), which is sound because a panic has stopped the
 * world and deliberately wants the current cpu state. */

/* Populate `ctx` from `frame`. `ctx->ContextFlags` is the REQUESTED mask on
 * entry. Only CONTEXT_CONTROL and CONTEXT_INTEGER can be satisfied; any other
 * requested group is zeroed (FltSave to architectural init state) and its bit
 * is CLEARED from ctx->ContextFlags, so a caller can never mistake fabricated
 * values for captured ones.
 * Returns the mask actually captured (equal to the resulting ctx->ContextFlags).
 * Returns 0 and touches nothing if either pointer is NULL. */
uint32_t context_from_frame(const struct interrupt_frame *frame, CONTEXT *ctx);

/* Restore `frame` from `ctx`, honouring ctx->ContextFlags -- never blindly.
 * Only CONTEXT_CONTROL and CONTEXT_INTEGER are restorable into a frame; other
 * groups are ignored. Returns the mask actually restored, or 0 on NULL.
 *
 * TRUSTED INPUT ONLY. This copier performs NO validation: it will place any
 * RIP/RSP/CS/SS/RFLAGS it is given into an IRETQ frame. It is safe only for
 * ring-0 callers supplying a kernel-produced CONTEXT. A ring-3-supplied CONTEXT
 * MUST go through the checked path -- previous mode, canonical RIP/RSP,
 * user-mode CS/SS selectors with RPL 3, and sanitised RFLAGS (IOPL/NT/VM
 * cleared, reserved bit 1 forced) -- which is owned by NtContinue (the ring-3
 * exception-delivery path). Do not call this directly from a syscall path. */
uint32_t frame_from_context(const CONTEXT *ctx, struct interrupt_frame *frame);

/* Fill `ctx->FltSave` with the architectural initial FPU state (FCW=0x037F,
 * MXCSR=0x1F80). Used for a CONTEXT whose FPU group was never captured: raw
 * zeroes would unmask every FP exception on a restore. */
void context_init_fpu_state(CONTEXT *ctx);

/* --- Exception dispatch ABI --------------------------------------------------
 *
 * Declared by the #PF triage; implemented by the exception-dispatcher, ring-3
 * delivery, and kernel-SEH stages that follow. KPROCESSOR_MODE is the NT
 * previous-mode enum; the canonical definition moves to a shared NT types
 * header when the native-API surface matures. Keep the values pinned (0/1) --
 * task.previous_mode already uses this encoding (0 = KernelMode, 1 = UserMode). */
typedef enum { KernelMode = 0, UserMode = 1 } KPROCESSOR_MODE;

/* Dispatch disposition. The #PF triage and every later dispatch stage agree on
 * this contract so the seam is stable across the dispatcher / ring-3 delivery /
 * kernel-SEH stages:
 *   UNHANDLED -- nobody resolved the exception; the CALLER performs the
 *                terminal action (deliver to ring-3 later, or panic now).
 *   HANDLED   -- resolved; the live `frame` MUST carry the final resume/delivery
 *                state before returning (the caller returns it through IRET and
 *                never re-applies `ctx`). A dispatcher that works in `ctx` must
 *                write it back via frame_from_context first -- returning HANDLED
 *                with only `ctx` updated would IRET the unchanged frame and
 *                refault in a loop.
 * The triage stub returns UNHANDLED, so today every fault the pager chain
 * declined terminates -- exactly the pre-triage behavior, but now routed
 * through the durable dispatch contract instead of an inline panic. */
typedef enum {
    KI_EXCEPTION_UNHANDLED = 0,
    KI_EXCEPTION_HANDLED   = 1,
} KI_EXCEPTION_DISPOSITION;

/* Master user-mode exception dispatcher. The full sequence (debugger
 * first-chance -> ring-3 handover via KiUserExceptionDispatcher -> second-chance
 * -> terminate) is owned by the dispatcher / ring-3 delivery stages; the #PF
 * triage ships a stub that returns UNHANDLED. `frame` is the live interrupt
 * frame: a HANDLED return means it was rewritten to the resume/delivery point.
 * `first_chance` is 1 on the initial raise. */
KI_EXCEPTION_DISPOSITION ki_dispatch_exception(EXCEPTION_RECORD *rec, CONTEXT *ctx,
                                               struct interrupt_frame *frame,
                                               KPROCESSOR_MODE mode, int first_chance);

/* Kernel-mode exception raise -- walks the per-thread kernel exception chain and
 * runs SEH filters. Owned by the kernel-mode __try/__except stage; the #PF
 * triage ships a stub that returns UNHANDLED so an unresolved kernel fault stays
 * terminal (panic) as it is today. */
KI_EXCEPTION_DISPOSITION ki_raise_kernel_exception(EXCEPTION_RECORD *rec, CONTEXT *ctx,
                                                   struct interrupt_frame *frame);

/* Kernel-debugger dispatch callback (NT's KiDebugRoutine). Called first- and
 * second-chance for kernel-mode exceptions. Returns nonzero if the debugger
 * resolved the exception: it MUST have rewritten the live `frame` to the resume
 * point (the KI_EXCEPTION_HANDLED frame-ownership contract -- ki_dispatch_exception
 * returns HANDLED without re-applying `ctx`). Set by KD attach; default NULL (no
 * debugger, notification skipped). -> XREF: TODO-29 KD kernel-debugger attach. */
typedef int (*KI_DEBUG_ROUTINE)(EXCEPTION_RECORD *rec, CONTEXT *ctx,
                                struct interrupt_frame *frame,
                                KPROCESSOR_MODE mode, int first_chance);

/* Publish the kernel-debugger callback (atomic release store). Passing NULL
 * detaches. -> XREF: TODO-29 KD kernel-debugger attach. */
void ki_set_debug_routine(KI_DEBUG_ROUTINE routine);

/* User-mode debug-port forward (NT's DbgkForwardException). Returns nonzero if a
 * debug port existed and continued execution (frame rewritten to resume). Stub
 * returns 0 (no debug port) until NtDebugActiveProcess lands.
 * -> XREF: TODO-29 user-mode debug port (NtDebugActiveProcess). */
int DbgkForwardException(EXCEPTION_RECORD *rec, CONTEXT *ctx, int first_chance);

/* Select the kernel-mode terminal bugcheck code: BUGCHECK_SYSTEM_SERVICE_EXCEPTION
 * (0x3B) when the fault occurred inside a user-originated system service (per-thread
 * in_system_service depth > 0), else BUGCHECK_KMODE_EXCEPTION_NOT_HANDLED (0x1E).
 * Pure/side-effect-free (unit-testable without invoking the terminal). Returns the
 * raw STOP code as uint32_t so this header stays independent of bugcheck.h. */
uint32_t ki_kernel_bugcheck_code(void);

/* Fill the four KeBugCheckEx parameters for the kernel-mode terminal and return
 * the selected STOP code. The layout is per-code (0x1E: code/addr/info0/info1;
 * 0x3B: code/instr-addr/CONTEXT-addr/0), matching NT so a dump consumer reads the
 * right slots. Pure/side-effect-free (unit-testable without invoking the terminal). */
uint32_t ki_kernel_bugcheck_params(EXCEPTION_RECORD *rec, CONTEXT *ctx, uint64_t params[4]);

/* --- Section 16: Exception Dispatch Telemetry -------------------------------
 *
 * Records each exception-dispatch decision the KERNEL can observe -- the ring-3
 * boundary: a debugger handoff, or the unhandled decline to ring-3 -- as a flat
 * JSON structured-log event, per-process rate-limited and compile-time gated.
 * The full ring-3 VEH -> SEH -> VCH handler chain runs in ntdll (there is NO
 * ring-0 VEH/VCH walker), so that chain's per-handler telemetry is owned by
 * 12-user-platform-sdk/TODO-04 s5, which shares this event schema.
 * ------------------------------------------------------------------------- */

/* Default the compile-time gate ON when the build did not set it. The Makefile
 * EXCEPT_TELEMETRY=on/off flavor emits -DCONFIG_EXCEPT_TELEMETRY=1 / =0. This
 * MUST be tested with #if (NOT #ifdef): a =0 build has to genuinely compile the
 * hook and the "exception_dispatch" event string out of the image, and #ifdef is
 * true even for a macro defined as 0. */
#ifndef CONFIG_EXCEPT_TELEMETRY
#define CONFIG_EXCEPT_TELEMETRY 1
#endif

/* Per-process telemetry rate limit: at most EXCEPT_TELEM_MAX_PER_WINDOW dispatch
 * events per EXCEPT_TELEM_WINDOW_MS window, so a process spraying intentional
 * exceptions (guard-page probing, a runaway JIT) cannot flood the structured log.
 * EXCEPT_TELEM_GLOBAL_MAX is a SYSTEM-WIDE aggregate ceiling on the same window,
 * checked after the per-process gate, so a farm of processes each within its own
 * budget cannot raise the aggregate telemetry RATE without bound. This is a
 * THROUGHPUT mitigation ONLY: 256/window slows a storm to a fraction of a single
 * subsystem's default klog rate. It does NOT reserve klog-ring capacity -- klog is
 * a continuously-overwritten 1000-entry ring, so a sustained storm can still churn
 * telemetry through most slots over several windows. GUARANTEED diagnostic
 * retention (occupancy reservation, priority lanes, async drain) is owned by the
 * per-CPU lockless logging rework (-> XREF: 02-kernel-core/TODO-32). */
#define EXCEPT_TELEM_WINDOW_MS       1000u
#define EXCEPT_TELEM_MAX_PER_WINDOW  100u
#define EXCEPT_TELEM_GLOBAL_MAX      256u

#if CONFIG_EXCEPT_TELEMETRY
/* Pure JSON formatter (unit-testable, no I/O): writes a flat
 * {"type":"exception_dispatch",...} object into buf and returns its length
 * (excluding the NUL), or 0 if buf is too small. `frames_unwound` is emitted as
 * null at the kernel boundary -- no frames are unwound in ring 0; TODO-04 s5
 * supplies the real count from the ntdll table-based unwind. `handler_name` is
 * escaped defensively for JSON string safety. */
uint32_t except_format_dispatch_json(char *buf, uint32_t buflen,
                                     uint32_t code, uint64_t fault_addr,
                                     const char *handler_name, int disposition,
                                     uint32_t pid, uint32_t tid);

/* Pure rate gate (unit-testable): packed {window_ms:44, count:20} state updated
 * by a lock-free CAS so concurrent faults on two CPUs cannot lose an increment or
 * tear the window reset. Admits up to `max` events per window. Returns 1 if this
 * event is within budget (EMIT); returns 0 to DROP in two cases: the window is
 * saturated (>= max this window), OR the bounded CAS retry budget is exhausted
 * under heavy multi-CPU contention (a fault-context loop must not spin unbounded
 * with interrupts disabled -- dropping a best-effort telemetry event is the safe
 * outcome). now_ms is a coarse monotonic ms stamp (KeQueryInterruptTimeCoarse()/
 * 10000 in the live path). Drives BOTH the per-process gate
 * (max=EXCEPT_TELEM_MAX_PER_WINDOW) and the system-wide aggregate gate
 * (max=EXCEPT_TELEM_GLOBAL_MAX). The contention-exhaustion path is reachable only
 * under real concurrent faults (single-threaded callers never lose the CAS), so it
 * is validated on multi-CPU hardware, not the unit harness. */
int except_telem_rate_gate(volatile uint64_t *state, uint32_t now_ms, uint32_t max);

/* Emit one exception-dispatch telemetry event for the current process. Safe ONLY
 * on ring-3-originated fault legs (a ring-3 fault holds no kernel spinlock, so the
 * klog path is deadlock-free); NEVER call from ki_raise_kernel_exception (its
 * lock-free contract). Rate-limited per process; routed through klog_unrated() so
 * the shared "except.telem" subsystem cap cannot clip one process's events on
 * behalf of another. */
void except_log_dispatch(EXCEPTION_RECORD *rec, const char *handler_name, int disposition);
#else
#define except_log_dispatch(rec, handler_name, disposition) ((void)0)
#endif

/* --- General fault-to-exception mapping -------------------------------------
 *
 * Registers ISR handlers for the CPU fault vectors that map to a Windows
 * exception rather than an unconditional panic (#DE/#DB/#BP/#OF/#UD/#NP/#SS/#GP,
 * the CET #CP when the CPU supports CET shadow stacks OR IBT, and the ring-3
 * __fastfail INT 0x29). MUST be called from kernel init phase 1 AFTER idt_init() -- that is
 * where the kernel IDT is loaded and where handlers[] is (re)initialised, so a
 * registration done earlier (phase 0) would be erased by the idt_init() clear. */
void except_init(void);

/* Pure mapping helpers (no locks, no allocation, no logging) -- fault-context
 * safe and directly unit-testable, the same discipline as pf_build_access_violation. */

/* NTSTATUS a given fault vector delivers to user mode, or 0 (STATUS_SUCCESS) if
 * the vector is not one this mapping owns. */
NTSTATUS except_vector_to_status(uint8_t vector);

/* Non-zero if a kernel-mode fault on this vector is unconditionally terminal
 * (#DE/#OF/#UD -- no SEH recovery is ever attempted, per the mapping contract). */
int except_vector_kernel_fatal(uint8_t vector);

/* Build an EXCEPTION_RECORD + CONTROL/INTEGER CONTEXT for a general fault.
 * ExceptionAddress and the CONTEXT are taken from `frame`; NumberParameters is
 * 0 (callers that carry fault-specific parameters, e.g. the #CP/__fastfail
 * subcode, set them afterwards). context_from_frame() performs the full CONTEXT
 * scrub, so the caller only seeds ctx->ContextFlags with the frame-backed groups. */
void except_build_record(EXCEPTION_RECORD *rec, CONTEXT *ctx,
                         const struct interrupt_frame *frame,
                         NTSTATUS code, uint32_t exception_flags);

/* --- Kernel-mode structured exception handling (KI_TRY / KI_EXCEPT) ---------
 *
 * A driver-facing __try/__except facility for wrapping dangerous kernel work
 * (MMIO probes, DMA buffer reads) so a fault becomes a recoverable branch
 * instead of a bugcheck. Owned by TODO-23 kernel-mode __try/__except.
 *
 * Why not the MSVC __try/__except keywords: the kernel is built with
 * clang-19 --target=x86_64-elf (SysV/ELF), which does NOT lower the MSVC SEH
 * keywords and emits no .pdata/.xdata for them. So this is a REGISTRATION-LIST
 * facility, independent of compiler-emitted table unwind: each KI_TRY pushes a
 * stack-local KI_EXCEPTION_REGISTRATION onto the current thread's chain and
 * captures a setjmp-style landing pad; a fault walks the chain and resumes at
 * the innermost matching handler by rewriting the trap frame (the same
 * KI_EXCEPTION_HANDLED frame-ownership contract the dispatcher already uses).
 *
 * Scope of this v1 (a deliberately limited facility):
 *   - __try/__except with an optional filter. filter == NULL is an
 *     unconditional EXECUTE_HANDLER (the common "catch any fault" driver case).
 *   - NO __try/__finally termination handlers and NO EXCEPTION_COLLIDED_UNWIND
 *     two-pass unwind -- those extend the table-based RtlUnwindEx work and are
 *     tracked in TODO-23 (RtlUnwindEx section).
 *
 * SMP + lifetime safety:
 *   - The chain head lives in `struct thread` so it FOLLOWS a thread across a
 *     CPU migration. Because thread_current() still resolves a process-global
 *     cursor (the per-CPU current-thread cursor is TODO-07 SMP phase-2), a fault
 *     on one CPU could read a DIFFERENT thread's chain. ki_raise_kernel_exception
 *     snapshots thread_current() EXACTLY ONCE and validates the trap frame RSP
 *     (and every node it walks) against that one thread's kernel-stack bounds: a
 *     cursor that does not match the faulting stack fails the check and the fault
 *     stays TERMINAL (safe decline) rather than IRET-ing into another thread. The
 *     per-CPU cursor only improves the SMP recovery RATE; it is not a safety
 *     prerequisite. -> XREF: 03-memory-concurrency/TODO-07 SMP phase-2.
 *   - A non-local exit (return/break/goto) out of a KI_TRY body is FORBIDDEN --
 *     it would leave a stale node published on the thread chain. KI_EXCEPTION_FRAME
 *     gives the node a clang `cleanup` handler (ki_seh_auto_pop) that pops it on
 *     ANY scope exit, and ki_raise revalidates node bounds + chain ordering, so
 *     the ban is defense-in-depth rather than the sole control.
 */

/* setjmp-style landing pad. ASSEMBLY-VISIBLE: the byte offsets below are
 * hardcoded in src/kernel/except_seh.asm (ki_seh_setjmp) and pinned by
 * _Static_assert (Layer 1). Rsp is the caller RSP AFTER ki_seh_setjmp returns
 * (post-`ret`); Rip is the return address into the KI_TRY caller. */
typedef struct ki_jmp_buf {
    uint64_t Rbx;   /* 0x00 */
    uint64_t Rbp;   /* 0x08 */
    uint64_t R12;   /* 0x10 */
    uint64_t R13;   /* 0x18 */
    uint64_t R14;   /* 0x20 */
    uint64_t R15;   /* 0x28 */
    uint64_t Rsp;   /* 0x30 -- caller RSP after ki_seh_setjmp returns */
    uint64_t Rip;   /* 0x38 -- return address into the KI_TRY caller */
} KI_JMP_BUF;

_Static_assert(__builtin_offsetof(KI_JMP_BUF, Rbx) == 0x00, "KI_JMP_BUF.Rbx @ 0x00");
_Static_assert(__builtin_offsetof(KI_JMP_BUF, Rbp) == 0x08, "KI_JMP_BUF.Rbp @ 0x08");
_Static_assert(__builtin_offsetof(KI_JMP_BUF, R12) == 0x10, "KI_JMP_BUF.R12 @ 0x10");
_Static_assert(__builtin_offsetof(KI_JMP_BUF, R13) == 0x18, "KI_JMP_BUF.R13 @ 0x18");
_Static_assert(__builtin_offsetof(KI_JMP_BUF, R14) == 0x20, "KI_JMP_BUF.R14 @ 0x20");
_Static_assert(__builtin_offsetof(KI_JMP_BUF, R15) == 0x28, "KI_JMP_BUF.R15 @ 0x28");
_Static_assert(__builtin_offsetof(KI_JMP_BUF, Rsp) == 0x30, "KI_JMP_BUF.Rsp @ 0x30");
_Static_assert(__builtin_offsetof(KI_JMP_BUF, Rip) == 0x38, "KI_JMP_BUF.Rip @ 0x38");
_Static_assert(sizeof(KI_JMP_BUF) == 0x40, "KI_JMP_BUF is 64 bytes");

/* __except filter. Returns EXCEPTION_EXECUTE_HANDLER (1), EXCEPTION_CONTINUE_SEARCH
 * (0), or EXCEPTION_CONTINUE_EXECUTION (-1). Runs in fault context (klog-free,
 * allocation-free); a fault RAISED inside a filter is a collided exception and
 * bugchecks. */
typedef int (*KI_EXCEPTION_FILTER)(NTSTATUS code, void *fault_addr, void *ctx);

/* Owner thread of a registration; forward-declared so this header stays free of
 * the scheduler include (full type in kernel/sched/task.h). */
struct thread;

/* Per-KI_TRY registration node. Allocated on the caller kernel stack; linked
 * newest-first on struct thread.kernel_exception_list. */
typedef struct ki_exception_registration {
    struct ki_exception_registration *prev;  /* next-older frame (NULL = base) */
    struct thread      *owner;                /* thread whose stack holds this node +
                                               * whose chain it is linked on; fixed at
                                               * publication so deregistration is
                                               * owner-stable across a cursor change */
    KI_JMP_BUF          jmp;                  /* landing pad (ki_seh_setjmp target) */
    KI_EXCEPTION_FILTER filter;               /* NULL = unconditional EXECUTE_HANDLER */
    void               *filter_ctx;           /* opaque cookie handed to the filter */
    NTSTATUS            code;                  /* fault code delivered to the handler */
    void               *fault_addr;           /* faulting address (best-effort) */
    uint8_t             linked;               /* 1 while on the chain (idempotent pop) */
} KI_EXCEPTION_REGISTRATION;

/* Capture the callee-saved regs + caller RSP/RIP into `buf`, return 0. The
 * "second return" (value 1) is synthesized by ki_raise_kernel_exception
 * rewriting the trap frame; this routine never itself returns 1. Declared
 * returns_twice so clang does not cache locals across the call assuming a single
 * return (SEH correctness under -O2). Implemented in except_seh.asm. */
int ki_seh_setjmp(KI_JMP_BUF *buf) __attribute__((returns_twice));

/* Push / pop a registration on thread_current()->kernel_exception_list. Normal
 * (non-fault) context only; ki_raise pops nodes it resolves. ki_seh_auto_pop is
 * the clang cleanup handler (idempotent: no-op once `linked` is clear). */
void ki_seh_register(KI_EXCEPTION_REGISTRATION *reg);
void ki_seh_deregister(KI_EXCEPTION_REGISTRATION *reg);
void ki_seh_auto_pop(KI_EXCEPTION_REGISTRATION *reg);
/* Pointer-indirection form for the KI_TRY do-block cleanup guard (clang passes
 * the address of the guarded `const` variable). Fires on every exit from the
 * construct. */
void ki_seh_auto_pop_ptr(KI_EXCEPTION_REGISTRATION *const *reg);

/* Declare a KI_TRY registration frame with automatic cleanup. MUST be used to
 * declare the node so a non-local exit cannot leak it onto the thread chain. */
#define KI_EXCEPTION_FRAME(reg) \
    KI_EXCEPTION_REGISTRATION reg __attribute__((cleanup(ki_seh_auto_pop))) = { 0 }

/* Structured-exception blocks. Usage (no user braces):
 *     KI_EXCEPTION_FRAME(reg);
 *     KI_TRY(reg)
 *         *(volatile uint32_t *)mmio = 0;   // may fault
 *     KI_EXCEPT(reg)
 *         // recovered; KI_EXCEPTION_CODE(reg) holds the NTSTATUS
 *     KI_END_TRY;
 * Non-local exits (return/break/goto/continue) out of the KI_TRY body are
 * discouraged, but the do-block cleanup guard (`_ki_seh_g`) pops the node on ANY
 * exit from the construct, so a non-local exit cannot leave a stale registration
 * on the chain. Non-volatile locals modified in the body are indeterminate in the
 * handler (the setjmp contract). */
#define KI_TRY(reg)                                              \
    do {                                                         \
        (reg).filter     = (KI_EXCEPTION_FILTER)0;               \
        (reg).filter_ctx = (void *)0;                            \
        (reg).code       = 0;                                    \
        (reg).fault_addr = (void *)0;                            \
        ki_seh_register(&(reg));                                 \
        KI_EXCEPTION_REGISTRATION *const _ki_seh_g                \
            __attribute__((cleanup(ki_seh_auto_pop_ptr))) = &(reg); \
        (void)_ki_seh_g;                                         \
        if (ki_seh_setjmp(&(reg).jmp) == 0) {

/* Variant with a filter expression callback + opaque cookie. */
#define KI_TRY_FILTER(reg, fn, cookie)                           \
    do {                                                         \
        (reg).filter     = (fn);                                 \
        (reg).filter_ctx = (cookie);                             \
        (reg).code       = 0;                                    \
        (reg).fault_addr = (void *)0;                            \
        ki_seh_register(&(reg));                                 \
        KI_EXCEPTION_REGISTRATION *const _ki_seh_g                \
            __attribute__((cleanup(ki_seh_auto_pop_ptr))) = &(reg); \
        (void)_ki_seh_g;                                         \
        if (ki_seh_setjmp(&(reg).jmp) == 0) {

#define KI_EXCEPT(reg)                                           \
            ki_seh_deregister(&(reg));                           \
        } else {

#define KI_END_TRY                                               \
        }                                                        \
    } while (0)

/* NTSTATUS + address delivered to the handler body (valid in a KI_EXCEPT block). */
#define KI_EXCEPTION_CODE(reg)  ((reg).code)
#define KI_EXCEPTION_ADDR(reg)  ((reg).fault_addr)

#ifdef KERNEL_TESTS
/* Test seams over the internal fault-address selectors. See the contract at
 * their definitions in src/kernel/except.c.
 *
 * ki_probe_exception_data_address() reports PRESENCE via its return value and
 * writes the address through `out`, so a genuine access violation AT address 0
 * (a NULL dereference, the commonest fault there is) stays distinguishable from
 * a record that carries no address at all. ki_probe_exception_fault_address()
 * is the handler-facing form: the data address when there is one, the faulting
 * instruction otherwise.
 *
 * XREF: 00-infrastructure/TODO-03-kernel-test-harness.md section 11 */
int      ki_probe_exception_data_address(const EXCEPTION_RECORD *rec, uint64_t *out);
uint64_t ki_probe_exception_fault_address(const EXCEPTION_RECORD *rec);
#endif
