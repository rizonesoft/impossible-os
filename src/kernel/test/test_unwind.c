/* ============================================================================
 * test_unwind.c -- x64 table-based unwind engine unit tests (TODO-23 s6)
 *
 * The kernel is an ELF image with no .pdata, so these tests exercise the engine
 * against SYNTHETIC RUNTIME_FUNCTION/UNWIND_INFO records registered through the
 * dynamic function-table registry (RtlAddFunctionTable). They cover: the ABI
 * struct layout, registry add/delete/validation, prolog-based RtlVirtualUnwind,
 * a 3-frame recovered-RIP chain, epilog detection, frame-register (SET_FPREG)
 * unwinding, the lazy callback path, and malformed-metadata fail-safe.
 *
 * XREF: 02-kernel-core/TODO-23-exception-dispatch-seh.md Unit Tests
 * ============================================================================ */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "kernel/rtl/unwind.h"
#include "kernel/cpu_security.h"   /* __kstack_read_u64 (fault-safe read under test) */
#include "kernel/mm/vmm.h"         /* vmm_get_physical: find an unmapped VA safely */

/* An 8-byte-aligned image scratch: RUNTIME_FUNCTION RVAs index into it, and
 * control_pc = (uint64_t)img + rva. Since img lives on the kernel stack (not a
 * registered module) the engine treats it as a trusted/unbounded table. */
typedef union {
    uint8_t  b[256];
    uint64_t align;
} img_buf_t;

/* Build the common "push rbp; sub rsp,0x20" UNWIND_INFO at img+0x40. */
static void build_simple_uinfo(img_buf_t *img)
{
    uint8_t *u = &img->b[0x40];
    u[0] = 0x01;   /* version 1, flags 0 */
    u[1] = 0x05;   /* SizeOfProlog = 5 */
    u[2] = 0x02;   /* CountOfCodes = 2 */
    u[3] = 0x00;   /* FrameRegister 0, FrameOffset 0 */
    /* code[0]: UWOP_ALLOC_SMALL, CodeOffset 5, size 0x20 (info=(0x20-8)/8=3) */
    u[4] = 0x05; u[5] = (uint8_t)(UWOP_ALLOC_SMALL | (3u << 4));
    /* code[1]: UWOP_PUSH_NONVOL rbp(5), CodeOffset 1 */
    u[6] = 0x01; u[7] = (uint8_t)(UWOP_PUSH_NONVOL | (5u << 4));
}

/* A RUNTIME_FUNCTION covering [0x10,0x30) with UNWIND_INFO at 0x40. */
static void build_simple_rf(RUNTIME_FUNCTION *rf)
{
    rf->BeginAddress = 0x10;
    rf->EndAddress = 0x30;
    rf->UnwindInfoAddress = 0x40;
}

/* ---- ABI layout ---- */

static void test_unwind_abi(void)
{
    TEST_ASSERT_EQ(sizeof(RUNTIME_FUNCTION), 12, "RUNTIME_FUNCTION is 12 bytes");
    TEST_ASSERT_EQ(sizeof(UNWIND_CODE), 2, "UNWIND_CODE is 2 bytes");
    TEST_ASSERT_EQ(sizeof(SCOPE_TABLE_ENTRY), 16, "SCOPE_TABLE_ENTRY is 16 bytes");

    UNWIND_CODE uc;
    uc.b.CodeOffset = 7;
    uc.b.OpAndInfo = (uint8_t)(UWOP_SAVE_NONVOL | (3u << 4));  /* op=4, info=3 */
    TEST_ASSERT_EQ(UNWIND_CODE_OP(uc), UWOP_SAVE_NONVOL, "UNWIND_CODE_OP extracts low nibble");
    TEST_ASSERT_EQ(UNWIND_CODE_INFO(uc), 3u, "UNWIND_CODE_INFO extracts high nibble");

    UNWIND_INFO ui;
    ui.VersionAndFlags = (uint8_t)(1u | (UNW_FLAG_CHAININFO << 3));
    ui.FrameRegAndOff = (uint8_t)(5u | (2u << 4));
    TEST_ASSERT_EQ(UNWIND_INFO_VERSION(&ui), 1u, "UNWIND_INFO_VERSION == 1");
    TEST_ASSERT_EQ(UNWIND_INFO_FLAGS(&ui), UNW_FLAG_CHAININFO, "UNWIND_INFO_FLAGS == CHAININFO");
    TEST_ASSERT_EQ(UNWIND_INFO_FRAMEREG(&ui), 5u, "UNWIND_INFO_FRAMEREG == 5 (rbp)");
    TEST_ASSERT_EQ(UNWIND_INFO_FRAMEOFF(&ui), 2u, "UNWIND_INFO_FRAMEOFF == 2");
}

/* ---- Registry add / lookup / delete ---- */

static void test_unwind_add_lookup_delete(void)
{
    img_buf_t img = { .b = { 0 } };
    RUNTIME_FUNCTION rf;
    build_simple_uinfo(&img);
    build_simple_rf(&rf);
    uint64_t base = (uint64_t)(uintptr_t)&img;

    uint32_t before = rtl_unwind_dynamic_table_count();
    int added = RtlAddFunctionTable(&rf, 1, base);
    TEST_ASSERT_EQ(added, 1, "RtlAddFunctionTable succeeds");
    TEST_ASSERT_EQ(rtl_unwind_dynamic_table_count(), before + 1u, "table count +1 after add");

    uint64_t got_base = 0;
    PRUNTIME_FUNCTION found = RtlLookupFunctionEntry(base + 0x18, &got_base, 0);
    TEST_ASSERT_NOT_NULL(found, "lookup finds the entry for an in-range PC");
    TEST_ASSERT_EQ(got_base, base, "lookup reports the table base");
    if (found) {
        TEST_ASSERT_EQ(found->BeginAddress, 0x10u, "found entry BeginAddress");
        TEST_ASSERT_EQ(found->EndAddress, 0x30u, "found entry EndAddress");
    }

    /* Out-of-range PC below and above the table returns NULL. */
    TEST_ASSERT_NULL(RtlLookupFunctionEntry(base + 0x08, 0, 0), "PC below range -> NULL");
    TEST_ASSERT_NULL(RtlLookupFunctionEntry(base + 0x40, 0, 0), "PC above range -> NULL");

    int removed = RtlDeleteFunctionTable(&rf);
    TEST_ASSERT_EQ(removed, 1, "RtlDeleteFunctionTable removes the table");
    TEST_ASSERT_EQ(rtl_unwind_dynamic_table_count(), before, "table count restored after delete");
    TEST_ASSERT_EQ(RtlDeleteFunctionTable(&rf), 0, "deleting an unknown table returns 0");
    TEST_ASSERT_NULL(RtlLookupFunctionEntry(base + 0x18, 0, 0), "lookup after delete -> NULL");
}

/* ---- Indirect (redirected) RUNTIME_FUNCTION handling ---- */

static PRUNTIME_FUNCTION g_ind_cb_return;
static PRUNTIME_FUNCTION test_ind_cb(uint64_t control_pc, void *context)
{
    (void)control_pc;
    (void)context;
    return g_ind_cb_return;   /* may be an INDIRECT entry (bit 0 set) */
}

static void test_unwind_indirect_entry(void)
{
    /* Static (copied) tables reject indirect entries -- a redirect would point
     * into the caller's freeable array. */
    img_buf_t simg = { .b = { 0 } };
    uint64_t sbase = (uint64_t)(uintptr_t)&simg;
    RUNTIME_FUNCTION ind = { .BeginAddress = 0x10, .EndAddress = 0x30, .UnwindInfoAddress = 0x50 | 1u };
    TEST_ASSERT_EQ(RtlAddFunctionTable(&ind, 1, sbase), 0, "static table rejects an indirect entry");

    /* Indirect (fragment) entries are not yet supported: a callback returning an
     * indirect entry yields NULL (fail-safe leaf) rather than a mis-ranged
     * unwind. A direct callback entry is returned normally. */
    img_buf_t img = { .b = { 0 } };
    build_simple_uinfo(&img);
    uint64_t base = (uint64_t)(uintptr_t)&img;
    static RUNTIME_FUNCTION ind_child;
    ind_child.BeginAddress = 0x10; ind_child.EndAddress = 0x30; ind_child.UnwindInfoAddress = 0x50 | 1u;
    g_ind_cb_return = &ind_child;
    TEST_ASSERT_EQ(RtlInstallFunctionTableCallback(base | 3u, base, 0x100, test_ind_cb, 0, 0), 1,
                   "indirect-callback table registered");
    TEST_ASSERT_NULL(RtlLookupFunctionEntry(base + 0x18, 0, 0),
                     "indirect callback entry -> NULL (unsupported, fail-safe)");
    RtlDeleteFunctionTable((PRUNTIME_FUNCTION)(uintptr_t)(base | 3u));

    /* A direct callback entry is returned normally. */
    static RUNTIME_FUNCTION direct;
    direct.BeginAddress = 0x10; direct.EndAddress = 0x30; direct.UnwindInfoAddress = 0x40;
    g_ind_cb_return = &direct;
    TEST_ASSERT_EQ(RtlInstallFunctionTableCallback(base | 3u, base, 0x100, test_ind_cb, 0, 0), 1,
                   "direct-callback table registered");
    uint64_t ib = 0;
    PRUNTIME_FUNCTION e = RtlLookupFunctionEntry(base + 0x18, &ib, 0);
    TEST_ASSERT_NOT_NULL(e, "direct callback entry returned");
    if (e)
        TEST_ASSERT_EQ(e->UnwindInfoAddress, 0x40u, "direct callback entry unchanged");
    RtlDeleteFunctionTable((PRUNTIME_FUNCTION)(uintptr_t)(base | 3u));
}

/* ---- Registration validation ---- */

static void test_unwind_add_validation(void)
{
    uint64_t base = 0x400000;

    RUNTIME_FUNCTION bad = { .BeginAddress = 0x20, .EndAddress = 0x10, .UnwindInfoAddress = 0 };
    TEST_ASSERT_EQ(RtlAddFunctionTable(&bad, 1, base), 0, "Begin >= End rejected");
    TEST_ASSERT_EQ(RtlAddFunctionTable(0, 1, base), 0, "NULL table rejected");
    TEST_ASSERT_EQ(RtlAddFunctionTable(&bad, 0, base), 0, "zero entry_count rejected");

    /* Unsorted / overlapping entries in one table rejected. */
    RUNTIME_FUNCTION unsorted[2] = {
        { .BeginAddress = 0x10, .EndAddress = 0x40, .UnwindInfoAddress = 0x80 },
        { .BeginAddress = 0x20, .EndAddress = 0x50, .UnwindInfoAddress = 0x90 },  /* overlaps [0x10,0x40) */
    };
    TEST_ASSERT_EQ(RtlAddFunctionTable(unsorted, 2, base), 0, "overlapping entries rejected");

    /* Two disjoint tables at overlapping ranges: second rejected. */
    RUNTIME_FUNCTION a = { .BeginAddress = 0x10, .EndAddress = 0x20, .UnwindInfoAddress = 0x80 };
    RUNTIME_FUNCTION b = { .BeginAddress = 0x18, .EndAddress = 0x28, .UnwindInfoAddress = 0x90 };
    TEST_ASSERT_EQ(RtlAddFunctionTable(&a, 1, base), 1, "first table added");
    TEST_ASSERT_EQ(RtlAddFunctionTable(&b, 1, base), 0, "overlapping second table rejected");
    RtlDeleteFunctionTable(&a);

    /* Re-registering the SAME pointer (identity) is rejected even for a disjoint
     * range, so RtlDeleteFunctionTable's identity key stays unambiguous. */
    RUNTIME_FUNCTION c = { .BeginAddress = 0x10, .EndAddress = 0x20, .UnwindInfoAddress = 0x80 };
    TEST_ASSERT_EQ(RtlAddFunctionTable(&c, 1, base), 1, "identity table added");
    c.BeginAddress = 0x100; c.EndAddress = 0x110;   /* mutate to a disjoint range */
    TEST_ASSERT_EQ(RtlAddFunctionTable(&c, 1, base), 0, "duplicate identity rejected (disjoint range)");
    RtlDeleteFunctionTable(&c);
}

/* ---- Prolog-based RtlVirtualUnwind ---- */

static void test_unwind_virtual_prolog(void)
{
    img_buf_t img = { .b = { 0 } };
    RUNTIME_FUNCTION rf;
    build_simple_uinfo(&img);
    build_simple_rf(&rf);
    uint64_t base = (uint64_t)(uintptr_t)&img;

    /* Stack: caller pushed RETADDR at entry_rsp = &stk[10]; prolog saved rbp at
     * stk[9] and allocated 0x20; body Rsp = &stk[5]. */
    uint64_t stk[16] = { 0 };
    const uint64_t RETADDR = 0xC0FFEE1234ULL;
    const uint64_t SAVED_RBP = 0xB1B2B3B4B5ULL;
    stk[10] = RETADDR;
    stk[9] = SAVED_RBP;

    CONTEXT ctx = { 0 };
    ctx.Rip = base + 0x18;             /* body PC (past 5-byte prolog) */
    ctx.Rsp = (uint64_t)(uintptr_t)&stk[5];
    ctx.Rbp = 0;

    uint64_t est = 0;
    void *handler = RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, ctx.Rip, &rf,
                                     &ctx, 0, &est, 0);
    TEST_ASSERT_NULL(handler, "no handler on a frame with UNW_FLAG_NHANDLER");
    TEST_ASSERT_EQ(ctx.Rip, RETADDR, "prolog unwind recovers the return address");
    TEST_ASSERT_EQ(ctx.Rbp, SAVED_RBP, "prolog unwind restores saved rbp");
    TEST_ASSERT_EQ(ctx.Rsp, (uint64_t)(uintptr_t)&stk[11], "Rsp popped past the return address");
}

/* ---- 3-frame recovered-RIP chain ---- */

static void test_unwind_three_frame_chain(void)
{
    img_buf_t img = { .b = { 0 } };
    RUNTIME_FUNCTION rf;
    build_simple_uinfo(&img);
    build_simple_rf(&rf);
    uint64_t base = (uint64_t)(uintptr_t)&img;
    RtlAddFunctionTable(&rf, 1, base);

    /* Three recursive frames, each 0x30 bytes: for frame k the alloc lands rbp
     * slot at 6k+4, retaddr at 6k+5. Innermost body Rsp = &stk[0]. */
    uint64_t stk[24] = { 0 };
    const uint64_t BODY_PC = base + 0x18;
    const uint64_t SENTINEL = base + 0x1000;   /* outside the table -> lookup NULL */
    stk[4] = 0xAA11; stk[5] = BODY_PC;         /* frame A -> B */
    stk[10] = 0xAA22; stk[11] = BODY_PC;       /* frame B -> C */
    stk[16] = 0xAA33; stk[17] = SENTINEL;      /* frame C -> stop */

    CONTEXT ctx = { 0 };
    ctx.Rip = BODY_PC;
    ctx.Rsp = (uint64_t)(uintptr_t)&stk[0];

    int frames = 0;
    uint64_t expect_rip[3] = { BODY_PC, BODY_PC, SENTINEL };
    for (int i = 0; i < 8; i++) {
        uint64_t ib = 0;
        PRUNTIME_FUNCTION e = RtlLookupFunctionEntry(ctx.Rip, &ib, 0);
        if (!e)
            break;
        uint64_t est = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, ib, ctx.Rip, e, &ctx, 0, &est, 0);
        TEST_ASSERT_EQ(ctx.Rip, expect_rip[frames], "chain step recovers expected RIP");
        frames++;
    }
    TEST_ASSERT_EQ(frames, 3, "walk recovers exactly 3 frames then stops at the sentinel");
    TEST_ASSERT_EQ(ctx.Rsp, (uint64_t)(uintptr_t)&stk[18], "final Rsp after 3 frames");

    RtlDeleteFunctionTable(&rf);
}

