/* ============================================================================
 * rtl/unwind.c -- x64 table-based unwind engine (RtlVirtualUnwind et al.)
 *
 * Implements the AMD64 PE/COFF unwind EXECUTION engine (TODO-23 s6). Consumes
 * RUNTIME_FUNCTION/UNWIND_INFO records from loaded-module .pdata (parsed by the
 * PE loader into loaded_module_t) or from the dynamic function-table registry
 * (JIT / no-backing-image code) and steps a CONTEXT from a frame to its caller.
 *
 * ARCH: x86-64. Reference: Microsoft "x64 exception handling"
 * (learn.microsoft.com/cpp/build/exception-handling-x64), Windows Internals 7e.
 *
 * SMP / lifetime:
 *  - The dynamic-table registry is a spinlock-protected sorted singly-linked
 *    list. It owns a PRIVATE COPY of each RtlAddFunctionTable array, so a caller
 *    freeing its own array cannot dangle a returned entry pointer.
 *  - A whole static-table lookup runs under the registry spinlock, so it is
 *    consistent against a concurrent delete (delete also unlinks under the
 *    lock). A callback lookup snapshots the callback fn + context + base into
 *    locals UNDER the lock and invokes the callback OUTSIDE the lock touching
 *    only those locals -- the node is never dereferenced after unlock, so delete
 *    is a plain unlink-under-lock + free with no teardown wait, and a callback
 *    that unregisters its OWN table does not self-deadlock.
 *  - Caller contract: the pointer RtlLookupFunctionEntry returns (into the
 *    registry copy for a static table, or into caller storage for a callback),
 *    and a callback table's callback code + context, stay valid only while no
 *    delete races the unwind. Per the Windows RtlDeleteFunctionTable contract a
 *    caller MUST NOT delete a table (nor free a callback's code/context) while a
 *    thread is still unwinding through it; the stateless lookup/unwind ABI cannot
 *    pin across the separate RtlVirtualUnwind call, and the snapshot of cb/cx
 *    protects the registry node but not the objects they point to. An epoch/RCU-
 *    backed lookup that would close this window is a tracked follow-up.
 *
 * Bounds safety: metadata reached from a LOADED MODULE is fully bounds-checked
 * against [image_base, image_base+size_of_image) -- that is the untrusted attack
 * surface (malformed PE .pdata). Dynamic tables are validated at registration
 * (sorted, non-overlapping, in-range) and are in-kernel/trusted.
 *
 * Frame trust: RtlVirtualUnwind reads saved registers/return addresses through
 * the establisher frame, which it TRUSTS to be a valid stack (kernel-mode
 * Windows RtlVirtualUnwind does the same). The metadata STRUCTURE (opcodes,
 * slot counts, chaining, OpInfo domains) is fully validated and a malformed
 * record fails safe transactionally; but SAVE_NONVOL/SAVE_XMM apply a
 * metadata-controlled offset to the (trusted) frame base and READ the result,
 * and the terminal pop reads [RSP] -- those are STACK reads, not metadata
 * reads, so a huge-but-structurally-valid offset can still address outside the
 * frame. Arithmetic on RSP/frame base is overflow/underflow-checked, but a
 * consumer that unwinds an UNTRUSTED user frame from kernel mode (the
 * user-exception-delivery, kernel-stack-walking, and crash-reporting paths)
 * must supply fault-safe stack reads + stack-range validation at its boundary.
 * ============================================================================ */

/* ARCH: x86-64 -- table-based unwind is the PE/COFF AMD64 mechanism. */

#include "kernel/rtl/unwind.h"
#include "kernel/exec.h"
#include "kernel/mm/heap.h"
#include "kernel/sched/spinlock.h"
#include "kernel/klog.h"

extern void *memcpy(void *dst, const void *src, uint64_t n);

/* Maximum chained-info depth before we treat the chain as malformed (cycle /
 * runaway). Real toolchains emit at most a handful of chain levels. */
#define UNWIND_MAX_CHAIN   32

/* Cap on entries in one dynamic function table: 256 * 12 bytes = 3072 <= the
 * kmalloc 4 KB ceiling, and the bound makes `entry_count * sizeof(...)` unable
 * to wrap. JIT tables larger than this must use the (deferred) growable API. */
#define UNWIND_MAX_DYNAMIC_ENTRIES   256

/* Cap on the number of registered dynamic tables. dft_lookup walks the sorted
 * list under s_dft_lock (IRQs off); the cap bounds that critical section until
 * an indexed/RCU-backed lookup lands (SMP-epoch follow-up). */
#define UNWIND_MAX_DYNAMIC_TABLES    512

/* --- Dynamic function-table registry node -------------------------------- */

typedef struct dyn_func_table {
    struct dyn_func_table *next;
    uint64_t          base_address;   /* image base the entries' RVAs are relative to */
    uint64_t          min_address;    /* base + lowest BeginAddress (inclusive) */
    uint64_t          max_address;    /* base + highest EndAddress (exclusive) */
    RUNTIME_FUNCTION *functions;      /* registry-owned COPY (NULL for callback tables) */
    uint32_t          entry_count;
    uint64_t          identity;       /* (uintptr_t)caller table ptr, or table_identifier */
    PGET_RUNTIME_FUNCTION_CALLBACK callback;  /* non-NULL for callback tables */
    void             *context;
} dyn_func_table_t;

static spinlock_t         s_dft_lock = SPINLOCK_INIT;
static dyn_func_table_t   *s_dft_head;          /* sorted by min_address, ascending */
static uint32_t            s_dft_count;
static int                 s_unwind_ready;

/* ==========================================================================
 * Small helpers
 * ========================================================================== */

/* CONTEXT integer registers are laid out Rax..R15 in AMD64 ABI order, so
 * register number n (0..15) is &ctx->Rax + n. */
static inline uint64_t *ctx_int_reg(CONTEXT *ctx, unsigned n)
{
    return &(&ctx->Rax)[n];
}

/* XMM register n is a 16-byte slice of the FXSAVE XmmRegisters area. */
static inline M128A *ctx_xmm_reg(CONTEXT *ctx, unsigned n)
{
    return (M128A *)&ctx->FltSave.XmmRegisters[n * 16u];
}

