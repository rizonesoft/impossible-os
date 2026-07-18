/* ============================================================================
 * rtl/unwind.h -- x64 PE32+ table-based unwind ABI + engine (RtlVirtualUnwind)
 *
 * Canonical owner of the x86-64 SEH/table-based unwind EXECUTION engine:
 * RtlLookupFunctionEntry, RtlVirtualUnwind, RtlPcToFileHeader, and the dynamic
 * (JIT / no-backing-image) function-table registry (RtlAddFunctionTable /
 * RtlDeleteFunctionTable / RtlInstallFunctionTableCallback).
 *
 * Ownership split (TODO-23 s6 / TODO-18 s5, ratified in the TODO): T23 owns the
 * unwind execution + lookup engine (this file); T18 owns unwind-metadata
 * lifetime + a normalized sorted representation + load-time record validation;
 * T17 owns parsing image sections into loaded_module_t.pdata_base/size.
 *
 * The kernel image is ELF (no .pdata); RtlLookupFunctionEntry returns NULL for a
 * kernel-mode PC that has no registered entry. Production kernel-mode unwind
 * metadata (ELF .eh_frame -> normalized table) is a separate provider owned by
 * TODO-18 s5; this engine unwinds any module or dynamic table that DOES carry
 * PE-shaped RUNTIME_FUNCTION/UNWIND_INFO records.
 *
 * ARCH: x86-64 -- RUNTIME_FUNCTION / UNWIND_INFO / UWOP_* are the PE/COFF AMD64
 * exception mechanism by definition.
 *
 * Reference: Microsoft "x64 exception handling" (learn.microsoft.com,
 *            exception-handling-x64), Windows SDK winnt.h (AMD64),
 *            Windows Internals 7e ch. 8.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"
#include "kernel/except.h"   /* CONTEXT, M128A, XMM_SAVE_AREA32 */

/* --- RUNTIME_FUNCTION -- one .pdata entry (12 bytes) ---------------------- */

typedef struct _RUNTIME_FUNCTION {
    uint32_t BeginAddress;       /* RVA of function start (relative to image base) */
    uint32_t EndAddress;         /* RVA of function end (exclusive) */
    uint32_t UnwindInfoAddress;  /* RVA of UNWIND_INFO; low bit set => chained RUNTIME_FUNCTION */
} RUNTIME_FUNCTION, *PRUNTIME_FUNCTION;

_Static_assert(sizeof(RUNTIME_FUNCTION) == 12, "RUNTIME_FUNCTION is 12 bytes (.pdata entry)");
_Static_assert(_Alignof(RUNTIME_FUNCTION) == 4, "RUNTIME_FUNCTION is DWORD-aligned");
_Static_assert(__builtin_offsetof(RUNTIME_FUNCTION, BeginAddress) == 0, "BeginAddress at 0");
_Static_assert(__builtin_offsetof(RUNTIME_FUNCTION, EndAddress) == 4, "EndAddress at 4");
_Static_assert(__builtin_offsetof(RUNTIME_FUNCTION, UnwindInfoAddress) == 8, "UnwindInfoAddress at 8");

/* A UnwindInfoAddress with bit 0 set is a chained-info pointer: the low bit is
 * masked off and the result is the RVA of the PARENT RUNTIME_FUNCTION. */
#define RUNTIME_FUNCTION_CHAINED(rf)   (((rf)->UnwindInfoAddress & 1u) != 0u)

/* --- UNWIND_CODE -- one prolog operation slot (2 bytes) ------------------- */

/* Byte layout (little-endian, per the AMD64 unwind spec):
 *   byte 0: CodeOffset  (offset in the prolog where this op takes effect)
 *   byte 1: low nibble  = UnwindOp (UWOP_*), high nibble = OpInfo
 * Some ops interpret the whole 2-byte slot as a u16 FrameOffset instead.
 * Accessed via explicit masks (not C bitfields) so slot decoding is independent
 * of the compiler's bitfield packing order. */