/* ---- Epilog detection ---- */

static void test_unwind_epilog(void)
{
    img_buf_t img = { .b = { 0 } };
    RUNTIME_FUNCTION rf;
    build_simple_uinfo(&img);
    build_simple_rf(&rf);
    uint64_t base = (uint64_t)(uintptr_t)&img;

    /* Epilog bytes at RVA 0x18: pop rbp (0x5D); ret (0xC3). */
    img.b[0x18] = 0x5D;
    img.b[0x19] = 0xC3;

    uint64_t stk[4] = { 0 };
    const uint64_t SAVED_RBP = 0xDEAD00ULL;
    const uint64_t RETADDR = 0xBEEF11ULL;
    stk[0] = SAVED_RBP;   /* pop rbp reads here */
    stk[1] = RETADDR;     /* ret reads here */

    CONTEXT ctx = { 0 };
    ctx.Rip = base + 0x18;
    ctx.Rsp = (uint64_t)(uintptr_t)&stk[0];
    uint64_t entry_rsp = ctx.Rsp;

    KNONVOLATILE_CONTEXT_POINTERS cp = { 0 };
    uint64_t est = 0;
    RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, ctx.Rip, &rf, &ctx, 0, &est, &cp);
    TEST_ASSERT_EQ(ctx.Rbp, SAVED_RBP, "epilog simulation pops rbp");
    TEST_ASSERT_EQ(ctx.Rip, RETADDR, "epilog simulation recovers the return address");
    TEST_ASSERT_EQ(ctx.Rsp, (uint64_t)(uintptr_t)&stk[2], "epilog Rsp past return address");
    /* M2: the popped rbp's saved slot is recorded in context_pointers. */
    TEST_ASSERT_EQ((uint64_t)(uintptr_t)cp.Integer[5], (uint64_t)(uintptr_t)&stk[0],
                   "epilog records rbp saved-slot in context_pointers");
    /* M3: establisher frame identifies this frame (RSP at the fault PC), not the
     * post-return caller SP. */
    TEST_ASSERT_EQ(est, entry_rsp, "epilog establisher frame is the fault-PC RSP");
}

/* ---- ret imm16 epilog ---- */

static void test_unwind_epilog_ret_imm16(void)
{
    img_buf_t img = { .b = { 0 } };
    RUNTIME_FUNCTION rf;
    build_simple_uinfo(&img);
    build_simple_rf(&rf);
    uint64_t base = (uint64_t)(uintptr_t)&img;

    /* Epilog at 0x18: pop rbp (5D); ret 0x10 (C2 10 00). */
    img.b[0x18] = 0x5D;
    img.b[0x19] = 0xC2; img.b[0x1A] = 0x10; img.b[0x1B] = 0x00;

    uint64_t stk[8] = { 0 };
    const uint64_t SAVED_RBP = 0xC201ULL, RETADDR = 0xC202ULL;
    stk[0] = SAVED_RBP;
    stk[1] = RETADDR;

    CONTEXT ctx = { 0 };
    ctx.Rip = base + 0x18;
    ctx.Rsp = (uint64_t)(uintptr_t)&stk[0];

    uint64_t est = 0;
    RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, ctx.Rip, &rf, &ctx, 0, &est, 0);
    TEST_ASSERT_EQ(ctx.Rbp, SAVED_RBP, "ret imm16 epilog pops rbp");
    TEST_ASSERT_EQ(ctx.Rip, RETADDR, "ret imm16 epilog recovers the return address");
    /* pop (8) + ret pop (8) + imm16 (0x10) => &stk[1] + 8 + 0x10 = &stk[4]. */
    TEST_ASSERT_EQ(ctx.Rsp, (uint64_t)(uintptr_t)&stk[4], "ret imm16 applies the imm16 stack adjustment");
}

/* ---- Epilog instruction straddling EndAddress is not an epilog ---- */

static void test_unwind_epilog_straddle(void)
{
    img_buf_t img = { .b = { 0 } };
    build_simple_uinfo(&img);   /* prolog: push rbp; sub rsp,0x20 */
    uint64_t base = (uint64_t)(uintptr_t)&img;

    /* A `ret imm16` (3 bytes) at 0x18, but the function ends at 0x1A -- the
     * immediate would be read from the next function, so it must NOT be
     * classified as an epilog; the prolog interpreter runs instead. */
    img.b[0x18] = 0xC2; img.b[0x19] = 0x10; img.b[0x1A] = 0x00;
    RUNTIME_FUNCTION rf = { .BeginAddress = 0x10, .EndAddress = 0x1A, .UnwindInfoAddress = 0x40 };

    uint64_t stk[16] = { 0 };
    const uint64_t SAVED_RBP = 0x57A1ULL, RETADDR = 0x57A2ULL;
    stk[9] = SAVED_RBP;    /* prolog-unwind saved rbp slot */
    stk[10] = RETADDR;     /* prolog-unwind return slot */

    CONTEXT ctx = { 0 };
    ctx.Rip = base + 0x18;
    ctx.Rsp = (uint64_t)(uintptr_t)&stk[5];   /* body RSP for the prolog form */

    uint64_t est = 0;
    RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, ctx.Rip, &rf, &ctx, 0, &est, 0);
    /* Prolog unwind (NOT the straddling ret imm16 epilog): alloc 0x20; pop rbp; pop ret. */
    TEST_ASSERT_EQ(ctx.Rip, RETADDR, "ret imm16 straddling EndAddress is not an epilog");
    TEST_ASSERT_EQ(ctx.Rbp, SAVED_RBP, "prolog unwind ran (rbp restored)");
    TEST_ASSERT_EQ(ctx.Rsp, (uint64_t)(uintptr_t)&stk[11], "prolog unwind Rsp (not epilog)");
}

/* ---- Malformed SET_FPREG with FrameRegister=0 fails safe ---- */

static void test_unwind_set_fpreg_no_framereg(void)
{
    img_buf_t img = { .b = { 0 } };
    uint64_t base = (uint64_t)(uintptr_t)&img;

    /* UNWIND_INFO at 0x40: FrameRegister=0 but a SET_FPREG code -> malformed. */
    uint8_t *u = &img.b[0x40];
    u[0] = 0x01; u[1] = 0x06; u[2] = 0x01; u[3] = 0x00;   /* FrameReg 0 */
    u[4] = 0x05; u[5] = (uint8_t)(UWOP_SET_FPREG | (0u << 4));

    RUNTIME_FUNCTION rf = { .BeginAddress = 0x10, .EndAddress = 0x30, .UnwindInfoAddress = 0x40 };

    uint64_t stk[8] = { 0 };
    const uint64_t RETADDR = 0xF9E1ULL;
    stk[2] = RETADDR;

    CONTEXT ctx = { 0 };
    ctx.Rip = base + 0x18;
    ctx.Rsp = (uint64_t)(uintptr_t)&stk[2];   /* entry RSP */
    ctx.Rax = 0xDEADBEEF00ULL;                /* would become RSP if the bug were live */

    uint64_t est = 0;
    RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, ctx.Rip, &rf, &ctx, 0, &est, 0);
    TEST_ASSERT_EQ(ctx.Rip, RETADDR, "SET_FPREG with FrameReg=0 is malformed -> leaf from entry RSP");
    TEST_ASSERT_EQ(ctx.Rsp, (uint64_t)(uintptr_t)&stk[3], "malformed SET_FPREG restores + single leaf pop");
}

/* ---- Frame-register (SET_FPREG) unwinding ---- */

static void test_unwind_frame_register(void)
{
    img_buf_t img = { .b = { 0 } };
    uint64_t base = (uint64_t)(uintptr_t)&img;

    /* UNWIND_INFO at 0x40 for prolog:
     *   push rbp; sub rsp,0x30; lea rbp,[rsp+0x20]; mov [rbp-0x18],rbx
     * FrameRegister=rbp(5), FrameOffset=2 (0x20/16), SizeOfProlog=13. */
    uint8_t *u = &img.b[0x40];
    u[0] = 0x01;                          /* version 1 */
    u[1] = 0x0D;                          /* SizeOfProlog = 13 */
    u[2] = 0x05;                          /* CountOfCodes = 5 (SAVE_NONVOL uses 2) */
    u[3] = (uint8_t)(5u | (2u << 4));     /* FrameReg=5, FrameOff=2 */
    /* code[0..1]: UWOP_SAVE_NONVOL rbx(3), CodeOffset 13, offset slot = 0x08/8=1 */
    u[4] = 0x0D; u[5] = (uint8_t)(UWOP_SAVE_NONVOL | (3u << 4));
    u[6] = 0x01; u[7] = 0x00;             /* FrameOffset value = 1 (=> 8 bytes) */
    /* code[2]: UWOP_SET_FPREG, CodeOffset 9 */
    u[8] = 0x09; u[9] = (uint8_t)(UWOP_SET_FPREG | (0u << 4));
    /* code[3]: UWOP_ALLOC_SMALL 0x30 (info=(0x30-8)/8=5), CodeOffset 5 */
    u[10] = 0x05; u[11] = (uint8_t)(UWOP_ALLOC_SMALL | (5u << 4));
    /* code[4]: UWOP_PUSH_NONVOL rbp(5), CodeOffset 1 */
    u[12] = 0x01; u[13] = (uint8_t)(UWOP_PUSH_NONVOL | (5u << 4));

    RUNTIME_FUNCTION rf = { .BeginAddress = 0x10, .EndAddress = 0x30, .UnwindInfoAddress = 0x40 };

    /* Stack layout: entry_rsp = &stk[16]. body_rsp = entry-0x38 = &stk[9];
     * rbp(body) = entry-0x18 = &stk[13]; frame_base = rbp-0x20 = &stk[9]. */
    uint64_t stk[20] = { 0 };
    const uint64_t RETADDR = 0x1111AAAAULL;
    const uint64_t SAVED_RBP = 0x2222BBBBULL;
    const uint64_t SAVED_RBX = 0x3333CCCCULL;
    stk[10] = SAVED_RBX;   /* frame_base + 0x08 */
    stk[15] = SAVED_RBP;   /* entry_rsp - 0x08 */
    stk[16] = RETADDR;     /* entry_rsp */

    CONTEXT ctx = { 0 };
    ctx.Rip = base + 0x22;                            /* body PC, past 13-byte prolog */
    ctx.Rsp = (uint64_t)(uintptr_t)&stk[9];
    ctx.Rbp = (uint64_t)(uintptr_t)&stk[13];

    uint64_t est = 0;
    RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, ctx.Rip, &rf, &ctx, 0, &est, 0);
    TEST_ASSERT_EQ(ctx.Rbx, SAVED_RBX, "SAVE_NONVOL recovers rbx from the frame base");
    TEST_ASSERT_EQ(ctx.Rbp, SAVED_RBP, "PUSH_NONVOL recovers rbp");
    TEST_ASSERT_EQ(ctx.Rip, RETADDR, "frame-register unwind recovers the return address");
    TEST_ASSERT_EQ(ctx.Rsp, (uint64_t)(uintptr_t)&stk[17], "final Rsp past return address");
    TEST_ASSERT_EQ(est, (uint64_t)(uintptr_t)&stk[9], "establisher frame == frame base");
}

/* ---- Lazy callback path ---- */

static RUNTIME_FUNCTION g_cb_rf;
static int g_cb_calls;

static PRUNTIME_FUNCTION test_cb(uint64_t control_pc, void *context)
{
    (void)control_pc;
    (void)context;
    g_cb_calls++;
    return &g_cb_rf;
}

static void test_unwind_callback(void)
{
    uint64_t base = 0x500000;
    g_cb_rf.BeginAddress = 0x10;
    g_cb_rf.EndAddress = 0x20;
    g_cb_rf.UnwindInfoAddress = 0x40;
    g_cb_calls = 0;

    /* Identifier without the low-2-bits convention is rejected. */
    TEST_ASSERT_EQ(RtlInstallFunctionTableCallback(base, base, 0x100, test_cb, 0, 0), 0,
                   "callback id without low bits set rejected");
    /* Out-of-process dll unsupported. */
    TEST_ASSERT_EQ(RtlInstallFunctionTableCallback(base | 3u, base, 0x100, test_cb, 0, "x.dll"), 0,
                   "out-of-process callback dll rejected");

    uint32_t before = rtl_unwind_dynamic_table_count();
    int ok = RtlInstallFunctionTableCallback(base | 3u, base, 0x100, test_cb, 0, 0);
    TEST_ASSERT_EQ(ok, 1, "valid callback table registered");
    TEST_ASSERT_EQ(rtl_unwind_dynamic_table_count(), before + 1u, "callback table counted");

    uint64_t ib = 0;
    PRUNTIME_FUNCTION e = RtlLookupFunctionEntry(base + 0x50, &ib, 0);
    TEST_ASSERT_NOT_NULL(e, "callback lookup returns the callback's entry");
    TEST_ASSERT_EQ(g_cb_calls, 1, "callback invoked exactly once");
    TEST_ASSERT_EQ(ib, base, "callback lookup reports table base");

    TEST_ASSERT_EQ(RtlDeleteFunctionTable((PRUNTIME_FUNCTION)(uintptr_t)(base | 3u)), 1,
                   "callback table deleted by identifier");
    TEST_ASSERT_EQ(rtl_unwind_dynamic_table_count(), before, "callback table count restored");
}

/* ---- Callback that unregisters its own table does not stall ---- */

static uint64_t g_selfdel_id;
static int g_selfdel_ret;
static RUNTIME_FUNCTION g_selfdel_rf;

static PRUNTIME_FUNCTION test_selfdel_cb(uint64_t control_pc, void *context)
{
    (void)control_pc;
    (void)context;
    /* Unregister our own table from within the callback -- must complete (no
     * teardown wait on our own in-flight call). */
    g_selfdel_ret = RtlDeleteFunctionTable((PRUNTIME_FUNCTION)(uintptr_t)g_selfdel_id);
    return &g_selfdel_rf;
}

static void test_unwind_callback_self_delete(void)
{
    uint64_t base = 0x600000;
    g_selfdel_id = base | 3u;
    g_selfdel_ret = 0;
    g_selfdel_rf.BeginAddress = 0x10;
    g_selfdel_rf.EndAddress = 0x20;
    g_selfdel_rf.UnwindInfoAddress = 0x40;

    uint32_t before = rtl_unwind_dynamic_table_count();
    TEST_ASSERT_EQ(RtlInstallFunctionTableCallback(base | 3u, base, 0x100, test_selfdel_cb, 0, 0), 1,
                   "self-deleting callback table registered");

    uint64_t ib = 0;
    PRUNTIME_FUNCTION e = RtlLookupFunctionEntry(base + 0x50, &ib, 0);   /* invokes the callback */
    TEST_ASSERT_NOT_NULL(e, "self-deleting callback still returns its entry");
    TEST_ASSERT_EQ(g_selfdel_ret, 1, "callback unregistered its own table without stalling");
    TEST_ASSERT_EQ(rtl_unwind_dynamic_table_count(), before, "table removed after self-delete");
    TEST_ASSERT_NULL(RtlLookupFunctionEntry(base + 0x50, 0, 0), "no table after self-delete");
}