/* Read helpers with explicit little-endian semantics (freestanding, no
 * unaligned-access assumptions beyond x86 tolerance). */
static inline uint32_t rd_u32(const void *p)
{
    const uint8_t *b = (const uint8_t *)p;
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
           ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

/* Number of UNWIND_CODE slots a given opcode consumes (including its own). */
static uint32_t unwind_op_slots(uint8_t op, uint8_t info)
{
    switch (op) {
    case UWOP_PUSH_NONVOL:     return 1;
    case UWOP_ALLOC_LARGE:     return (info == 0) ? 2 : 3;
    case UWOP_ALLOC_SMALL:     return 1;
    case UWOP_SET_FPREG:       return 1;
    case UWOP_SAVE_NONVOL:     return 2;
    case UWOP_SAVE_NONVOL_FAR: return 3;
    case UWOP_EPILOG:          return 1;   /* v2 descriptor: skip its slots */
    case UWOP_SPARE_CODE:      return 3;
    case UWOP_SAVE_XMM128:     return 2;
    case UWOP_SAVE_XMM128_FAR: return 3;
    case UWOP_PUSH_MACHFRAME:  return 1;
    default:                   return 0;   /* unknown -> caller bails out */
    }
}

/* Derive the safe read window for image-based metadata reached from control_pc.
 * Returns 1 and sets [*lo,*hi) when control_pc belongs to a loaded module whose
 * base matches image_base (fully bounded, untrusted surface). Returns 0 when no
 * module owns it (dynamic/trusted table -- structural checks only). */
static int unwind_module_extent(uint64_t control_pc, uint64_t image_base,
                                uint64_t *lo, uint64_t *hi)
{
    loaded_module_t mod;
    if (exec_find_module_by_pc(control_pc, &mod) != 0)
        return 0;
    if (mod.base_address != image_base)
        return 0;
    *lo = mod.base_address;
    *hi = mod.base_address + mod.size_of_image;
    return 1;
}

/* True when [ptr, ptr+len) is safe to dereference: either unbounded (trusted
 * dynamic table) or fully inside [lo,hi) with no wrap. */
static int in_bounds(uint64_t ptr, uint64_t len, uint64_t lo, uint64_t hi, int bounded)
{
    if (!bounded)
        return 1;
    uint64_t end = ptr + len;
    return (ptr >= lo) && (end >= ptr) && (end <= hi);
}

/* Checked 64-bit add: *out = a + b, returns 0 (and leaves *out untouched) if the
 * sum wraps. Every stack/frame address computed from metadata-controlled offsets
 * routes through this so malformed unwind data cannot wrap into an unrelated
 * address that is then dereferenced. */
static int ckadd(uint64_t a, uint64_t b, uint64_t *out)
{
    uint64_t s = a + b;
    if (s < a)
        return 0;
    *out = s;
    return 1;
}

/* Checked signed add for epilog RSP simulation: *out = a + b (b may be
 * negative), returns 0 on wrap in either direction. */
static int ckadd_s(uint64_t a, int64_t b, uint64_t *out)
{
    uint64_t r = a + (uint64_t)b;
    if ((b > 0 && r < a) || (b < 0 && r > a))
        return 0;
    *out = r;
    return 1;
}

/* ==========================================================================
 * Epilog detection + simulation
 *
 * If control_pc sits inside a function epilog, the prolog-code unwind would
 * double-restore the frame. Detect the epilog instruction pattern
 *   [ add rsp,imm | lea rsp,[reg+disp] ]  (pop r64)*  ( ret | rep ret | jmp )
 * and simulate the remaining instructions instead. Returns 1 and steps ctx to
 * the caller frame if control_pc is an epilog, else 0 (ctx untouched).
 * ========================================================================== */

/* Total length of a legal `[optional REX] FF /4` indirect-jmp EPILOG terminator
 * beginning at `ins`, bounded by `avail` readable bytes. The AMD64 epilog
 * contract restricts the tail-call jmp to a mod==00 MEMORY reference (any legal
 * mod==00 form, including scaled-index SIB); register-direct (mod==11) and the
 * disp8/disp32 base forms (mod==01/10) are NOT legal epilog terminators. Returns
 * 0 when the form is prohibited, is not an FF /4, or `avail` is too small. */
static uint32_t indirect_jmp_len(const uint8_t *ins, uint32_t avail)
{
    uint32_t i = 0;
    if (avail >= 1u && (ins[0] & 0xF0u) == 0x40u)
        i = 1u;                                  /* skip a REX prefix */
    if (avail < i + 2u || ins[i] != 0xFFu)
        return 0;
    uint8_t modrm = ins[i + 1u];
    if (((modrm >> 3) & 7u) != 4u)
        return 0;                                /* ModRM reg field must be /4 */
    uint8_t mod = (modrm >> 6) & 3u;
    uint8_t rm  = modrm & 7u;
    if (mod != 0u)
        return 0;                                /* only mod==00 memory refs */
    uint32_t len = i + 2u;                        /* [REX] FF modrm */
    if (rm == 4u) {                              /* SIB byte follows (any index) */
        if (avail < len + 1u)
            return 0;
        uint8_t sib = ins[len];
        len += 1u;
        if ((sib & 7u) == 5u)
            len += 4u;                           /* SIB base 101 at mod0 => disp32 */
    } else if (rm == 5u) {
        len += 4u;                               /* RIP-relative disp32 */
    }
    /* else mod==00, rm in {0..3,6,7}: [reg] with no displacement. */
    return len;
}

static int unwind_try_epilog(uint64_t control_pc, CONTEXT *ctx,
                             uint64_t lo, uint64_t hi, int bounded,
                             uint64_t func_start, uint64_t func_end,
                             uint8_t frame_reg,
                             KNONVOLATILE_CONTEXT_POINTERS *context_pointers)
{
    const uint8_t *p = (const uint8_t *)control_pc;
    uint64_t pc = control_pc;
    uint64_t rsp = ctx->Rsp;
    /* Scratch register restores + their stack-slot addresses; committed only if
     * a terminator is reached. */
    uint64_t  regs[16];
    uint64_t *reg_slot[16] = { 0 };
    int       reg_touched[16] = { 0 };

    /* Every epilog instruction must lie WHOLLY within [pc, func_end) AND the
     * image bounds -- a multi-byte op (REX-pop, ret imm16, indirect jmp) must
     * not straddle EndAddress into an adjacent function. Subtraction form so the
     * function-extent check cannot wrap even for an unbounded dynamic table whose
     * range ends at UINT64_MAX. */
#define EPI_FITS(n) (in_bounds(pc, (n), lo, hi, bounded) && (pc) >= func_start && \
                     (pc) < func_end && (uint64_t)(n) <= func_end - (pc))

    /* Step 1: optional stack teardown. Only the exact legal RSP-teardown
     * encodings are accepted so a non-RSP instruction with a similar shape
     * (e.g. `add r12,imm` = REX.WB) is never mistaken for a teardown:
     *   add rsp, imm8/imm32 : REX.W (0x48, REX.B CLEAR) 83/81 /0 (modrm 0xC4)
     *   lea rsp, [fp + disp] : REX.W[.B] 8D, reg field = rsp, base = the
     *                          UNWIND_INFO frame register, no SIB index. */
    if (EPI_FITS(2)) {
        uint8_t b0 = p[0];
        if (b0 == 0x48 && p[1] == 0x83 && EPI_FITS(4) && p[2] == 0xC4) {
            if (!ckadd_s(rsp, (int64_t)(int8_t)p[3], &rsp)) return 0;   /* add rsp, imm8 */
            p += 4; pc += 4;
        } else if (b0 == 0x48 && p[1] == 0x81 && EPI_FITS(7) && p[2] == 0xC4) {
            if (!ckadd_s(rsp, (int64_t)(int32_t)rd_u32(p + 3), &rsp)) return 0;  /* add rsp, imm32 */
            p += 7; pc += 7;
        } else if ((b0 == 0x48 || b0 == 0x49) && p[1] == 0x8D && frame_reg != 0 &&
                   EPI_FITS(3)) {
            /* lea rsp, [frame_reg + disp]. reg field must be rsp (100b). Two
             * encodings of the base: a direct base (rm != 100), and the SIB
             * no-index form (rm == 100) that r12 and rsp REQUIRE. Indexed SIB,
             * RIP-relative (mod0,rm5), and disp32-no-base (SIB base 101) are
             * rejected -- none is a simple frame-pointer restore. */
            uint8_t rex_b = (b0 & 0x01) ? 8u : 0u;
            uint8_t modrm = p[2];
            uint8_t mod = (modrm >> 6) & 3u;
            uint8_t reg = (modrm >> 3) & 7u;
            uint8_t rm  = modrm & 7u;
            uint8_t base = 0xFFu;
            uint32_t hdr = 3;   /* REX + 0x8D + modrm */
            int ok = (reg == 4u && mod != 3u);
            if (ok && rm == 4u) {
                /* disp32-no-base (base field 101) applies ONLY at mod==0; at
                 * mod 1/2 base 101 is a valid rbp/r13 (via REX.B) base. */
                if (EPI_FITS(4) &&
                    ((p[3] >> 3) & 7u) == 4u &&                    /* no index */
                    !(mod == 0u && (p[3] & 7u) == 5u)) {          /* not disp32-no-base */
                    base = (uint8_t)((p[3] & 7u) + rex_b);
                    hdr = 4;
                } else {
                    ok = 0;
                }
            } else if (ok && mod == 0u && rm == 5u) {
                ok = 0;                              /* RIP-relative */
            } else if (ok) {
                base = (uint8_t)(rm + rex_b);
            }
            if (ok && base == frame_reg) {
                int64_t disp = 0;
                uint32_t ilen = hdr;
                if (mod == 1u) {
                    if (EPI_FITS(hdr + 1u)) {
                        disp = (int64_t)(int8_t)p[hdr]; ilen = hdr + 1;
                    } else ok = 0;
                } else if (mod == 2u) {
                    if (EPI_FITS(hdr + 4u)) {
                        disp = (int64_t)(int32_t)rd_u32(p + hdr); ilen = hdr + 4;
                    } else ok = 0;
                }   /* mod == 0: base-only, disp 0 */
                if (ok && ckadd_s(*ctx_int_reg(ctx, frame_reg), disp, &rsp)) {
                    p += ilen; pc += ilen;
                }
            }
        }
    }

    /* Whether the scan consumed a real epilog prefix (a stack teardown and/or
     * pops). Used to disambiguate an AMBIGUOUS indirect tail-call jmp from a body
     * switch-dispatch jmp: only an indirect jmp reached AFTER a valid teardown/
     * pops sequence is an epilog. */
    int had_teardown = (pc != control_pc);
    int pops = 0;

    /* Step 2: run of pop r64 (optional REX.B for r8-r15). A legal epilog pops at
     * most the nonvolatile integer registers, so the scan is capped at 16 and
     * confined to the function body -- a degenerate/crafted pop sled cannot make
     * one unwind cost O(image size). */
    for (;;) {
        if (!EPI_FITS(1))
            return 0;
        uint8_t rex_b = 0;
        uint32_t adv = 0;
        uint8_t b = p[0];
        if ((b & 0xF0) == 0x40) {                 /* REX prefix */
            rex_b = (b & 0x01) ? 8u : 0u;
            if (!EPI_FITS(2))
                return 0;
            b = p[1];
            adv = 1;
        }
        if (b >= 0x58 && b <= 0x5F) {             /* pop r64 */
            if (pops >= 16)
                return 0;   /* cap BEFORE dereferencing another slot */
            unsigned reg = (unsigned)(b - 0x58) + rex_b;
            uint64_t new_rsp;
            if (!ckadd(rsp, 8, &new_rsp))
                return 0;
            uint64_t *slot = (uint64_t *)rsp;      /* stack read (not image-bounded) */
            regs[reg] = *slot;
            reg_slot[reg] = slot;
            reg_touched[reg] = 1;
            rsp = new_rsp;
            pops++;
            p += adv + 1; pc += adv + 1;
            continue;
        }
        break;
    }

    /* Step 3: terminator. A `ret`/`rep ret` is unconditionally a function exit,
     * hence an epilog. Tail-call jumps come in two disambiguation classes:
     *   - RELATIVE (`jmp rel8`/`rel32`, EB/E9): an epilog iff the target is
     *     OUTBOUND (outside [func_start,func_end)). An outbound relative jump is
     *     always a tail call in valid code; an intra-function branch is a body
     *     branch. The target check works from ANY control_pc position.
     *   - INDIRECT (`[REX.W] FF /4`: rip-relative `FF 25`, register `FF E0-E7`,
     *     indexed-SIB `FF 24 ...`): the SAME encodings are emitted for BOTH
     *     indexed tail calls AND body switch dispatch, so the encoding alone is
     *     ambiguous. We accept an indirect jmp as an epilog ONLY when the scan
     *     consumed a real epilog prefix (a teardown and/or pops), which a body
     *     switch never has. This detects the common `add rsp,N; pop; jmp <ind>`
     *     tail-call epilog; a control_pc landing EXACTLY on the indirect jmp
     *     (teardown already retired, nothing left to scan) is not detected and
     *     falls back to prolog unwind -- the robust fix is UWOP_EPILOG (unwind
     *     info v2) epilog metadata, tracked as a follow-up.
     * `ret imm16` (0xC2 iw) is a legal epilog terminator: it pops the return
     * address and then adds imm16 to RSP (its stack adjustment is simulated). */
    if (!EPI_FITS(1))
        return 0;
    uint8_t t = p[0];
    int is_epilog = 0;
    uint32_t ret_imm = 0;   /* extra RSP adjustment from `ret imm16` */
    int had_progress = (had_teardown || pops > 0);
    /* Skip an optional REX prefix (0x40-0x4F) on an indirect jmp: Clang emits
     * `rex64 jmpq *[rip+d]` as 48 FF /4 and `jmp r8-r15` as 41 FF /4 (REX.B).
     * REX does not apply to a relative jmp, so EB/E9 are matched unprefixed. */
    if (t == 0xC3) {                              /* ret */
        is_epilog = 1;
    } else if (t == 0xC2 && EPI_FITS(3)) {        /* ret imm16 */
        is_epilog = 1;
        ret_imm = (uint32_t)p[1] | ((uint32_t)p[2] << 8);
    } else if (t == 0xF3 && EPI_FITS(2) && p[1] == 0xC3) {
        is_epilog = 1;                            /* rep ret */
    } else if (t == 0xEB && EPI_FITS(2)) {        /* jmp rel8 */
        uint64_t tgt = pc + 2 + (uint64_t)(int64_t)(int8_t)p[1];
        if (tgt < func_start || tgt >= func_end)
            is_epilog = 1;
    } else if (t == 0xE9 && EPI_FITS(5)) {        /* jmp rel32 */
        uint64_t tgt = pc + 5 + (uint64_t)(int64_t)(int32_t)rd_u32(p + 1);
        if (tgt < func_start || tgt >= func_end)
            is_epilog = 1;
    } else if (had_progress) {                    /* `[REX] FF /4` indirect jmp */
        uint32_t avail = (func_end - pc > 16u) ? 16u : (uint32_t)(func_end - pc);
        uint32_t jl = indirect_jmp_len(p, avail);
        if (jl != 0 && EPI_FITS(jl))
            is_epilog = 1;
    }
    if (!is_epilog)
        return 0;

    /* Commit: nonvolatile register restores (+ their saved-slot addresses), then
     * pop the return address (and apply a `ret imm16` stack adjustment). */
    uint64_t after_ret;
    if (!ckadd(rsp, 8u + (uint64_t)ret_imm, &after_ret))
        return 0;
    for (unsigned i = 0; i < 16; i++) {
        if (reg_touched[i]) {
            *ctx_int_reg(ctx, i) = regs[i];
            if (context_pointers)
                context_pointers->Integer[i] = reg_slot[i];
        }
    }
    uint64_t *ret_slot = (uint64_t *)rsp;
    ctx->Rip = *ret_slot;
    ctx->Rsp = after_ret;
    return 1;
#undef EPI_FITS
}

