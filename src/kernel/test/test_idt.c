/* ============================================================================
 * test_idt.c -- IDT and ISR entry-stub structure (x86-64)
 *
 * These assert the SHAPE of the emitted interrupt entry code, which is a
 * different question from the behaviour its C helpers implement: the depth
 * arithmetic is covered by the `nmi_depth:` cases in test_serial_emergency.c,
 * next to the panic-context predicate that consumes it. What lives here is the
 * part only the machine code can answer -- where the NMI marker is moved
 * relative to the shared prologue and the return.
 *
 * No interrupt is raised and no handler is registered; these read .text.
 * ============================================================================ */

#include "kernel/test/test.h"
#include "kernel/types.h"
#include "kernel/idt.h"            /* g_nmi_depth -- the counter under test */
#include "kernel/vectors.h"        /* VECTOR_NMI */
#include "kernel/gdt.h"            /* GDT_KERNEL_CODE -- the gate selector */

/* ---- the dedicated NMI entry stub (S22) ----
 *
 * The `nmi_depth:` cases in test_serial_emergency.c drive the C mirror of the
 * depth arithmetic (idt_nmi_enter/exit, nesting, zero-saturation). What the
 * hardening actually rests on is WHERE the assembly moves the counter, and that
 * is an ORDERING property no executing handler can observe: a handler reached
 * through vector 2 sees only the aggregate depth, so it cannot tell a marker
 * raised ahead of the register pushes from one raised after them. The evidence
 * has to come from the emitted machine code, so that is what these read.
 *
 * Deliberately NOT verified by executing `int $2`. Vector 2 is registered to
 * nmi_crash_handler (panic.c), which never returns, and handlers[] is global --
 * so borrowing the vector would displace the real crash path on every CPU, while
 * a software INT (which establishes no hardware NMI blocking) would leave the
 * live IST2 frame exposed to a real NMI. Reading .text needs no interrupt at
 * all: the stubs are ordinary symbols in the kernel image.
 *
 * The entry stub is matched as a CONTIGUOUS template rather than by searching
 * for landmarks, and that choice is load-bearing in both directions. Searching
 * for the first `push`-immediate or `jmp` opcode byte reads bytes that follow a
 * RIP-relative displacement, so a link layout whose displacement happened to
 * contain 0x6A or 0xE9 would fail a CORRECT stub -- a failure that would arrive
 * as a mystery, months later, from an unrelated change. And matching only an
 * ORDER permits an extra instruction to be inserted between two matched points
 * without any assertion noticing. A contiguous template has neither problem:
 * nothing can be inserted, and nothing is searched for. Only the two
 * displacements vary with link layout, and those are RESOLVED and compared
 * against the symbols they must reach. */
#define X86_OP_LEA         0x8Du  /* LEA opcode, both operand sizes */
#define X86_LEA_NOREX_LEN  6u     /* 8D /r <rel32> -- 32-bit dest    */
#define X86_REX_W          0x48u  /* REX with W set: 0x48..0x4F      */
#define X86_REX_W_MASK     0xF8u
#define X86_JMP_REL32_LEN  5u
#define X86_LEA_RIP_LEN    7u
/* A RIP-relative memory operand is mod=00, rm=101; the reg field is the
 * destination and is deliberately NOT constrained. */
#define X86_MODRM_RIP_MASK 0xC7u
#define X86_MODRM_RIP_BITS 0x05u

/* isr2 in full: 40 bytes, two displacement holes. */
#define ISR2_EXPECTED_LEN  40u
#define ISR2_LEA_AT        16u
#define ISR2_TAIL_AT       23u
#define ISR2_JMP_AT        35u