/* ---- Malformed metadata fails safe ---- */

static void test_unwind_malformed(void)
{
    img_buf_t img = { .b = { 0 } };
    uint64_t base = (uint64_t)(uintptr_t)&img;

    /* UNWIND_INFO with an invalid version (3) at 0x40. */
    img.b[0x40] = 0x03;   /* version 3, flags 0 */
    img.b[0x41] = 0x02;   /* SizeOfProlog 2 */
    img.b[0x42] = 0x00;   /* CountOfCodes 0 */
    img.b[0x43] = 0x00;
    RUNTIME_FUNCTION rf = { .BeginAddress = 0x10, .EndAddress = 0x30, .UnwindInfoAddress = 0x40 };

    uint64_t stk[4] = { 0 };
    const uint64_t RETADDR = 0x9999ULL;
    stk[0] = RETADDR;

    CONTEXT ctx = { 0 };
    ctx.Rip = base + 0x11;   /* offset 1, < SizeOfProlog(2): skip epilog scan */
    ctx.Rsp = (uint64_t)(uintptr_t)&stk[0];

    uint64_t est = 0;
    /* Must not crash; must make forward progress (leaf-style return-address pop). */
    RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, ctx.Rip, &rf, &ctx, 0, &est, 0);
    TEST_ASSERT_EQ(ctx.Rip, RETADDR, "malformed version falls back to leaf unwind");
    TEST_ASSERT_EQ(ctx.Rsp, (uint64_t)(uintptr_t)&stk[1], "malformed unwind still advances Rsp");
}

/* ---- Chained unwind info: handler lives in the terminal parent ---- */

static void test_unwind_chained_handler(void)
{
    img_buf_t img = { .b = { 0 } };
    uint64_t base = (uint64_t)(uintptr_t)&img;

    /* Child UNWIND_INFO at 0x40: CHAININFO, 1 push, chained RUNTIME_FUNCTION. */
    uint8_t *cu = &img.b[0x40];
    cu[0] = (uint8_t)(1u | (UNW_FLAG_CHAININFO << 3));  /* v1 + CHAININFO */
    cu[1] = 0x01;                                       /* SizeOfProlog */
    cu[2] = 0x01;                                       /* CountOfCodes */
    cu[3] = 0x00;
    cu[4] = 0x01; cu[5] = (uint8_t)(UWOP_PUSH_NONVOL | (5u << 4));   /* push rbp @1 */
    /* aligned = (1+1)&~1 = 2 -> chained RUNTIME_FUNCTION at codes+2 = 0x48 */
    RUNTIME_FUNCTION *chain = (RUNTIME_FUNCTION *)&img.b[0x48];
    chain->BeginAddress = 0x10; chain->EndAddress = 0x30; chain->UnwindInfoAddress = 0x60;

    /* Parent UNWIND_INFO at 0x60: EHANDLER, 2 codes, then handler RVA 0x100. */
    uint8_t *pu = &img.b[0x60];
    pu[0] = (uint8_t)(1u | (UNW_FLAG_EHANDLER << 3));   /* v1 + EHANDLER */
    pu[1] = 0x05; pu[2] = 0x02; pu[3] = 0x00;
    pu[4] = 0x05; pu[5] = (uint8_t)(UWOP_ALLOC_SMALL | (3u << 4));   /* alloc 0x20 @5 */
    pu[6] = 0x01; pu[7] = (uint8_t)(UWOP_PUSH_NONVOL | (5u << 4));   /* push rbp @1 */
    /* aligned = 2 -> handler RVA at codes+2 = 0x68 */
    img.b[0x68] = 0x00; img.b[0x69] = 0x01; img.b[0x6A] = 0x00; img.b[0x6B] = 0x00;  /* RVA 0x100 */

    RUNTIME_FUNCTION child = { .BeginAddress = 0x10, .EndAddress = 0x30, .UnwindInfoAddress = 0x40 };

    uint64_t stk[8] = { 0 };
    CONTEXT ctx = { 0 };
    ctx.Rip = base + 0x18;
    ctx.Rsp = (uint64_t)(uintptr_t)&stk[0];

    uint64_t est = 0;
    void *handler = RtlVirtualUnwind(UNW_FLAG_EHANDLER, base, ctx.Rip, &child,
                                     &ctx, 0, &est, 0);
    TEST_ASSERT_EQ((uint64_t)(uintptr_t)handler, base + 0x100,
                   "chained unwind returns the terminal parent's handler");

    /* When the requested handler type is not present, no handler is returned. */
    CONTEXT ctx2 = { 0 };
    ctx2.Rip = base + 0x18;
    ctx2.Rsp = (uint64_t)(uintptr_t)&stk[0];
    void *h2 = RtlVirtualUnwind(UNW_FLAG_UHANDLER, base, ctx2.Rip, &child, &ctx2, 0, &est, 0);
    TEST_ASSERT_NULL(h2, "no handler returned when handler_type does not match the record");
}

/* ---- Handler is suppressed when control PC is in the prolog ---- */

static void test_unwind_prolog_no_handler(void)
{
    img_buf_t img = { .b = { 0 } };
    uint64_t base = (uint64_t)(uintptr_t)&img;

    /* Non-chained EHANDLER function; SizeOfProlog=5; handler RVA 0x100 at 0x48. */
    uint8_t *u = &img.b[0x40];
    u[0] = (uint8_t)(1u | (UNW_FLAG_EHANDLER << 3));
    u[1] = 0x05; u[2] = 0x02; u[3] = 0x00;
    u[4] = 0x05; u[5] = (uint8_t)(UWOP_ALLOC_SMALL | (3u << 4));
    u[6] = 0x01; u[7] = (uint8_t)(UWOP_PUSH_NONVOL | (5u << 4));
    img.b[0x48] = 0x00; img.b[0x49] = 0x01;   /* handler RVA 0x100 */

    RUNTIME_FUNCTION rf = { .BeginAddress = 0x10, .EndAddress = 0x30, .UnwindInfoAddress = 0x40 };
    uint64_t stk[8] = { 0 };

    /* Body PC (offset 8 >= SizeOfProlog 5): handler is active. */
    CONTEXT cb = { 0 };
    cb.Rip = base + 0x18;
    cb.Rsp = (uint64_t)(uintptr_t)&stk[0];
    uint64_t est = 0;
    void *hb = RtlVirtualUnwind(UNW_FLAG_EHANDLER, base, cb.Rip, &rf, &cb, 0, &est, 0);
    TEST_ASSERT_EQ((uint64_t)(uintptr_t)hb, base + 0x100, "handler active for a body PC");

    /* Prolog PC (offset 2 < SizeOfProlog 5): NO handler is active yet. */
    CONTEXT cp = { 0 };
    cp.Rip = base + 0x12;
    cp.Rsp = (uint64_t)(uintptr_t)&stk[0];
    void *hp = RtlVirtualUnwind(UNW_FLAG_EHANDLER, base, cp.Rip, &rf, &cp, 0, &est, 0);
    TEST_ASSERT_NULL(hp, "no handler for a control PC inside the prolog");
}

/* ---- Tail-call (outbound jmp) epilog detection ---- */

static void test_unwind_tailcall_epilog(void)
{
    img_buf_t img = { .b = { 0 } };
    uint64_t base = (uint64_t)(uintptr_t)&img;

    /* Prolog "push rsi; sub rsp,0x20" (5 bytes). Epilog at 0x18:
     *   add rsp,0x20 (48 83 C4 20) ; pop rsi (5E) ; jmp rel32 outbound. */
    uint8_t *u = &img.b[0x40];
    u[0] = 0x01; u[1] = 0x05; u[2] = 0x02; u[3] = 0x00;
    u[4] = 0x05; u[5] = (uint8_t)(UWOP_ALLOC_SMALL | (3u << 4));   /* alloc 0x20 @5 */
    u[6] = 0x01; u[7] = (uint8_t)(UWOP_PUSH_NONVOL | (6u << 4));   /* push rsi @1 */

    img.b[0x18] = 0x48; img.b[0x19] = 0x83; img.b[0x1A] = 0xC4; img.b[0x1B] = 0x20; /* add rsp,0x20 */
    img.b[0x1C] = 0x5E;                                                             /* pop rsi */
    img.b[0x1D] = 0xE9;                                                             /* jmp rel32 */
    /* target = pc(0x1D)+5+rel = 0x22+0xFDE = 0x1000 (outbound, past EndAddress 0x30) */
    img.b[0x1E] = 0xDE; img.b[0x1F] = 0x0F; img.b[0x20] = 0x00; img.b[0x21] = 0x00;

    RUNTIME_FUNCTION rf = { .BeginAddress = 0x10, .EndAddress = 0x30, .UnwindInfoAddress = 0x40 };

    /* (a) PC AT the jmp: teardown already executed by the CPU, [Rsp] = retaddr. */
    uint64_t stk[8] = { 0 };
    const uint64_t RETADDR = 0xAB01ULL;
    stk[0] = RETADDR;
    CONTEXT c1 = { 0 };
    c1.Rip = base + 0x1D;
    c1.Rsp = (uint64_t)(uintptr_t)&stk[0];
    uint64_t est = 0;
    RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, c1.Rip, &rf, &c1, 0, &est, 0);
    TEST_ASSERT_EQ(c1.Rip, RETADDR, "tail-call epilog at the jmp recovers the return address");
    TEST_ASSERT_EQ(c1.Rsp, (uint64_t)(uintptr_t)&stk[1], "tail-call epilog at jmp pops once");

    /* (b) PC AT the add: scanner simulates add+pop+jmp. */
    uint64_t stk2[8] = { 0 };
    const uint64_t SAVED_RSI = 0xC0DEULL;
    const uint64_t RETADDR2 = 0xAB02ULL;
    stk2[4] = SAVED_RSI;    /* &stk2[0] + 0x20 */
    stk2[5] = RETADDR2;
    CONTEXT c2 = { 0 };
    c2.Rip = base + 0x18;
    c2.Rsp = (uint64_t)(uintptr_t)&stk2[0];
    RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, c2.Rip, &rf, &c2, 0, &est, 0);
    TEST_ASSERT_EQ(c2.Rsi, SAVED_RSI, "tail-call epilog at add restores rsi");
    TEST_ASSERT_EQ(c2.Rip, RETADDR2, "tail-call epilog at add recovers the return address");
    TEST_ASSERT_EQ(c2.Rsp, (uint64_t)(uintptr_t)&stk2[6], "tail-call epilog at add pops add+pop+ret");

    /* (c) An INTRA-function jmp is NOT an epilog: it runs the prolog unwind. */
    img_buf_t img3 = { .b = { 0 } };
    uint64_t base3 = (uint64_t)(uintptr_t)&img3;
    uint8_t *u3 = &img3.b[0x40];
    u3[0] = 0x01; u3[1] = 0x05; u3[2] = 0x02; u3[3] = 0x00;
    u3[4] = 0x05; u3[5] = (uint8_t)(UWOP_ALLOC_SMALL | (3u << 4));
    u3[6] = 0x01; u3[7] = (uint8_t)(UWOP_PUSH_NONVOL | (5u << 4));  /* push rbp */
    img3.b[0x18] = 0xE9;                                            /* jmp rel32 */
    /* target = 0x1D + rel; rel = 0x10 - 0x1D = -0x0D -> lands at 0x10 (INTRA) */
    img3.b[0x19] = 0xF3; img3.b[0x1A] = 0xFF; img3.b[0x1B] = 0xFF; img3.b[0x1C] = 0xFF;
    RUNTIME_FUNCTION rf3 = { .BeginAddress = 0x10, .EndAddress = 0x30, .UnwindInfoAddress = 0x40 };
    uint64_t stk3[8] = { 0 };
    const uint64_t RBP3 = 0xBB01ULL, RET3 = 0xBB02ULL;
    stk3[4] = RBP3;    /* after alloc 0x20 */
    stk3[5] = RET3;
    CONTEXT c3 = { 0 };
    c3.Rip = base3 + 0x18;
    c3.Rsp = (uint64_t)(uintptr_t)&stk3[0];
    RtlVirtualUnwind(UNW_FLAG_NHANDLER, base3, c3.Rip, &rf3, &c3, 0, &est, 0);
    /* Prolog unwind (NOT epilog): alloc 0x20 -> pop rbp -> pop ret. */
    TEST_ASSERT_EQ(c3.Rbp, RBP3, "intra-function jmp runs prolog unwind (restores rbp)");
    TEST_ASSERT_EQ(c3.Rip, RET3, "intra-function jmp is not an epilog");
    TEST_ASSERT_EQ(c3.Rsp, (uint64_t)(uintptr_t)&stk3[6], "intra-jmp prolog unwind Rsp");
}

/* ---- Indirect tail-call epilogs (Clang-emitted FF /4 forms) ---- *
 * Indirect jmps are ambiguous with body switch dispatch, so the engine detects
 * them only after a real teardown/pops prefix (PC at the `add`, the common
 * fault position). Covers unprefixed `FF 25`, REX-prefixed `48 FF 25` (Clang
 * import thunk) and `48 FF E0` (register tail call); FF 24 body switch rejected.
 */

/* Run one indirect-tail-call epilog with `tail_bytes` after `add rsp,0x20; pop
 * rsi`, control_pc at the add, and assert the caller RIP/RSI/RSP are recovered. */
static void run_indirect_epilog(const uint8_t *tail, uint32_t tail_len, const char *label)
{
    img_buf_t img = { .b = { 0 } };
    uint64_t base = (uint64_t)(uintptr_t)&img;
    uint8_t *u = &img.b[0x40];
    u[0] = 0x01; u[1] = 0x05; u[2] = 0x02; u[3] = 0x00;
    u[4] = 0x05; u[5] = (uint8_t)(UWOP_ALLOC_SMALL | (3u << 4));
    u[6] = 0x01; u[7] = (uint8_t)(UWOP_PUSH_NONVOL | (6u << 4));   /* push rsi */
    img.b[0x18] = 0x48; img.b[0x19] = 0x83; img.b[0x1A] = 0xC4; img.b[0x1B] = 0x20; /* add rsp,0x20 */
    img.b[0x1C] = 0x5E;                                                             /* pop rsi */
    for (uint32_t i = 0; i < tail_len; i++)
        img.b[0x1D + i] = tail[i];

    RUNTIME_FUNCTION rf = { .BeginAddress = 0x10, .EndAddress = 0x30, .UnwindInfoAddress = 0x40 };
    uint64_t stk[8] = { 0 };
    const uint64_t SAVED_RSI = 0x1771ULL, RETADDR = 0x1772ULL;
    stk[4] = SAVED_RSI; stk[5] = RETADDR;
    CONTEXT ctx = { 0 };
    ctx.Rip = base + 0x18;
    ctx.Rsp = (uint64_t)(uintptr_t)&stk[0];
    uint64_t est = 0;
    RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, ctx.Rip, &rf, &ctx, 0, &est, 0);
    TEST_ASSERT_EQ(ctx.Rsi, SAVED_RSI, label);
    TEST_ASSERT_EQ(ctx.Rip, RETADDR, label);
    TEST_ASSERT_EQ(ctx.Rsp, (uint64_t)(uintptr_t)&stk[6], label);
}