/* ==========================================================================
 * RtlVirtualUnwind -- prolog-based unwind with iterative chaining
 * ========================================================================== */

/* Advance ctx one frame using a leaf convention (no unwind data): the return
 * address is at [Rsp]. Used as the fail-safe so a malformed record makes forward
 * progress instead of looping. */
static void unwind_leaf(CONTEXT *ctx, uint64_t *establisher_frame)
{
    if (establisher_frame)
        *establisher_frame = ctx->Rsp;
    uint64_t *ret_slot = (uint64_t *)ctx->Rsp;
    ctx->Rip = *ret_slot;
    ctx->Rsp += 8;
}

void *RtlVirtualUnwind(uint32_t handler_type, uint64_t image_base, uint64_t control_pc,
                       PRUNTIME_FUNCTION function_entry, CONTEXT *context,
                       void **handler_data, uint64_t *establisher_frame,
                       KNONVOLATILE_CONTEXT_POINTERS *context_pointers)
{
    if (handler_data)
        *handler_data = 0;
    if (establisher_frame)
        *establisher_frame = 0;
    if (!context)
        return 0;
    if (!function_entry || image_base == 0) {
        unwind_leaf(context, establisher_frame);
        return 0;
    }

    uint64_t lo = 0, hi = 0;
    int bounded = unwind_module_extent(control_pc, image_base, &lo, &hi);

    /* Primary UNWIND_INFO for the innermost function. */
    uint64_t ui_addr = image_base + (function_entry->UnwindInfoAddress & ~1u);
    if (!in_bounds(ui_addr, sizeof(UNWIND_INFO), lo, hi, bounded)) {
        unwind_leaf(context, establisher_frame);
        return 0;
    }
    UNWIND_INFO *ui = (UNWIND_INFO *)ui_addr;

    /* Reject unsupported versions BEFORE any code-stream / epilog interpretation
     * so a v2-or-later record with a body-offset PC fails safe (leaf) instead of
     * being mis-simulated. */
    if (UNWIND_INFO_VERSION(ui) != 1) {
        unwind_leaf(context, establisher_frame);
        return 0;
    }

    uint32_t prolog_off = (uint32_t)(control_pc - (image_base + function_entry->BeginAddress));
    /* A language handler is active only when control is in the function BODY --
     * not mid-prolog (frame not yet established) and not in the epilog. */
    int pc_in_body = (prolog_off >= ui->SizeOfProlog);

    /* Epilog check only when past the prolog (in body or epilog). */
    if (pc_in_body) {
        uint64_t func_start = image_base + function_entry->BeginAddress;
        uint64_t func_end = image_base + function_entry->EndAddress;
        uint8_t innermost_frame_reg = UNWIND_INFO_FRAMEREG(ui);
        uint64_t entry_rsp = context->Rsp;   /* RSP at the fault PC, before sim */
        if (unwind_try_epilog(control_pc, context, lo, hi, bounded, func_start, func_end,
                              innermost_frame_reg, context_pointers)) {
            /* Identify the frame being unwound by its RSP at the fault PC (a real
             * address within this frame), not the post-return caller SP. Precise
             * in-epilog frame-base recovery needs UWOP_EPILOG (v2) metadata. */
            if (establisher_frame)
                *establisher_frame = entry_rsp;
            return 0;
        }
    }

    void *handler = 0;
    PRUNTIME_FUNCTION cur = function_entry;
    int depth = 0;
    uint64_t est_frame = context->Rsp;   /* refined below if a frame register is used */

    /* Transactional snapshot: untrusted metadata is interpreted directly into
     * `context`, but if any opcode/chain step turns out malformed we must NOT
     * leaf-fall-back through the partially mutated state (a moved RSP would read
     * RIP from the wrong slot). On failure the snapshot is restored and a single
     * leaf pop is taken from the ORIGINAL RSP. Only the fields the interpreter
     * can mutate are journaled -- the integer file (Rax..Rip, contiguous) and
     * the FXSAVE XMM area -- avoiding a full 1232-byte CONTEXT copy per frame. */
    uint64_t saved_ints[17];   /* Rax..R15 (16) + Rip */
    memcpy(saved_ints, &context->Rax, sizeof(saved_ints));
    uint8_t saved_xmm[256];
    memcpy(saved_xmm, context->FltSave.XmmRegisters, sizeof(saved_xmm));
    int have_cp = (context_pointers != 0);
    KNONVOLATILE_CONTEXT_POINTERS saved_cp;
    if (have_cp)
        saved_cp = *context_pointers;
    int bad = 0;

    for (;;) {
        ui_addr = image_base + (cur->UnwindInfoAddress & ~1u);
        if (!in_bounds(ui_addr, sizeof(UNWIND_INFO), lo, hi, bounded)) {
            bad = 1; break;
        }
        ui = (UNWIND_INFO *)ui_addr;
        /* Only unwind-info version 1 is supported. Version 2 adds paired
         * epilog descriptor slots whose decoding differs; accepting it without
         * version-specific slot handling would misparse the code array, so a v2
         * record fails safe (leaf pop) rather than being mis-walked. */
        if (UNWIND_INFO_VERSION(ui) != 1) {
            bad = 1; break;
        }
        uint8_t count = ui->CountOfCodes;
        UNWIND_CODE *codes = ui->UnwindCode;
        if (!in_bounds((uint64_t)(uintptr_t)codes, (uint64_t)count * 2u, lo, hi, bounded)) {
            bad = 1; break;
        }

        /* Codes past the current prolog offset have not executed yet; for a
         * chained parent, treat the whole prolog as executed. */
        uint32_t eff_off = (depth == 0)
            ? (uint32_t)(control_pc - (image_base + cur->BeginAddress))
            : 0xFFFFFFFFu;

        /* Establish the fixed frame base if a frame register is used and its
         * SET_FPREG has executed (SAVE_NONVOL/XMM offsets are relative to it). */
        uint8_t frame_reg = UNWIND_INFO_FRAMEREG(ui);
        uint64_t frame_base;
        if (frame_reg != 0) {
            /* Find the SET_FPREG code offset (at most one). */
            uint32_t fp_off = 0xFFFFFFFFu;
            uint32_t j = 0;
            while (j < count) {
                uint8_t op = UNWIND_CODE_OP(codes[j]);
                uint8_t info = UNWIND_CODE_INFO(codes[j]);
                uint32_t slots = unwind_op_slots(op, info);
                if (slots == 0 || j + slots > count)
                    break;
                if (op == UWOP_SET_FPREG) {
                    fp_off = codes[j].b.CodeOffset;
                    break;
                }
                j += slots;
            }
            if (eff_off >= fp_off) {
                uint64_t reg_val = *ctx_int_reg(context, frame_reg);
                uint64_t off16 = (uint64_t)UNWIND_INFO_FRAMEOFF(ui) * 16u;
                if (reg_val < off16) {
                    bad = 1; break;   /* frame-offset underflow: malformed */
                }
                frame_base = reg_val - off16;
            } else {
                frame_base = context->Rsp;
            }
        } else {
            frame_base = context->Rsp;
        }
        if (depth == 0)
            est_frame = frame_base;

        /* Apply codes in array order (reverse-prolog order). */
        int malformed = 0;
        uint32_t i = 0;
        while (i < count) {
            UNWIND_CODE *uc = &codes[i];
            uint8_t op = UNWIND_CODE_OP(*uc);
            uint8_t info = UNWIND_CODE_INFO(*uc);
            uint32_t slots = unwind_op_slots(op, info);
            if (slots == 0 || i + slots > count) {
                malformed = 1;
                break;
            }
            if (uc->b.CodeOffset <= eff_off) {
                switch (op) {
                case UWOP_PUSH_NONVOL: {
                    uint64_t next_rsp;
                    if (!ckadd(context->Rsp, 8, &next_rsp)) { malformed = 1; break; }
                    uint64_t *slot = (uint64_t *)context->Rsp;
                    *ctx_int_reg(context, info) = *slot;
                    if (context_pointers && info < 16)
                        context_pointers->Integer[info] = slot;
                    context->Rsp = next_rsp;
                    break;
                }
                case UWOP_ALLOC_SMALL: {
                    uint64_t d = (uint64_t)info * 8u + 8u;
                    if (context->Rsp + d < context->Rsp) { malformed = 1; break; }
                    context->Rsp += d;
                    break;
                }
                case UWOP_ALLOC_LARGE: {
                    if (info > 1) { malformed = 1; break; }   /* only forms 0/1 exist */
                    uint64_t d = (info == 0)
                        ? (uint64_t)codes[i + 1].FrameOffset * 8u
                        : (uint64_t)rd_u32(&codes[i + 1]);
                    if (context->Rsp + d < context->Rsp) { malformed = 1; break; }
                    context->Rsp += d;
                    break;
                }
                case UWOP_SET_FPREG: {
                    /* SET_FPREG is only legal when a frame register is declared,
                     * and its OpInfo is reserved (0). Otherwise the record is
                     * malformed and would set RSP from a bogus register (e.g. a
                     * crafted FrameRegister==0 would use RAX). */
                    if (frame_reg == 0 || info != 0) { malformed = 1; break; }
                    uint64_t reg_val = *ctx_int_reg(context, frame_reg);
                    uint64_t off16 = (uint64_t)UNWIND_INFO_FRAMEOFF(ui) * 16u;
                    if (reg_val < off16) { malformed = 1; break; }
                    context->Rsp = reg_val - off16;
                    break;
                }
                case UWOP_SAVE_NONVOL: {
                    uint64_t addr;
                    if (!ckadd(frame_base, (uint64_t)codes[i + 1].FrameOffset * 8u, &addr)) {
                        malformed = 1; break;
                    }
                    uint64_t *slot = (uint64_t *)addr;
                    *ctx_int_reg(context, info) = *slot;
                    if (context_pointers && info < 16)
                        context_pointers->Integer[info] = slot;
                    break;
                }
                case UWOP_SAVE_NONVOL_FAR: {
                    uint64_t addr;
                    if (!ckadd(frame_base, (uint64_t)rd_u32(&codes[i + 1]), &addr)) {
                        malformed = 1; break;
                    }
                    uint64_t *slot = (uint64_t *)addr;
                    *ctx_int_reg(context, info) = *slot;
                    if (context_pointers && info < 16)
                        context_pointers->Integer[info] = slot;
                    break;
                }
                case UWOP_SAVE_XMM128: {
                    uint64_t addr;
                    if (!ckadd(frame_base, (uint64_t)codes[i + 1].FrameOffset * 16u, &addr)) {
                        malformed = 1; break;
                    }
                    M128A *slot = (M128A *)addr;
                    *ctx_xmm_reg(context, info) = *slot;
                    if (context_pointers && info < 16)
                        context_pointers->Xmm[info] = slot;
                    break;
                }
                case UWOP_SAVE_XMM128_FAR: {
                    uint64_t addr;
                    if (!ckadd(frame_base, (uint64_t)rd_u32(&codes[i + 1]), &addr)) {
                        malformed = 1; break;
                    }
                    M128A *slot = (M128A *)addr;
                    *ctx_xmm_reg(context, info) = *slot;
                    if (context_pointers && info < 16)
                        context_pointers->Xmm[info] = slot;
                    break;
                }
                case UWOP_PUSH_MACHFRAME: {
                    if (info > 1) { malformed = 1; break; }   /* only 0/1 (no/with err code) */
                    /* Interrupt/exception machine frame on the stack:
                     * [RIP][CS][EFLAGS][RSP][SS], preceded by an error code when
                     * info==1. RSP currently points at (error code or) RIP. */
                    uint64_t *mf = (uint64_t *)context->Rsp;
                    if (info == 1)
                        mf += 1;             /* skip pushed error code */
                    context->Rip = mf[0];
                    context->Rsp = mf[3];    /* RSP field of the iret frame */
                    /* Terminal frame -- the machine frame fully defines caller. */
                    if (establisher_frame)
                        *establisher_frame = est_frame;
                    return handler;
                }
                case UWOP_EPILOG:
                case UWOP_SPARE_CODE:
                    /* Not valid version-1 operations (v2-only descriptors). */
                default:
                    malformed = 1;
                    break;
                }
            }
            if (malformed)
                break;
            i += slots;
        }
        if (malformed) {
            bad = 1;
            break;
        }

        /* Handler + language data live after the even-slot-aligned code array.
         * Per the AMD64 contract a CHAININFO record clears the handler flags, so
         * the active handler always lives in the TERMINAL (non-chained) record;
         * extract it there, never from a chained link (which stores a parent
         * RUNTIME_FUNCTION at the same offset). The resolved target must land
         * inside the owning image (checked add) before it can be returned. */
        uint32_t aligned = ((uint32_t)count + 1u) & ~1u;
        uint8_t flags = UNWIND_INFO_FLAGS(ui);
        if (pc_in_body && !(flags & UNW_FLAG_CHAININFO) &&
            (flags & handler_type & (UNW_FLAG_EHANDLER | UNW_FLAG_UHANDLER))) {
            uint64_t hp = (uint64_t)(uintptr_t)(codes + aligned);
            if (in_bounds(hp, 4, lo, hi, bounded)) {
                uint32_t hrva = rd_u32((const void *)(uintptr_t)hp);
                uint64_t htarget = image_base + hrva;
                if (!bounded || (htarget >= lo && htarget < hi)) {
                    handler = (void *)(uintptr_t)htarget;
                    if (handler_data)
                        *handler_data = (void *)(uintptr_t)(hp + 4);
                }
            }
        }

        if (flags & UNW_FLAG_CHAININFO) {
            uint64_t cp = (uint64_t)(uintptr_t)(codes + aligned);
            if (!in_bounds(cp, sizeof(RUNTIME_FUNCTION), lo, hi, bounded)) {
                bad = 1; break;
            }
            if (++depth > UNWIND_MAX_CHAIN) {
                bad = 1; break;
            }
            cur = (PRUNTIME_FUNCTION)(uintptr_t)cp;
            continue;
        }
        break;   /* success: no more chaining */
    }

    if (bad) {
        /* Restore the mutated register blocks and take exactly one leaf pop from
         * the ORIGINAL RSP (transactional fail-safe for malformed metadata). */
        memcpy(&context->Rax, saved_ints, sizeof(saved_ints));
        memcpy(context->FltSave.XmmRegisters, saved_xmm, sizeof(saved_xmm));
        if (have_cp)
            *context_pointers = saved_cp;
        unwind_leaf(context, establisher_frame);
        return 0;
    }

    /* Terminal: pop the return address exactly once. */
    if (establisher_frame)
        *establisher_frame = est_frame;
    uint64_t *ret_slot = (uint64_t *)context->Rsp;
    context->Rip = *ret_slot;
    context->Rsp += 8;
    return handler;
}