static const uint8_t ISR2_HEAD[] = {
    0x50u, 0x53u, 0x51u, 0x52u,        /* push rax ; rbx ; rcx ; rdx */
    0xB8u, 0x01u, 0x00u, 0x00u, 0x00u, /* mov  eax, 1                */
    0x31u, 0xC9u,                      /* xor  ecx, ecx              */
    0x0Fu, 0xA2u,                      /* cpuid                      */
    0xC1u, 0xEBu, 0x18u,               /* shr  ebx, 24               */
    0x48u, 0x8Du, 0x0Du                /* lea  rcx, [rip+disp32] ... */
};
static const uint8_t ISR2_TAIL[] = {
    0xF0u, 0xFFu, 0x04u, 0x99u,        /* lock incl (rcx + rbx*4)    */
    0x5Au, 0x59u, 0x5Bu, 0x58u,        /* pop  rdx ; rcx ; rbx ; rax */
    0x6Au, 0x00u,                      /* push $0   (dummy err)      */
    0x6Au, 0x02u,                      /* push $2   (vector)         */
    0xE9u                              /* jmp  rel32 ...             */
};

/* The NMI lower, from the counter's own address computation through the return,
 * as ONE contiguous 25-byte span with the displacement as its only hole.
 *
 * Every part of it is load-bearing. The second LEA carries the scale and index,
 * so asserting it is what rejects a stub that lowers the WRONG CPU's slot -- a
 * changed scale, or an `add rcx, 4` slipped in between, would otherwise pass
 * while decrementing a neighbour's entry and leaving this CPU permanently inside
 * an NMI. The comparison's immediate and the branch distance are the claim too:
 * `je +3` skips exactly the 3-byte decrement and lands on the `pop`, so a
 * saturation test rewritten to `cmp $1` would skip the decrement at the ordinary
 * depth of one while every byte of a shorter match stayed identical. */
#define NMI_LOWER_SEQ_LEN  41u   /* first save .. IRETQ, displacement included */
#define NMI_LOWER_HEAD_LEN 19u
#define NMI_LOWER_TAIL_LEN 18u
#define NMI_LOWER_LEA_OFF  16u   /* the LEA opcode, within the head */

/* The span starts at the FIRST register save, not at the LEA, because the id
 * derivation is part of what decides which slot gets decremented. With the span
 * starting later, `shr ebx, 23` or a wrong CPUID leaf would decrement another
 * CPU's entry with every asserted byte unchanged -- and the C mirror cannot see
 * it, because the mirror never executes this assembly. The push order matters
 * for the same reason: rcx is pushed FIRST so it is restored LAST. */
static const uint8_t NMI_LOWER_HEAD[] = {
    0x51u, 0x50u, 0x53u, 0x52u,        /* push rcx ; rax ; rbx ; rdx */
    0xB8u, 0x01u, 0x00u, 0x00u, 0x00u, /* mov  eax, 1                */
    0x31u, 0xC9u,                      /* xor  ecx, ecx              */
    0x0Fu, 0xA2u,                      /* cpuid                      */
    0xC1u, 0xEBu, 0x18u,               /* shr  ebx, 24               */
    0x48u, 0x8Du, 0x0Du                /* lea  rcx, [rip+disp32] ... */
};
static const uint8_t NMI_LOWER_TAIL[] = {
    0x48u, 0x8Du, 0x0Cu, 0x99u,        /* lea  rcx, (rcx + rbx*4)    */
    0x5Au, 0x5Bu, 0x58u,               /* pop  rdx ; rbx ; rax       */
    0x83u, 0x39u, 0x00u,               /* cmpl $0, (rcx)             */
    0x74u, 0x03u,                      /* je   +3 -> the pop below   */
    0xF0u, 0xFFu, 0x09u,               /* lock decl (rcx)            */
    0x59u,                             /* pop  rcx                   */
    0x48u, 0xCFu                       /* iretq                      */
};

static int test_bytes_match(const uint8_t *code, uint32_t len, uint32_t at,
                            const uint8_t *seq, uint32_t seq_len)
{
    if (at > len || (len - at) < seq_len)
        return 0;
    for (uint32_t i = 0; i < seq_len; i++)
        if (code[at + i] != seq[i])
            return 0;
    return 1;
}

/* The stubs export _end symbols so every scan below is bounded by the SYMBOL. */
extern void isr2(void);              extern void isr2_end(void);
extern void isr_common_stub(void);   extern void isr_common_stub_end(void);
extern void isr_nmi_stub(void);      extern void isr_nmi_stub_end(void);

static const uint8_t *test_sym_bytes(void (*fn)(void))
{
    return (const uint8_t *)(uintptr_t)fn;
}