typedef union _UNWIND_CODE {
    struct {
        uint8_t CodeOffset;
        uint8_t OpAndInfo;       /* UnwindOp:lo4 | OpInfo:hi4 */
    } b;
    uint16_t FrameOffset;
} UNWIND_CODE;

_Static_assert(sizeof(UNWIND_CODE) == 2, "UNWIND_CODE is 2 bytes");
_Static_assert(_Alignof(UNWIND_CODE) == 2, "UNWIND_CODE is USHORT-aligned");
_Static_assert(__builtin_offsetof(UNWIND_CODE, b.CodeOffset) == 0, "UNWIND_CODE CodeOffset at 0");
_Static_assert(__builtin_offsetof(UNWIND_CODE, b.OpAndInfo) == 1, "UNWIND_CODE OpAndInfo at 1");
_Static_assert(__builtin_offsetof(UNWIND_CODE, FrameOffset) == 0, "UNWIND_CODE FrameOffset aliases at 0");

#define UNWIND_CODE_OP(uc)     ((uint8_t)((uc).b.OpAndInfo & 0x0Fu))
#define UNWIND_CODE_INFO(uc)   ((uint8_t)(((uc).b.OpAndInfo >> 4) & 0x0Fu))

/* --- UWOP opcodes (AMD64 unwind operation codes) -------------------------- */

#define UWOP_PUSH_NONVOL      0   /* OpInfo = pushed integer register number */
#define UWOP_ALLOC_LARGE      1   /* OpInfo 0: size = slot[1]*8; OpInfo 1: size = u32(slot[1..2]) */
#define UWOP_ALLOC_SMALL      2   /* size = OpInfo*8 + 8 */
#define UWOP_SET_FPREG        3   /* RSP = FrameRegister - FrameOffset*16 */
#define UWOP_SAVE_NONVOL      4   /* OpInfo = reg; offset = slot[1]*8 */
#define UWOP_SAVE_NONVOL_FAR  5   /* OpInfo = reg; offset = u32(slot[1..2]) */
#define UWOP_EPILOG           6   /* v2 epilog descriptor (skipped by the engine) */
#define UWOP_SPARE_CODE       7   /* reserved */
#define UWOP_SAVE_XMM128      8   /* OpInfo = xmm reg; offset = slot[1]*16 */
#define UWOP_SAVE_XMM128_FAR  9   /* OpInfo = xmm reg; offset = u32(slot[1..2]) */
#define UWOP_PUSH_MACHFRAME  10   /* OpInfo 0: no error code; OpInfo 1: with error code */

/* --- UNWIND_INFO -- per-function unwind descriptor ------------------------ */

/* Header layout (little-endian):
 *   byte 0: Version:lo3 | Flags:hi5      (Flags = UNW_FLAG_*)
 *   byte 1: SizeOfProlog
 *   byte 2: CountOfCodes                 (number of UNWIND_CODE slots)
 *   byte 3: FrameRegister:lo4 | FrameOffset:hi4
 * followed by CountOfCodes UNWIND_CODE slots, then (2-slot aligned) the handler
 * RVA + language-specific data, or (if UNW_FLAG_CHAININFO) a RUNTIME_FUNCTION. */
typedef struct _UNWIND_INFO {
    uint8_t VersionAndFlags;
    uint8_t SizeOfProlog;
    uint8_t CountOfCodes;
    uint8_t FrameRegAndOff;
    UNWIND_CODE UnwindCode[1];   /* variable-length: CountOfCodes entries */
} UNWIND_INFO, *PUNWIND_INFO;