/* ==========================================================================
 * RtlLookupFunctionEntry
 * ========================================================================== */

/* Binary-search a sorted .pdata array for the entry covering `rva`. */
static PRUNTIME_FUNCTION pdata_search(RUNTIME_FUNCTION *table, uint32_t count, uint32_t rva)
{
    uint32_t lo = 0, hi = count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        if (rva < table[mid].BeginAddress)
            hi = mid;
        else if (rva >= table[mid].EndAddress)
            lo = mid + 1;
        else
            return &table[mid];
    }
    return 0;
}

/* Search the dynamic-table registry for control_pc. Static-array nodes complete
 * entirely under the registry lock (the returned pointer is into registry-owned
 * storage that delete drains before freeing). Callback nodes acquire rundown,
 * drop the lock, and invoke the callback outside it. */
static PRUNTIME_FUNCTION dft_lookup(uint64_t control_pc, uint64_t *image_base)
{
    uint64_t flags;
    spin_lock_irqsave(&s_dft_lock, &flags);
    for (dyn_func_table_t *n = s_dft_head; n; n = n->next) {
        if (n->min_address > control_pc)
            break;   /* sorted ascending: no later node can match */
        if (control_pc < n->min_address || control_pc >= n->max_address)
            continue;
        if (n->callback) {
            /* Snapshot the callback + context + base under the lock, then invoke
             * OUTSIDE the lock using only those locals -- the node is never
             * touched after unlock, so a concurrent (or self-issued) delete can
             * unlink + free it with no use-after-free and no teardown wait. The
             * callback function and its context lifetime are the caller's per
             * the Windows RtlInstallFunctionTableCallback contract. */
            PGET_RUNTIME_FUNCTION_CALLBACK cb = n->callback;
            void *cx = n->context;
            uint64_t base = n->base_address;
            spin_unlock_irqrestore(&s_dft_lock, flags);
            PRUNTIME_FUNCTION rf = cb(control_pc, cx);
            /* Indirect (fragment) entries are not yet supported; fail safe. */
            if (rf && RUNTIME_FUNCTION_CHAINED(rf))
                return 0;
            if (rf && image_base)
                *image_base = base;
            return rf;
        }
        uint32_t rva = (uint32_t)(control_pc - n->base_address);
        /* Static-table entries are direct-only (RtlAddFunctionTable rejects
         * indirect ones), so no redirection resolution is needed here. */
        PRUNTIME_FUNCTION rf = pdata_search(n->functions, n->entry_count, rva);
        uint64_t base = n->base_address;
        spin_unlock_irqrestore(&s_dft_lock, flags);
        if (rf && image_base)
            *image_base = base;
        return rf;
    }
    spin_unlock_irqrestore(&s_dft_lock, flags);
    return 0;
}