/* Largest span any stub in this file can legitimately have. A bound is needed
 * because a misplaced `_end` would otherwise yield an enormous or (after the
 * narrowing) a wrapped-but-plausible length, and every scan below would then
 * read unrelated text -- turning the diagnostic into the fault it diagnoses.
 * Callers treat 0 as "not a sane symbol" and stop. */
#define TEST_SYM_LEN_MAX 512u

static uint32_t test_sym_len(void (*start)(void), void (*end)(void))
{
    uintptr_t a = (uintptr_t)start, b = (uintptr_t)end;
    uintptr_t span;

    if (b <= a)
        return 0u;
    span = b - a;
    if (span > (uintptr_t)TEST_SYM_LEN_MAX)
        return 0u;
    return (uint32_t)span;
}

/* Resolve a RIP-relative displacement: x86-64 measures it from the END of the
 * instruction, so this is what the linker's relocation actually produced. */
/* Index of a RIP-relative LEA whose resolved target is `want`, or -1.
 *
 * BEST-EFFORT, and the limitation is the point rather than an excuse. This is a
 * NEGATIVE claim -- "the shared stub never addresses the counter" -- and a
 * negative claim cannot be soundly byte-matched over a variable-length
 * instruction set. That is not a prediction: four consecutive review rounds each
 * found another encoding this scan missed (first only `lea rcx`, then r8-r15 via
 * REX.R, then the redundant REX.X/B forms, then the no-REX 32-bit form, which is
 * a valid pointer here because the kernel lives below 4 GiB). Each was a real
 * hole and each widening was correct, but the sequence is the actual evidence:
 * the instrument is wrong for the job.
 *
 * What it DOES buy is a canary on the realistic regression -- depth code
 * re-entering the shared body, which comes from the NMI_DEPTH_* macros and emits
 * an ordinary LEA -- and it is negative-controlled in that shape. What it cannot
 * do is rule out every reference; a direct RIP-relative RMW has no LEA at all.
 * The sound version is a build-time relocation assertion over isr_stubs.o, which
 * is encoding-independent by construction and is PARKED in the roadmap against
 * the receipt-surface blocker that prevents this run from adding a build step. */
static int test_find_lea_to(const uint8_t *code, uint32_t len, uintptr_t want);

static uintptr_t test_rip_target(const uint8_t *code, uint32_t at, uint32_t insn_len)
{
    uint32_t off = at + insn_len - 4u;
    uint32_t raw = (uint32_t)code[off]
                 | ((uint32_t)code[off + 1u] << 8)
                 | ((uint32_t)code[off + 2u] << 16)
                 | ((uint32_t)code[off + 3u] << 24);
    /* The displacement is signed and measured from the end of the instruction.
     * Assembled byte-wise rather than via memcpy: this is freestanding kernel
     * code, and a sign-extended add is exactly what the CPU does here. */
    uint64_t sext = (uint64_t)(int64_t)(int32_t)raw;

    return (uintptr_t)&code[at + insn_len] + (uintptr_t)sext;
}

/* Resolve a RIP-relative displacement ONLY if the whole instruction lies inside
 * the symbol. The bound is not defensive clutter: TEST_ASSERT records a failure
 * and CONTINUES, so on the exact regression these tests exist to catch -- a
 * misplaced _end symbol or a stub that shrank -- an unguarded `len - N` wraps
 * and the resolver reads far outside the symbol. A diagnostic that faults on the
 * fault it diagnoses is worse than no diagnostic. Returns 0 when out of range,
 * which no real target can be. */
static uintptr_t test_rip_target_bounded(const uint8_t *code, uint32_t len,
                                         uint32_t at, uint32_t insn_len)
{
    if (at > len || (len - at) < insn_len)
        return 0u;
    return test_rip_target(code, at, insn_len);
}