static void test_unwind_epilog_import_tailcall(void)
{
    /* Only mod==00 memory-reference indirect jmps are legal epilog terminators
     * per the AMD64 epilog contract (register-direct FF E0 is NOT). */
    const uint8_t ff25[6]   = { 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00 };        /* jmp [rip+d] */
    const uint8_t rex_ff25[7] = { 0x48, 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00 };/* rex jmp [rip+d] */
    run_indirect_epilog(ff25, sizeof(ff25), "FF 25 indirect tail-call epilog (PC at add)");
    run_indirect_epilog(rex_ff25, sizeof(rex_ff25), "48 FF 25 import tail-call epilog (PC at add)");

    /* FF 24 switch dispatch at a BODY PC (no teardown/pops prefix) must NOT be
     * classified as an epilog -- the prolog unwind runs instead. */
    img_buf_t img = { .b = { 0 } };
    uint64_t base = (uint64_t)(uintptr_t)&img;
    uint8_t *u = &img.b[0x40];
    u[0] = 0x01; u[1] = 0x05; u[2] = 0x02; u[3] = 0x00;
    u[4] = 0x05; u[5] = (uint8_t)(UWOP_ALLOC_SMALL | (3u << 4));
    u[6] = 0x01; u[7] = (uint8_t)(UWOP_PUSH_NONVOL | (5u << 4));   /* push rbp */
    img.b[0x18] = 0xFF; img.b[0x19] = 0x24; img.b[0x1A] = 0x25;    /* jmp [disp32] SIB */
    img.b[0x1B] = 0x00; img.b[0x1C] = 0x00; img.b[0x1D] = 0x00; img.b[0x1E] = 0x00;
    RUNTIME_FUNCTION rf = { .BeginAddress = 0x10, .EndAddress = 0x30, .UnwindInfoAddress = 0x40 };
    uint64_t stk[8] = { 0 };
    const uint64_t RBP = 0x1781ULL, RET = 0x1782ULL;
    stk[4] = RBP; stk[5] = RET;
    CONTEXT ctx = { 0 };
    ctx.Rip = base + 0x18;
    ctx.Rsp = (uint64_t)(uintptr_t)&stk[0];
    uint64_t est = 0;
    RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, ctx.Rip, &rf, &ctx, 0, &est, 0);
    TEST_ASSERT_EQ(ctx.Rbp, RBP, "FF 24 switch dispatch not an epilog (prolog unwind ran)");
    TEST_ASSERT_EQ(ctx.Rip, RET, "FF 24 indexed indirect jmp rejected without epilog prefix");
}

/* ---- R12 frame-pointer epilog: lea rsp,[r12+disp] uses SIB ---- */

static void test_unwind_epilog_r12_lea(void)
{
    img_buf_t img = { .b = { 0 } };
    uint64_t base = (uint64_t)(uintptr_t)&img;

    /* UNWIND_INFO at 0x40: FrameRegister = r12 (12), SizeOfProlog 8, no codes
     * (the epilog path is code-independent). */
    uint8_t *u = &img.b[0x40];
    u[0] = 0x01;                          /* version 1 */
    u[1] = 0x08;                          /* SizeOfProlog */
    u[2] = 0x00;                          /* CountOfCodes */
    u[3] = (uint8_t)(12u | (0u << 4));    /* FrameReg = r12, FrameOff 0 */

    /* Epilog at 0x18: lea rsp,[r12+0x20] (49 8D 64 24 20); pop r12 (41 5C); ret. */
    img.b[0x18] = 0x49; img.b[0x19] = 0x8D; img.b[0x1A] = 0x64; img.b[0x1B] = 0x24; img.b[0x1C] = 0x20;
    img.b[0x1D] = 0x41; img.b[0x1E] = 0x5C;   /* pop r12 */
    img.b[0x1F] = 0xC3;                       /* ret */

    RUNTIME_FUNCTION rf = { .BeginAddress = 0x10, .EndAddress = 0x30, .UnwindInfoAddress = 0x40 };

    uint64_t stk[8] = { 0 };
    const uint64_t SAVED_R12 = 0xF12AULL;
    const uint64_t RETADDR = 0xF12BULL;
    stk[4] = SAVED_R12;   /* r12 frame base + 0x20 */
    stk[5] = RETADDR;

    CONTEXT ctx = { 0 };
    ctx.Rip = base + 0x18;                       /* at the lea (epilog start) */
    ctx.R12 = (uint64_t)(uintptr_t)&stk[0];      /* frame base */
    ctx.Rsp = 0;                                 /* the lea sets rsp = r12 + 0x20 */

    uint64_t est = 0;
    RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, ctx.Rip, &rf, &ctx, 0, &est, 0);
    TEST_ASSERT_EQ(ctx.R12, SAVED_R12, "r12 frame-pointer epilog restores r12");
    TEST_ASSERT_EQ(ctx.Rip, RETADDR, "lea rsp,[r12+disp] (SIB) epilog recovers the return address");
    TEST_ASSERT_EQ(ctx.Rsp, (uint64_t)(uintptr_t)&stk[6], "r12 epilog final Rsp");
}

/* ---- RBP frame-pointer epilog via the SIB base-101 encoding ---- */

static void test_unwind_epilog_rbp_sib(void)
{
    img_buf_t img = { .b = { 0 } };
    uint64_t base = (uint64_t)(uintptr_t)&img;

    uint8_t *u = &img.b[0x40];
    u[0] = 0x01; u[1] = 0x08; u[2] = 0x00;
    u[3] = (uint8_t)(5u | (0u << 4));    /* FrameReg = rbp (5) */

    /* Epilog: lea rsp,[rbp+0x20] via SIB (48 8D 64 25 20); pop rbp (5D); ret. */
    img.b[0x18] = 0x48; img.b[0x19] = 0x8D; img.b[0x1A] = 0x64; img.b[0x1B] = 0x25; img.b[0x1C] = 0x20;
    img.b[0x1D] = 0x5D;                       /* pop rbp */
    img.b[0x1E] = 0xC3;                       /* ret */

    RUNTIME_FUNCTION rf = { .BeginAddress = 0x10, .EndAddress = 0x30, .UnwindInfoAddress = 0x40 };

    uint64_t stk[8] = { 0 };
    const uint64_t SAVED_RBP = 0xB59AULL;
    const uint64_t RETADDR = 0xB59BULL;
    stk[4] = SAVED_RBP;
    stk[5] = RETADDR;

    CONTEXT ctx = { 0 };
    ctx.Rip = base + 0x18;
    ctx.Rbp = (uint64_t)(uintptr_t)&stk[0];
    ctx.Rsp = 0;

    uint64_t est = 0;
    RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, ctx.Rip, &rf, &ctx, 0, &est, 0);
    TEST_ASSERT_EQ(ctx.Rbp, SAVED_RBP, "rbp SIB-form epilog restores rbp");
    TEST_ASSERT_EQ(ctx.Rip, RETADDR, "lea rsp,[rbp+disp] SIB base-101 (mod1) epilog recovers RIP");
    TEST_ASSERT_EQ(ctx.Rsp, (uint64_t)(uintptr_t)&stk[6], "rbp SIB epilog final Rsp");
}

/* ---- REX.B add is not a false RSP teardown (epilog misdecode) ---- */

static void test_unwind_epilog_rexb_not_rsp(void)
{
    img_buf_t img = { .b = { 0 } };
    uint64_t base = (uint64_t)(uintptr_t)&img;

    /* Prolog "push rbp; sub rsp,0x20" (SizeOfProlog 5). */
    uint8_t *u = &img.b[0x40];
    u[0] = 0x01; u[1] = 0x05; u[2] = 0x02; u[3] = 0x00;
    u[4] = 0x05; u[5] = (uint8_t)(UWOP_ALLOC_SMALL | (3u << 4));
    u[6] = 0x01; u[7] = (uint8_t)(UWOP_PUSH_NONVOL | (5u << 4));

    /* Body PC bytes: `add r12,0x20` (49 83 C4 20) then `ret` (C3). The REX.B
     * targets r12, NOT rsp, so this must NOT be classified as an epilog
     * teardown -- the engine must run the prolog unwind instead. */
    img.b[0x18] = 0x49; img.b[0x19] = 0x83; img.b[0x1A] = 0xC4; img.b[0x1B] = 0x20;
    img.b[0x1C] = 0xC3;

    RUNTIME_FUNCTION rf = { .BeginAddress = 0x10, .EndAddress = 0x30, .UnwindInfoAddress = 0x40 };

    uint64_t stk[8] = { 0 };
    const uint64_t WRONG = 0xE401ULL;   /* what a false `add rsp,0x20; ret` would return */
    const uint64_t RIGHT = 0xE402ULL;   /* what the real prolog unwind returns */
    stk[4] = WRONG;    /* after alloc 0x20: rbp slot / false-epilog ret slot */
    stk[5] = RIGHT;    /* real return address after pop rbp */

    CONTEXT ctx = { 0 };
    ctx.Rip = base + 0x18;
    ctx.Rsp = (uint64_t)(uintptr_t)&stk[0];

    uint64_t est = 0;
    RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, ctx.Rip, &rf, &ctx, 0, &est, 0);
    TEST_ASSERT_EQ(ctx.Rip, RIGHT, "REX.B add r12 is not a false rsp teardown; prolog unwind ran");
    TEST_ASSERT_EQ(ctx.Rsp, (uint64_t)(uintptr_t)&stk[6], "prolog unwind Rsp (not the false-epilog Rsp)");
}

/* ---- Unwind-info version 2 is rejected (fails safe) ---- */

static void test_unwind_version2_rejected(void)
{
    img_buf_t img = { .b = { 0 } };
    uint64_t base = (uint64_t)(uintptr_t)&img;

    img.b[0x40] = 0x02;   /* version 2, flags 0 */
    img.b[0x41] = 0x01;   /* SizeOfProlog */
    img.b[0x42] = 0x00;   /* CountOfCodes */
    img.b[0x43] = 0x00;
    /* Epilog-looking bytes at a BODY PC: version rejection must run BEFORE the
     * epilog interpreter, so these must NOT be simulated. */
    img.b[0x18] = 0x5D;   /* pop rbp */
    img.b[0x19] = 0xC3;   /* ret */
    RUNTIME_FUNCTION rf = { .BeginAddress = 0x10, .EndAddress = 0x30, .UnwindInfoAddress = 0x40 };

    uint64_t stk[4] = { 0 };
    const uint64_t LEAF_RET = 0x7777ULL;
    const uint64_t EPILOG_RET = 0x8888ULL;
    stk[0] = LEAF_RET;     /* leaf pop reads here */
    stk[1] = EPILOG_RET;   /* an epilog sim (pop rbp; ret) would return here */

    CONTEXT ctx = { 0 };
    ctx.Rip = base + 0x18;   /* body PC, past 1-byte prolog, with epilog bytes */
    ctx.Rsp = (uint64_t)(uintptr_t)&stk[0];

    uint64_t est = 0;
    RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, ctx.Rip, &rf, &ctx, 0, &est, 0);
    TEST_ASSERT_EQ(ctx.Rip, LEAF_RET, "version 2 rejected before epilog sim (leaf pop, not epilog)");
    TEST_ASSERT_EQ(ctx.Rbp, 0u, "version 2 rejection did not simulate the pop rbp");
    TEST_ASSERT_EQ(ctx.Rsp, (uint64_t)(uintptr_t)&stk[1], "version 2 fail-safe advances Rsp once");
}

/* ---- Registration arithmetic boundaries ---- */

static void test_unwind_add_arithmetic_bounds(void)
{
    RUNTIME_FUNCTION rf = { .BeginAddress = 0x10, .EndAddress = 0x30, .UnwindInfoAddress = 0x40 };

    /* entry_count over the cap is rejected before the array is walked. */
    TEST_ASSERT_EQ(RtlAddFunctionTable(&rf, 1000u, 0x400000), 0,
                   "entry_count over cap rejected (no OOB read)");

    /* A base+RVA span that wraps the address space is rejected (base within
     * EndAddress of UINT64_MAX so base + 0x30 overflows). */
    TEST_ASSERT_EQ(RtlAddFunctionTable(&rf, 1, 0xFFFFFFFFFFFFFFF0ULL), 0,
                   "wrapping base+RVA span rejected");

    /* A callback range that wraps is rejected. */
    TEST_ASSERT_EQ(RtlInstallFunctionTableCallback(0xFFFFFFFFFFFFFF03ULL,
                                                   0xFFFFFFFFFFFFFF00ULL, 0x200,
                                                   test_cb, 0, 0), 0,
                   "wrapping callback range rejected");
}

/* ---- Malformed after a stack mutation restores the entry state ---- */

static void test_unwind_malformed_transactional(void)
{
    img_buf_t img = { .b = { 0 } };
    uint64_t base = (uint64_t)(uintptr_t)&img;

    /* UNWIND_INFO at 0x40: version 1, SizeOfProlog 6, 2 codes.
     *   code[0]: UWOP_ALLOC_SMALL 0x20 @5  (executes, moves RSP by 0x20)
     *   code[1]: opcode 15 (invalid) -> malformed AFTER the alloc mutated RSP. */
    uint8_t *u = &img.b[0x40];
    u[0] = 0x01; u[1] = 0x06; u[2] = 0x02; u[3] = 0x00;
    u[4] = 0x05; u[5] = (uint8_t)(UWOP_ALLOC_SMALL | (3u << 4));
    u[6] = 0x05; u[7] = (uint8_t)(15u | (0u << 4));   /* invalid opcode */

    RUNTIME_FUNCTION rf = { .BeginAddress = 0x10, .EndAddress = 0x30, .UnwindInfoAddress = 0x40 };

    uint64_t stk[8] = { 0 };
    const uint64_t RETADDR = 0x7EA1ULL;   /* at the ENTRY RSP slot */
    const uint64_t WRONG = 0x7EA2ULL;     /* at entry RSP + 0x20 (mutated-RSP slot) */
    stk[2] = RETADDR;
    stk[6] = WRONG;

    CONTEXT ctx = { 0 };
    ctx.Rip = base + 0x18;                          /* body PC (>= SizeOfProlog 6) */
    ctx.Rsp = (uint64_t)(uintptr_t)&stk[2];         /* entry RSP */

    uint64_t est = 0;
    RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, ctx.Rip, &rf, &ctx, 0, &est, 0);
    /* Transactional: the alloc's RSP mutation is rolled back, so the leaf pop
     * reads the return address from the ORIGINAL RSP, not the moved one. */
    TEST_ASSERT_EQ(ctx.Rip, RETADDR, "malformed-after-mutation leaf pop uses the entry RSP");
    TEST_ASSERT_EQ(ctx.Rsp, (uint64_t)(uintptr_t)&stk[3], "transactional restore + single leaf pop");

    /* Semantically-invalid OpInfo (ALLOC_LARGE form 2) after a real allocation
     * must ALSO take the transactional failure path, not advance RSP by an
     * attacker-controlled u32 and pop from there. */
    img_buf_t img2 = { .b = { 0 } };
    uint64_t base2 = (uint64_t)(uintptr_t)&img2;
    uint8_t *u2 = &img2.b[0x40];
    u2[0] = 0x01; u2[1] = 0x06; u2[2] = 0x04; u2[3] = 0x00;   /* 4 code slots */
    u2[4] = 0x05; u2[5] = (uint8_t)(UWOP_ALLOC_SMALL | (3u << 4));   /* alloc 0x20 */
    u2[6] = 0x05; u2[7] = (uint8_t)(UWOP_ALLOC_LARGE | (2u << 4));   /* invalid form 2 */
    u2[8] = 0xFF; u2[9] = 0xFF; u2[10] = 0xFF; u2[11] = 0xFF;       /* would-be huge size */
    RUNTIME_FUNCTION rf2 = { .BeginAddress = 0x10, .EndAddress = 0x30, .UnwindInfoAddress = 0x40 };
    uint64_t stk2[8] = { 0 };
    const uint64_t RET2 = 0x7EB1ULL;
    stk2[2] = RET2;
    CONTEXT ctx2 = { 0 };
    ctx2.Rip = base2 + 0x18;
    ctx2.Rsp = (uint64_t)(uintptr_t)&stk2[2];
    RtlVirtualUnwind(UNW_FLAG_NHANDLER, base2, ctx2.Rip, &rf2, &ctx2, 0, &est, 0);
    TEST_ASSERT_EQ(ctx2.Rip, RET2, "invalid ALLOC_LARGE OpInfo takes transactional failure path");
    TEST_ASSERT_EQ(ctx2.Rsp, (uint64_t)(uintptr_t)&stk2[3], "invalid OpInfo -> restore + single leaf pop");
}