PRUNTIME_FUNCTION RtlLookupFunctionEntry(uint64_t control_pc, uint64_t *image_base,
                                         void *history_table)
{
    (void)history_table;   /* no caching layer yet (ABI-compat parameter) */

    /* 1. Loaded-module .pdata (the common, untrusted case). */
    loaded_module_t mod;
    if (exec_find_module_by_pc(control_pc, &mod) == 0 && mod.pdata_size >= sizeof(RUNTIME_FUNCTION)) {
        uint32_t count = (uint32_t)(mod.pdata_size / sizeof(RUNTIME_FUNCTION));
        uint32_t rva = (uint32_t)(control_pc - mod.base_address);
        RUNTIME_FUNCTION *table = (RUNTIME_FUNCTION *)(uintptr_t)mod.pdata_base;
        PRUNTIME_FUNCTION rf = pdata_search(table, count, rva);
        if (rf) {
            /* Indirect (redirected) entries describe a function FRAGMENT sharing
             * another entry's unwind info; unwinding them correctly needs the
             * fragment's own range paired with the parent's UNWIND_INFO. Until
             * that is implemented, fail safe (NULL) rather than mis-unwind with
             * the wrong range. RVAs are checked against size_of_image directly
             * (not base+RVA vs img_end, which could wrap for a high base). */
            if (!RUNTIME_FUNCTION_CHAINED(rf) &&
                rf->BeginAddress < rf->EndAddress &&
                (uint64_t)rf->EndAddress <= mod.size_of_image &&
                (uint64_t)rf->UnwindInfoAddress < mod.size_of_image) {
                if (image_base)
                    *image_base = mod.base_address;
                return rf;
            }
            return 0;   /* indirect or malformed entry: fail closed */
        }
    }

    /* 2. Dynamic function-table registry (JIT / no backing image). */
    return dft_lookup(control_pc, image_base);
}