static int test_find_lea_to(const uint8_t *code, uint32_t len, uintptr_t want)
{
    if (len < X86_LEA_NOREX_LEN)
        return -1;
    for (uint32_t i = 0; i <= len - X86_LEA_NOREX_LEN; i++) {
        uint32_t insn_len = 0u;

        /* Two shapes, both resolving the same displacement:
         *   8D /r <rel32>            6 bytes, 32-bit destination (no REX)
         *   4{8..F} 8D /r <rel32>    7 bytes, 64-bit destination
         * A REX form also matches the no-REX shape one byte in, which is
         * harmless: the displacement and the end-of-instruction are identical,
         * so both readings resolve to the same target. */
        if (code[i] == X86_OP_LEA && (code[i + 1u] & X86_MODRM_RIP_MASK) ==
                                     X86_MODRM_RIP_BITS) {
            insn_len = X86_LEA_NOREX_LEN;
        } else if ((code[i] & X86_REX_W_MASK) == X86_REX_W &&
                   (len - i) >= X86_LEA_RIP_LEN &&
                   code[i + 1u] == X86_OP_LEA &&
                   (code[i + 2u] & X86_MODRM_RIP_MASK) == X86_MODRM_RIP_BITS) {
            insn_len = X86_LEA_RIP_LEN;
        } else {
            continue;
        }

        if ((len - i) < insn_len)
            continue;
        if (test_rip_target(code, i, insn_len) == want)
            return (int)i;
    }
    return -1;
}

/* The whole stub, asserted contiguously. The raise lands ahead of `push qword 0`
 * / `push qword 2`, which are the first instructions the generic ISR_NOERRCODE
 * macro would have emitted -- so proving the locked increment precedes them
 * proves it precedes everything downstream too: the 15 register pushes, the
 * conditional swapgs, the LFENCE, and every C statement in isr_handler.
 *
 * Contiguity is what makes that a proof rather than an ordering hint. Because
 * the template covers all 40 bytes with no gaps, no instruction can be inserted
 * between the raise and the pushes, and nothing is located by searching, so no
 * link layout can make a correct stub fail. The two displacements are the only
 * bytes allowed to vary, and each is resolved against the symbol it must
 * reach. */
static void test_nmi_stub_raises_before_shared_prologue(void)
{
    const uint8_t *code = test_sym_bytes(isr2);
    uint32_t len = test_sym_len(isr2, isr2_end);

    TEST_ASSERT_EQ((uint64_t)len, (uint64_t)ISR2_EXPECTED_LEN,
                   "isr2 is exactly the sequence this test knows");

    TEST_ASSERT(test_bytes_match(code, len, 0u,
                                 ISR2_HEAD, (uint32_t)sizeof(ISR2_HEAD)),
                "isr2 saves the CPUID clobber set, derives the id, addresses the "
                "counter -- and does nothing else first");
    TEST_ASSERT(test_bytes_match(code, len, ISR2_TAIL_AT,
                                 ISR2_TAIL, (uint32_t)sizeof(ISR2_TAIL)),
                "the depth is RAISED, the four saves restored, and only THEN are "
                "the error code and vector pushed");

    /* Bind to the counter's relocated ADDRESS: a template alone would still pass
     * if the stub incremented some other symbol's first word. */
    TEST_ASSERT_EQ((uint64_t)test_rip_target_bounded(code, len, ISR2_LEA_AT, X86_LEA_RIP_LEN),
                   (uint64_t)(uintptr_t)&g_nmi_depth[0],
                   "isr2 indexes g_nmi_depth itself, not some other symbol");

    /* And control must reach the NMI copy of the body, not the shared one. */
    TEST_ASSERT_EQ((uint64_t)test_rip_target_bounded(code, len, ISR2_JMP_AT, X86_JMP_REL32_LEN),
                   (uint64_t)(uintptr_t)isr_nmi_stub,
                   "isr2 jumps to the NMI body -- reaching isr_common_stub instead "
                   "would silently drop the epilogue lower");
}

/* The other half of the bargain: the stub every other vector on the machine runs
 * through must pay nothing for a counter only the NMI path reads. A per-vector
 * test in the common stub is exactly the cost S18 refused to impose, so the
 * absence of the depth code there is a real invariant, not an implementation
 * detail -- it is why the depth could be moved into assembly at all. */