_Static_assert(__builtin_offsetof(UNWIND_INFO, VersionAndFlags) == 0, "UNWIND_INFO VersionAndFlags at 0");
_Static_assert(__builtin_offsetof(UNWIND_INFO, SizeOfProlog) == 1, "UNWIND_INFO SizeOfProlog at 1");
_Static_assert(__builtin_offsetof(UNWIND_INFO, CountOfCodes) == 2, "UNWIND_INFO CountOfCodes at 2");
_Static_assert(__builtin_offsetof(UNWIND_INFO, FrameRegAndOff) == 3, "UNWIND_INFO FrameRegAndOff at 3");
_Static_assert(__builtin_offsetof(UNWIND_INFO, UnwindCode) == 4, "UnwindCode array at offset 4");
/* sizeof pins the minimum record (4-byte header + one 2-byte code slot) used by
 * the metadata bounds checks; the DWORD placement the AMD64 spec requires for
 * .xdata records is a PE-format property guaranteed by the loader, not this
 * overlay struct's natural (2-byte) alignment. */
_Static_assert(sizeof(UNWIND_INFO) == 6, "UNWIND_INFO minimum record is 6 bytes (header + 1 code slot)");
_Static_assert(_Alignof(UNWIND_INFO) == 2, "UNWIND_INFO overlay alignment (uint16 code slot)");

#define UNWIND_INFO_VERSION(ui)   ((uint8_t)((ui)->VersionAndFlags & 0x07u))
#define UNWIND_INFO_FLAGS(ui)     ((uint8_t)(((ui)->VersionAndFlags >> 3) & 0x1Fu))
#define UNWIND_INFO_FRAMEREG(ui)  ((uint8_t)((ui)->FrameRegAndOff & 0x0Fu))
#define UNWIND_INFO_FRAMEOFF(ui)  ((uint8_t)(((ui)->FrameRegAndOff >> 4) & 0x0Fu))

/* --- UNW_FLAG_* -- UNWIND_INFO flags / RtlVirtualUnwind handler-type mask -- */

#define UNW_FLAG_NHANDLER   0x0   /* no handler */
#define UNW_FLAG_EHANDLER   0x1   /* has an exception handler (scan pass) */
#define UNW_FLAG_UHANDLER   0x2   /* has a termination handler (unwind pass) */
#define UNW_FLAG_CHAININFO  0x4   /* this info chains to a parent RUNTIME_FUNCTION */

/* --- SCOPE_TABLE -- language-specific handler data (__try/__except) -------- */

typedef struct _SCOPE_TABLE_ENTRY {
    uint32_t BeginAddress;    /* RVA of guarded region start */
    uint32_t EndAddress;      /* RVA of guarded region end */
    uint32_t HandlerAddress;  /* RVA of filter (except) or 1 (finally) */
    uint32_t JumpTarget;      /* RVA of __except block (0 for finally) */
} SCOPE_TABLE_ENTRY;

typedef struct _SCOPE_TABLE {
    uint32_t Count;
    SCOPE_TABLE_ENTRY ScopeRecord[1];   /* Count entries */
} SCOPE_TABLE, *PSCOPE_TABLE;

_Static_assert(sizeof(SCOPE_TABLE_ENTRY) == 16, "SCOPE_TABLE_ENTRY is 16 bytes");
_Static_assert(_Alignof(SCOPE_TABLE_ENTRY) == 4, "SCOPE_TABLE_ENTRY DWORD-aligned");
_Static_assert(_Alignof(SCOPE_TABLE) == 4, "SCOPE_TABLE DWORD-aligned");
_Static_assert(__builtin_offsetof(SCOPE_TABLE_ENTRY, BeginAddress) == 0, "SCOPE_TABLE_ENTRY BeginAddress at 0");
_Static_assert(__builtin_offsetof(SCOPE_TABLE_ENTRY, EndAddress) == 4, "SCOPE_TABLE_ENTRY EndAddress at 4");
_Static_assert(__builtin_offsetof(SCOPE_TABLE_ENTRY, HandlerAddress) == 8, "SCOPE_TABLE_ENTRY HandlerAddress at 8");
_Static_assert(__builtin_offsetof(SCOPE_TABLE_ENTRY, JumpTarget) == 12, "SCOPE_TABLE_ENTRY JumpTarget at 12");
_Static_assert(__builtin_offsetof(SCOPE_TABLE, Count) == 0, "SCOPE_TABLE Count at 0");
_Static_assert(__builtin_offsetof(SCOPE_TABLE, ScopeRecord) == 4, "SCOPE_TABLE ScopeRecord at 4");
_Static_assert(sizeof(SCOPE_TABLE) == 20, "SCOPE_TABLE is 20 bytes (Count + one ScopeRecord)");