/* ---- RtlPcToFileHeader on an unregistered PC ---- */

static void test_unwind_pctofileheader_null(void)
{
    uint64_t stk = 0;
    void *bad_pc = (void *)(uintptr_t)&stk;   /* stack addr: not a loaded module */
    void *bimg = (void *)0x1;
    void *r = RtlPcToFileHeader(bad_pc, &bimg);
    TEST_ASSERT_NULL(r, "RtlPcToFileHeader returns NULL for a non-module PC");
    TEST_ASSERT_NULL(bimg, "RtlPcToFileHeader zeroes *base_of_image on miss");
}

/* ============================================================================
 * TODO-23 s7: kernel-mode stack walking (RtlCaptureStackBackTrace,
 * RtlWalkFrameChain, rtl_capture_stack_from_context)
 * ==========================================================================*/

/* Live 4-deep call chain. Each level does work AFTER the callee returns (the
 * `g_sw_sink += n`), so the call is NOT in tail position -- with -fno-omit-frame-
 * pointer that guarantees a distinct RBP frame per level, and prevents tail-call
 * collapse from folding the chain. noinline keeps the frames separate. */
static void    **g_sw_buf;
static uint32_t   g_sw_skip;
static uint32_t  *g_sw_hash;
static volatile int g_sw_sink;

static __attribute__((noinline)) uint16_t sw_leaf(void)
{
    uint16_t n = RtlCaptureStackBackTrace(g_sw_skip, 8, g_sw_buf, g_sw_hash);
    g_sw_sink += n;                 /* defeat DCE + hold the frame live */
    return n;
}
static __attribute__((noinline)) uint16_t sw_l3(void) { uint16_t n = sw_leaf(); g_sw_sink += n; return n; }
static __attribute__((noinline)) uint16_t sw_l2(void) { uint16_t n = sw_l3();   g_sw_sink += n; return n; }
static __attribute__((noinline)) uint16_t sw_l1(void) { uint16_t n = sw_l2();   g_sw_sink += n; return n; }

/* skip=0 from a 4-deep chain records >= 4 frames, each a kernel code address,
 * and yields a non-zero deterministic hash. */
static void test_stackwalk_capture_depth(void)
{
    void    *buf[8] = {0};
    uint32_t hash = 0;
    uint16_t n;
    int i;

    g_sw_buf = buf; g_sw_skip = 0; g_sw_hash = &hash;
    n = sw_l1();
    TEST_ASSERT(n >= 4, "capture >= 4 frames from a 4-deep call chain");
    for (i = 0; i < (int)n; i++)
        TEST_ASSERT(buf[i] != 0, "captured frame is a non-NULL code address");
    TEST_ASSERT(hash != 0, "back-trace hash is non-zero for a non-empty trace");
    /* Hash determinism is asserted on the fully-controlled synthetic chain in
     * test_stackwalk_from_context (a live capture's deep frames -- test runner,
     * boot path -- are not this test's to pin). */
}

/* skip=1 drops the immediate caller: the first captured RIP differs from skip=0,
 * and equals the second frame of the skip=0 capture. */
static void test_stackwalk_skip(void)
{
    void *buf0[8] = {0};
    void *buf1[8] = {0};
    uint16_t n0, n1;

    g_sw_hash = 0;
    g_sw_buf = buf0; g_sw_skip = 0; n0 = sw_l1();
    g_sw_buf = buf1; g_sw_skip = 1; n1 = sw_l1();

    TEST_ASSERT(n0 >= 2, "skip=0 captured at least two frames");
    TEST_ASSERT(n1 >= 1, "skip=1 captured at least one frame");
    TEST_ASSERT(buf1[0] != buf0[0], "skip=1 drops the immediate caller");
    TEST_ASSERT(buf1[0] == buf0[1], "skip=1 first frame == skip=0 second frame");
}

/* rtl_capture_stack_from_context walks a SYNTHETIC RBP chain built in a stack
 * buffer (monotonically increasing frame pointers, real code addresses as return
 * slots), terminated by a zero saved-RBP. */
static void test_stackwalk_from_context(void)
{
    uint64_t fr[6];
    CONTEXT  ctx;
    void    *buf[8] = {0};
    uint16_t n;

    /* fr[2k] = saved RBP (-> next frame), fr[2k+1] = return address. */
    fr[0] = (uint64_t)(uintptr_t)&fr[2];
    fr[1] = (uint64_t)(uintptr_t)&sw_leaf;
    fr[2] = (uint64_t)(uintptr_t)&fr[4];
    fr[3] = (uint64_t)(uintptr_t)&sw_l2;
    fr[4] = 0;                                   /* terminator: saved RBP = 0 */
    fr[5] = (uint64_t)(uintptr_t)&sw_l3;

    for (int i = 0; i < (int)(sizeof(ctx)); i++)
        ((uint8_t *)&ctx)[i] = 0;
    ctx.Rsp = (uint64_t)(uintptr_t)&fr[0];
    ctx.Rbp = (uint64_t)(uintptr_t)&fr[0];

    n = rtl_capture_stack_from_context(&ctx, 0, 8, buf, 0);
    TEST_ASSERT_EQ((uint64_t)n, 3u, "synthetic 3-frame chain -> 3 frames");
    TEST_ASSERT(buf[0] == (void *)(uintptr_t)&sw_leaf, "frame 0 return address");
    TEST_ASSERT(buf[1] == (void *)(uintptr_t)&sw_l2,   "frame 1 return address");
    TEST_ASSERT(buf[2] == (void *)(uintptr_t)&sw_l3,   "frame 2 return address");

    /* Hash: non-zero, and deterministic for identical input (two walks of the
     * same synthetic chain produce the same hash). */
    {
        void *b2[8] = {0};
        uint32_t h1 = 0, h2 = 0;
        (void)rtl_capture_stack_from_context(&ctx, 0, 8, buf, &h1);
        (void)rtl_capture_stack_from_context(&ctx, 0, 8, b2, &h2);
        TEST_ASSERT(h1 != 0, "synthetic-chain hash is non-zero");
        TEST_ASSERT_EQ(h2, h1, "hash is deterministic for identical input");
    }
}

/* A non-monotonic (backward) saved-RBP terminates the walk instead of looping:
 * only the first valid frame is recorded. */
static void test_stackwalk_no_runaway(void)
{
    uint64_t fr[6];
    CONTEXT  ctx;
    void    *buf[8] = {0};
    uint16_t n;

    fr[0] = (uint64_t)(uintptr_t)&fr[4];   /* frame 0 saved RBP -> higher (ok) */
    fr[1] = (uint64_t)(uintptr_t)&sw_leaf; /* frame 0 return */
    fr[4] = (uint64_t)(uintptr_t)&fr[0];   /* frame 1 saved RBP -> BACKWARD (stop) */
    fr[5] = (uint64_t)(uintptr_t)&sw_l2;   /* frame 1 return */
    fr[2] = fr[3] = 0;

    for (int i = 0; i < (int)(sizeof(ctx)); i++)
        ((uint8_t *)&ctx)[i] = 0;
    ctx.Rsp = (uint64_t)(uintptr_t)&fr[0];
    ctx.Rbp = (uint64_t)(uintptr_t)&fr[0];

    n = rtl_capture_stack_from_context(&ctx, 0, 8, buf, 0);
    TEST_ASSERT_EQ((uint64_t)n, 2u, "non-monotonic RBP stops after the two valid frames");
    TEST_ASSERT(buf[0] == (void *)(uintptr_t)&sw_leaf, "frame 0 recorded before the bad link");
    TEST_ASSERT(buf[1] == (void *)(uintptr_t)&sw_l2,   "frame 1 recorded before the bad link");
}

/* A non-canonical starting frame pointer (would #GP, not #PF) is rejected without
 * dereferencing: the walk returns 0 instead of bugchecking. */
static void test_stackwalk_noncanonical(void)
{
    CONTEXT ctx;
    void   *buf[8] = {0};

    for (int i = 0; i < (int)(sizeof(ctx)); i++)
        ((uint8_t *)&ctx)[i] = 0;
    ctx.Rsp = 0x0000800000000000ULL;   /* first non-canonical address */
    ctx.Rbp = 0x0000800000000000ULL;
    TEST_ASSERT_EQ((uint64_t)rtl_capture_stack_from_context(&ctx, 0, 8, buf, 0), 0u,
                   "non-canonical RBP -> 0 frames (no #GP bugcheck)");
}

/* Degenerate arguments return 0 without touching memory. */
static void test_stackwalk_degenerate(void)
{
    void *buf[4] = {0};
    TEST_ASSERT_EQ((uint64_t)RtlCaptureStackBackTrace(0, 0, buf, 0), 0u,
                   "count == 0 -> 0 frames");
    TEST_ASSERT_EQ((uint64_t)RtlCaptureStackBackTrace(0, 4, 0, 0), 0u,
                   "NULL buffer -> 0 frames");
    TEST_ASSERT_EQ((uint64_t)rtl_capture_stack_from_context(0, 0, 4, buf, 0), 0u,
                   "NULL context -> 0 frames");
}

/* RtlWalkFrameChain: kernel walk (flags==0) captures frames; user walk
 * (RTL_STACK_WALK_USER_MODE) is deferred and returns 0; the upper flag bits carry
 * frames-to-skip (>> RTL_STACK_WALK_SKIP_SHIFT). */
static void test_stackwalk_frame_chain(void)
{
    void *b0[8] = {0};
    void *b1[8] = {0};
    uint32_t f;

    TEST_ASSERT(RtlWalkFrameChain(b0, 8, 0) >= 1u,
                "RtlWalkFrameChain(flags=0) walks the kernel stack");
    TEST_ASSERT_EQ((uint64_t)RtlWalkFrameChain(b0, 8, RTL_STACK_WALK_USER_MODE), 0u,
                   "RtlWalkFrameChain user-mode walk deferred -> 0");

    /* Skip encoded in the upper bits: flags=0 vs (1 << SKIP_SHIFT) from ONE call
     * site -- skip=1 drops the immediate caller. */
    f = (1u << RTL_STACK_WALK_SKIP_SHIFT);
    {
        void **bufs[2]; uint32_t flg[2]; int k;
        bufs[0] = b0; flg[0] = 0;
        bufs[1] = b1; flg[1] = f;
        for (k = 0; k < 2; k++)
            (void)RtlWalkFrameChain(bufs[k], 8, flg[k]);
        TEST_ASSERT(b1[0] != b0[0], "RtlWalkFrameChain skip bits drop the caller");
    }
}

/* The fault-safe read primitive: reads a valid kernel address correctly, and an
 * unmapped kernel VA is recovered (returns -1) instead of bugchecking. */
static void test_stackwalk_kstack_read(void)
{
    uint64_t src = 0xABCDEF0123456789ULL;
    uint64_t out = 0;
    uint64_t bad = 0;
    uint64_t a;

    TEST_ASSERT_EQ((uint64_t)__kstack_read_u64(&out, &src), 0u,
                   "__kstack_read_u64 on a valid address succeeds");
    TEST_ASSERT_EQ(out, src, "__kstack_read_u64 returns the correct value");

    /* Non-canonical operand raises #GP (not #PF), which the fixup cannot recover;
     * it must be rejected up front (returns -1 without dereferencing). */
    TEST_ASSERT_EQ((uint64_t)__kstack_read_u64(&out, (const void *)0x0000800000000000ULL),
                   (uint64_t)(int64_t)-1, "__kstack_read_u64 rejects a non-canonical address");

    /* NULL output storage is rejected (no unguarded store-fault). */
    TEST_ASSERT_EQ((uint64_t)__kstack_read_u64(0, &src),
                   (uint64_t)(int64_t)-1, "__kstack_read_u64 rejects NULL out");

    /* Top-of-address-space wrap: [a, a+7] overflows -- rejected before the load
     * (a wrapped endpoint could straddle the linear-address boundary -> #GP). */
    TEST_ASSERT_EQ((uint64_t)__kstack_read_u64(&out, (const void *)(uintptr_t)(~(uint64_t)0 - 3u)),
                   (uint64_t)(int64_t)-1, "__kstack_read_u64 rejects an address+7 wrap");

    /* Find a genuinely unmapped high-canonical kernel VA (vmm_get_physical == 0
     * is a non-faulting page-table walk; 0 reliably means not present). */
    for (a = 0xffffff0000000000ULL; a < 0xffffff0000200000ULL; a += 0x1000ULL) {
        if (vmm_get_physical((uintptr_t)a) == 0) { bad = a; break; }
    }
    if (bad) {
        /* TEST-SIDE-EFFECT-ALLOWED: deliberately dereferences an unmapped kernel
         * VA to prove the RIP-keyed __kstack_read fixup recovers the #PF instead
         * of bugchecking (the crash-path fault boundary for stack walking). */
        out = 0x1111;
        TEST_ASSERT_EQ((uint64_t)__kstack_read_u64(&out, (const void *)(uintptr_t)bad),
                       (uint64_t)(int64_t)-1, "__kstack_read_u64 on unmapped VA returns -1");
    } else {
        TEST_SKIP("no unmapped kernel VA found in the probe window");
    }
}

/* ==========================================================================
 * RtlUnwindEx -- unwind to a target frame (TODO-23 kernel unwind)
 * ========================================================================== */

/* ms_abi termination handlers used by the RtlUnwindEx tests. They MUST be ms_abi
 * -- a SysV handler would receive the arguments in the wrong registers, so a
 * SysV synthetic handler would validate the wrong calling convention. */
static volatile uint32_t s_fin_calls;
static volatile uint32_t s_fin_saw_unwinding;
static volatile uint32_t s_fin_saw_target;
static volatile uint64_t s_fin_establishers[8];

static EXCEPTION_DISPOSITION __attribute__((ms_abi))
tu_finally_handler(struct _EXCEPTION_RECORD *rec, void *establisher,
                   struct _CONTEXT *ctx, struct _DISPATCHER_CONTEXT *dctx)
{
    (void)ctx; (void)dctx;
    EXCEPTION_RECORD *r = (EXCEPTION_RECORD *)rec;
    if (s_fin_calls < 8)
        s_fin_establishers[s_fin_calls] = (uint64_t)(uintptr_t)establisher;
    s_fin_calls++;
    if (r->ExceptionFlags & EXCEPTION_UNWINDING)
        s_fin_saw_unwinding++;
    if (r->ExceptionFlags & EXCEPTION_TARGET_UNWIND)
        s_fin_saw_target++;
    return ExceptionContinueSearch;
}