static void test_nmi_stub_leaves_shared_stub_uncharged(void)
{
    const uint8_t *code = test_sym_bytes(isr_common_stub);
    uint32_t len = test_sym_len(isr_common_stub, isr_common_stub_end);

    TEST_ASSERT(len > 0u, "the shared stub is symbol-bounded");

    /* Asserted by RESOLVED ADDRESS, not by opcode search. A raw scan for the
     * CPUID pair sat here and was removed rather than kept: undecoded bytes can
     * acquire `0F A2` inside a displacement after an unrelated link-layout
     * change, which would fail a correct shared stub for no reason. This scan
     * reports a hit only when a displacement actually resolves to the counter,
     * so it cannot false-fail that way -- and CPUID that never reaches
     * g_nmi_depth could not move the depth anyway. A full byte template was
     * rejected too: the shared body carries the MDS VERW gate, the C call and
     * the swapgs pair, all of which may legitimately change. */
    TEST_ASSERT(test_find_lea_to(code, len, (uintptr_t)&g_nmi_depth[0]) < 0,
                "the shared stub never addresses the NMI depth counter -- "
                "ordinary interrupts pay nothing for it");
}

/* The epilogue half of the coverage claim. Nothing else can see this: the C
 * mirror (idt_nmi_exit) is a separate implementation, so deleting NMI_DEPTH_LOWER
 * outright would leave every other test in this file green while any returning
 * NMI left its CPU permanently classified as inside an NMI.
 *
 * Asserting the lower exists is not enough either -- WHERE it sits is the claim.
 * Placing it before the register restores, which is the shape this code shipped
 * first, put four faultable stack reads inside the window. So the assertion is
 * the exact return tail, comparison and branch included. */
static void test_nmi_stub_lowers_immediately_before_return(void)
{
    const uint8_t *code = test_sym_bytes(isr_nmi_stub);
    uint32_t len = test_sym_len(isr_nmi_stub, isr_nmi_stub_end);
    uint32_t head_at, lea_at;

    TEST_ASSERT(len > NMI_LOWER_SEQ_LEN, "the NMI body is symbol-bounded");
    if (len <= NMI_LOWER_SEQ_LEN)
        return;
    head_at = len - NMI_LOWER_SEQ_LEN;
    lea_at  = head_at + NMI_LOWER_LEA_OFF;

    /* One contiguous span from the first register save to the return, anchored
     * at the END of the body, so nothing can sit between the id it derives, the
     * address it resolves, and the entry it decrements. */
    TEST_ASSERT(test_bytes_match(code, len, head_at,
                                 NMI_LOWER_HEAD, NMI_LOWER_HEAD_LEN),
                "the NMI body saves rcx first, derives its id, and addresses the "
                "counter before returning");
    TEST_ASSERT(test_bytes_match(code, len, len - NMI_LOWER_TAIL_LEN,
                                 NMI_LOWER_TAIL, NMI_LOWER_TAIL_LEN),
                "it indexes its OWN slot, restores three, then lock-dec, one "
                "restore, iretq");

    /* And the address it computes is the counter, not another symbol. */
    TEST_ASSERT_EQ((uint64_t)test_rip_target_bounded(code, len, lea_at, X86_LEA_RIP_LEN),
                   (uint64_t)(uintptr_t)&g_nmi_depth[0],
                   "the lower decrements g_nmi_depth itself");
}


/* ---- the integration boundary: does vector 2 actually REACH that stub? ----
 *
 * Every assertion above starts AT the isr2 symbol, so all of them stay green if
 * the IDT maps vector 2 somewhere else entirely -- the stub would be perfect and
 * unreachable, both depth transitions skipped on a real NMI, and the nested-abort
 * hazard back with nothing to show for it. That gap would surface only on the
 * terminal crash path, which is the worst place to discover anything.
 *
 * Read from the LOADED IDTR rather than from idt.c's table, so this tests what
 * the CPU will actually use. Strictly read-only: `sidt` stores the register, and
 * nothing here initialises the IDT or touches the handler table.
 *
 * SCOPE, stated because the assertion is easy to over-read: this proves the
 * gate on the CPU RUNNING THE SUITE, which is the BSP. It does NOT prove an NMI
 * on an AP reaches the stub, and today it could not -- gdt.c:130-134 configures
 * the BSP TSS only, so APs have no per-CPU IST2 to switch to and an AP NMI can
 * fault before the stub is entered. That gap is pre-existing, owned, and already
 * carried as a Critical accepted item on this TODO pointing at TODO-09 section
 * 10 (per-CPU TSS + IST); it is not this section's to close, and this test does
 * not pretend otherwise. */