/* --- KNONVOLATILE_CONTEXT_POINTERS ---------------------------------------- *
 * RtlVirtualUnwind records, for each nonvolatile register it restored, the
 * address in the establisher frame where that register's value was saved. The
 * exception dispatcher writes through these to change a register in the parent
 * frame. Indexed by register number (integer regs in CONTEXT/ABI order, XMM by
 * XMM number). A NULL slot means "not saved by this frame". Passing NULL to
 * RtlVirtualUnwind disables recording. */
typedef struct _KNONVOLATILE_CONTEXT_POINTERS {
    M128A    *Xmm[16];       /* saved-XMM addresses, indexed by XMM number */
    uint64_t *Integer[16];   /* saved-GPR addresses, indexed by ABI reg number */
} KNONVOLATILE_CONTEXT_POINTERS;

_Static_assert(sizeof(KNONVOLATILE_CONTEXT_POINTERS) == 256, "KNONVOLATILE_CONTEXT_POINTERS is 256 bytes");
_Static_assert(_Alignof(KNONVOLATILE_CONTEXT_POINTERS) == 8, "KNONVOLATILE_CONTEXT_POINTERS pointer-aligned");
_Static_assert(__builtin_offsetof(KNONVOLATILE_CONTEXT_POINTERS, Xmm) == 0, "Xmm pointers at 0");
_Static_assert(__builtin_offsetof(KNONVOLATILE_CONTEXT_POINTERS, Integer) == 128, "Integer pointers at 128");

/* --- Dynamic function-table callback -------------------------------------- *
 * RtlInstallFunctionTableCallback registers a callback that lazily produces the
 * RUNTIME_FUNCTION for a control PC in a dynamic code region with no static
 * .pdata. The returned pointer's lifetime is the callback owner's contract. */
typedef PRUNTIME_FUNCTION (*PGET_RUNTIME_FUNCTION_CALLBACK)(uint64_t control_pc,
                                                            void *context);

/* ==========================================================================
 * Public engine API
 * ========================================================================== */

/* Initialize the unwind engine (arms the dynamic-table registry lock, logs a
 * one-line readiness banner). Called once during Phase 3 exec init. Idempotent. */
void rtl_unwind_init(void);

/* Find the RUNTIME_FUNCTION covering control_pc. Searches loaded-module .pdata
 * (via exec_find_module_by_pc) first, then the dynamic-table registry. On a
 * match: returns the entry (pointer into registry- or image-owned storage) and
 * writes the owning image/table base to *image_base. Returns NULL when no entry
 * covers control_pc (a leaf function, or a module -- e.g. the ELF kernel --
 * with no unwind metadata), OR when the covering entry is an INDIRECT (fragment)
 * entry (UnwindInfoAddress bit 0 set) -- those are not yet supported. A caller
 * then applies the leaf convention, which is memory-safe (it reads the valid
 * stack at RSP) but returns an imprecise RIP for a rare non-leaf ICF fragment;
 * full fragment support is a tracked follow-up. history_table is accepted for
 * ABI compatibility and currently unused (no caching layer yet). */
PRUNTIME_FUNCTION RtlLookupFunctionEntry(uint64_t control_pc, uint64_t *image_base,
                                         void *history_table);