/* ==========================================================================
 * Dynamic function-table registry: add / delete / callback
 * ========================================================================== */

/* Insert a fully-built node into the sorted list (caller holds the lock). */
static void dft_insert_locked(dyn_func_table_t *node)
{
    dyn_func_table_t **pp = &s_dft_head;
    while (*pp && (*pp)->min_address < node->min_address)
        pp = &(*pp)->next;
    node->next = *pp;
    *pp = node;
    s_dft_count++;
}

/* True if [min,max) overlaps any registered node (caller holds the lock). */
static int dft_overlaps_locked(uint64_t min_addr, uint64_t max_addr)
{
    for (dyn_func_table_t *n = s_dft_head; n; n = n->next) {
        if (min_addr < n->max_address && n->min_address < max_addr)
            return 1;
    }
    return 0;
}

/* True if a node with this delete-identity is already registered (caller holds
 * the lock). Rejecting a duplicate keeps RtlDeleteFunctionTable's identity key
 * unambiguous even if a caller frees and reuses the array/id address. */
static int dft_identity_exists_locked(uint64_t identity)
{
    for (dyn_func_table_t *n = s_dft_head; n; n = n->next) {
        if (n->identity == identity)
            return 1;
    }
    return 0;
}

int RtlAddFunctionTable(PRUNTIME_FUNCTION function_table, uint32_t entry_count,
                        uint64_t base_address)
{
    if (!function_table || entry_count == 0 || base_address == 0)
        return 0;
    if (entry_count > UNWIND_MAX_DYNAMIC_ENTRIES)
        return 0;   /* bounds the copy size and prevents multiply wrap */

    /* Validate sorted, non-overlapping, in-order entries; compute span. */
    uint64_t min_rva = function_table[0].BeginAddress;
    uint64_t max_rva = function_table[0].EndAddress;
    for (uint32_t i = 0; i < entry_count; i++) {
        RUNTIME_FUNCTION *e = &function_table[i];
        if (e->BeginAddress >= e->EndAddress)
            return 0;
        if (i > 0 && e->BeginAddress < function_table[i - 1].EndAddress)
            return 0;   /* not sorted / overlapping */
        if (RUNTIME_FUNCTION_CHAINED(e))
            return 0;   /* indirect entries would redirect into the caller's
                         * (freeable) array; not permitted in a copied table */
        if (e->EndAddress > max_rva)
            max_rva = e->EndAddress;
    }
    /* Reject a base+RVA span that would wrap the address space. */
    if (base_address + max_rva < base_address)
        return 0;

    /* Private copy so a caller freeing its array cannot dangle a returned
     * entry pointer. `bytes` cannot wrap: entry_count is capped above. */
    size_t bytes = (size_t)entry_count * sizeof(RUNTIME_FUNCTION);
    dyn_func_table_t *node = (dyn_func_table_t *)kmalloc(sizeof(*node));
    RUNTIME_FUNCTION *copy = (RUNTIME_FUNCTION *)kmalloc(bytes);
    if (!node || !copy) {
        if (node) kfree(node);
        if (copy) kfree(copy);
        return 0;
    }
    memcpy(copy, function_table, bytes);

    node->base_address = base_address;
    node->min_address  = base_address + min_rva;
    node->max_address  = base_address + max_rva;
    node->functions    = copy;
    node->entry_count  = entry_count;
    node->identity     = (uint64_t)(uintptr_t)function_table;
    node->callback     = 0;
    node->context      = 0;

    uint64_t flags;
    spin_lock_irqsave(&s_dft_lock, &flags);
    if (s_dft_count >= UNWIND_MAX_DYNAMIC_TABLES ||
        dft_overlaps_locked(node->min_address, node->max_address) ||
        dft_identity_exists_locked(node->identity)) {
        spin_unlock_irqrestore(&s_dft_lock, flags);
        kfree(copy);
        kfree(node);
        return 0;
    }
    dft_insert_locked(node);
    spin_unlock_irqrestore(&s_dft_lock, flags);
    return 1;
}