struct test_idt_descriptor {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  ist;
    uint8_t  type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t zero;
} __attribute__((packed));

_Static_assert(sizeof(struct test_idt_descriptor) == 16u,
               "an x86-64 IDT descriptor is 16 bytes; this mirrors idt.c's layout");

#define TEST_IDT_IST_MASK    0x07u
#define TEST_NMI_IST_INDEX   2u
/* Present, DPL=0, 64-bit interrupt gate -- what idt.c installs for vectors 0-31.
 * The FULL byte is asserted, not just the present bit: a descriptor can be
 * present and still architecturally undispatchable, so checking presence alone
 * would let a wrong gate type through while claiming the vector is reachable. */
#define TEST_IDT_GATE_TYPE   0x8Eu

static void test_nmi_vector_is_wired_to_the_dedicated_stub(void)
{
    struct { uint16_t limit; uint64_t base; } __attribute__((packed)) idtr;
    const struct test_idt_descriptor *gate;
    uint64_t offset;

    __asm__ volatile ("sidt %0" : "=m"(idtr));

    /* BOTH validations gate the dereference below. TEST_ASSERT records a failure
     * and CONTINUES, so a malformed-but-nonzero base or a short table would
     * otherwise walk straight into `gate->...` -- and SIDT proves only what the
     * register holds, not that it points at a mapped table big enough to index.
     * A corrupt IDT is exactly when this must report rather than fault. */
    if (idtr.base == 0u ||
        idtr.limit < (uint16_t)(3u * sizeof(struct test_idt_descriptor) - 1u)) {
        TEST_ASSERT(0, "an IDT is loaded and covers vector 2");
        return;
    }

    gate = (const struct test_idt_descriptor *)(uintptr_t)idtr.base + VECTOR_NMI;
    offset = (uint64_t)gate->offset_low
           | ((uint64_t)gate->offset_mid << 16)
           | ((uint64_t)gate->offset_high << 32);

    TEST_ASSERT_EQ(offset, (uint64_t)(uintptr_t)isr2,
                   "this CPU's loaded vector 2 gate targets the dedicated NMI stub");
    TEST_ASSERT_EQ((uint64_t)(gate->ist & TEST_IDT_IST_MASK),
                   (uint64_t)TEST_NMI_IST_INDEX,
                   "and still runs on IST2, unchanged by the dedicated stub");

    /* The rest of the descriptor decides whether the CPU can dispatch there at
     * all. Offset plus present bit is NOT the "actually reaches it" claim: a
     * null selector, or a present descriptor whose gate type is wrong, is
     * unusable while satisfying both. */
    TEST_ASSERT_EQ((uint64_t)gate->selector, (uint64_t)GDT_KERNEL_CODE,
                   "through the ring-0 code selector");
    TEST_ASSERT_EQ((uint64_t)gate->type_attr, (uint64_t)TEST_IDT_GATE_TYPE,
                   "as a present DPL-0 64-bit interrupt gate");
    TEST_ASSERT_EQ((uint64_t)(gate->ist & (uint8_t)~TEST_IDT_IST_MASK), 0u,
                   "with the reserved IST bits clear");
    TEST_ASSERT_EQ((uint64_t)gate->zero, 0u,
                   "and the reserved word clear");
}

void test_register_idt(void)
{
    test_suite_register_cat("nmi_stub: BSP vector 2 gate targets isr2",
                            test_nmi_vector_is_wired_to_the_dedicated_stub,
                            TEST_CAT_BOOT);
    test_suite_register_cat("nmi_stub: depth is raised before the shared prologue",
                            test_nmi_stub_raises_before_shared_prologue,
                            TEST_CAT_BOOT);
    test_suite_register_cat("nmi_stub: shared stub pays nothing for the depth",
                            test_nmi_stub_leaves_shared_stub_uncharged,
                            TEST_CAT_BOOT);
    test_suite_register_cat("nmi_stub: depth is lowered immediately before IRETQ",
                            test_nmi_stub_lowers_immediately_before_return,
                            TEST_CAT_BOOT);
}