static EXCEPTION_DISPOSITION __attribute__((ms_abi))
tu_bad_disposition_handler(struct _EXCEPTION_RECORD *rec, void *establisher,
                           struct _CONTEXT *ctx, struct _DISPATCHER_CONTEXT *dctx)
{
    (void)rec; (void)establisher; (void)ctx; (void)dctx;
    s_fin_calls++;
    return ExceptionNestedException;   /* invalid on the unwind pass */
}

/* First call runs a real __finally (ContinueSearch); a LATER call fails with an
 * invalid disposition -- models a failure AFTER irreversible cleanup already ran. */
static EXCEPTION_DISPOSITION __attribute__((ms_abi))
tu_deferred_fail_handler(struct _EXCEPTION_RECORD *rec, void *establisher,
                         struct _CONTEXT *ctx, struct _DISPATCHER_CONTEXT *dctx)
{
    (void)rec; (void)establisher; (void)ctx; (void)dctx;
    s_fin_calls++;
    return (s_fin_calls == 1) ? ExceptionContinueSearch : ExceptionNestedException;
}

static volatile uint64_t s_collide_new_rip;
static EXCEPTION_DISPOSITION __attribute__((ms_abi))
tu_collided_handler(struct _EXCEPTION_RECORD *rec, void *establisher,
                    struct _CONTEXT *ctx, struct _DISPATCHER_CONTEXT *dctx)
{
    (void)establisher; (void)ctx;
    EXCEPTION_RECORD *r = (EXCEPTION_RECORD *)rec;
    s_fin_calls++;
    /* Only the EXITED frames report a collision; the target-frame call resolves
     * normally (a target-frame collision is a distinct fail-safe path). */
    if (r->ExceptionFlags & EXCEPTION_TARGET_UNWIND)
        return ExceptionContinueSearch;
    /* Simulate a nested unwind handing back a new dispatcher state: repoint the
     * working context's Rip (the engine must adopt it and continue). */
    if (dctx && dctx->ContextRecord)
        dctx->ContextRecord->Rip = s_collide_new_rip;
    return ExceptionCollidedUnwind;
}

/* Reports a collision on EVERY call, including the target frame (R2-3 path). */
static EXCEPTION_DISPOSITION __attribute__((ms_abi))
tu_collided_target_handler(struct _EXCEPTION_RECORD *rec, void *establisher,
                           struct _CONTEXT *ctx, struct _DISPATCHER_CONTEXT *dctx)
{
    (void)rec; (void)establisher; (void)ctx;
    s_fin_calls++;
    if (dctx && dctx->ContextRecord)
        dctx->ContextRecord->Rip = s_collide_new_rip;
    return ExceptionCollidedUnwind;
}

/* Adopts a wild high-canonical RSP far beyond the current frame; the engine must
 * reject it (span backstop) BEFORE the next step dereferences that RSP. */
static EXCEPTION_DISPOSITION __attribute__((ms_abi))
tu_collided_wildrsp_handler(struct _EXCEPTION_RECORD *rec, void *establisher,
                            struct _CONTEXT *ctx, struct _DISPATCHER_CONTEXT *dctx)
{
    (void)rec; (void)establisher; (void)ctx;
    s_fin_calls++;
    if (dctx && dctx->ContextRecord) {
        dctx->ContextRecord->Rip = s_collide_new_rip;
        dctx->ContextRecord->Rsp += 0x100000ULL;   /* 1 MiB > 64 KiB span backstop */
    }
    return ExceptionCollidedUnwind;
}

/* Repoints dctx.ContextRecord at a foreign buffer (the deferred nested protocol);
 * the engine must reject it without dereferencing the untrusted pointer. */
static CONTEXT s_foreign_ctx;
static EXCEPTION_DISPOSITION __attribute__((ms_abi))
tu_collided_repoint_handler(struct _EXCEPTION_RECORD *rec, void *establisher,
                            struct _CONTEXT *ctx, struct _DISPATCHER_CONTEXT *dctx)
{
    (void)rec; (void)establisher; (void)ctx;
    s_fin_calls++;
    if (dctx)
        dctx->ContextRecord = &s_foreign_ctx;   /* repoint to a foreign buffer */
    return ExceptionCollidedUnwind;
}

/* Returns ExceptionContinueExecution -- invalid on the unwind pass. */
static EXCEPTION_DISPOSITION __attribute__((ms_abi))
tu_continue_execution_handler(struct _EXCEPTION_RECORD *rec, void *establisher,
                              struct _CONTEXT *ctx, struct _DISPATCHER_CONTEXT *dctx)
{
    (void)rec; (void)establisher; (void)ctx; (void)dctx;
    s_fin_calls++;
    return ExceptionContinueExecution;
}

/* Build a "push rbp; sub rsp,0x20" prolog UNWIND_INFO at `u` with a UHANDLER
 * referencing handler_rva (0 = no handler flag). Mirrors build_simple_uinfo. */
static void tu_build_uhandler_uinfo(uint8_t *u, uint32_t handler_rva, int has_handler)
{
    u[0] = (uint8_t)(1u | (has_handler ? (UNW_FLAG_UHANDLER << 3) : 0));
    u[1] = 0x05;   /* SizeOfProlog = 5 */
    u[2] = 0x02;   /* CountOfCodes = 2 */
    u[3] = 0x00;   /* FrameRegister 0 */
    u[4] = 0x05; u[5] = (uint8_t)(UWOP_ALLOC_SMALL | (3u << 4));   /* alloc 0x20 @5 */
    u[6] = 0x01; u[7] = (uint8_t)(UWOP_PUSH_NONVOL | (5u << 4));   /* push rbp @1 */
    /* aligned = 2 -> handler RVA at codes+2 = u[8..11] */
    u[8]  = (uint8_t)(handler_rva & 0xFFu);
    u[9]  = (uint8_t)((handler_rva >> 8) & 0xFFu);
    u[10] = (uint8_t)((handler_rva >> 16) & 0xFFu);
    u[11] = (uint8_t)((handler_rva >> 24) & 0xFFu);
}

/* Byte offset, inside the metadata image, of the SYNTHETIC function's code
 * range. It sits above the UNWIND_INFO at 0x40 and well inside img_buf_t's
 * 256 bytes, so the range and the metadata never overlap. */
#define TU_SYN_CODE_OFF  0x80u
#define TU_SYN_CODE_LEN  0x20u
/* Where inside that range a control PC sits: past the UNWIND_INFO's
 * SizeOfProlog (5), so a lookup lands in the body rather than the prolog. */
#define TU_SYN_BODY_OFF  0x08u

/* Register one synthetic function whose UHANDLER points at `handler`. base is
 * chosen below both the metadata image and the handler so the handler RVA
 * (handler - base) and UnwindInfoAddress (img - base) both fit u32 and
 * RtlVirtualUnwind reconstructs the real handler VA (base + handler_rva).
 * Returns base via *out_base and the body PC via *out_body_pc.
 *
 * THE CODE RANGE LIVES INSIDE `img`, NOT AT A FIXED SMALL RVA FROM `base`.
 * That is a correctness requirement, not tidiness. `base` is the LOWER of the
 * metadata image and the handler, and the handler is real kernel .text, so
 * `base` is normally a real code address -- which made the old fixed range
 * [base+0x10, base+0x30) alias whatever kernel function happened to be laid
 * out just after the handler. When a real function starts inside that window
 * it carries its own .pdata entry covering the body PC, so
 * RtlLookupFunctionEntry could resolve REAL unwind info instead of this
 * table's, and the test then measured the kernel's own frame layout.
 *
 * That is a coin flip decided by the linker: measured 2026-08-01, an
 * unrelated growth of src/kernel/test/test_usermode_launcher.c (test code in
 * a different category, which does not even execute in this suite) moved
 * test_register_kworker to exactly base+0x10 for tu_continue_execution_
 * handler, and test_rtlunwind_continue_execution_invalid began reporting
 * STATUS_SUCCESS instead of STATUS_INVALID_DISPOSITION. Anchoring the range
 * to the img buffer -- static storage that no .pdata entry can cover --
 * removes the aliasing entirely rather than moving the window somewhere
 * currently-empty. */
static void tu_register_handler_fn(img_buf_t *img, RUNTIME_FUNCTION *rf, void *handler,
                                   uint64_t *out_base, uint64_t *out_body_pc)
{
    uint64_t uimg = (uint64_t)(uintptr_t)img;
    uint64_t uh   = (uint64_t)(uintptr_t)handler;
    uint64_t base = (uimg < uh) ? uimg : uh;
    uint32_t handler_rva = (uint32_t)(uh - base);
    uint32_t uinfo_rva   = (uint32_t)((uimg + 0x40) - base);
    tu_build_uhandler_uinfo(&img->b[0x40], handler_rva, 1);
    rf->BeginAddress = (uint32_t)((uimg + TU_SYN_CODE_OFF) - base);
    rf->EndAddress = (uint32_t)((uimg + TU_SYN_CODE_OFF + TU_SYN_CODE_LEN) - base);
    rf->UnwindInfoAddress = uinfo_rva;
    RtlAddFunctionTable(rf, 1, base);
    *out_base = base;
    *out_body_pc = uimg + TU_SYN_CODE_OFF + TU_SYN_BODY_OFF;
}

/* ---- RtlUnwindEx: __finally chain + flags + target resume ---- */

static void test_rtlunwind_finally_chain(void)
{
    static img_buf_t img;
    for (int i = 0; i < (int)sizeof(img.b); i++) img.b[i] = 0;
    RUNTIME_FUNCTION rf;
    uint64_t base = 0, body_pc = 0;
    tu_register_handler_fn(&img, &rf, (void *)&tu_finally_handler, &base, &body_pc);

    /* Three recursive frames of 0x30 bytes each (6 u64 slots): frame k has its
     * saved-rbp at stk[6k+4] and return address at stk[6k+5]; body rsp = stk[6k]. */
    uint64_t stk[24] = { 0 };
    const uint64_t SENTINEL = base + 0x1000;   /* outside the table -> lookup NULL */
    stk[4] = 0xAA11; stk[5] = body_pc;
    stk[10] = 0xAA22; stk[11] = body_pc;
    stk[16] = 0xAA33; stk[17] = SENTINEL;

    /* Discover establisher frames via a dry RtlVirtualUnwind walk (the engine's
     * own establisher semantics), so the target matches exactly. */
    uint64_t est[3] = { 0 };
    {
        CONTEXT c = { 0 };
        c.Rip = body_pc; c.Rsp = (uint64_t)(uintptr_t)&stk[0];
        for (int k = 0; k < 3; k++) {
            uint64_t ib = 0, e = 0;
            PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(c.Rip, &ib, 0);
            if (!fe) break;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, ib, c.Rip, fe, &c, 0, &e, 0);
            est[k] = e;
        }
    }
    TEST_ASSERT(est[0] < est[1] && est[1] < est[2], "establisher frames strictly ascend");

    s_fin_calls = 0; s_fin_saw_unwinding = 0; s_fin_saw_target = 0;
    EXCEPTION_RECORD rec = { 0 };
    rec.ExceptionCode = (NTSTATUS)0xC0000005;
    CONTEXT ctx = { 0 };
    ctx.ContextFlags = CONTEXT_FULL;
    ctx.Rip = body_pc; ctx.Rsp = (uint64_t)(uintptr_t)&stk[0];

    const uint64_t TARGET_IP = 0xDEAD1234ULL, RETVAL = 0x99AA55ULL;
    uint32_t fcount = 0;
    NTSTATUS s = rtl_unwind_to_target((void *)(uintptr_t)est[2], (void *)TARGET_IP,
                                      &rec, (void *)RETVAL, &ctx, 0, &fcount);

    /* Frames 0 and 1 run their __finally (EXITED); the target frame's handler runs
     * with EXCEPTION_TARGET_UNWIND so it can run any exited in-frame scopes. */
    TEST_ASSERT_EQ((uint64_t)s, 0u, "rtl_unwind_to_target succeeds to a reachable target");
    TEST_ASSERT_EQ((uint64_t)fcount, 3u, "handler ran for frames 0,1 (exited) + target frame");
    TEST_ASSERT_EQ((uint64_t)s_fin_calls, 3u, "termination handler invoked three times");
    TEST_ASSERT_EQ((uint64_t)s_fin_saw_unwinding, 3u, "EXCEPTION_UNWINDING set on every handler call");
    TEST_ASSERT_EQ((uint64_t)s_fin_saw_target, 1u, "only the target-frame call sees EXCEPTION_TARGET_UNWIND");
    TEST_ASSERT_EQ(s_fin_establishers[0], est[0], "first __finally sees frame 0 establisher");
    TEST_ASSERT_EQ(s_fin_establishers[1], est[1], "second __finally sees frame 1 establisher");
    TEST_ASSERT_EQ(s_fin_establishers[2], est[2], "target-frame handler sees the target establisher");
    TEST_ASSERT_EQ(ctx.Rip, TARGET_IP, "resume Rip is target_ip");
    TEST_ASSERT_EQ(ctx.Rax, RETVAL, "resume Rax is return_value");
    TEST_ASSERT_EQ(ctx.Rsp, est[2], "resume Rsp is the target frame (not unwound past it)");
    TEST_ASSERT_EQ((uint64_t)(rec.ExceptionFlags & EXCEPTION_UNWINDING), 0u,
                   "EXCEPTION_UNWINDING cleared after the unwind completes");

    RtlDeleteFunctionTable(&rf);
}

/* ---- RtlUnwindEx: a reused record's stale unwind flags are normalized ---- */

static void test_rtlunwind_stale_flags_normalized(void)
{
    static img_buf_t img;
    for (int i = 0; i < (int)sizeof(img.b); i++) img.b[i] = 0;
    RUNTIME_FUNCTION rf;
    uint64_t base = 0, body_pc = 0;
    tu_register_handler_fn(&img, &rf, (void *)&tu_finally_handler, &base, &body_pc);

    uint64_t stk[24] = { 0 };
    const uint64_t SENTINEL = base + 0x1000;
    stk[4] = 0xAA11; stk[5] = body_pc;
    stk[10] = 0xAA22; stk[11] = body_pc;
    stk[16] = 0xAA33; stk[17] = SENTINEL;

    uint64_t est[3] = { 0 };
    {
        CONTEXT c = { 0 };
        c.Rip = body_pc; c.Rsp = (uint64_t)(uintptr_t)&stk[0];
        for (int k = 0; k < 3; k++) {
            uint64_t ib = 0, e = 0;
            PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(c.Rip, &ib, 0);
            if (!fe) break;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, ib, c.Rip, fe, &c, 0, &e, 0);
            est[k] = e;
        }
    }

    s_fin_calls = 0; s_fin_saw_unwinding = 0; s_fin_saw_target = 0;
    EXCEPTION_RECORD rec = { 0 };
    rec.ExceptionFlags = EXCEPTION_TARGET_UNWIND | EXCEPTION_EXIT_UNWIND;   /* stale */
    CONTEXT ctx = { 0 };
    ctx.ContextFlags = CONTEXT_FULL;
    ctx.Rip = body_pc; ctx.Rsp = (uint64_t)(uintptr_t)&stk[0];

    NTSTATUS s = rtl_unwind_to_target((void *)(uintptr_t)est[2], (void *)0x6060ULL,
                                      &rec, 0, &ctx, 0, 0);
    TEST_ASSERT_EQ((uint64_t)s, 0u, "unwind succeeds despite the reused record's stale flags");
    TEST_ASSERT_EQ((uint64_t)s_fin_saw_target, 1u,
                   "stale TARGET_UNWIND normalized: only the target-frame handler sees it");
    TEST_ASSERT_EQ((uint64_t)(rec.ExceptionFlags &
                              (EXCEPTION_UNWINDING | EXCEPTION_TARGET_UNWIND | EXCEPTION_EXIT_UNWIND)),
                   0u, "all in-progress unwind flags cleared after completion");

    RtlDeleteFunctionTable(&rf);
}