int RtlInstallFunctionTableCallback(uint64_t table_identifier, uint64_t base_address,
                                    uint32_t length, PGET_RUNTIME_FUNCTION_CALLBACK callback,
                                    void *context, const char *out_of_process_dll)
{
    /* Low 2 bits set = callback-table convention; out-of-process unsupported. */
    if (!callback || base_address == 0 || length == 0 || out_of_process_dll != 0)
        return 0;
    if ((table_identifier & 3u) != 3u)
        return 0;
    if (base_address + length < base_address)
        return 0;   /* range would wrap the address space */

    dyn_func_table_t *node = (dyn_func_table_t *)kmalloc(sizeof(*node));
    if (!node)
        return 0;
    node->base_address = base_address;
    node->min_address  = base_address;
    node->max_address  = base_address + length;
    node->functions    = 0;
    node->entry_count  = 0;
    node->identity     = table_identifier;
    node->callback     = callback;
    node->context      = context;

    uint64_t flags;
    spin_lock_irqsave(&s_dft_lock, &flags);
    if (s_dft_count >= UNWIND_MAX_DYNAMIC_TABLES ||
        dft_overlaps_locked(node->min_address, node->max_address) ||
        dft_identity_exists_locked(node->identity)) {
        spin_unlock_irqrestore(&s_dft_lock, flags);
        kfree(node);
        return 0;
    }
    dft_insert_locked(node);
    spin_unlock_irqrestore(&s_dft_lock, flags);
    return 1;
}