/* Apply function_entry's UNWIND_INFO to context, stepping it from the frame at
 * control_pc to its caller's frame (updates Rip, Rsp, and any restored
 * nonvolatile registers). handler_type is a mask of UNW_FLAG_EHANDLER /
 * UNW_FLAG_UHANDLER selecting which language handler to return. On return:
 *   *establisher_frame = the frame base used (for handler dispatch),
 *   *handler_data      = language-specific handler data (if a handler matched),
 *   context_pointers   = filled with saved-register addresses (may be NULL).
 * Returns the exception/termination handler routine address (image VA) when the
 * requested handler is present, else NULL. Fails safe (leaves context minimally
 * advanced) on malformed metadata. */
void *RtlVirtualUnwind(uint32_t handler_type, uint64_t image_base, uint64_t control_pc,
                       PRUNTIME_FUNCTION function_entry, CONTEXT *context,
                       void **handler_data, uint64_t *establisher_frame,
                       KNONVOLATILE_CONTEXT_POINTERS *context_pointers);

/* Register a dynamic function table for generated code with no backing image.
 * The registry takes a private COPY of function_table[0..entry_count) (the
 * caller may free its array immediately). base_address is the image base the
 * entries' RVAs are relative to. Returns 1 on success, 0 on failure (bad args,
 * overlapping range, out-of-order/out-of-range entries, or OOM). */
int RtlAddFunctionTable(PRUNTIME_FUNCTION function_table, uint32_t entry_count,
                        uint64_t base_address);

/* Remove a previously RtlAddFunctionTable-registered table. function_table is
 * the SAME pointer passed to RtlAddFunctionTable (used as the identity key).
 * Unlinks the table under the registry lock and frees the private copy; it does
 * NOT block. Per the Windows RtlDeleteFunctionTable caller contract, the caller
 * must ensure no thread is concurrently unwinding through this table (holding a
 * returned entry pointer, or executing its callback) before deleting -- and, for
 * a callback table, before releasing the callback code/context. A callback may
 * safely unregister its own table from within itself. Returns 1 if a table was
 * removed, 0 if none matched. */
int RtlDeleteFunctionTable(PRUNTIME_FUNCTION function_table);

/* Register a lazy callback that produces RUNTIME_FUNCTIONs for [base_address,
 * base_address+length). table_identifier must have its low 2 bits set (Windows
 * convention distinguishing it from a plain base). out_of_process_dll is
 * accepted for ABI compatibility and must be NULL (in-process only). Returns 1
 * on success, 0 on failure. */
int RtlInstallFunctionTableCallback(uint64_t table_identifier, uint64_t base_address,
                                    uint32_t length, PGET_RUNTIME_FUNCTION_CALLBACK callback,
                                    void *context, const char *out_of_process_dll);

/* "Which module owns this PC" -- wraps exec_find_module_by_pc. Returns the
 * module's image base (also written to *base_of_image when non-NULL), or NULL if
 * no loaded module contains pc_value. Consumed by crash reporting (s12) and the
 * kernel debugger. */
void *RtlPcToFileHeader(void *pc_value, void **base_of_image);