/* ---- RtlUnwindEx: unreachable target fails preflight, no __finally runs ---- */

static void test_rtlunwind_unreachable_target(void)
{
    static img_buf_t img;
    for (int i = 0; i < (int)sizeof(img.b); i++) img.b[i] = 0;
    RUNTIME_FUNCTION rf;
    uint64_t base = 0, body_pc = 0;
    tu_register_handler_fn(&img, &rf, (void *)&tu_finally_handler, &base, &body_pc);

    uint64_t stk[24] = { 0 };
    const uint64_t SENTINEL = base + 0x1000;
    stk[4] = 0xAA11; stk[5] = body_pc;
    stk[10] = 0xAA22; stk[11] = body_pc;
    stk[16] = 0xAA33; stk[17] = SENTINEL;   /* top of chain: only 3 frames */

    s_fin_calls = 0; s_fin_saw_unwinding = 0;
    EXCEPTION_RECORD rec = { 0 };
    CONTEXT ctx = { 0 };
    ctx.ContextFlags = CONTEXT_FULL;
    ctx.Rip = body_pc; ctx.Rsp = (uint64_t)(uintptr_t)&stk[0];

    /* A target frame ABOVE the whole 3-frame chain is never reached. */
    uint64_t bogus_target = (uint64_t)(uintptr_t)&stk[64];
    uint32_t fcount = 123;
    NTSTATUS s = rtl_unwind_to_target((void *)(uintptr_t)bogus_target, (void *)0xCAFEULL,
                                      &rec, (void *)0, &ctx, 0, &fcount);

    TEST_ASSERT_EQ((uint64_t)(uint32_t)s, (uint64_t)(uint32_t)STATUS_BAD_STACK,
                   "unreachable target -> STATUS_BAD_STACK");
    TEST_ASSERT_EQ((uint64_t)s_fin_calls, 0u, "no __finally runs when preflight fails");
    TEST_ASSERT_EQ((uint64_t)fcount, 0u, "finally count is zero on preflight failure");
    TEST_ASSERT_EQ((uint64_t)(rec.ExceptionFlags & EXCEPTION_UNWINDING), 0u,
                   "unwind flags cleared on preflight failure");

    RtlDeleteFunctionTable(&rf);
}

/* ---- RtlUnwindEx: an invalid handler disposition fails safe ---- */

static void test_rtlunwind_invalid_disposition(void)
{
    static img_buf_t img;
    for (int i = 0; i < (int)sizeof(img.b); i++) img.b[i] = 0;
    RUNTIME_FUNCTION rf;
    uint64_t base = 0, body_pc = 0;
    tu_register_handler_fn(&img, &rf, (void *)&tu_bad_disposition_handler, &base, &body_pc);

    uint64_t stk[24] = { 0 };
    const uint64_t SENTINEL = base + 0x1000;
    stk[4] = 0xAA11; stk[5] = body_pc;
    stk[10] = 0xAA22; stk[11] = body_pc;
    stk[16] = 0xAA33; stk[17] = SENTINEL;

    uint64_t est2;
    {
        CONTEXT c = { 0 };
        c.Rip = body_pc; c.Rsp = (uint64_t)(uintptr_t)&stk[0];
        uint64_t ib = 0, e = 0;
        for (int k = 0; k < 3; k++) {
            PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(c.Rip, &ib, 0);
            if (!fe) break;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, ib, c.Rip, fe, &c, 0, &e, 0);
        }
        est2 = e;
    }

    s_fin_calls = 0;
    EXCEPTION_RECORD rec = { 0 };
    CONTEXT ctx = { 0 };
    ctx.ContextFlags = CONTEXT_FULL;
    ctx.Rip = body_pc; ctx.Rsp = (uint64_t)(uintptr_t)&stk[0];

    NTSTATUS s = rtl_unwind_to_target((void *)(uintptr_t)est2, (void *)0xBEEFULL,
                                      &rec, (void *)0, &ctx, 0, 0);
    TEST_ASSERT_EQ((uint64_t)(uint32_t)s, (uint64_t)(uint32_t)STATUS_INVALID_DISPOSITION,
                   "invalid handler disposition -> STATUS_INVALID_DISPOSITION");
    TEST_ASSERT_EQ((uint64_t)(rec.ExceptionFlags & EXCEPTION_UNWINDING), 0u,
                   "unwind flags cleared on invalid-disposition failure");

    RtlDeleteFunctionTable(&rf);
}

/* ---- RtlUnwindEx: a collided unwind is adopted (single-level) ---- */

static void test_rtlunwind_collided(void)
{
    static img_buf_t img;
    for (int i = 0; i < (int)sizeof(img.b); i++) img.b[i] = 0;
    RUNTIME_FUNCTION rf;
    uint64_t base = 0, body_pc = 0;
    tu_register_handler_fn(&img, &rf, (void *)&tu_collided_handler, &base, &body_pc);

    uint64_t stk[24] = { 0 };
    const uint64_t SENTINEL = base + 0x1000;
    stk[4] = 0xAA11; stk[5] = body_pc;
    stk[10] = 0xAA22; stk[11] = body_pc;
    stk[16] = 0xAA33; stk[17] = SENTINEL;

    uint64_t est[3] = { 0 };
    {
        CONTEXT c = { 0 };
        c.Rip = body_pc; c.Rsp = (uint64_t)(uintptr_t)&stk[0];
        for (int k = 0; k < 3; k++) {
            uint64_t ib = 0, e = 0;
            PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(c.Rip, &ib, 0);
            if (!fe) break;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, ib, c.Rip, fe, &c, 0, &e, 0);
            est[k] = e;
        }
    }

    /* The collided handler repoints Rip to body_pc so the walk continues from a
     * valid frame after adoption. */
    s_collide_new_rip = body_pc;
    s_fin_calls = 0;
    EXCEPTION_RECORD rec = { 0 };
    CONTEXT ctx = { 0 };
    ctx.ContextFlags = CONTEXT_FULL;
    ctx.Rip = body_pc; ctx.Rsp = (uint64_t)(uintptr_t)&stk[0];

    NTSTATUS s = rtl_unwind_to_target((void *)(uintptr_t)est[2], (void *)0x1357ULL,
                                      &rec, (void *)0x2468ULL, &ctx, 0, 0);
    TEST_ASSERT_EQ((uint64_t)s, 0u, "collided unwind still resolves to the target");
    TEST_ASSERT_EQ((uint64_t)(rec.ExceptionFlags & EXCEPTION_COLLIDED_UNWIND),
                   (uint64_t)EXCEPTION_COLLIDED_UNWIND,
                   "EXCEPTION_COLLIDED_UNWIND recorded on the record");
    TEST_ASSERT_EQ(ctx.Rip, 0x1357ULL, "resume Rip is target_ip after adoption");

    RtlDeleteFunctionTable(&rf);
}

/* ---- RtlUnwindEx: a failure AFTER cleanup ran is reported (finally_count>0) ---- *
 * The core returns the failure status AND a non-zero finally_count so the public
 * RtlUnwindEx wrapper knows cleanup already ran and must NOT return to its caller
 * (it bugchecks instead -- untestable here without halting the suite). */
static void test_rtlunwind_fail_after_cleanup(void)
{
    static img_buf_t img;
    for (int i = 0; i < (int)sizeof(img.b); i++) img.b[i] = 0;
    RUNTIME_FUNCTION rf;
    uint64_t base = 0, body_pc = 0;
    tu_register_handler_fn(&img, &rf, (void *)&tu_deferred_fail_handler, &base, &body_pc);

    uint64_t stk[24] = { 0 };
    const uint64_t SENTINEL = base + 0x1000;
    stk[4] = 0xAA11; stk[5] = body_pc;
    stk[10] = 0xAA22; stk[11] = body_pc;
    stk[16] = 0xAA33; stk[17] = SENTINEL;

    uint64_t est2;
    {
        CONTEXT c = { 0 };
        c.Rip = body_pc; c.Rsp = (uint64_t)(uintptr_t)&stk[0];
        uint64_t ib = 0, e = 0;
        for (int k = 0; k < 3; k++) {
            PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(c.Rip, &ib, 0);
            if (!fe) break;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, ib, c.Rip, fe, &c, 0, &e, 0);
        }
        est2 = e;
    }

    s_fin_calls = 0;
    EXCEPTION_RECORD rec = { 0 };
    CONTEXT ctx = { 0 };
    ctx.ContextFlags = CONTEXT_FULL;
    ctx.Rip = body_pc; ctx.Rsp = (uint64_t)(uintptr_t)&stk[0];

    uint32_t fcount = 0;
    NTSTATUS s = rtl_unwind_to_target((void *)(uintptr_t)est2, (void *)0x3030ULL,
                                      &rec, 0, &ctx, 0, &fcount);
    TEST_ASSERT_EQ((uint64_t)(uint32_t)s, (uint64_t)(uint32_t)STATUS_INVALID_DISPOSITION,
                   "a later invalid disposition fails the unwind");
    TEST_ASSERT_EQ((uint64_t)fcount, 2u, "finally_count reports the cleanup that ran before the failure");
    TEST_ASSERT((uint64_t)fcount > 0u, "non-zero finally_count -> wrapper must go terminal, not return");

    RtlDeleteFunctionTable(&rf);
}

/* ---- RtlUnwindEx: a collision IN the target frame fails safe ---- */

static void test_rtlunwind_target_collision(void)
{
    static img_buf_t img;
    for (int i = 0; i < (int)sizeof(img.b); i++) img.b[i] = 0;
    RUNTIME_FUNCTION rf;
    uint64_t base = 0, body_pc = 0;
    tu_register_handler_fn(&img, &rf, (void *)&tu_collided_target_handler, &base, &body_pc);

    uint64_t stk[24] = { 0 };
    const uint64_t SENTINEL = base + 0x1000;
    stk[4] = 0xAA11; stk[5] = SENTINEL;   /* single frame: its establisher is the target */

    uint64_t est0;
    {
        CONTEXT c = { 0 };
        c.Rip = body_pc; c.Rsp = (uint64_t)(uintptr_t)&stk[0];
        uint64_t ib = 0, e = 0;
        PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(c.Rip, &ib, 0);
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, ib, c.Rip, fe, &c, 0, &e, 0);
        est0 = e;
    }

    s_collide_new_rip = body_pc;
    s_fin_calls = 0;
    EXCEPTION_RECORD rec = { 0 };
    CONTEXT ctx = { 0 };
    ctx.ContextFlags = CONTEXT_FULL;
    ctx.Rip = body_pc; ctx.Rsp = (uint64_t)(uintptr_t)&stk[0];

    NTSTATUS s = rtl_unwind_to_target((void *)(uintptr_t)est0, (void *)0xF00DULL,
                                      &rec, 0, &ctx, 0, 0);
    TEST_ASSERT_EQ((uint64_t)(uint32_t)s, (uint64_t)(uint32_t)STATUS_INVALID_DISPOSITION,
                   "a collision in the target frame fails safe (not silently resumed)");
    TEST_ASSERT_EQ((uint64_t)(rec.ExceptionFlags & EXCEPTION_UNWINDING), 0u,
                   "unwind flags cleared on the target-collision fail-safe path");

    RtlDeleteFunctionTable(&rf);
}

/* ---- RtlUnwindEx: a collided redirect to a wild RSP is rejected pre-deref ---- */

static void test_rtlunwind_collided_wildrsp(void)
{
    static img_buf_t img;
    for (int i = 0; i < (int)sizeof(img.b); i++) img.b[i] = 0;
    RUNTIME_FUNCTION rf;
    uint64_t base = 0, body_pc = 0;
    tu_register_handler_fn(&img, &rf, (void *)&tu_collided_wildrsp_handler, &base, &body_pc);

    uint64_t stk[24] = { 0 };
    const uint64_t SENTINEL = base + 0x1000;
    stk[4] = 0xAA11; stk[5] = body_pc;
    stk[10] = 0xAA22; stk[11] = body_pc;
    stk[16] = 0xAA33; stk[17] = SENTINEL;

    uint64_t est[3] = { 0 };
    {
        CONTEXT c = { 0 };
        c.Rip = body_pc; c.Rsp = (uint64_t)(uintptr_t)&stk[0];
        for (int k = 0; k < 3; k++) {
            uint64_t ib = 0, e = 0;
            PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(c.Rip, &ib, 0);
            if (!fe) break;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, ib, c.Rip, fe, &c, 0, &e, 0);
            est[k] = e;
        }
    }

    s_collide_new_rip = body_pc;
    s_fin_calls = 0;
    EXCEPTION_RECORD rec = { 0 };
    CONTEXT ctx = { 0 };
    ctx.ContextFlags = CONTEXT_FULL;
    ctx.Rip = body_pc; ctx.Rsp = (uint64_t)(uintptr_t)&stk[0];

    /* The first exited frame's handler redirects RSP a wild 1 MiB forward; the
     * span backstop must reject it before the next step reads that RSP. */
    NTSTATUS s = rtl_unwind_to_target((void *)(uintptr_t)est[2], (void *)0x2020ULL,
                                      &rec, 0, &ctx, 0, 0);
    TEST_ASSERT_EQ((uint64_t)(uint32_t)s, (uint64_t)(uint32_t)STATUS_BAD_STACK,
                   "a collided redirect to a wild RSP is rejected (STATUS_BAD_STACK)");
    TEST_ASSERT_EQ((uint64_t)(rec.ExceptionFlags & EXCEPTION_UNWINDING), 0u,
                   "unwind flags cleared on the wild-RSP fail-safe path");

    RtlDeleteFunctionTable(&rf);
}

/* ---- RtlUnwindEx: exit-unwind / NULL target_ip rejected + stale flags cleared ---- */

static void test_rtlunwind_exit_rejected(void)
{
    /* A reused record entering with the unwind bits already set must not stay
     * marked actively unwinding after a rejected request (R2-4). */
    EXCEPTION_RECORD rec = { 0 };
    rec.ExceptionFlags = EXCEPTION_UNWINDING | EXCEPTION_TARGET_UNWIND;
    CONTEXT ctx = { 0 };
    ctx.ContextFlags = CONTEXT_FULL;

    /* target_frame == NULL (whole-stack exit unwind) is not yet supported. */
    NTSTATUS s1 = rtl_unwind_to_target(0, (void *)0x1000ULL, &rec, 0, &ctx, 0, 0);
    TEST_ASSERT_EQ((uint64_t)(uint32_t)s1, (uint64_t)(uint32_t)STATUS_INVALID_PARAMETER,
                   "exit unwind (NULL target_frame) rejected");
    TEST_ASSERT_EQ((uint64_t)(rec.ExceptionFlags & (EXCEPTION_UNWINDING | EXCEPTION_TARGET_UNWIND)),
                   0u, "pre-set unwind flags cleared on rejection");
    /* NULL target_ip would resume to address 0. */
    rec.ExceptionFlags = EXCEPTION_UNWINDING;
    NTSTATUS s2 = rtl_unwind_to_target((void *)0x2000ULL, 0, &rec, 0, &ctx, 0, 0);
    TEST_ASSERT_EQ((uint64_t)(uint32_t)s2, (uint64_t)(uint32_t)STATUS_INVALID_PARAMETER,
                   "NULL target_ip rejected");
    TEST_ASSERT_EQ((uint64_t)(rec.ExceptionFlags & EXCEPTION_UNWINDING), 0u,
                   "no unwind flags left set on a rejected request");
}