/* Shared teardown for both delete keys. Returns 1 if a node was removed. */
static int dft_delete_by_identity(uint64_t identity)
{
    uint64_t flags;
    spin_lock_irqsave(&s_dft_lock, &flags);
    dyn_func_table_t **pp = &s_dft_head;
    dyn_func_table_t *node = 0;
    while (*pp) {
        if ((*pp)->identity == identity) {
            node = *pp;
            *pp = node->next;   /* unlink under the lock */
            s_dft_count--;
            break;
        }
        pp = &(*pp)->next;
    }
    spin_unlock_irqrestore(&s_dft_lock, flags);
    if (!node)
        return 0;

    /* The node is unlinked, so no new lookup can reach it, and an in-flight
     * callback lookup holds only local copies of the callback/context (never the
     * node), so the free is safe immediately -- including when this delete is
     * issued from within that very callback (self-unregister does not stall). */
    if (node->functions)
        kfree(node->functions);
    kfree(node);
    return 1;
}

int RtlDeleteFunctionTable(PRUNTIME_FUNCTION function_table)
{
    if (!function_table)
        return 0;
    return dft_delete_by_identity((uint64_t)(uintptr_t)function_table);
}

/* ==========================================================================
 * RtlPcToFileHeader + init
 * ========================================================================== */

void *RtlPcToFileHeader(void *pc_value, void **base_of_image)
{
    loaded_module_t mod;
    if (exec_find_module_by_pc((uint64_t)(uintptr_t)pc_value, &mod) != 0) {
        if (base_of_image)
            *base_of_image = 0;
        return 0;
    }
    void *base = (void *)(uintptr_t)mod.base_address;
    if (base_of_image)
        *base_of_image = base;
    return base;
}

uint32_t rtl_unwind_dynamic_table_count(void)
{
    uint64_t flags;
    spin_lock_irqsave(&s_dft_lock, &flags);
    uint32_t c = s_dft_count;
    spin_unlock_irqrestore(&s_dft_lock, flags);
    return c;
}

void rtl_unwind_init(void)
{
    if (s_unwind_ready)
        return;
    s_unwind_ready = 1;
    klog(LOG_INFO, "rtl",
         "rtl: unwind engine ready (dynamic tables: %u, kernel .pdata: ELF none)",
         (uint64_t)s_dft_count);
}