/* ==========================================================================
 * Kernel-mode stack walking (TODO-23 s7)
 * ==========================================================================
 * The ELF kernel carries NO .pdata, so RtlLookupFunctionEntry returns NULL for
 * every kernel PC and RtlVirtualUnwind cannot walk a kernel frame. Until the
 * TODO-18 s5 .eh_frame-derived metadata provider + a bounds-checked engine read
 * path land, kernel-mode walking uses a FRAME-POINTER (RBP) chain: each frame's
 * saved RBP is at [RBP] and its return address at [RBP+8] (the standard x64
 * frame layout, retained kernel-wide by -fno-omit-frame-pointer). The walk never
 * invokes RtlVirtualUnwind on the (untrusted, possibly crash-time) live stack.
 *
 * Fault safety is the RIP-keyed recovery primitive, not a bounds table: every
 * [RBP] / [RBP+8] slot is read through __kstack_read_u64 (cpu_security.c), whose
 * guarded load recovers a #PF via page_fault_handler (redirected ahead of the
 * pager). A corrupt or off-stack RBP -- including one on an unmapped stack guard
 * page, a swapped page, or a page another CPU frees mid-walk -- returns an error
 * from the read and terminates the walk instead of faulting, with no CR3/TOCTOU
 * assumptions. Correctness guards (bound fabricated frames, not for safety): RBP
 * is anchored to the captured RSP (RBP >= RSP), 8-aligned, strictly monotonically
 * increasing, kept within the resolved containing kernel stack when known (else a
 * bounded RTL_STACK_WALK_MAX_SPAN of the start); each recorded return address
 * must fall in the kernel text range [__text_start, __text_end) or a loaded
 * module (RtlPcToFileHeader). Metadata-accurate walking is the deferred
 * TODO-18 s5 follow-up. */

/* Absolute walk ceiling: the maximum number of frames the walk will EVER examine
 * (skipped + captured), bounding total work on a pathological deep/corrupt chain.
 * Kernel stacks are far shallower, so this is a safety bound, not a real capture
 * limit. (Not the same as the Windows ~254 FramesToSkip field width.) */
#define RTL_MAX_STACK_FRAMES   0xFEu

/* RtlWalkFrameChain flags: bit 0 selects a user-mode walk; the frames-to-skip
 * count is encoded in the upper bits (>> RTL_STACK_WALK_SKIP_SHIFT), matching the
 * documented ntdll RtlWalkFrameChain convention. */
#define RTL_STACK_WALK_USER_MODE   0x1u
#define RTL_STACK_WALK_SKIP_SHIFT  8

/* Capture the current kernel-mode call stack: walk the RBP chain, skip the first
 * frames_to_skip frames (frames_to_skip == 0 -> first captured frame is the
 * CALLER of RtlCaptureStackBackTrace), record up to min(frames_to_capture,
 * RTL_MAX_STACK_FRAMES) return addresses into back_trace, and (if non-NULL) write
 * an implementation-defined running hash of the captured addresses to
 * back_trace_hash (deterministic for a given call site, non-zero for a non-empty
 * trace). Returns the number of frames recorded. Takes no lock, does not block,
 * and reads only through the fault-safe primitive, so it is callable at
 * IRQL <= DISPATCH_LEVEL (-> XREF: TODO-07 s3). */
uint16_t RtlCaptureStackBackTrace(uint32_t frames_to_skip, uint32_t frames_to_capture,
                                  void **back_trace, uint32_t *back_trace_hash);

/* Crash/exception consumers pass a starting CONTEXT built from the trap frame
 * (context_from_frame) instead of capturing a fresh live context; the walk
 * starts from that frame's Rsp/Rbp. skip/count/hash semantics match
 * RtlCaptureStackBackTrace. Returns the number of frames recorded. */
uint16_t rtl_capture_stack_from_context(const CONTEXT *context, uint32_t frames_to_skip,
                                        uint32_t frames_to_capture, void **back_trace,
                                        uint32_t *back_trace_hash);

/* Thin frame-chain wrapper. The frames-to-skip count is (flags >>
 * RTL_STACK_WALK_SKIP_SHIFT); RTL_STACK_WALK_USER_MODE (bit 0) selects a
 * user-mode walk. Kernel walk (bit 0 clear): walk the current kernel stack,
 * skipping the encoded count. User walk (bit 0 set): deferred (returns 0) until a
 * saved current-user-CONTEXT accessor exists to source the ring-3 RSP/RBP from
 * kernel mode (the user reads themselves would use the s13 fault-safe user path).
 * Returns the number of callers recorded. */
uint32_t RtlWalkFrameChain(void **callers, uint32_t count, uint32_t flags);

/* Test-support: current number of registered dynamic function tables. */
uint32_t rtl_unwind_dynamic_table_count(void);