/* ---- RtlUnwindEx: a repointed collided context is rejected (no untrusted deref) ---- */

static void test_rtlunwind_collided_repoint(void)
{
    static img_buf_t img;
    for (int i = 0; i < (int)sizeof(img.b); i++) img.b[i] = 0;
    RUNTIME_FUNCTION rf;
    uint64_t base = 0, body_pc = 0;
    tu_register_handler_fn(&img, &rf, (void *)&tu_collided_repoint_handler, &base, &body_pc);

    uint64_t stk[24] = { 0 };
    const uint64_t SENTINEL = base + 0x1000;
    stk[4] = 0xAA11; stk[5] = body_pc;
    stk[10] = 0xAA22; stk[11] = body_pc;
    stk[16] = 0xAA33; stk[17] = SENTINEL;

    uint64_t est2;
    {
        CONTEXT c = { 0 };
        c.Rip = body_pc; c.Rsp = (uint64_t)(uintptr_t)&stk[0];
        uint64_t ib = 0, e = 0;
        for (int k = 0; k < 3; k++) {
            PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(c.Rip, &ib, 0);
            if (!fe) break;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, ib, c.Rip, fe, &c, 0, &e, 0);
        }
        est2 = e;
    }

    s_fin_calls = 0;
    EXCEPTION_RECORD rec = { 0 };
    CONTEXT ctx = { 0 };
    ctx.ContextFlags = CONTEXT_FULL;
    ctx.Rip = body_pc; ctx.Rsp = (uint64_t)(uintptr_t)&stk[0];

    NTSTATUS s = rtl_unwind_to_target((void *)(uintptr_t)est2, (void *)0x4040ULL,
                                      &rec, 0, &ctx, 0, 0);
    TEST_ASSERT_EQ((uint64_t)(uint32_t)s, (uint64_t)(uint32_t)STATUS_BAD_STACK,
                   "a handler repointing dctx.ContextRecord is rejected (STATUS_BAD_STACK)");
    TEST_ASSERT_EQ((uint64_t)(rec.ExceptionFlags & EXCEPTION_UNWINDING), 0u,
                   "unwind flags cleared on the repoint fail-safe path");

    RtlDeleteFunctionTable(&rf);
}

/* ---- RtlUnwindEx: ExceptionContinueExecution is invalid during unwind ---- */

static void test_rtlunwind_continue_execution_invalid(void)
{
    static img_buf_t img;
    for (int i = 0; i < (int)sizeof(img.b); i++) img.b[i] = 0;
    RUNTIME_FUNCTION rf;
    uint64_t base = 0, body_pc = 0;
    tu_register_handler_fn(&img, &rf, (void *)&tu_continue_execution_handler, &base, &body_pc);

    uint64_t stk[24] = { 0 };
    const uint64_t SENTINEL = base + 0x1000;
    stk[4] = 0xAA11; stk[5] = body_pc;
    stk[10] = 0xAA22; stk[11] = body_pc;
    stk[16] = 0xAA33; stk[17] = SENTINEL;

    uint64_t est2;
    {
        CONTEXT c = { 0 };
        c.Rip = body_pc; c.Rsp = (uint64_t)(uintptr_t)&stk[0];
        uint64_t ib = 0, e = 0;
        for (int k = 0; k < 3; k++) {
            PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(c.Rip, &ib, 0);
            if (!fe) break;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, ib, c.Rip, fe, &c, 0, &e, 0);
        }
        est2 = e;
    }

    s_fin_calls = 0;
    EXCEPTION_RECORD rec = { 0 };
    CONTEXT ctx = { 0 };
    ctx.ContextFlags = CONTEXT_FULL;
    ctx.Rip = body_pc; ctx.Rsp = (uint64_t)(uintptr_t)&stk[0];

    NTSTATUS s = rtl_unwind_to_target((void *)(uintptr_t)est2, (void *)0x5050ULL,
                                      &rec, 0, &ctx, 0, 0);
    TEST_ASSERT_EQ((uint64_t)(uint32_t)s, (uint64_t)(uint32_t)STATUS_INVALID_DISPOSITION,
                   "ExceptionContinueExecution on the unwind pass is invalid");

    RtlDeleteFunctionTable(&rf);
}

/* ---- DISPATCHER_CONTEXT ABI layout ---- */

static void test_rtlunwind_dispatcher_abi(void)
{
    TEST_ASSERT_EQ(sizeof(DISPATCHER_CONTEXT), 0x50, "DISPATCHER_CONTEXT is 80 bytes");
    TEST_ASSERT_EQ(__builtin_offsetof(DISPATCHER_CONTEXT, ContextRecord), 0x28,
                   "DISPATCHER_CONTEXT.ContextRecord at 0x28");
    TEST_ASSERT_EQ(__builtin_offsetof(DISPATCHER_CONTEXT, LanguageHandler), 0x30,
                   "DISPATCHER_CONTEXT.LanguageHandler at 0x30");
    TEST_ASSERT_EQ(__builtin_offsetof(DISPATCHER_CONTEXT, ScopeIndex), 0x48,
                   "DISPATCHER_CONTEXT.ScopeIndex at 0x48");
    TEST_ASSERT_EQ((uint64_t)ExceptionContinueSearch, 1u, "ExceptionContinueSearch == 1");
    TEST_ASSERT_EQ((uint64_t)ExceptionCollidedUnwind, 3u, "ExceptionCollidedUnwind == 3");
}

/* ---- RtlRestoreContext: same-CPL register + RSP + RIP transfer ---- *
 * A restore transfers every register, so the resumed state cannot be observed
 * from C without violating the compiler's callee-saved assumptions. The asm
 * harness rtl_restore_selftest (unwind_asm.asm) drives the resume into its own
 * landing label, records the resumed rbx/r12/rsp into a caller buffer (memory,
 * not a clobbered register), and returns cleanly -- proving control transferred
 * to ctx->Rip with ctx->Rsp and the integer registers restored. */
extern int rtl_restore_selftest(CONTEXT *ctx, uint64_t out[3]);

static void test_rtlrestorecontext(void)
{
    static uint64_t scratch[64];
    const uint64_t SENT_RBX = 0xB1B2B3B4B5ULL, SENT_R12 = 0x1212121212ULL;
    const uint64_t RESUME_RSP = (uint64_t)(uintptr_t)&scratch[48];   /* headroom both sides */

    CONTEXT ctx;
    for (int i = 0; i < (int)sizeof(ctx); i++) ((uint8_t *)&ctx)[i] = 0;
    ctx.ContextFlags = CONTEXT_FULL;
    ctx.Rbx = SENT_RBX;
    ctx.R12 = SENT_R12;
    ctx.Rsp = RESUME_RSP;
    ctx.EFlags = 0x202;    /* IF + reserved bit 1 (Rip is set by the harness) */

    uint64_t out[3] = { 0, 0, 0 };
    int rc = rtl_restore_selftest(&ctx, out);

    TEST_ASSERT_EQ((uint64_t)rc, 1u, "rtl_restore_selftest completed the round trip");
    TEST_ASSERT_EQ(out[0], SENT_RBX, "RtlRestoreContext restored rbx");
    TEST_ASSERT_EQ(out[1], SENT_R12, "RtlRestoreContext restored r12");
    TEST_ASSERT_EQ(out[2], RESUME_RSP, "RtlRestoreContext restored rsp and transferred to Rip");
}

/* ---- Registration ---- */

void test_register_unwind(void)
{
    test_suite_register_cat("Unwind: ABI layout",
                            test_unwind_abi, TEST_CAT_EXCEPT);
    test_suite_register_cat("Unwind: add/lookup/delete",
                            test_unwind_add_lookup_delete, TEST_CAT_EXCEPT);
    test_suite_register_cat("Unwind: add validation",
                            test_unwind_add_validation, TEST_CAT_EXCEPT);
    test_suite_register_cat("Unwind: indirect entry resolution",
                            test_unwind_indirect_entry, TEST_CAT_EXCEPT);
    test_suite_register_cat("Unwind: virtual unwind (prolog)",
                            test_unwind_virtual_prolog, TEST_CAT_EXCEPT);
    test_suite_register_cat("Unwind: 3-frame RIP chain",
                            test_unwind_three_frame_chain, TEST_CAT_EXCEPT);
    test_suite_register_cat("Unwind: epilog detection",
                            test_unwind_epilog, TEST_CAT_EXCEPT);
    test_suite_register_cat("Unwind: ret imm16 epilog",
                            test_unwind_epilog_ret_imm16, TEST_CAT_EXCEPT);
    test_suite_register_cat("Unwind: epilog straddling EndAddress rejected",
                            test_unwind_epilog_straddle, TEST_CAT_EXCEPT);
    test_suite_register_cat("Unwind: SET_FPREG FrameReg=0 fail-safe",
                            test_unwind_set_fpreg_no_framereg, TEST_CAT_EXCEPT);
    test_suite_register_cat("Unwind: frame register (SET_FPREG)",
                            test_unwind_frame_register, TEST_CAT_EXCEPT);
    test_suite_register_cat("Unwind: lazy callback table",
                            test_unwind_callback, TEST_CAT_EXCEPT);
    test_suite_register_cat("Unwind: callback self-unregister no stall",
                            test_unwind_callback_self_delete, TEST_CAT_EXCEPT);
    test_suite_register_cat("Unwind: chained handler (terminal parent)",
                            test_unwind_chained_handler, TEST_CAT_EXCEPT);
    test_suite_register_cat("Unwind: no handler for prolog PC",
                            test_unwind_prolog_no_handler, TEST_CAT_EXCEPT);
    test_suite_register_cat("Unwind: tail-call (outbound jmp) epilog",
                            test_unwind_tailcall_epilog, TEST_CAT_EXCEPT);
    test_suite_register_cat("Unwind: import tail-call (FF 25) epilog",
                            test_unwind_epilog_import_tailcall, TEST_CAT_EXCEPT);
    test_suite_register_cat("Unwind: REX.B add not a false teardown",
                            test_unwind_epilog_rexb_not_rsp, TEST_CAT_EXCEPT);
    test_suite_register_cat("Unwind: r12 frame-pointer epilog (SIB lea)",
                            test_unwind_epilog_r12_lea, TEST_CAT_EXCEPT);
    test_suite_register_cat("Unwind: rbp frame-pointer epilog (SIB base-101)",
                            test_unwind_epilog_rbp_sib, TEST_CAT_EXCEPT);
    test_suite_register_cat("Unwind: version 2 rejected",
                            test_unwind_version2_rejected, TEST_CAT_EXCEPT);
    test_suite_register_cat("Unwind: registration arithmetic bounds",
                            test_unwind_add_arithmetic_bounds, TEST_CAT_EXCEPT);
    test_suite_register_cat("Unwind: malformed fail-safe",
                            test_unwind_malformed, TEST_CAT_EXCEPT);
    test_suite_register_cat("Unwind: transactional malformed restore",
                            test_unwind_malformed_transactional, TEST_CAT_EXCEPT);
    test_suite_register_cat("Unwind: RtlPcToFileHeader NULL",
                            test_unwind_pctofileheader_null, TEST_CAT_EXCEPT);

    /* TODO-23 s7: kernel-mode stack walking */
    test_suite_register_cat("StackWalk: capture depth + hash",
                            test_stackwalk_capture_depth, TEST_CAT_EXCEPT);
    test_suite_register_cat("StackWalk: skip drops immediate caller",
                            test_stackwalk_skip, TEST_CAT_EXCEPT);
    test_suite_register_cat("StackWalk: from-context synthetic chain",
                            test_stackwalk_from_context, TEST_CAT_EXCEPT);
    test_suite_register_cat("StackWalk: non-monotonic RBP no runaway",
                            test_stackwalk_no_runaway, TEST_CAT_EXCEPT);
    test_suite_register_cat("StackWalk: non-canonical RBP rejected",
                            test_stackwalk_noncanonical, TEST_CAT_EXCEPT);
    test_suite_register_cat("StackWalk: degenerate args return 0",
                            test_stackwalk_degenerate, TEST_CAT_EXCEPT);
    test_suite_register_cat("StackWalk: RtlWalkFrameChain kernel + deferred user",
                            test_stackwalk_frame_chain, TEST_CAT_EXCEPT);
    test_suite_register_cat("StackWalk: __kstack_read_u64 valid + fault-recover",
                            test_stackwalk_kstack_read, TEST_CAT_EXCEPT);

    /* TODO-23 kernel unwind: RtlUnwindEx + RtlRestoreContext */
    test_suite_register_cat("UnwindEx: __finally chain + flags + target resume",
                            test_rtlunwind_finally_chain, TEST_CAT_EXCEPT);
    test_suite_register_cat("UnwindEx: unreachable target fails preflight (no __finally)",
                            test_rtlunwind_unreachable_target, TEST_CAT_EXCEPT);
    test_suite_register_cat("UnwindEx: reused record stale unwind flags normalized",
                            test_rtlunwind_stale_flags_normalized, TEST_CAT_EXCEPT);
    test_suite_register_cat("UnwindEx: invalid handler disposition fails safe",
                            test_rtlunwind_invalid_disposition, TEST_CAT_EXCEPT);
    test_suite_register_cat("UnwindEx: failure after cleanup reports finally_count",
                            test_rtlunwind_fail_after_cleanup, TEST_CAT_EXCEPT);
    test_suite_register_cat("UnwindEx: collided unwind adopted (single-level)",
                            test_rtlunwind_collided, TEST_CAT_EXCEPT);
    test_suite_register_cat("UnwindEx: target-frame collision fails safe",
                            test_rtlunwind_target_collision, TEST_CAT_EXCEPT);
    test_suite_register_cat("UnwindEx: collided wild-RSP redirect rejected pre-deref",
                            test_rtlunwind_collided_wildrsp, TEST_CAT_EXCEPT);
    test_suite_register_cat("UnwindEx: repointed collided context rejected",
                            test_rtlunwind_collided_repoint, TEST_CAT_EXCEPT);
    test_suite_register_cat("UnwindEx: ExceptionContinueExecution invalid on unwind",
                            test_rtlunwind_continue_execution_invalid, TEST_CAT_EXCEPT);
    test_suite_register_cat("UnwindEx: exit-unwind / NULL target_ip rejected + flags cleared",
                            test_rtlunwind_exit_rejected, TEST_CAT_EXCEPT);
    test_suite_register_cat("UnwindEx: DISPATCHER_CONTEXT ABI layout",
                            test_rtlunwind_dispatcher_abi, TEST_CAT_EXCEPT);
    test_suite_register_cat("UnwindEx: RtlRestoreContext register/RSP/RIP transfer",
                            test_rtlrestorecontext, TEST_CAT_EXCEPT);
}

#endif /* KERNEL_TESTS */
