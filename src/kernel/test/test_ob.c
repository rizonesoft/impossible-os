/* ============================================================================
 * test_ob.c -- Object Manager unit tests
 *
 * Tests object allocation, reference counting, handle table, namespace
 * lookup, handle duplication, inheritance, and directory enumeration.
 *
 * XREF: 00-infrastructure/TODO-02-ai-development-system.md (unit tests)
 * ============================================================================ */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "kernel/atomic.h"
#include "kernel/ob/ob.h"
#include "kernel/mm/heap.h"

/* snprintf is not in freestanding kernel headers; declared extern here at
 * file scope so all test functions below can build per-iteration assertion
 * messages (see implement-unit-tests skill -- per-iteration messages
 * mandatory for any TEST_ASSERT inside a loop). */
extern int snprintf(char *buf, size_t size, const char *fmt, ...);
#include "kernel/ob/ob_type.h"
#include "kernel/ob/ob_callback.h"
#include "kernel/ob/ob_trace.h"
#include "kernel/ob/ob_ns.h"
#include "kernel/ob/ob_info_file.h"
#include "kernel/ob/handle_table.h"
#include "kernel/etw.h"
#include "desktop/wm.h"
#include "kernel/sched/task.h"

/* ---- Test type with on_delete callback ---- */

static volatile uint32_t g_delete_called = 0;

static void test_on_delete(void *body)
{
    (void)body;
    g_delete_called++;
}

static const OBJECT_TYPE test_type = {
    .name      = "TestObj",
    .body_size = 64,
    .on_close  = (void *)0,
    .on_delete = test_on_delete,
    .on_open   = (void *)0,
    .on_parse  = (void *)0,
};

/* ---- ob_alloc_object + OB_HEADER_FROM_BODY round-trip ---- */

static void test_ob_alloc_header_roundtrip(void)
{
    void *body = ob_alloc_object(&test_type);
    TEST_ASSERT(body != (void *)0, "ob_alloc_object returns non-NULL");

    OBJECT_HEADER *hdr = OB_HEADER_FROM_BODY(body);
    TEST_ASSERT(hdr->type == &test_type,
                "OB_HEADER_FROM_BODY->type matches allocated type");
    TEST_ASSERT(atomic_read(&hdr->ref_count) == 1,
                "initial ref_count is 1");

    ObDereferenceObject(body);  /* cleanup */
}

/* ---- ObReferenceObject + ObDereferenceObject → on_delete ---- */

static void test_ob_refcount_lifecycle(void)
{
    void *body = ob_alloc_object(&test_type);
    TEST_ASSERT(body != (void *)0, "alloc for refcount test");

    ObReferenceObject(body);
    OBJECT_HEADER *hdr = OB_HEADER_FROM_BODY(body);
    TEST_ASSERT(atomic_read(&hdr->ref_count) == 2, "ref after ObReferenceObject is 2");

    g_delete_called = 0;
    ObDereferenceObject(body);  /* ref → 1 */
    TEST_ASSERT(g_delete_called == 0, "on_delete not called at ref=1");

    int32_t final = ObDereferenceObject(body);  /* ref → 0, freed */
    TEST_ASSERT(final == 0, "ObDereferenceObject returns 0 when freed");
    TEST_ASSERT(g_delete_called == 1, "on_delete called when ref reaches 0");
}

/* ---- Handle table: alloc + lookup + free ---- */

static void test_ob_handle_table(void)
{
    HANDLE_TABLE ht;
    ob_handle_table_init(&ht);

    void *body = ob_alloc_object(&test_type);
    HANDLE h = ObpAllocateHandle(&ht, body, 0x1F01FF, 0);
    TEST_ASSERT(h != INVALID_HANDLE_VALUE, "ObpAllocateHandle succeeds");

    HANDLE_TABLE_ENTRY *entry = ObpLookupHandle(&ht, h);
    TEST_ASSERT(entry != (void *)0, "ObpLookupHandle finds the handle");
    TEST_ASSERT(entry->object == body, "looked-up object matches allocated body");

    int rc = ObpFreeHandle(&ht, h);
    TEST_ASSERT(rc == 0, "ObpFreeHandle succeeds");

    HANDLE_TABLE_ENTRY *gone = ObpLookupHandle(&ht, h);
    TEST_ASSERT(gone == (void *)0 || gone->object == (void *)0,
                "handle slot is empty after free");

    /* -9 LEAK retrofit: drop creation ref + free entries array. */
    ObDereferenceObject(body);
    ob_handle_table_destroy(&ht);
}

/* ---- Named object: insert + lookup via ObLookupObjectByName ---- */

static void test_ob_namespace_lookup(void)
{
    void *result = (void *)0;
    int rc = ObLookupObjectByName("\\BaseNamedObjects", (void *)0, 0, &result);
    TEST_ASSERT(rc == 0, "ObLookupObjectByName finds \\BaseNamedObjects");
    TEST_ASSERT(result != (void *)0, "result is non-NULL");
    if (result)
        ObDereferenceObject(result);
}

/* ---- NtDuplicateObject ---- */

static void test_ob_duplicate_handle(void)
{
    HANDLE_TABLE src_ht, dst_ht;
    ob_handle_table_init(&src_ht);
    ob_handle_table_init(&dst_ht);

    void *body = ob_alloc_object(&test_type);
    HANDLE src_h = ObpAllocateHandle(&src_ht, body, 0x1F01FF, 0);

    HANDLE dst_h = INVALID_HANDLE_VALUE;
    int rc = NtDuplicateObject(&src_ht, src_h, &dst_ht, &dst_h, 0, 0, 0);
    TEST_ASSERT(rc == 0, "NtDuplicateObject succeeds");
    TEST_ASSERT(dst_h != INVALID_HANDLE_VALUE, "duplicate handle is valid");

    /* Close source, duplicate should still be valid */
    ObpFreeHandle(&src_ht, src_h);
    HANDLE_TABLE_ENTRY *dup_entry = ObpLookupHandle(&dst_ht, dst_h);
    TEST_ASSERT(dup_entry != (void *)0 && dup_entry->object == body,
                "duplicate handle still valid after source closed");

    ObpFreeHandle(&dst_ht, dst_h);
    /* -9 LEAK retrofit. */
    ObDereferenceObject(body);
    ob_handle_table_destroy(&src_ht);
    ob_handle_table_destroy(&dst_ht);
}

/* ---- Non-canonical handle values must not alias real slots ---- */

static void test_ob_handle_low_bits_rejected(void)
{
    HANDLE_TABLE ht;
    ob_handle_table_init(&ht);
    void *body = ob_alloc_object(&test_type);
    HANDLE h = ObpAllocateHandle(&ht, body, 0x1, 0);  /* always a multiple of 4 */

    /* The low 2 bits are reserved; a handle with them set must not divide down
     * to the same slot as the real handle. */
    TEST_ASSERT(ObpLookupHandle(&ht, h | 1) == (void *)0,
                "handle with low bit set does not resolve to a slot");
    TEST_ASSERT(NtClose(&ht, h | 2) != 0,
                "NtClose of a non-canonical handle fails (no slot aliasing)");
    TEST_ASSERT(ObpLookupHandle(&ht, h) != (void *)0,
                "real handle still open after the aliased-close attempt");

    ObpFreeHandle(&ht, h);
    ObDereferenceObject(body);
    ob_handle_table_destroy(&ht);
}

/* ---- NtDuplicateObject access capping ---- */

static void test_ob_duplicate_access_cap(void)
{
    HANDLE_TABLE src_ht, dst_ht;
    ob_handle_table_init(&src_ht);
    ob_handle_table_init(&dst_ht);

    void *body = ob_alloc_object(&test_type);
    /* Source handle holds only a single right (0x0001). */
    HANDLE src_h = ObpAllocateHandle(&src_ht, body, 0x0001, 0);

    /* Without DUPLICATE_SAME_ACCESS, a request for full access must be masked
     * down to what the source actually holds -- never escalate. */
    HANDLE dst_h = INVALID_HANDLE_VALUE;
    int rc = NtDuplicateObject(&src_ht, src_h, &dst_ht, &dst_h, 0x1F01FF, 0, 0);
    TEST_ASSERT(rc == 0, "NtDuplicateObject (capped) succeeds");
    HANDLE_TABLE_ENTRY *de = ObpLookupHandle(&dst_ht, dst_h);
    TEST_ASSERT(de != (void *)0 && de->granted_access == 0x0001,
                "duplicate access capped to source granted_access");
    if (dst_h != INVALID_HANDLE_VALUE) ObpFreeHandle(&dst_ht, dst_h);

    /* DUPLICATE_SAME_ACCESS copies the source mask verbatim. */
    HANDLE same_h = INVALID_HANDLE_VALUE;
    rc = NtDuplicateObject(&src_ht, src_h, &dst_ht, &same_h, 0,
                           0, DUPLICATE_SAME_ACCESS);
    TEST_ASSERT(rc == 0, "NtDuplicateObject (same-access) succeeds");
    HANDLE_TABLE_ENTRY *se = ObpLookupHandle(&dst_ht, same_h);
    TEST_ASSERT(se != (void *)0 && se->granted_access == 0x0001,
                "same-access duplicate copies source granted_access");
    if (same_h != INVALID_HANDLE_VALUE) ObpFreeHandle(&dst_ht, same_h);

    ObpFreeHandle(&src_ht, src_h);
    ObDereferenceObject(body);
    ob_handle_table_destroy(&src_ht);
    ob_handle_table_destroy(&dst_ht);
}

/* ---- NtDuplicateObject + DUPLICATE_CLOSE_SOURCE on a protected source ---- */

static void test_ob_duplicate_close_protected(void)
{
    HANDLE_TABLE src_ht, dst_ht;
    ob_handle_table_init(&src_ht);
    ob_handle_table_init(&dst_ht);

    void *body = ob_alloc_object(&test_type);
    /* Source handle is protected from close (OBJ_PROTECT_CLOSE). */
    HANDLE src_h = ObpAllocateHandle(&src_ht, body, 0x1F01FF, OBJ_PROTECT_CLOSE);

    /* DUPLICATE_CLOSE_SOURCE cannot close a protected source: the dup must fail
     * atomically -- no destination handle leaked, source still open. */
    /* Also request a protected destination (attrs=OBJ_PROTECT_CLOSE): the
     * rollback must force-free the dest even though it too is protected, so the
     * dst table is left with no leaked entry. */
    uint32_t dst_before = dst_ht.count;
    HANDLE dst_h = INVALID_HANDLE_VALUE;
    int rc = NtDuplicateObject(&src_ht, src_h, &dst_ht, &dst_h, 0,
                               OBJ_PROTECT_CLOSE, DUPLICATE_CLOSE_SOURCE);
    TEST_ASSERT(rc != 0, "duplicate-close of protected source fails");
    TEST_ASSERT(dst_h == INVALID_HANDLE_VALUE, "no destination handle leaked");
    TEST_ASSERT(dst_ht.count == dst_before,
                "destination table has no leaked entry after rollback");
    TEST_ASSERT(ObpLookupHandle(&src_ht, src_h) != (void *)0,
                "protected source handle still open after refused close");

    /* Drop protection and clean up. */
    HANDLE_TABLE_ENTRY *se = ObpLookupHandle(&src_ht, src_h);
    if (se) se->attributes = 0;
    ObpFreeHandle(&src_ht, src_h);
    ObDereferenceObject(body);
    ob_handle_table_destroy(&src_ht);
    ob_handle_table_destroy(&dst_ht);
}

/* ---- ob_handle_table_inherit ---- */

static void test_ob_handle_inherit(void)
{
    /* Dedicated type so per-type handle stats are not perturbed by other tests. */
    OBJECT_TYPE tmpl = {
        .name = "InhStat", .body_size = 32,
        .on_close = (void *)0, .on_delete = (void *)0,
        .on_open = (void *)0, .on_parse = (void *)0,
    };
    OBJECT_TYPE *t = (OBJECT_TYPE *)ob_create_type(&tmpl);
    TEST_ASSERT(t != (void *)0, "ob_create_type for inherit-stats");
    if (!t) return;

    HANDLE_TABLE parent, child;
    ob_handle_table_init(&parent);
    ob_handle_table_init(&child);

    void *body = ob_alloc_object(t);
    HANDLE h = ObpAllocateHandle(&parent, body, 0x1F01FF, OBJ_INHERIT);
    TEST_ASSERT(atomic_read(&t->total_handles) == 1,
                "one open handle after parent alloc");

    int inherited = ob_handle_table_inherit(&parent, &child);
    TEST_ASSERT(inherited == 1, "exactly 1 handle inherited");

    HANDLE_TABLE_ENTRY *child_entry = ObpLookupHandle(&child, h);
    TEST_ASSERT(child_entry != (void *)0 && child_entry->object == body,
                "inherited handle at same index points to same object");
    TEST_ASSERT(atomic_read(&t->total_handles) == 2,
                "inherited handle counted in per-type total_handles");

    ObpFreeHandle(&parent, h);
    ObpFreeHandle(&child, h);
    TEST_ASSERT(atomic_read(&t->total_handles) == 0,
                "total_handles back to 0 after both closes (no undercount)");

    ObDereferenceObject(body);
    ob_handle_table_destroy(&parent);
    ob_handle_table_destroy(&child);
}

/* ---- Inheritance with no inheritable handles is success, not OOM ---- */

static void test_ob_inherit_none(void)
{
    HANDLE_TABLE parent, child;
    ob_handle_table_init(&parent);
    ob_handle_table_init(&child);

    void *body = ob_alloc_object(&test_type);
    /* Parent holds a handle WITHOUT OBJ_INHERIT. */
    HANDLE h = ObpAllocateHandle(&parent, body, 0x1, 0);

    int n = ob_handle_table_inherit(&parent, &child);
    TEST_ASSERT(n == 0, "no inheritable handles -> returns 0 (not -1/OOM)");
    TEST_ASSERT(ObpLookupHandle(&child, h) == (void *)0,
                "non-inheritable handle not copied to child");

    ObpFreeHandle(&parent, h);
    ObDereferenceObject(body);
    ob_handle_table_destroy(&parent);
    ob_handle_table_destroy(&child);
}

/* ---- NtQueryDirectoryObject: enumerate root ---- */

static void test_ob_query_directory(void)
{
    HANDLE_TABLE ht;
    ob_handle_table_init(&ht);

    HANDLE dir_h = INVALID_HANDLE_VALUE;
    int rc = NtOpenDirectoryObject(&ht, "\\", 0, &dir_h);
    TEST_ASSERT(rc == 0, "NtOpenDirectoryObject(\"\\\\\") succeeds");

    OBJECT_DIRECTORY_INFORMATION buf[16];
    uint32_t ctx = 0, count = 0;
    rc = NtQueryDirectoryObject(&ht, dir_h, buf, 16, &ctx, &count);
    TEST_ASSERT(rc == 0, "NtQueryDirectoryObject succeeds");
    TEST_ASSERT(count >= 3,
                "root directory has >= 3 entries (Device, KernelObjects, BaseNamedObjects)");

    ObpFreeHandle(&ht, dir_h);
    /* -9 LEAK retrofit. */
    ob_handle_table_destroy(&ht);
}

/* ---- Per-type object and handle statistics ---- */

static void test_ob_type_stats(void)
{
    /* Register a fresh type for isolated stats testing */
    static const OBJECT_TYPE stats_tmpl = {
        .name      = "StatsTest",
        .body_size = 32,
        .on_close  = (void *)0,
        .on_delete = (void *)0,
        .on_open   = (void *)0,
        .on_parse  = (void *)0,
    };
    const OBJECT_TYPE *stype = ob_create_type(&stats_tmpl);
    TEST_ASSERT(stype != (void *)0, "ob_create_type for stats test");

    /* Baseline: 0 objects, 0 handles */
    TEST_ASSERT_EQ(atomic_read(&stype->total_objects), 0,
                   "initial total_objects == 0");
    TEST_ASSERT_EQ(atomic_read(&stype->total_handles), 0,
                   "initial total_handles == 0");

    /* Allocate 3 objects */
    void *o1 = ob_alloc_object(stype);
    void *o2 = ob_alloc_object(stype);
    void *o3 = ob_alloc_object(stype);
    TEST_ASSERT(o1 && o2 && o3, "3 objects allocated");
    TEST_ASSERT_EQ(atomic_read(&stype->total_objects), 3,
                   "total_objects == 3 after 3 allocs");
    TEST_ASSERT_EQ(stype->peak_objects, 3,
                   "peak_objects == 3");

    /* Free 1 object */
    ObDereferenceObject(o3);
    TEST_ASSERT_EQ(atomic_read(&stype->total_objects), 2,
                   "total_objects == 2 after 1 free");
    TEST_ASSERT_EQ(stype->peak_objects, 3,
                   "peak_objects still 3 after free");

    /* Allocate handle and check handle stats */
    HANDLE_TABLE ht;
    ob_handle_table_init(&ht);
    HANDLE h1 = ObpAllocateHandle(&ht, o1, 0x1F0FFF, 0);
    TEST_ASSERT(h1 >= 0, "handle allocated");
    TEST_ASSERT_EQ(atomic_read(&stype->total_handles), 1,
                   "total_handles == 1 after 1 handle alloc");
    TEST_ASSERT_EQ(stype->peak_handles, 1,
                   "peak_handles == 1");

    HANDLE h2 = ObpAllocateHandle(&ht, o2, 0x1F0FFF, 0);
    TEST_ASSERT_EQ(atomic_read(&stype->total_handles), 2,
                   "total_handles == 2 after 2 handle allocs");

    /* Free 1 handle */
    ObpFreeHandle(&ht, h1);
    TEST_ASSERT_EQ(atomic_read(&stype->total_handles), 1,
                   "total_handles == 1 after 1 handle free");
    TEST_ASSERT_EQ(stype->peak_handles, 2,
                   "peak_handles still 2 after handle free");

    /* NtQueryObject ObjectTypeInformation */
    OBJECT_TYPE_INFORMATION ti;
    uint32_t rlen = 0;
    int rc = NtQueryObject(&ht, h2, ObjectTypeInformation,
                           &ti, sizeof(ti), &rlen);
    TEST_ASSERT(rc == 0, "NtQueryObject(ObjectTypeInformation) succeeds");
    TEST_ASSERT_EQ(rlen, sizeof(OBJECT_TYPE_INFORMATION),
                   "return length matches struct size");
    TEST_ASSERT_EQ(ti.body_size, 32, "body_size == 32");

    /* Cleanup */
    ObpFreeHandle(&ht, h2);
    ObDereferenceObject(o1);
    ObDereferenceObject(o2);
    ob_handle_table_destroy(&ht);
}

/* Regression for per-type handle-statistics export: the NtQueryObject export
 * path must floor a transient-negative total_handles to 0 (never surface ~4
 * billion), and ObjectTypesInformation must enumerate the global type table
 * with NO valid object handle. */
extern int strncmp(const char *a, const char *b, size_t n);
static void test_ob_stat_export_clamp(void)
{
    static const OBJECT_TYPE clamp_tmpl = {
        .name      = "ClampTest",
        .body_size = 16,
        .on_close  = (void *)0,
        .on_delete = (void *)0,
        .on_open   = (void *)0,
        .on_parse  = (void *)0,
    };
    OBJECT_TYPE *ctype = (OBJECT_TYPE *)ob_create_type(&clamp_tmpl);
    TEST_ASSERT(ctype != (void *)0, "ob_create_type for clamp test");

    void *o1 = ob_alloc_object(ctype);
    TEST_ASSERT(o1 != (void *)0, "clamp obj allocated");

    HANDLE_TABLE ht;
    ob_handle_table_init(&ht);
    HANDLE h1 = ObpAllocateHandle(&ht, o1, 0x1F0FFF, 0);
    TEST_ASSERT(h1 >= 0, "clamp handle allocated");

    /* Simulate the S3 unserialized-slot transient: drive total_handles below 0.
     * The export path floors it; it must NOT surface as a huge unsigned value. */
    atomic_set(&ctype->total_handles, -3);

    OBJECT_TYPE_INFORMATION ti;
    uint32_t rlen = 0;
    int rc = NtQueryObject(&ht, h1, ObjectTypeInformation, &ti, sizeof(ti), &rlen);
    TEST_ASSERT(rc == 0, "NtQueryObject(ObjectTypeInformation) succeeds");
    TEST_ASSERT_EQ(ti.total_handles, 0u,
                   "negative total_handles floored to 0 on export");

    /* ObjectTypesInformation enumerates global types with NO object handle. */
    OBJECT_TYPES_INFORMATION oti;
    uint32_t rlen2 = 0;
    int rc2 = NtQueryObject(&ht, 0, ObjectTypesInformation,
                            &oti, sizeof(oti), &rlen2);
    TEST_ASSERT(rc2 == 0, "ObjectTypesInformation succeeds with no object handle");

    int found = 0;
    for (uint32_t i = 0; i < oti.number_of_types; i++) {
        if (strncmp(oti.types[i].type_name, "ClampTest", sizeof("ClampTest")) == 0) {
            found = 1;
            TEST_ASSERT_EQ(oti.types[i].total_handles, 0u,
                           "enumerated ClampTest total_handles floored to 0");
            break;
        }
    }
    TEST_ASSERT(found, "ClampTest present in ObjectTypesInformation enumeration");

    /* Restore a sane count so teardown math is not skewed by the injected value */
    atomic_set(&ctype->total_handles, 1);

    ObpFreeHandle(&ht, h1);
    ObDereferenceObject(o1);
    ob_handle_table_destroy(&ht);
}

/* ---- Object callbacks -- handle operation filtering (S13) ---- */

/* Local test constant matching Win32 spec (TODO-05 S7 will define canonical) */
#define TEST_PROCESS_TERMINATE  0x0001

static void test_pre_strip_terminate(OB_PRE_OPERATION_INFORMATION *info)
{
    /* Strip PROCESS_TERMINATE from desired_access */
    *info->desired_access &= ~TEST_PROCESS_TERMINATE;
}

static volatile uint32_t g_post_cb_called = 0;
static volatile uint32_t g_post_cb_granted = 0;

static void test_post_record(OB_POST_OPERATION_INFORMATION *info)
{
    g_post_cb_called++;
    g_post_cb_granted = info->granted_access;
}

static void test_ob_callbacks(void)
{
    /* Create a type for callback testing */
    static const OBJECT_TYPE cb_tmpl = {
        .name      = "CbTest",
        .body_size = 16,
        .on_close  = (void *)0,
        .on_delete = (void *)0,
        .on_open   = (void *)0,
        .on_parse  = (void *)0,
    };
    const OBJECT_TYPE *ctype = ob_create_type(&cb_tmpl);
    TEST_ASSERT(ctype != (void *)0, "ob_create_type for callback test");

    /* Register a pre-callback that strips PROCESS_TERMINATE */
    OB_CALLBACK_REGISTRATION reg;
    reg.version = OB_CALLBACK_VERSION;
    reg.operation_count = 1;
    reg.altitude = 100;
    reg.context = (void *)0;
    reg.operations[0].object_type  = ctype;
    reg.operations[0].operations   = (uint32_t)OB_OPERATION_HANDLE_CREATE;
    reg.operations[0].pre_callback = test_pre_strip_terminate;
    reg.operations[0].post_callback = test_post_record;

    OB_CALLBACK_HANDLE cbh = OB_INVALID_CALLBACK_HANDLE;
    int rc = ObRegisterCallbacks(&reg, &cbh);
    TEST_ASSERT(rc == 0, "ObRegisterCallbacks succeeds");
    TEST_ASSERT(cbh != OB_INVALID_CALLBACK_HANDLE, "callback handle is valid");

    /* Allocate an object and create a handle with PROCESS_TERMINATE set */
    void *obj = ob_alloc_object(ctype);
    TEST_ASSERT(obj != (void *)0, "alloc object for callback test");

    HANDLE_TABLE ht;
    ob_handle_table_init(&ht);
    g_post_cb_called = 0;
    g_post_cb_granted = 0;

    uint32_t requested = 0x1F0FFF;  /* includes PROCESS_TERMINATE bit */
    HANDLE h = ObpAllocateHandle(&ht, obj, requested, 0);
    TEST_ASSERT(h >= 0, "handle allocated with callback active");

    /* Verify pre-callback stripped TERMINATE -- check granted_access in handle table */
    HANDLE_TABLE_ENTRY *entry = ObpLookupHandle(&ht, h);
    TEST_ASSERT(entry != (void *)0, "handle lookup succeeds");
    TEST_ASSERT((entry->granted_access & TEST_PROCESS_TERMINATE) == 0,
                "pre-callback stripped PROCESS_TERMINATE from granted access");

    /* Verify post-callback was invoked */
    TEST_ASSERT_EQ(g_post_cb_called, 1, "post-callback invoked once");
    TEST_ASSERT((g_post_cb_granted & TEST_PROCESS_TERMINATE) == 0,
                "post-callback sees stripped access");

    /* Unregister and verify full access is restored */
    ObUnRegisterCallbacks(cbh);

    g_post_cb_called = 0;
    HANDLE h2 = ObpAllocateHandle(&ht, obj, requested, 0);
    TEST_ASSERT(h2 >= 0, "handle allocated after unregister");
    entry = ObpLookupHandle(&ht, h2);
    TEST_ASSERT((entry->granted_access & TEST_PROCESS_TERMINATE) != 0,
                "full access restored after unregister -- TERMINATE present");
    TEST_ASSERT_EQ(g_post_cb_called, 0,
                   "post-callback NOT invoked after unregister");

    /* Cleanup */
    ObpFreeHandle(&ht, h);
    ObpFreeHandle(&ht, h2);
    ObDereferenceObject(obj);
    ob_handle_table_destroy(&ht);
}

/* ---- Object-callback hardening: CREATE-path access ceiling + stable handle id ---- */

static void test_pre_raise_access(OB_PRE_OPERATION_INFORMATION *info);  /* defined below */

static void test_pre_strip_bit1(OB_PRE_OPERATION_INFORMATION *info)
{
    *info->desired_access &= ~0x2u;   /* strip bit 0x2 only */
}

static void test_pre_readd_bit1(OB_PRE_OPERATION_INFORMATION *info)
{
    *info->desired_access |= 0x2u;    /* try to RE-ADD bit 0x2 a prior cb stripped */
}

static void test_ob_callbacks_hardening(void)
{
    static const OBJECT_TYPE hb_tmpl = {
        .name = "CbHarden", .body_size = 16,
        .on_close = (void *)0, .on_delete = (void *)0,
        .on_open = (void *)0, .on_parse = (void *)0,
    };
    const OBJECT_TYPE *ctype = ob_create_type(&hb_tmpl);
    TEST_ASSERT(ctype != (void *)0, "ob_create_type for cb-harden");
    if (!ctype) return;

    HANDLE_TABLE ht;
    ob_handle_table_init(&ht);
    void *obj = ob_alloc_object(ctype);
    TEST_ASSERT(obj != (void *)0, "alloc object for cb-harden");

    /* (1) CREATE-path ceiling: a callback that ORs in a bit ABOVE the requested
     * mask must not mint unrequested access. Request 0x0001; callback ORs
     * 0x10000002; ob_invoke_pre_callbacks clamps to the request ceiling. */
    OB_CALLBACK_REGISTRATION raise_reg;
    raise_reg.version = OB_CALLBACK_VERSION;
    raise_reg.operation_count = 1;
    raise_reg.altitude = 100;
    raise_reg.context = (void *)0;
    raise_reg.operations[0].object_type   = ctype;
    raise_reg.operations[0].operations    = (uint32_t)OB_OPERATION_HANDLE_CREATE;
    raise_reg.operations[0].pre_callback  = test_pre_raise_access;  /* ORs 0x10000002 */
    raise_reg.operations[0].post_callback = (void *)0;
    OB_CALLBACK_HANDLE rh = OB_INVALID_CALLBACK_HANDLE;
    TEST_ASSERT(ObRegisterCallbacks(&raise_reg, &rh) == 0, "register raise cb");

    HANDLE he = ObpAllocateHandle(&ht, obj, 0x0001, 0);
    TEST_ASSERT(he >= 0, "create-path handle allocated");
    HANDLE_TABLE_ENTRY *ee = ObpLookupHandle(&ht, he);
    TEST_ASSERT(ee && ee->granted_access == 0x0001,
                "CREATE-path access clamped to request ceiling (no elevation)");
    ObpFreeHandle(&ht, he);
    ObUnRegisterCallbacks(rh);

    /* (2) Stable handle identity across an altitude-shift insertion. Register
     * cb_hi (altitude 200) -> h_hi; then cb_lo (altitude 100) inserts BEFORE it
     * and shifts the array. h_hi must still identify cb_hi, not cb_lo's slot. */
    OB_CALLBACK_REGISTRATION hi, lo;
    hi = raise_reg;  /* reuse fields */
    hi.altitude = 200;
    hi.operations[0].pre_callback  = test_pre_strip_terminate;  /* strips 0x1 */
    hi.operations[0].post_callback = (void *)0;
    lo = hi;
    lo.altitude = 100;
    lo.operations[0].pre_callback  = test_pre_strip_bit1;       /* strips 0x2 */

    OB_CALLBACK_HANDLE h_hi = OB_INVALID_CALLBACK_HANDLE;
    OB_CALLBACK_HANDLE h_lo = OB_INVALID_CALLBACK_HANDLE;
    TEST_ASSERT(ObRegisterCallbacks(&hi, &h_hi) == 0, "register hi-altitude cb");
    TEST_ASSERT(ObRegisterCallbacks(&lo, &h_lo) == 0, "register lo-altitude cb");
    TEST_ASSERT(h_hi != h_lo, "callback handles are distinct identities");

    /* Remove cb_hi by its handle. If handles were array positions, the shift
     * from inserting cb_lo would make h_hi remove the wrong node. */
    ObUnRegisterCallbacks(h_hi);

    /* Only cb_lo (strips 0x2) should remain. Request 0x3 -> 0x2 stripped, 0x1 kept. */
    HANDLE hs = ObpAllocateHandle(&ht, obj, 0x3, 0);
    TEST_ASSERT(hs >= 0, "handle allocated after selective unregister");
    HANDLE_TABLE_ENTRY *es = ObpLookupHandle(&ht, hs);
    TEST_ASSERT(es && (es->granted_access & 0x2) == 0,
                "lo-altitude cb still active (bit 0x2 stripped)");
    TEST_ASSERT(es && (es->granted_access & 0x1) != 0,
                "hi-altitude cb correctly removed by stable id (bit 0x1 kept)");

    ObpFreeHandle(&ht, hs);
    ObUnRegisterCallbacks(h_lo);

    /* (3) Chain monotonicity: a later (higher-altitude) callback must NOT be
     * able to re-add a right an earlier (lower-altitude) callback stripped.
     * cb_strip (altitude 100) strips 0x2; cb_readd (altitude 200) ORs 0x2 back.
     * The monotonic ceiling clamps cb_readd back to the pre-callback mask, so
     * 0x2 stays stripped -- the earlier anti-tamper filter wins. */
    OB_CALLBACK_REGISTRATION cstrip, creadd;
    cstrip = lo;   /* altitude 100, strips 0x2 */
    cstrip.operations[0].pre_callback = test_pre_strip_bit1;
    creadd = lo;
    creadd.altitude = 200;
    creadd.operations[0].pre_callback = test_pre_readd_bit1;   /* ORs 0x2 back */

    OB_CALLBACK_HANDLE h_s = OB_INVALID_CALLBACK_HANDLE;
    OB_CALLBACK_HANDLE h_r = OB_INVALID_CALLBACK_HANDLE;
    TEST_ASSERT(ObRegisterCallbacks(&cstrip, &h_s) == 0, "register strip cb");
    TEST_ASSERT(ObRegisterCallbacks(&creadd, &h_r) == 0, "register re-add cb");

    HANDLE hc = ObpAllocateHandle(&ht, obj, 0x3, 0);
    TEST_ASSERT(hc >= 0, "handle allocated through strip+readd chain");
    HANDLE_TABLE_ENTRY *ec = ObpLookupHandle(&ht, hc);
    TEST_ASSERT(ec && (ec->granted_access & 0x2) == 0,
                "later cb cannot re-add a right an earlier cb stripped (monotonic)");
    TEST_ASSERT(ec && (ec->granted_access & 0x1) != 0,
                "unrelated requested bit 0x1 preserved through the chain");

    ObpFreeHandle(&ht, hc);
    ObUnRegisterCallbacks(h_r);
    ObUnRegisterCallbacks(h_s);
    ObDereferenceObject(obj);
    ob_handle_table_destroy(&ht);
}

/* ---- NtDuplicateObject access cap is callback-proof ---- */

static void test_pre_raise_access(OB_PRE_OPERATION_INFORMATION *info)
{
    /* Misbehaving callback: OR in a right outside the source mask (0x10000000)
     * AND a source-held right above what the caller requested (0x0002). The
     * NtDuplicateObject request_ceiling must strip BOTH back out of the stored
     * grant -- callbacks may only reduce access, never elevate it. */
    *info->desired_access |= 0x10000002u;
}

static void test_ob_duplicate_cap_callback_proof(void)
{
    OBJECT_TYPE cb_tmpl = {
        .name = "DupCapCb", .body_size = 32,
        .on_close = (void *)0, .on_delete = (void *)0,
        .on_open = (void *)0, .on_parse = (void *)0,
    };
    const OBJECT_TYPE *ctype = ob_create_type(&cb_tmpl);
    TEST_ASSERT(ctype != (void *)0, "ob_create_type for dup-cap-cb");
    if (!ctype) return;

    OB_CALLBACK_REGISTRATION reg;
    reg.version = OB_CALLBACK_VERSION;
    reg.operation_count = 1;
    reg.altitude = 101;
    reg.context = (void *)0;
    reg.operations[0].object_type   = ctype;
    reg.operations[0].operations    = (uint32_t)OB_OPERATION_HANDLE_DUPLICATE;
    reg.operations[0].pre_callback  = test_pre_raise_access;
    reg.operations[0].post_callback = test_post_record;
    OB_CALLBACK_HANDLE cbh = OB_INVALID_CALLBACK_HANDLE;
    int rc = ObRegisterCallbacks(&reg, &cbh);
    TEST_ASSERT(rc == 0, "ObRegisterCallbacks (dup raise) succeeds");

    void *obj = ob_alloc_object(ctype);
    HANDLE_TABLE src_ht, dst_ht;
    ob_handle_table_init(&src_ht);
    ob_handle_table_init(&dst_ht);
    /* Source holds 0x0003; the caller requests only 0x0001. */
    HANDLE src_h = ObpAllocateHandle(&src_ht, obj, 0x0003, 0);

    g_post_cb_called = 0;
    g_post_cb_granted = 0xFFFFFFFFu;
    HANDLE dst_h = INVALID_HANDLE_VALUE;
    rc = NtDuplicateObject(&src_ht, src_h, &dst_ht, &dst_h, 0x0001, 0, 0);
    TEST_ASSERT(rc == 0, "dup with raising callback still succeeds");
    HANDLE_TABLE_ENTRY *de = ObpLookupHandle(&dst_ht, dst_h);
    /* request_ceiling = desired(0x0001) & src(0x0003) = 0x0001: the callback's
     * 0x10000000 (outside source) and 0x0002 (source-held but above request)
     * must both be stripped. */
    TEST_ASSERT(de != (void *)0 && de->granted_access == 0x0001,
                "stored dup access capped to request ceiling, not src mask");
    TEST_ASSERT(de != (void *)0 && (de->granted_access & 0x10000002u) == 0,
                "callback-raised bits (outside-source + above-request) clamped out");
    TEST_ASSERT((g_post_cb_granted & 0x10000002u) == 0,
                "dup post-callback sees the clamped stored access");

    ObUnRegisterCallbacks(cbh);
    if (dst_h != INVALID_HANDLE_VALUE) ObpFreeHandle(&dst_ht, dst_h);
    ObpFreeHandle(&src_ht, src_h);
    ObDereferenceObject(obj);
    ob_handle_table_destroy(&src_ht);
    ob_handle_table_destroy(&dst_ht);
}

/* ---- A CREATE-only callback must NOT fire on a duplicate (operation mask is authoritative) ---- */

static void test_pre_noop(OB_PRE_OPERATION_INFORMATION *info)
{
    (void)info;   /* inspect-only: never mutates desired_access */
}

static void test_ob_callbacks_dup_create_only_unaffected(void)
{
    OBJECT_TYPE tmpl = {
        .name = "DupCreOnly", .body_size = 32,
        .on_close = (void *)0, .on_delete = (void *)0,
        .on_open = (void *)0, .on_parse = (void *)0,
    };
    const OBJECT_TYPE *t = ob_create_type(&tmpl);
    TEST_ASSERT(t != (void *)0, "ob_create_type for dup-create-only");
    if (!t) return;

    void *obj = ob_alloc_object(t);
    HANDLE_TABLE src_ht, dst_ht;
    ob_handle_table_init(&src_ht);
    ob_handle_table_init(&dst_ht);
    /* Create the source BEFORE registering the callback, so its own grant keeps
     * the full 0x3 (the source is a real create but the cb isn't active yet). */
    HANDLE src_h = ObpAllocateHandle(&src_ht, obj, 0x3, 0);
    TEST_ASSERT(src_h >= 0, "source handle allocated");

    /* Register a callback for HANDLE_CREATE ONLY that strips 0x1. */
    OB_CALLBACK_REGISTRATION reg;
    reg.version = OB_CALLBACK_VERSION;
    reg.operation_count = 1;
    reg.altitude = 90;
    reg.context = (void *)0;
    reg.operations[0].object_type   = t;
    reg.operations[0].operations    = (uint32_t)OB_OPERATION_HANDLE_CREATE;  /* CREATE only */
    reg.operations[0].pre_callback  = test_pre_strip_terminate;              /* strips 0x1 */
    reg.operations[0].post_callback = (void *)0;
    OB_CALLBACK_HANDLE cbh = OB_INVALID_CALLBACK_HANDLE;
    TEST_ASSERT(ObRegisterCallbacks(&reg, &cbh) == 0, "register create-only strip cb");

    /* Duplicate. No HANDLE_DUPLICATE callback is registered, and the CREATE-only
     * callback must NOT fire on the duplicate path -> bit 0x1 must survive. */
    HANDLE dst_h = INVALID_HANDLE_VALUE;
    int rc = NtDuplicateObject(&src_ht, src_h, &dst_ht, &dst_h, 0x3, 0, 0);
    TEST_ASSERT(rc == 0, "duplicate succeeds with create-only cb registered");
    HANDLE_TABLE_ENTRY *de = ObpLookupHandle(&dst_ht, dst_h);
    TEST_ASSERT(de != (void *)0 && (de->granted_access & 0x1) != 0,
                "CREATE-only callback did NOT strip the duplicate (operation mask authoritative)");
    TEST_ASSERT(de != (void *)0 && de->granted_access == 0x3,
                "duplicate retains full requested access (no spurious create-cb filtering)");

    ObUnRegisterCallbacks(cbh);
    if (dst_h != INVALID_HANDLE_VALUE) ObpFreeHandle(&dst_ht, dst_h);
    ObpFreeHandle(&src_ht, src_h);
    ObDereferenceObject(obj);
    ob_handle_table_destroy(&src_ht);
    ob_handle_table_destroy(&dst_ht);
}

/* ---- A no-op callback must not turn a legitimately zero-access request into a denial ---- */

static void test_ob_callbacks_zero_access_noop_allowed(void)
{
    OBJECT_TYPE tmpl = {
        .name = "ZeroAcc", .body_size = 16,
        .on_close = (void *)0, .on_delete = (void *)0,
        .on_open = (void *)0, .on_parse = (void *)0,
    };
    const OBJECT_TYPE *t = ob_create_type(&tmpl);
    TEST_ASSERT(t != (void *)0, "ob_create_type for zero-access");
    if (!t) return;

    /* Inspect-only CREATE callback that never mutates desired_access. */
    OB_CALLBACK_REGISTRATION reg;
    reg.version = OB_CALLBACK_VERSION;
    reg.operation_count = 1;
    reg.altitude = 90;
    reg.context = (void *)0;
    reg.operations[0].object_type   = t;
    reg.operations[0].operations    = (uint32_t)OB_OPERATION_HANDLE_CREATE;
    reg.operations[0].pre_callback  = test_pre_noop;
    reg.operations[0].post_callback = (void *)0;
    OB_CALLBACK_HANDLE cbh = OB_INVALID_CALLBACK_HANDLE;
    TEST_ASSERT(ObRegisterCallbacks(&reg, &cbh) == 0, "register no-op create cb");

    void *obj = ob_alloc_object(t);
    HANDLE_TABLE ht;
    ob_handle_table_init(&ht);

    /* A zero-access request with a no-op callback must NOT be denied: the deny
     * contract fires only when a callback ZEROES a previously-nonzero mask, not
     * when the request entered the callback already at 0. */
    HANDLE h = ObpAllocateHandle(&ht, obj, 0x0, 0);
    TEST_ASSERT(h >= 0, "zero-access request allowed through no-op callback (not denied)");
    HANDLE_TABLE_ENTRY *e = ObpLookupHandle(&ht, h);
    TEST_ASSERT(e != (void *)0 && e->granted_access == 0x0,
                "zero-access handle granted exactly zero rights");

    ObUnRegisterCallbacks(cbh);
    if (h >= 0) ObpFreeHandle(&ht, h);
    ObDereferenceObject(obj);
    ob_handle_table_destroy(&ht);
}

/* ---- DUPLICATE_CLOSE_SOURCE does not prematurely tear down a last handle ---- */

static volatile uint32_t g_onclose_count = 0;
static void test_on_close_count(void *body, uint32_t handle_count)
{
    (void)body; (void)handle_count;
    g_onclose_count++;
}

static void test_ob_duplicate_close_no_premature_onclose(void)
{
    OBJECT_TYPE tmpl = {
        .name = "DupOnClose", .body_size = 32,
        .on_close = test_on_close_count, .on_delete = (void *)0,
        .on_open = (void *)0, .on_parse = (void *)0,
    };
    const OBJECT_TYPE *t = ob_create_type(&tmpl);
    TEST_ASSERT(t != (void *)0, "ob_create_type for dup-onclose");
    if (!t) return;

    void *obj = ob_alloc_object(t);
    HANDLE_TABLE src_ht, dst_ht;
    ob_handle_table_init(&src_ht);
    ob_handle_table_init(&dst_ht);
    /* The source handle is the object's ONLY handle. */
    HANDLE src_h = ObpAllocateHandle(&src_ht, obj, 0x1, 0);

    g_onclose_count = 0;
    HANDLE dst_h = INVALID_HANDLE_VALUE;
    int rc = NtDuplicateObject(&src_ht, src_h, &dst_ht, &dst_h, 0x1,
                               0, DUPLICATE_CLOSE_SOURCE);
    TEST_ASSERT(rc == 0, "duplicate-close of last handle succeeds");
    /* Dest is allocated before the source close, so handle_count never hits 0
     * and on_close must NOT have fired -- the object is not torn down. */
    TEST_ASSERT(g_onclose_count == 0,
                "on_close NOT fired during dup (dest keeps handle_count >= 1)");
    TEST_ASSERT(dst_h != INVALID_HANDLE_VALUE
                && ObpLookupHandle(&dst_ht, dst_h) != (void *)0,
                "destination handle is live after source close");

    /* Closing the dest (now the true last handle) DOES fire on_close once. */
    ObpFreeHandle(&dst_ht, dst_h);
    TEST_ASSERT(g_onclose_count == 1,
                "on_close fires exactly when the true last handle closes");

    ObDereferenceObject(obj);
    ob_handle_table_destroy(&src_ht);
    ob_handle_table_destroy(&dst_ht);
}

/* ---- Tagged reference tracing (S15) ---- */

/* Pack 4-char tag into uint32_t: big-endian so dump shows readable chars */
#define TAG4(a,b,c,d) (((uint32_t)(a)<<24)|((uint32_t)(b)<<16)|((uint32_t)(c)<<8)|(uint32_t)(d))

static void test_ob_trace(void)
{
    /* Create a type with tracing enabled */
    static const OBJECT_TYPE trace_tmpl = {
        .name      = "TraceTest",
        .body_size = 16,
        .on_close  = (void *)0,
        .on_delete = (void *)0,
        .on_open   = (void *)0,
        .on_parse  = (void *)0,
    };
    const OBJECT_TYPE *ttype = ob_create_type(&trace_tmpl);
    TEST_ASSERT(ttype != (void *)0, "ob_create_type for trace test");

    ob_enable_type_tracing(ttype);
    TEST_ASSERT_EQ(ob_type_tracing_enabled(ttype), 1,
                   "type tracing enabled");

    /* Allocate -- should get trace info */
    void *obj = ob_alloc_object(ttype);
    TEST_ASSERT(obj != (void *)0, "alloc object with tracing");
    OBJECT_HEADER *hdr = OB_HEADER_FROM_BODY(obj);
    TEST_ASSERT(hdr->trace != (void *)0, "trace info allocated");

    /* Ref with tag, deref with tag */
    ObReferenceObjectWithTag(obj, TAG4('T','s','0','1'));
    TEST_ASSERT_EQ(atomic_read(&hdr->ref_count), 2,
                   "ref_count == 2 after tagged ref");

    ObDereferenceObjectWithTag(obj, TAG4('T','s','0','1'));
    TEST_ASSERT_EQ(atomic_read(&hdr->ref_count), 1,
                   "ref_count == 1 after tagged deref");

    /* Trace log should have entries */
    TEST_ASSERT(hdr->trace->count >= 2, "trace log has >= 2 entries");

    /* Disable tracing */
    ob_disable_type_tracing(ttype);
    TEST_ASSERT_EQ(ob_type_tracing_enabled(ttype), 0,
                   "type tracing disabled");

    /* Final deref frees the object (and trace log) */
    ObDereferenceObject(obj);
}

/* ---- S15: trace ring wrap + >16 distinct tags + explicit dump (no crash) ---- */

static void test_ob_trace_wrap_and_tags(void)
{
    static const OBJECT_TYPE wt_tmpl = {
        .name = "TraceWrap", .body_size = 16,
        .on_close = (void *)0, .on_delete = (void *)0,
        .on_open = (void *)0, .on_parse = (void *)0,
    };
    const OBJECT_TYPE *t = ob_create_type(&wt_tmpl);
    TEST_ASSERT(t != (void *)0, "ob_create_type for trace-wrap");
    if (!t) return;
    ob_enable_type_tracing(t);

    void *obj = ob_alloc_object(t);
    TEST_ASSERT(obj != (void *)0, "alloc object for trace-wrap");
    if (!obj) { ob_disable_type_tracing(t); return; }
    OBJECT_HEADER *hdr = OB_HEADER_FROM_BODY(obj);
    TEST_ASSERT(hdr->trace != (void *)0, "trace info allocated for wrap test");

    /* 35 balanced ref/deref PAIRS = 70 ring entries (> OB_TRACE_RING_SIZE 64, so
     * the ring wraps and count exceeds ring size) using 20 distinct tags (> the
     * old MAX_TAGS 16, so the summary must NOT drop any). ref_count returns to
     * its baseline after each pair. */
    for (uint32_t i = 0; i < 35; i++) {
        uint32_t tag = TAG4('T', 'w', (char)('0' + (i % 20) / 10), (char)('0' + (i % 20) % 10));
        ObReferenceObjectWithTag(obj, tag);
        ObDereferenceObjectWithTag(obj, tag);
    }
    TEST_ASSERT_EQ((uint32_t)hdr->trace->count, 70,
                   "70 trace entries recorded (count is 64-bit, no wrap)");
    TEST_ASSERT_EQ(atomic_read(&hdr->ref_count), 1,
                   "ref_count balanced back to baseline after 35 pairs");
    /* Lifetime tallies survive ring wrap even though the ring overwrote the
     * first 6 entries (70 written, 64-slot ring). */
    TEST_ASSERT_EQ((uint32_t)hdr->trace->total_refs, 35,
                   "lifetime total_refs == 35 (ring-independent)");
    TEST_ASSERT_EQ((uint32_t)hdr->trace->total_derefs, 35,
                   "lifetime total_derefs == 35 (ring-independent)");

    /* A leak burst of 70 single-tag tagged refs (no matching deref) overruns the
     * 64-entry ring, so the per-tag summary scans only the last 64 'Leak' refs
     * and the 35 earlier balanced pairs are GONE from the ring -- but the
     * lifetime counters still record the true totals, so the dump's LIFETIME
     * verdict reports the leak that the truncated per-tag view would understate. */
    for (uint32_t i = 0; i < 70; i++)
        ObReferenceObjectWithTag(obj, TAG4('L', 'e', 'a', 'k'));
    TEST_ASSERT_EQ((uint32_t)hdr->trace->total_refs, 105,
                   "lifetime total_refs == 105 after 70-ref leak burst");
    TEST_ASSERT_EQ((uint32_t)hdr->trace->total_derefs, 35,
                   "lifetime total_derefs unchanged at 35 (leak outstanding)");
    TEST_ASSERT_EQ((int32_t)(hdr->trace->total_refs - hdr->trace->total_derefs), 70,
                   "lifetime leak verdict = 70 outstanding (survives ring wrap)");

    /* Explicit dump must traverse the wrapped ring + truncation note + lifetime
     * leak line without crashing (klog output is not unit-assertable). */
    ob_dump_trace(obj);

    /* Rebalance the 70 leaked refs so the object can free cleanly. */
    for (uint32_t i = 0; i < 70; i++)
        ObDereferenceObjectWithTag(obj, TAG4('L', 'e', 'a', 'k'));
    TEST_ASSERT_EQ(atomic_read(&hdr->ref_count), 1,
                   "ref_count back to baseline after rebalancing leak burst");

    ob_disable_type_tracing(t);
    ObDereferenceObject(obj);   /* frees object + trace log (dumps once more) */
}

/* ---- S15: per-tag lifetime ledger catches a net-balanced mis-tag leak ---- */

/* Find a tag's lifetime slot in the per-tag ledger; returns -1 if absent. */
static int trace_life_index(OB_TRACE_INFO *ti, uint32_t tag)
{
    for (uint32_t i = 0; i < ti->life_ntags && i < OB_TRACE_LIFE_TAGS; i++)
        if (ti->life_tags[i].tag == tag)
            return (int)i;
    return -1;
}

static void test_ob_trace_mistag_lifetime(void)
{
    static const OBJECT_TYPE mt_tmpl = {
        .name = "TraceMistag", .body_size = 16,
        .on_close = (void *)0, .on_delete = (void *)0,
        .on_open = (void *)0, .on_parse = (void *)0,
    };
    const OBJECT_TYPE *t = ob_create_type(&mt_tmpl);
    TEST_ASSERT(t != (void *)0, "ob_create_type for mistag");
    if (!t) return;
    ob_enable_type_tracing(t);

    void *obj = ob_alloc_object(t);
    TEST_ASSERT(obj != (void *)0, "alloc object for mistag");
    if (!obj) { ob_disable_type_tracing(t); return; }
    OBJECT_HEADER *hdr = OB_HEADER_FROM_BODY(obj);
    TEST_ASSERT(hdr->trace != (void *)0, "trace info allocated for mistag");

    /* Mis-tag leak: 10 refs tagged 'Aaaa', 10 derefs tagged 'Bbbb' -- net
     * balanced (ref_count returns to baseline) but tag A over-refs and tag B
     * over-derefs. (Refs before derefs so ref_count never hits 0 mid-test.) */
    for (uint32_t i = 0; i < 10; i++)
        ObReferenceObjectWithTag(obj, TAG4('A', 'a', 'a', 'a'));
    for (uint32_t i = 0; i < 10; i++)
        ObDereferenceObjectWithTag(obj, TAG4('B', 'b', 'b', 'b'));

    /* 30 balanced 'Cccc' pairs = 60 ops; total 80 ops > 64-entry ring, so the
     * ring wraps and the A/B events are overwritten -- the ring-only view would
     * now look balanced and hide the mis-tag. */
    for (uint32_t i = 0; i < 30; i++) {
        ObReferenceObjectWithTag(obj, TAG4('C', 'c', 'c', 'c'));
        ObDereferenceObjectWithTag(obj, TAG4('C', 'c', 'c', 'c'));
    }

    TEST_ASSERT_EQ((uint32_t)hdr->trace->count, 80, "80 events recorded (ring wrapped)");
    TEST_ASSERT_EQ(atomic_read(&hdr->ref_count), 1, "ref_count net balanced after mis-tag");
    /* Net counters report balanced -- this is exactly where the per-tag ledger
     * must rescue the verdict. */
    TEST_ASSERT_EQ((int32_t)(hdr->trace->total_refs - hdr->trace->total_derefs), 0,
                   "net lifetime balanced (mis-tag hidden from net verdict)");

    /* Per-tag ledger survived the wrap and pins the mis-tag. */
    TEST_ASSERT_EQ(hdr->trace->life_overflow, 0, "ledger not overflowed (3 tags < 32)");
    int ia = trace_life_index(hdr->trace, TAG4('A', 'a', 'a', 'a'));
    int ib = trace_life_index(hdr->trace, TAG4('B', 'b', 'b', 'b'));
    TEST_ASSERT(ia >= 0 && ib >= 0, "tags A and B present in lifetime ledger");
    if (ia >= 0) {
        TEST_ASSERT_EQ((int32_t)(hdr->trace->life_tags[ia].refs -
                                 hdr->trace->life_tags[ia].derefs), 10,
                       "tag A lifetime imbalance = +10 (survives ring wrap)");
    }
    if (ib >= 0) {
        TEST_ASSERT_EQ((int32_t)(hdr->trace->life_tags[ib].refs -
                                 hdr->trace->life_tags[ib].derefs), -10,
                       "tag B lifetime imbalance = -10 (survives ring wrap)");
    }

    ob_dump_trace(obj);   /* exercises net-balanced + per-tag-imbalanced path */

    ob_disable_type_tracing(t);
    ObDereferenceObject(obj);
}

/* ---- Per-process handle quota (S14) ---- */

static void test_ob_handle_quota(void)
{
    /* Create a type for quota testing */
    static const OBJECT_TYPE quota_tmpl = {
        .name      = "QuotaTest",
        .body_size = 16,
        .on_close  = (void *)0,
        .on_delete = (void *)0,
        .on_open   = (void *)0,
        .on_parse  = (void *)0,
    };
    const OBJECT_TYPE *qtype = ob_create_type(&quota_tmpl);
    TEST_ASSERT(qtype != (void *)0, "ob_create_type for quota test");

    void *obj = ob_alloc_object(qtype);
    TEST_ASSERT(obj != (void *)0, "alloc object for quota test");

    HANDLE_TABLE ht;
    ob_handle_table_init(&ht);

    /* Set a very small limit */
    ob_handle_table_set_limit(&ht, 3);
    TEST_ASSERT_EQ(ht.handle_limit, 3, "handle_limit set to 3");

    /* Allocate 3 handles -- should succeed */
    HANDLE h0 = ObpAllocateHandle(&ht, obj, 0x1F0FFF, 0);
    HANDLE h1 = ObpAllocateHandle(&ht, obj, 0x1F0FFF, 0);
    HANDLE h2 = ObpAllocateHandle(&ht, obj, 0x1F0FFF, 0);
    TEST_ASSERT(h0 >= 0, "handle 0 allocated within quota");
    TEST_ASSERT(h1 >= 0, "handle 1 allocated within quota");
    TEST_ASSERT(h2 >= 0, "handle 2 allocated within quota");

    /* 4th allocation should fail -- quota exceeded */
    HANDLE h3 = ObpAllocateHandle(&ht, obj, 0x1F0FFF, 0);
    TEST_ASSERT_EQ(h3, INVALID_HANDLE_VALUE,
                   "4th handle denied by quota (3/3)");
    /* One-shot exhaustion warning armed on first denial. */
    TEST_ASSERT_EQ(ht.quota_warned, 1, "quota_warned set on first exhaustion");
    /* A second denial does not re-arm (stays 1, no per-denial log flood). */
    (void)ObpAllocateHandle(&ht, obj, 0x1F0FFF, 0);
    TEST_ASSERT_EQ(ht.quota_warned, 1, "quota_warned stays one-shot on repeat denial");

    /* Free 1 handle and retry -- should succeed, and re-arm the warning. */
    ObpFreeHandle(&ht, h0);
    TEST_ASSERT_EQ(ht.quota_warned, 0, "quota_warned re-armed once back below limit");
    HANDLE h4 = ObpAllocateHandle(&ht, obj, 0x1F0FFF, 0);
    TEST_ASSERT(h4 >= 0, "handle succeeds after freeing one within quota");

    /* Clamp to absolute max */
    ob_handle_table_set_limit(&ht, HANDLE_TABLE_ABSOLUTE_MAX + 1000);
    TEST_ASSERT_EQ(ht.handle_limit, HANDLE_TABLE_ABSOLUTE_MAX,
                   "handle_limit clamped to ABSOLUTE_MAX");

    /* Cleanup */
    ObpFreeHandle(&ht, h1);
    ObpFreeHandle(&ht, h2);
    ObpFreeHandle(&ht, h4);
    ObDereferenceObject(obj);
    ob_handle_table_destroy(&ht);
}

/* ---- S14: the quota (not the old fixed 4096 cap) governs table growth ---- */

static void test_ob_handle_quota_grows_past_old_cap(void)
{
    static const OBJECT_TYPE g_tmpl = {
        .name = "QuotaGrow", .body_size = 16,
        .on_close = (void *)0, .on_delete = (void *)0,
        .on_open = (void *)0, .on_parse = (void *)0,
    };
    const OBJECT_TYPE *t = ob_create_type(&g_tmpl);
    TEST_ASSERT(t != (void *)0, "ob_create_type for quota-grow");
    if (!t) return;

    void *obj = ob_alloc_object(t);
    TEST_ASSERT(obj != (void *)0, "alloc object for quota-grow");
    if (!obj) return;

    HANDLE_TABLE ht;
    ob_handle_table_init(&ht);
    /* Default limit (16384) is well above the retired 4096 grow cap. Allocate
     * 4097 handles to a single object: the 4097th can only succeed if the table
     * grew past the old HANDLE_TABLE_MAX_CAP (4096) -- i.e. the quota, not a
     * fixed array cap, now governs how large the table becomes. */
    enum { N = 4097 };
    HANDLE *hs = (HANDLE *)kmalloc(sizeof(HANDLE) * N);
    TEST_ASSERT(hs != (void *)0, "scratch handle array allocated");
    if (!hs) { ObDereferenceObject(obj); ob_handle_table_destroy(&ht); return; }

    int all_ok = 1;
    uint32_t got = 0;
    for (uint32_t i = 0; i < N; i++) {
        hs[i] = ObpAllocateHandle(&ht, obj, 0x1, 0);
        if (hs[i] < 0) { all_ok = 0; break; }
        got++;
    }
    TEST_ASSERT(all_ok, "all 4097 handles allocated (table grew past old 4096 cap)");
    TEST_ASSERT(ht.capacity > 4096, "table capacity grew beyond the retired 4096 cap");

    for (uint32_t i = 0; i < got; i++)
        ObpFreeHandle(&ht, hs[i]);
    kfree(hs);

    /* Exact reachability at a non-power-of-2 limit: with slot 0 reserved, a quota
     * of N must yield exactly N handles (the grow ceiling is N+1, not 2*cap). */
    HANDLE_TABLE et;
    ob_handle_table_init(&et);
    ob_handle_table_set_limit(&et, 100);
    uint32_t ok = 0;
    for (uint32_t i = 0; i < 100; i++) {
        HANDLE h = ObpAllocateHandle(&et, obj, 0x1, 0);
        if (h < 0) break;
        ok++;
    }
    TEST_ASSERT_EQ(ok, 100, "all 100 handles reachable at a non-power-of-2 limit");
    HANDLE over = ObpAllocateHandle(&et, obj, 0x1, 0);
    TEST_ASSERT_EQ(over, INVALID_HANDLE_VALUE, "101st denied (quota exactly 100)");
    TEST_ASSERT_EQ(et.quota_warned, 1, "quota_warned armed at exhaustion");
    /* Raising the limit re-arms the one-shot so a later episode is reported. */
    ob_handle_table_set_limit(&et, 200);
    TEST_ASSERT_EQ(et.quota_warned, 0, "set_limit re-arms quota_warned");
    ob_handle_table_destroy(&et);

    /* Zero-limit sentinel: HANDLE_TABLE_LIMIT_UNLIMITED disables the quota. */
    ob_handle_table_set_limit(&ht, HANDLE_TABLE_LIMIT_UNLIMITED);
    TEST_ASSERT_EQ(ht.handle_limit, 0, "set_limit(UNLIMITED) leaves handle_limit 0");
    HANDLE u0 = ObpAllocateHandle(&ht, obj, 0x1, 0);
    HANDLE u1 = ObpAllocateHandle(&ht, obj, 0x1, 0);
    TEST_ASSERT(u0 >= 0 && u1 >= 0, "unlimited (0) quota does not deny allocations");
    if (u0 >= 0) ObpFreeHandle(&ht, u0);
    if (u1 >= 0) ObpFreeHandle(&ht, u1);

    ObDereferenceObject(obj);
    ob_handle_table_destroy(&ht);
}

/* ============================================================================
 * NT namespace syscall tests (SSDT dispatch path)
 * ============================================================================ */

#include "kernel/nt/ssdt.h"
#include "kernel/nt/ntstatus.h"
#include "kernel/nt/service_numbers.h"
#include "kernel/nt/nt_types.h"
#include "kernel/sched/task.h"

/* Build OBJECT_ATTRIBUTES + UNICODE_STRING for an NT namespace path */
static void s17_build_oa(OBJECT_ATTRIBUTES *oa, UNICODE_STRING *us,
                         const char *path)
{
    uint32_t len = 0;
    while (path[len]) len++;
    us->Buffer = (uint16_t *)(uintptr_t)path;  /* ASCII via uint16_t* cast */
    us->Length = (uint16_t)len;
    us->MaximumLength = (uint16_t)(len + 1);
    oa->Length = sizeof(OBJECT_ATTRIBUTES);
    oa->RootDirectory = INVALID_HANDLE_VALUE;
    oa->_pad1 = 0;
    oa->ObjectName = us;
    oa->Attributes = OBJ_CASE_INSENSITIVE;
    oa->_pad2 = 0;
    oa->SecurityDescriptor = (void *)0;
    oa->SecurityQualityOfService = (void *)0;
}

/* -9 LEAK retrofit helper: unlink a named object from \BaseNamedObjects.
 *
 * NtClose drops the creator's handle ref, but named objects are pinned in
 * the directory by the OB_FLAG_PERMANENT bit + the dir entry's own ref on
 * the body. ObMakeTemporaryObject clears PERMANENT; ObpRemoveFromDirectory
 * unlinks the OBJECT_DIRECTORY_ENTRY node, frees it, and drops the dir's
 * ref on the body. Callers pass the exact OBJECT_TYPE so ObLookupObjectByName
 * never follows a symlink (would return the target instead of the link body).
 */
static void test_ob_cleanup_named(const char *leaf,
                                  const OBJECT_TYPE *type)
{
    char path[128];
    void *body = NULL;
    void *bno = NULL;

    snprintf(path, sizeof(path), "\\BaseNamedObjects\\%s", leaf);
    if (ObLookupObjectByName(path, type, 0, &body) != 0 || !body)
        return;
    ObMakeTemporaryObject(body);
    if (ObLookupObjectByName("\\BaseNamedObjects", ObpDirectoryType, 0,
                             &bno) == 0 && bno) {
        ObpRemoveFromDirectory(bno, body);
        ObDereferenceObject(bno);
    }
    ObDereferenceObject(body);
}

/* Test: NtCreateDirectoryObject + NtOpenDirectoryObject round-trip */
static void test_nt_create_open_directory(void)
{
    OBJECT_ATTRIBUTES oa;
    UNICODE_STRING us;
    HANDLE create_h = INVALID_HANDLE_VALUE;
    HANDLE open_h = INVALID_HANDLE_VALUE;
    NTSTATUS status;

    s17_build_oa(&oa, &us, "\\BaseNamedObjects\\NtDirTest");

    status = ssdt_dispatch(SSDT_NtCreateDirectoryObject,
                           (uint64_t)(uintptr_t)&create_h,
                           0, (uint64_t)(uintptr_t)&oa, 0, 0, 0);
    TEST_ASSERT(NT_SUCCESS(status), "NtCreateDirectoryObject succeeds");
    TEST_ASSERT(create_h != INVALID_HANDLE_VALUE,
                "NtCreateDirectoryObject returns valid handle");

    /* Open the just-created directory */
    status = ssdt_dispatch(SSDT_NtOpenDirectoryObject,
                           (uint64_t)(uintptr_t)&open_h,
                           0, (uint64_t)(uintptr_t)&oa, 0, 0, 0);
    TEST_ASSERT(NT_SUCCESS(status), "NtOpenDirectoryObject succeeds");
    TEST_ASSERT(open_h != INVALID_HANDLE_VALUE,
                "NtOpenDirectoryObject returns valid handle");

    /* Re-create with same path -> STATUS_OBJECT_NAME_COLLISION */
    {
        HANDLE dup_h = INVALID_HANDLE_VALUE;
        status = ssdt_dispatch(SSDT_NtCreateDirectoryObject,
                               (uint64_t)(uintptr_t)&dup_h,
                               0, (uint64_t)(uintptr_t)&oa, 0, 0, 0);
        TEST_ASSERT(status == STATUS_OBJECT_NAME_COLLISION,
                    "Duplicate create returns STATUS_OBJECT_NAME_COLLISION");
    }

    /* -9 LEAK retrofit cleanup. Drop both handles then unlink the
     * directory from \BaseNamedObjects so the dir entry + body are freed. */
    ssdt_dispatch(SSDT_NtClose, (uint64_t)create_h, 0, 0, 0, 0, 0);
    ssdt_dispatch(SSDT_NtClose, (uint64_t)open_h, 0, 0, 0, 0, 0);
    test_ob_cleanup_named("NtDirTest", ObpDirectoryType);
}

/* Test: NtOpenDirectoryObject on non-existent path */
static void test_nt_open_directory_not_found(void)
{
    OBJECT_ATTRIBUTES oa;
    UNICODE_STRING us;
    HANDLE h = INVALID_HANDLE_VALUE;
    NTSTATUS status;

    s17_build_oa(&oa, &us, "\\BaseNamedObjects\\NtDirNoSuch12345");
    status = ssdt_dispatch(SSDT_NtOpenDirectoryObject,
                           (uint64_t)(uintptr_t)&h,
                           0, (uint64_t)(uintptr_t)&oa, 0, 0, 0);
    TEST_ASSERT(status == STATUS_OBJECT_NAME_NOT_FOUND,
                "NtOpenDirectoryObject returns OBJECT_NAME_NOT_FOUND");
}

/* Test: NtCreateSymbolicLinkObject + NtOpenSymbolicLinkObject + Query */
static void test_nt_symlink_roundtrip(void)
{
    OBJECT_ATTRIBUTES oa;
    UNICODE_STRING us;
    UNICODE_STRING target_us;
    HANDLE link_h = INVALID_HANDLE_VALUE;
    HANDLE open_h = INVALID_HANDLE_VALUE;
    NTSTATUS status;
    const char *target = "\\BaseNamedObjects";

    s17_build_oa(&oa, &us, "\\BaseNamedObjects\\NtSymLinkTest");
    target_us.Buffer = (uint16_t *)(uintptr_t)target;
    {
        uint16_t tlen = 0;
        while (target[tlen]) tlen++;
        target_us.Length = tlen;
        target_us.MaximumLength = tlen + 1;
    }

    status = ssdt_dispatch(SSDT_NtCreateSymbolicLinkObject,
                           (uint64_t)(uintptr_t)&link_h,
                           0, (uint64_t)(uintptr_t)&oa,
                           (uint64_t)(uintptr_t)&target_us, 0, 0);
    TEST_ASSERT(NT_SUCCESS(status), "NtCreateSymbolicLinkObject succeeds");
    TEST_ASSERT(link_h != INVALID_HANDLE_VALUE,
                "NtCreateSymbolicLinkObject returns valid handle");

    /* Open the link and query target */
    status = ssdt_dispatch(SSDT_NtOpenSymbolicLinkObject,
                           (uint64_t)(uintptr_t)&open_h,
                           0, (uint64_t)(uintptr_t)&oa, 0, 0, 0);
    TEST_ASSERT(NT_SUCCESS(status), "NtOpenSymbolicLinkObject succeeds");

    {
        char target_buf[OB_SYMLINK_MAX];
        UNICODE_STRING out_us;
        uint32_t ret_len = 0;

        out_us.Buffer = (uint16_t *)(uintptr_t)target_buf;
        out_us.Length = 0;
        out_us.MaximumLength = sizeof(target_buf);

        status = ssdt_dispatch(SSDT_NtQuerySymbolicLinkObject,
                               (uint64_t)(uintptr_t)open_h,
                               (uint64_t)(uintptr_t)&out_us,
                               (uint64_t)(uintptr_t)&ret_len, 0, 0, 0);
        TEST_ASSERT(NT_SUCCESS(status), "NtQuerySymbolicLinkObject succeeds");
        TEST_ASSERT(ret_len > 0, "Query returns non-zero length");
        TEST_ASSERT(target_buf[0] == '\\' && target_buf[1] == 'B',
                    "Query returns target starting with \\B");
    }

    /* -9 LEAK retrofit cleanup. Drop handles then unlink. Pass
     * ObpSymlinkType so ObLookupObjectByName returns the link body
     * itself -- without the type hint the lookup would follow the link
     * to \BaseNamedObjects and we'd attempt to remove the root dir. */
    ssdt_dispatch(SSDT_NtClose, (uint64_t)link_h, 0, 0, 0, 0, 0);
    ssdt_dispatch(SSDT_NtClose, (uint64_t)open_h, 0, 0, 0, 0, 0);
    test_ob_cleanup_named("NtSymLinkTest", ObpSymlinkType);
}

/* Test: NtQuerySymbolicLinkObject rejects non-symlink handle */
static void test_nt_query_symlink_wrong_type(void)
{
    OBJECT_ATTRIBUTES oa;
    UNICODE_STRING us;
    UNICODE_STRING out_us;
    char buf[64];
    HANDLE dir_h = INVALID_HANDLE_VALUE;
    NTSTATUS status;
    uint32_t ret_len = 0;

    s17_build_oa(&oa, &us, "\\BaseNamedObjects");
    status = ssdt_dispatch(SSDT_NtOpenDirectoryObject,
                           (uint64_t)(uintptr_t)&dir_h,
                           0, (uint64_t)(uintptr_t)&oa, 0, 0, 0);
    if (!NT_SUCCESS(status)) return;

    out_us.Buffer = (uint16_t *)(uintptr_t)buf;
    out_us.Length = 0;
    out_us.MaximumLength = sizeof(buf);

    status = ssdt_dispatch(SSDT_NtQuerySymbolicLinkObject,
                           (uint64_t)(uintptr_t)dir_h,
                           (uint64_t)(uintptr_t)&out_us,
                           (uint64_t)(uintptr_t)&ret_len, 0, 0, 0);
    TEST_ASSERT(status == STATUS_OBJECT_TYPE_MISMATCH,
                "NtQuerySymbolicLinkObject rejects directory handle");
}

/* Test: a symbolic-link cycle cannot exhaust the kernel stack -- the cumulative
 * OB_SYMLINK_DEPTH budget bounds resolution. CycleA -> ...\CycleB and
 * CycleB -> ...\CycleA; looking up a path THROUGH CycleA must RETURN failure
 * (not hang / stack-overflow). The test completing at all is the proof. */
static void test_ob_symlink_cycle_bounded(void)
{
    void *bno = NULL;
    void *sa, *sb, *result = NULL;
    int rc;

    if (ObLookupObjectByName("\\BaseNamedObjects", ObpDirectoryType, 0, &bno) != 0
        || !bno) {
        TEST_SKIP("BaseNamedObjects root not available for symlink-cycle test");
        return;
    }

    sa = ob_ns_create_symlink("\\BaseNamedObjects\\ObCycleB");
    sb = ob_ns_create_symlink("\\BaseNamedObjects\\ObCycleA");
    TEST_ASSERT(sa && sb, "two cycle symlinks allocated");
    if (!sa || !sb) { ObDereferenceObject(bno); return; }

    TEST_ASSERT(ObInsertObject(sa, "ObCycleA", bno) == 0, "ObCycleA inserted");
    TEST_ASSERT(ObInsertObject(sb, "ObCycleB", bno) == 0, "ObCycleB inserted");
    /* ObInsertObject took the directory's reference; drop our creation refs so
     * the cleanup unlink frees the symlink bodies (no leak). */
    ObDereferenceObject(sa);
    ObDereferenceObject(sb);

    /* Resolve a path that walks INTO the cycle (trailing component forces the
     * symlinks to be followed, not returned as leaves). Must return non-zero. */
    rc = ObLookupObjectByName("\\BaseNamedObjects\\ObCycleA\\Leaf", NULL, 0, &result);
    TEST_ASSERT(rc != 0, "symlink cycle lookup fails instead of recursing forever");

    /* Cleanup: clear the flag so the named symlinks can be removed + freed. */
    test_ob_cleanup_named("ObCycleA", ObpSymlinkType);
    test_ob_cleanup_named("ObCycleB", ObpSymlinkType);
    ObDereferenceObject(bno);
}

/* Test: all 6 SSDT slots are registered (not stubs) */
static void test_nt_namespace_ssdt_registered(void)
{
    const SSDT_TABLE *tbl = ssdt_get_table(SSDT_TABLE_MAIN);
    extern NTSTATUS ssdt_stub_not_implemented(uint64_t, uint64_t, uint64_t,
                                              uint64_t, uint64_t, uint64_t);
    static const struct { uint32_t svc; const char *name; } slots[] = {
        { SSDT_NtCreateDirectoryObject,  "NtCreateDirectoryObject" },
        { SSDT_NtOpenDirectoryObject,    "NtOpenDirectoryObject" },
        { SSDT_NtQueryDirectoryObject,   "NtQueryDirectoryObject" },
        { SSDT_NtCreateSymbolicLinkObject, "NtCreateSymbolicLinkObject" },
        { SSDT_NtOpenSymbolicLinkObject, "NtOpenSymbolicLinkObject" },
        { SSDT_NtQuerySymbolicLinkObject, "NtQuerySymbolicLinkObject" },
    };
    uint32_t i;
    char msg[96];
    TEST_ASSERT(tbl != (const SSDT_TABLE *)0, "SSDT main table exists");
    if (!tbl) return;
    for (i = 0; i < sizeof(slots) / sizeof(slots[0]); i++) {
        uint32_t idx = slots[i].svc & 0xFFF;
        snprintf(msg, sizeof(msg), "%s (0x%x) registered",
                 slots[i].name, (uint64_t)slots[i].svc);
        TEST_ASSERT(tbl->handlers[idx] != ssdt_stub_not_implemented, msg);
    }
}


/* ============================================================================
 * NT section / mapped file syscall tests
 * ============================================================================ */

#include "kernel/fs/vfs.h"
#include "kernel/ob/ob_file.h"
#include "kernel/ob/ob_section.h"
#include "kernel/nt/nt_section.h"
#include "kernel/nt/nt_memory.h"

#define NTSTA_NOT_MAPPED_VIEW ((NTSTATUS)0xC0000019)

static void test_nt_section_anon_roundtrip(void)
{
    HANDLE sh = INVALID_HANDLE_VALUE;
    void *base = (void *)0;
    NTSTATUS st;
    volatile uint8_t *mp;

    st = ssdt_dispatch(SSDT_NtCreateSection,
                       (uint64_t)(uintptr_t)&sh,
                       (uint64_t)SECTION_ALL_ACCESS,
                       0,
                       4096,
                       (uint64_t)PAGE_READWRITE | ((uint64_t)SEC_COMMIT << 32),
                       0);
    TEST_ASSERT(NT_SUCCESS(st), "NtCreateSection SEC_COMMIT anon");
    TEST_ASSERT(sh != INVALID_HANDLE_VALUE, "section handle valid");

    st = ssdt_dispatch(SSDT_NtMapViewOfSection,
                       (uint64_t)sh,
                       (uint64_t)CURRENT_PROCESS,
                       (uint64_t)(uintptr_t)&base,
                       0, 0, 0);
    TEST_ASSERT(NT_SUCCESS(st), "NtMapViewOfSection anon");
    TEST_ASSERT(base != (void *)0, "mapped base non-NULL");

    mp = (volatile uint8_t *)base;
    mp[0] = 0x5A;
    TEST_ASSERT(mp[0] == 0x5A, "mapped write visible");

    st = ssdt_dispatch(SSDT_NtUnmapViewOfSection,
                       (uint64_t)CURRENT_PROCESS, (uint64_t)(uintptr_t)base,
                       0, 0, 0, 0);
    TEST_ASSERT(NT_SUCCESS(st), "NtUnmapViewOfSection anon");

    st = ssdt_dispatch(SSDT_NtClose, (uint64_t)sh, 0, 0, 0, 0, 0);
    TEST_ASSERT(NT_SUCCESS(st), "NtClose section");
}

static void test_nt_section_file_backed(void)
{
    const char *path = "C:\\Impossible\\test_section_ssdt.txt";
    const char payload[] = "HELLO";
    HANDLE fh;
    HANDLE sh = INVALID_HANDLE_VALUE;
    void *base = (void *)0;
    NTSTATUS st;
    uint8_t *rb;

    if (vfs_create(path, VFS_FILE) != 0) {
        TEST_SKIP("vfs_create failed (C: not mounted?)");
        return;
    }
    {
        struct vfs_node *n = vfs_open(path, VFS_O_WRITE | VFS_O_READ);
        if (!n) {
            vfs_unlink(path);
            TEST_SKIP("vfs_open failed for section file test");
            return;
        }
        vfs_write(n, 0, (uint32_t)sizeof(payload) - 1u,
                  (const uint8_t *)payload);
        vfs_close(n);
    }

    fh = ob_create_file_handle(path, VFS_O_READ);
    if (fh == INVALID_HANDLE_VALUE) {
        vfs_unlink(path);
        TEST_SKIP("ob_create_file_handle failed for section file test");
        return;
    }

    st = ssdt_dispatch(SSDT_NtCreateSection,
                       (uint64_t)(uintptr_t)&sh,
                       (uint64_t)SECTION_ALL_ACCESS,
                       0,
                       0,
                       (uint64_t)PAGE_READONLY | ((uint64_t)SEC_COMMIT << 32),
                       (uint64_t)fh);
    TEST_ASSERT(NT_SUCCESS(st), "NtCreateSection file-backed");
    st = ssdt_dispatch(SSDT_NtMapViewOfSection,
                       (uint64_t)sh,
                       (uint64_t)CURRENT_PROCESS,
                       (uint64_t)(uintptr_t)&base,
                       0, 0, 0);
    TEST_ASSERT(NT_SUCCESS(st), "NtMapViewOfSection file-backed");
    rb = (uint8_t *)base;
    TEST_ASSERT(rb[0] == 'H' && rb[4] == 'O', "file bytes visible in view");

    ssdt_dispatch(SSDT_NtUnmapViewOfSection,
                  (uint64_t)CURRENT_PROCESS, (uint64_t)(uintptr_t)base,
                  0, 0, 0, 0);
    ssdt_dispatch(SSDT_NtClose, (uint64_t)sh, 0, 0, 0, 0, 0);
    ssdt_dispatch(SSDT_NtClose, (uint64_t)fh, 0, 0, 0, 0, 0);
    vfs_unlink(path);
}

static void test_nt_section_named_open_query_extend(void)
{
    OBJECT_ATTRIBUTES oa;
    UNICODE_STRING us;
    HANDLE cr = INVALID_HANDLE_VALUE;
    HANDLE op = INVALID_HANDLE_VALUE;
    void *base = (void *)0;
    NTSTATUS st;
    SECTION_BASIC_INFORMATION sbi;
    uint64_t ret_len = 0;
    LARGE_INTEGER grow;

    s17_build_oa(&oa, &us, "SectNamedX1");

    st = ssdt_dispatch(SSDT_NtCreateSection,
                       (uint64_t)(uintptr_t)&cr,
                       (uint64_t)SECTION_ALL_ACCESS,
                       (uint64_t)(uintptr_t)&oa,
                       4096,
                       (uint64_t)PAGE_READWRITE | ((uint64_t)SEC_COMMIT << 32),
                       0);
    TEST_ASSERT(NT_SUCCESS(st), "NtCreateSection named");

    s17_build_oa(&oa, &us, "SectNamedX1");
    st = ssdt_dispatch(SSDT_NtOpenSection,
                       (uint64_t)(uintptr_t)&op,
                       (uint64_t)SECTION_ALL_ACCESS,
                       (uint64_t)(uintptr_t)&oa, 0, 0, 0);
    TEST_ASSERT(NT_SUCCESS(st), "NtOpenSection named");
    TEST_ASSERT(op != INVALID_HANDLE_VALUE, "open handle valid");

    st = ssdt_dispatch(SSDT_NtQuerySection,
                       (uint64_t)op,
                       SectionBasicInformation,
                       (uint64_t)(uintptr_t)&sbi,
                       sizeof(sbi),
                       (uint64_t)(uintptr_t)&ret_len, 0);
    TEST_ASSERT(NT_SUCCESS(st), "NtQuerySection basic");
    TEST_ASSERT(sbi.MaximumSize.QuadPart == 4096, "query max size");

    grow.QuadPart = 8192;
    st = ssdt_dispatch(SSDT_NtExtendSection, (uint64_t)op,
                       (uint64_t)(uintptr_t)&grow, 0, 0, 0, 0);
    TEST_ASSERT(NT_SUCCESS(st), "NtExtendSection");

    st = ssdt_dispatch(SSDT_NtMapViewOfSection,
                       (uint64_t)op,
                       (uint64_t)CURRENT_PROCESS,
                       (uint64_t)(uintptr_t)&base,
                       0, 0, 0);
    TEST_ASSERT(NT_SUCCESS(st), "map after extend");

    st = ssdt_dispatch(SSDT_NtUnmapViewOfSection,
                       (uint64_t)CURRENT_PROCESS, (uint64_t)(uintptr_t)base,
                       0, 0, 0, 0);
    TEST_ASSERT(NT_SUCCESS(st), "unmap named");

    st = ssdt_dispatch(SSDT_NtClose, (uint64_t)op, 0, 0, 0, 0, 0);
    TEST_ASSERT(NT_SUCCESS(st), "NtClose open section");
    st = ssdt_dispatch(SSDT_NtClose, (uint64_t)cr, 0, 0, 0, 0, 0);
    TEST_ASSERT(NT_SUCCESS(st), "NtClose create section");

    /* -9 LEAK retrofit cleanup -- unlink from \BaseNamedObjects. */
    test_ob_cleanup_named("SectNamedX1", ObpSectionType);
}

static void test_nt_section_unmap_bad_base(void)
{
    NTSTATUS st = ssdt_dispatch(SSDT_NtUnmapViewOfSection,
                                (uint64_t)CURRENT_PROCESS,
                                (uint64_t)(uintptr_t)0x12345000,
                                0, 0, 0, 0);
    TEST_ASSERT(st == NTSTA_NOT_MAPPED_VIEW,
                "NtUnmapViewOfSection bad base");
}

static void test_nt_section_ssdt_registered(void)
{
    const SSDT_TABLE *tbl = ssdt_get_table(SSDT_TABLE_MAIN);
    extern NTSTATUS ssdt_stub_not_implemented(uint64_t, uint64_t, uint64_t,
                                              uint64_t, uint64_t, uint64_t);
    struct { uint32_t svc; const char *name; } slots[] = {
        { SSDT_NtCreateSection,         "NtCreateSection" },
        { SSDT_NtOpenSection,           "NtOpenSection" },
        { SSDT_NtMapViewOfSection,      "NtMapViewOfSection" },
        { SSDT_NtUnmapViewOfSection,    "NtUnmapViewOfSection" },
        { SSDT_NtExtendSection,         "NtExtendSection" },
        { SSDT_NtQuerySection,          "NtQuerySection" },
        { SSDT_NtAreMappedFilesTheSame, "NtAreMappedFilesTheSame" },
    };
    uint32_t i;
    char msg[96];
    TEST_ASSERT(tbl != (const SSDT_TABLE *)0, "SSDT main table exists");
    if (!tbl) return;
    for (i = 0; i < sizeof(slots) / sizeof(slots[0]); i++) {
        uint32_t idx = slots[i].svc & 0xFFF;
        snprintf(msg, sizeof(msg), "%s (0x%x) registered",
                 slots[i].name, (uint64_t)slots[i].svc);
        TEST_ASSERT(tbl->handlers[idx] != ssdt_stub_not_implemented, msg);
    }
}

/* ============================================================================
 * NT timer syscall tests
 * ============================================================================ */

#include "kernel/nt/nt_timer.h"
#include "kernel/ob/ob_event.h"
#include "kernel/timer.h"

static void test_nt_timer_create_and_query(void)
{
    HANDLE th = INVALID_HANDLE_VALUE;
    OBJECT_ATTRIBUTES oa;
    UNICODE_STRING us;
    NTSTATUS st;
    TIMER_BASIC_INFORMATION bi;
    uint64_t ret_len = 0;

    s17_build_oa(&oa, &us, "TimerTest19A");

    st = ssdt_dispatch(SSDT_NtCreateTimer,
                       (uint64_t)(uintptr_t)&th,
                       (uint64_t)0,  /* access */
                       (uint64_t)(uintptr_t)&oa,
                       (uint64_t)TIMER_TYPE_NOTIFICATION,
                       0, 0);
    TEST_ASSERT(NT_SUCCESS(st), "NtCreateTimer");
    TEST_ASSERT(th != INVALID_HANDLE_VALUE, "timer handle valid");

    /* Query before arm: TimerState should be 1 (signalled/not pending). */
    st = ssdt_dispatch(SSDT_NtQueryTimer,
                       (uint64_t)th,
                       (uint64_t)TimerBasicInformation,
                       (uint64_t)(uintptr_t)&bi,
                       sizeof(bi),
                       (uint64_t)(uintptr_t)&ret_len, 0);
    TEST_ASSERT(NT_SUCCESS(st), "NtQueryTimer basic");
    TEST_ASSERT(ret_len == sizeof(bi), "ret_len == sizeof(TIMER_BASIC_INFORMATION)");
    TEST_ASSERT(bi.TimerState == 1, "unarmed timer reports TimerState=1");
    TEST_ASSERT(bi.RemainingTime.QuadPart == 0, "unarmed RemainingTime==0");

    ssdt_dispatch(SSDT_NtClose, (uint64_t)th, 0, 0, 0, 0, 0);
    /* -9 LEAK retrofit. */
    test_ob_cleanup_named("TimerTest19A", ObpTimerType);
}

static void test_nt_timer_set_cancel(void)
{
    HANDLE th = INVALID_HANDLE_VALUE;
    OBJECT_ATTRIBUTES oa;
    UNICODE_STRING us;
    NTSTATUS st;
    LARGE_INTEGER due;
    uint8_t prev_state = 0xFF;
    uint8_t current_state = 0xFF;

    s17_build_oa(&oa, &us, "TimerTest19B");

    st = ssdt_dispatch(SSDT_NtCreateTimer,
                       (uint64_t)(uintptr_t)&th,
                       (uint64_t)0,
                       (uint64_t)(uintptr_t)&oa,
                       (uint64_t)TIMER_TYPE_NOTIFICATION,
                       0, 0);
    TEST_ASSERT(NT_SUCCESS(st), "NtCreateTimer set/cancel");

    /* Arm for 1 second in the future: -10_000_000 (100-ns units). */
    due.QuadPart = -(int64_t)10000000;
    st = ssdt_dispatch(SSDT_NtSetTimer,
                       (uint64_t)th,
                       (uint64_t)(uintptr_t)&due,
                       0, 0,  /* ApcRoutine, ApcContext */
                       NT_SETTIMER_PACK(0, 0),  /* Period=0, Resume=0 */
                       (uint64_t)(uintptr_t)&prev_state);
    TEST_ASSERT(NT_SUCCESS(st), "NtSetTimer");
    TEST_ASSERT(prev_state == 0, "first set: previous state 0");

    /* Arm again: previous state should now be 1 (armed). */
    due.QuadPart = -(int64_t)20000000;
    st = ssdt_dispatch(SSDT_NtSetTimer,
                       (uint64_t)th,
                       (uint64_t)(uintptr_t)&due,
                       0, 0,
                       NT_SETTIMER_PACK(0, 0),
                       (uint64_t)(uintptr_t)&prev_state);
    TEST_ASSERT(NT_SUCCESS(st), "NtSetTimer re-arm");
    TEST_ASSERT(prev_state == 1, "re-arm: previous state 1");

    /* Cancel: current state should be 1 (was armed). */
    st = ssdt_dispatch(SSDT_NtCancelTimer,
                       (uint64_t)th,
                       (uint64_t)(uintptr_t)&current_state,
                       0, 0, 0, 0);
    TEST_ASSERT(NT_SUCCESS(st), "NtCancelTimer");
    TEST_ASSERT(current_state == 1, "cancel: current state 1");

    /* Cancel again: should now report 0 (not armed). */
    current_state = 0xFF;
    st = ssdt_dispatch(SSDT_NtCancelTimer,
                       (uint64_t)th,
                       (uint64_t)(uintptr_t)&current_state,
                       0, 0, 0, 0);
    TEST_ASSERT(NT_SUCCESS(st), "NtCancelTimer idempotent");
    TEST_ASSERT(current_state == 0, "second cancel: current state 0");

    ssdt_dispatch(SSDT_NtClose, (uint64_t)th, 0, 0, 0, 0, 0);
    /* -9 LEAK retrofit. */
    test_ob_cleanup_named("TimerTest19B", ObpTimerType);
}

static void test_nt_timer_open_existing(void)
{
    HANDLE ch = INVALID_HANDLE_VALUE;
    HANDLE oh = INVALID_HANDLE_VALUE;
    OBJECT_ATTRIBUTES oa;
    UNICODE_STRING us;
    NTSTATUS st;

    s17_build_oa(&oa, &us, "TimerTest19C");
    st = ssdt_dispatch(SSDT_NtCreateTimer,
                       (uint64_t)(uintptr_t)&ch,
                       0,
                       (uint64_t)(uintptr_t)&oa,
                       (uint64_t)TIMER_TYPE_SYNCHRONIZATION,
                       0, 0);
    TEST_ASSERT(NT_SUCCESS(st), "NtCreateTimer (named)");

    s17_build_oa(&oa, &us, "TimerTest19C");
    st = ssdt_dispatch(SSDT_NtOpenTimer,
                       (uint64_t)(uintptr_t)&oh,
                       0,
                       (uint64_t)(uintptr_t)&oa,
                       0, 0, 0);
    TEST_ASSERT(NT_SUCCESS(st), "NtOpenTimer");
    TEST_ASSERT(oh != INVALID_HANDLE_VALUE, "open handle valid");

    ssdt_dispatch(SSDT_NtClose, (uint64_t)oh, 0, 0, 0, 0, 0);
    ssdt_dispatch(SSDT_NtClose, (uint64_t)ch, 0, 0, 0, 0, 0);
    /* -9 LEAK retrofit. */
    test_ob_cleanup_named("TimerTest19C", ObpTimerType);
}

static void test_nt_timer_wrong_type(void)
{
    /* Attempt to query an event handle (wrong object type) via NtQueryTimer. */
    HANDLE eh = NtCreateEvent(&task_current()->handle_table, NULL,
                              EVENT_MANUAL_RESET, 0);
    TIMER_BASIC_INFORMATION bi;
    NTSTATUS st;

    TEST_ASSERT(eh != INVALID_HANDLE_VALUE, "NtCreateEvent precondition");

    st = ssdt_dispatch(SSDT_NtQueryTimer,
                       (uint64_t)eh,
                       (uint64_t)TimerBasicInformation,
                       (uint64_t)(uintptr_t)&bi,
                       sizeof(bi),
                       0, 0);
    TEST_ASSERT(st == STATUS_OBJECT_TYPE_MISMATCH,
                "NtQueryTimer on event -> OBJECT_TYPE_MISMATCH");

    ssdt_dispatch(SSDT_NtClose, (uint64_t)eh, 0, 0, 0, 0, 0);
}

static void test_nt_timer_ssdt_registered(void)
{
    const SSDT_TABLE *tbl = ssdt_get_table(SSDT_TABLE_MAIN);
    extern NTSTATUS ssdt_stub_not_implemented(uint64_t, uint64_t, uint64_t,
                                              uint64_t, uint64_t, uint64_t);
    struct { uint32_t svc; const char *name; } slots[] = {
        { SSDT_NtCreateTimer, "NtCreateTimer" },
        { SSDT_NtOpenTimer,   "NtOpenTimer" },
        { SSDT_NtSetTimer,    "NtSetTimer" },
        { SSDT_NtCancelTimer, "NtCancelTimer" },
        { SSDT_NtQueryTimer,  "NtQueryTimer" },
        { SSDT_NtSetTimerEx,  "NtSetTimerEx" },
    };
    uint32_t i;
    char msg[96];
    TEST_ASSERT(tbl != (const SSDT_TABLE *)0, "SSDT main table exists");
    if (!tbl) return;
    for (i = 0; i < sizeof(slots) / sizeof(slots[0]); i++) {
        uint32_t idx = slots[i].svc & 0xFFF;
        snprintf(msg, sizeof(msg), "%s (0x%x) registered",
                 slots[i].name, (uint64_t)slots[i].svc);
        TEST_ASSERT(tbl->handlers[idx] != ssdt_stub_not_implemented, msg);
    }
}

/* ============================================================================
 * NT legacy LPC SSDT stub-wiring tests
 * ============================================================================ */

#include "kernel/nt/nt_lpc.h"

/* Shared LPC slot table -- used by both registration and dispatch tests so
 * the two stay in lockstep. Named so each assertion message identifies
 * exactly which syscall is being exercised. */
static const struct { uint32_t svc; const char *name; } s_lpc_slots[] = {
    { SSDT_NtCreatePort,              "NtCreatePort" },
    { SSDT_NtCreateWaitablePort,      "NtCreateWaitablePort" },
    { SSDT_NtConnectPort,             "NtConnectPort" },
    { SSDT_NtSecureConnectPort,       "NtSecureConnectPort" },
    { SSDT_NtAcceptConnectPort,       "NtAcceptConnectPort" },
    { SSDT_NtCompleteConnectPort,     "NtCompleteConnectPort" },
    { SSDT_NtListenPort,              "NtListenPort" },
    { SSDT_NtReplyPort,               "NtReplyPort" },
    { SSDT_NtReplyWaitReceivePort,    "NtReplyWaitReceivePort" },
    { SSDT_NtReplyWaitReceivePortEx,  "NtReplyWaitReceivePortEx" },
    { SSDT_NtRequestPort,             "NtRequestPort" },
    { SSDT_NtRequestWaitReplyPort,    "NtRequestWaitReplyPort" },
    { SSDT_NtImpersonateClientOfPort, "NtImpersonateClientOfPort" },
    { SSDT_NtReadRequestData,         "NtReadRequestData" },
    { SSDT_NtWriteRequestData,        "NtWriteRequestData" },
};

static void test_nt_lpc_slots_registered(void)
{
    const SSDT_TABLE *tbl = ssdt_get_table(SSDT_TABLE_MAIN);
    extern NTSTATUS ssdt_stub_not_implemented(uint64_t, uint64_t, uint64_t,
                                              uint64_t, uint64_t, uint64_t);
    uint32_t i;
    char msg[96];
    TEST_ASSERT(tbl != (const SSDT_TABLE *)0, "SSDT main table exists");
    if (!tbl) return;
    for (i = 0; i < sizeof(s_lpc_slots) / sizeof(s_lpc_slots[0]); i++) {
        uint32_t idx = s_lpc_slots[i].svc & 0xFFF;
        snprintf(msg, sizeof(msg), "%s (0x%x) registered",
                 s_lpc_slots[i].name, (uint64_t)s_lpc_slots[i].svc);
        TEST_ASSERT(tbl->handlers[idx] != ssdt_stub_not_implemented, msg);
    }
}

static void test_nt_lpc_pending_features(void)
{
    /* Each LPC slot is reserved but the underlying engine is not yet
     * implemented. TEST_PENDING bumps the pending counter so the
     * end-of-run summary shows total reserved-but-unimplemented at a
     * glance. A break here (slot returns something other than the
     * deferred status) means mis-registration -- logged as FAIL.
     *
     * Message format: name + slot + brief gap. Suite name "NT LPC
     * pending features" already says it is an LPC stub. NO TODO refs
     * in the runtime message: they drift, the source comment above
     * is the durable record, and shorter messages reduce klog rate-
     * limit pressure during the 31-slot LPC+ALPC pending sweep. */
    uint32_t i;
    char msg[64];
    for (i = 0; i < sizeof(s_lpc_slots) / sizeof(s_lpc_slots[0]); i++) {
        NTSTATUS st = ssdt_dispatch(s_lpc_slots[i].svc, 0, 0, 0, 0, 0, 0);
        snprintf(msg, sizeof(msg), "%s (0x%x): no LPC engine yet",
                 s_lpc_slots[i].name, (uint64_t)s_lpc_slots[i].svc);
        TEST_PENDING(st == STATUS_NOT_IMPLEMENTED, msg);
    }
}

/* ============================================================================
 * NT modern ALPC SSDT stub-wiring tests
 * ============================================================================ */

#include "kernel/nt/nt_alpc.h"
#include "kernel/pe.h"

static void test_pe_ntdll_exports_sorted(void)
{
    /* pe_lookup_export() binary-searches each DLL export table, which is
     * correct only if the table is strictly sorted by name. A single
     * out-of-order entry silently breaks import resolution from that
     * point forward. This test scans all tables every boot. */
    int v = pe_exports_sorted_check();
    TEST_ASSERT(v == 0, "PE DLL export tables strictly sorted by name");
}

/* Shared ALPC slot table -- same pattern as s_lpc_slots above. */
static const struct { uint32_t svc; const char *name; } s_alpc_slots[] = {
    { SSDT_NtAlpcCreatePort,             "NtAlpcCreatePort" },
    { SSDT_NtAlpcConnectPort,            "NtAlpcConnectPort" },
    { SSDT_NtAlpcConnectPortEx,          "NtAlpcConnectPortEx" },
    { SSDT_NtAlpcAcceptConnectPort,      "NtAlpcAcceptConnectPort" },
    { SSDT_NtAlpcSendWaitReceivePort,    "NtAlpcSendWaitReceivePort" },
    { SSDT_NtAlpcDisconnectPort,         "NtAlpcDisconnectPort" },
    { SSDT_NtAlpcCancelMessage,          "NtAlpcCancelMessage" },
    { SSDT_NtAlpcCreatePortSection,      "NtAlpcCreatePortSection" },
    { SSDT_NtAlpcDeletePortSection,      "NtAlpcDeletePortSection" },
    { SSDT_NtAlpcCreateSectionView,      "NtAlpcCreateSectionView" },
    { SSDT_NtAlpcDeleteSectionView,      "NtAlpcDeleteSectionView" },
    { SSDT_NtAlpcCreateResourceReserve,  "NtAlpcCreateResourceReserve" },
    { SSDT_NtAlpcDeleteResourceReserve,  "NtAlpcDeleteResourceReserve" },
    { SSDT_NtAlpcQueryInformation,       "NtAlpcQueryInformation" },
    { SSDT_NtAlpcSetInformation,         "NtAlpcSetInformation" },
    { SSDT_NtAlpcQueryInformationMessage, "NtAlpcQueryInformationMessage" },
};

static void test_nt_alpc_slots_registered(void)
{
    const SSDT_TABLE *tbl = ssdt_get_table(SSDT_TABLE_MAIN);
    extern NTSTATUS ssdt_stub_not_implemented(uint64_t, uint64_t, uint64_t,
                                              uint64_t, uint64_t, uint64_t);
    uint32_t i;
    char msg[96];
    TEST_ASSERT(tbl != (const SSDT_TABLE *)0, "SSDT main table exists");
    if (!tbl) return;
    for (i = 0; i < sizeof(s_alpc_slots) / sizeof(s_alpc_slots[0]); i++) {
        uint32_t idx = s_alpc_slots[i].svc & 0xFFF;
        snprintf(msg, sizeof(msg), "%s (0x%x) registered",
                 s_alpc_slots[i].name, (uint64_t)s_alpc_slots[i].svc);
        TEST_ASSERT(tbl->handlers[idx] != ssdt_stub_not_implemented, msg);
    }
}

static void test_nt_alpc_pending_features(void)
{
    /* Most ALPC engine not yet implemented. 15 slots still reserved;
     * tests verify the deferred contract holds via TEST_PENDING -- a
     * break here means a slot returned the wrong status
     * (mis-registration).
     *
     * NtAlpcCreatePort (0x010F) is now REAL (the ALPC port-type +
     * \RPC Control + AlpcCreatePort work shipped); skip that slot here
     * to keep the pending inventory honest. When dispatched with
     * null args it returns STATUS_INVALID_PARAMETER, not
     * STATUS_NOT_IMPLEMENTED.
     *
     * Message format: name + slot + brief gap. Suite name "NT ALPC
     * pending features" already names the subsystem. NO TODO refs in
     * the runtime message: they drift, the source comment carries the
     * durable record, and short messages reduce klog rate-limit
     * pressure during the 16-slot sweep. ASCII only -- the section
     * sign U+00A7 (UTF-8 C2 A7) garbles in Windows serial terminals
     * and overflows snprintf -> klog buffers when packed into long
     * strings (see CLAUDE.md "No Unicode Dashes"). */
    uint32_t i;
    char msg[64];
    for (i = 0; i < sizeof(s_alpc_slots) / sizeof(s_alpc_slots[0]); i++) {
        /* Slots retired by the CreatePort + ConnectPort work -- they
         * now return real statuses (INVALID_PARAMETER / INVALID_HANDLE /
         * etc. on the null dispatch this test uses), not
         * STATUS_NOT_IMPLEMENTED. */
        if (s_alpc_slots[i].svc == SSDT_NtAlpcCreatePort)        continue;
        if (s_alpc_slots[i].svc == SSDT_NtAlpcConnectPort)       continue;
        if (s_alpc_slots[i].svc == SSDT_NtAlpcAcceptConnectPort) continue; /* */
        if (s_alpc_slots[i].svc == SSDT_NtAlpcDisconnectPort) continue; /* */
        if (s_alpc_slots[i].svc == SSDT_NtAlpcSendWaitReceivePort) continue; /* */
        if (s_alpc_slots[i].svc == SSDT_NtAlpcSetInformation) continue; /* (partial: AssociateCompletionPort) */
        NTSTATUS st = ssdt_dispatch(s_alpc_slots[i].svc, 0, 0, 0, 0, 0, 0);
        snprintf(msg, sizeof(msg), "%s (0x%x): no ALPC engine yet",
                 s_alpc_slots[i].name, (uint64_t)s_alpc_slots[i].svc);
        TEST_PENDING(st == STATUS_NOT_IMPLEMENTED, msg);
    }
}

/* ============================================================================
 * Namespace locking regression test (-9 line 328)
 * ============================================================================ */

#include "kernel/sched/task.h"

/* Worker thread: rapidly insert + unlink a named object in a test
 * directory. Runs while the main thread repeatedly enumerates the same
 * directory via NtQueryDirectoryObject. The stress exercises the new
 * per-directory spinlock: without it, a timer-preempted enumerator
 * would UAF a freed OBJECT_DIRECTORY_ENTRY. */

static volatile int s_ns_stress_stop;
static void *s_ns_stress_dir;
static const OBJECT_TYPE *s_ns_stress_type;

static void ns_stress_worker(void *arg)
{
    (void)arg;
    uint32_t i;
    for (i = 0; i < 200 && !s_ns_stress_stop; i++) {
        void *body = ob_alloc_object(s_ns_stress_type);
        if (!body)
            break;
        if (ObInsertObject(body, "StressNode", s_ns_stress_dir) == 0)
            ObpRemoveFromDirectory(s_ns_stress_dir, body);
        ObDereferenceObject(body);
    }
}

static void test_ob_ns_locking_stress(void)
{
    /* Create a private test directory + object type so the stress
     * doesn't pollute \BaseNamedObjects or race other suites. */
    void *ko_dir = NULL;
    if (ObLookupObjectByName("\\KernelObjects", ObpDirectoryType, 0,
                             &ko_dir) != 0 || !ko_dir) {
        TEST_SKIP("KernelObjects root not available for stress test");
        return;
    }

    void *testdir = ob_ns_create_directory(ko_dir);
    TEST_ASSERT(testdir != NULL, "stress test directory created");
    if (!testdir) {
        ObDereferenceObject(ko_dir);
        return;
    }
    TEST_ASSERT(ObInsertObject(testdir, "NsStressDir", ko_dir) == 0,
                "stress dir inserted into \\KernelObjects");

    static const OBJECT_TYPE stress_tmpl = {
        .name = "NsStressObj", .body_size = 16,
    };
    s_ns_stress_type = ob_create_type(&stress_tmpl);
    TEST_ASSERT(s_ns_stress_type != NULL, "stress type created");
    s_ns_stress_dir  = testdir;
    s_ns_stress_stop = 0;

    int tid = kthread_create(ns_stress_worker, (void *)0, 0);
    TEST_ASSERT(tid >= 0, "stress worker spawned");

    /* Main thread: enumerate the directory repeatedly. Each iteration
     * acquires + releases dir->lock; interleavings with the worker's
     * Insert/Remove prove the list never tears. */
    HANDLE_TABLE ht;
    ob_handle_table_init(&ht);
    HANDLE dh = ObpAllocateHandle(&ht, testdir, 0, 0);
    TEST_ASSERT(dh >= 0, "stress dir handle allocated");

    uint32_t iterations = 0;
    OBJECT_DIRECTORY_INFORMATION info;
    uint32_t ctx = 0, ret = 0;
    while (iterations < 500) {
        ctx = 0;
        (void)NtQueryDirectoryObject(&ht, dh, &info, 1, &ctx, &ret);
        iterations++;
    }
    TEST_ASSERT_EQ(iterations, 500u, "500 enumeration iterations completed");

    s_ns_stress_stop = 1;
    thread_join((uint32_t)tid);

    /* Teardown: drop dir from KernelObjects, clear PERMANENT, drop
     * creation ref so the testdir body is freed. */
    ObpFreeHandle(&ht, dh);
    ObpRemoveFromDirectory(ko_dir, testdir);
    ObMakeTemporaryObject(testdir);
    ObDereferenceObject(testdir);
    ObDereferenceObject(ko_dir);
    ob_handle_table_destroy(&ht);
}

/* ---- Info-file pseudo-file type (\ObjectManager\<name>) ---------------- */

/* Deterministic test callback: fills buf with [offset, offset+size) of a
 * 64-byte sequence where byte i = (uint8_t)i. Lets the roundtrip test
 * verify both the offset-slicing in ob_info_file_read AND that the
 * callback receives the right offset argument. */
static int32_t test_info_read_pattern(uint8_t *buf, uint32_t size,
                                      uint32_t offset)
{
    if (!buf)
        return -1;
    for (uint32_t i = 0; i < size; i++)
        buf[i] = (uint8_t)(offset + i);
    return (int32_t)size;
}

static void test_ob_info_file_register_and_read(void)
{
    /* The wm_framestats_register_info_file path registers "FrameStats";
     * this test uses a distinct name so both tests can coexist without
     * the duplicate-name guard rejecting. */
    int rc = ob_info_file_register("TestInfoPattern",
                                   test_info_read_pattern, 64);
    TEST_ASSERT_EQ(rc, 0, "ob_info_file_register TestInfoPattern");

    /* Duplicate register must refuse with -1 (duplicate-name guard). */
    rc = ob_info_file_register("TestInfoPattern",
                               test_info_read_pattern, 64);
    TEST_ASSERT_EQ(rc, -1, "duplicate register rejected");

    /* Bad args: null callback, null name, zero size -> -1. */
    TEST_ASSERT_EQ(ob_info_file_register(NULL,
                                         test_info_read_pattern, 64),
                   -1, "null name rejected");
    TEST_ASSERT_EQ(ob_info_file_register("X", NULL, 64),
                   -1, "null callback rejected");
    TEST_ASSERT_EQ(ob_info_file_register("X",
                                         test_info_read_pattern, 0),
                   -1, "zero size rejected");

    /* Open + read full pattern. */
    HANDLE_TABLE *ht = &task_current()->handle_table;
    HANDLE h = ob_info_file_open_handle(ht, "TestInfoPattern");
    TEST_ASSERT(h != INVALID_HANDLE_VALUE, "open TestInfoPattern");

    uint8_t buf[64];
    for (uint32_t i = 0; i < sizeof(buf); i++)
        buf[i] = 0xFF;

    int32_t n = ob_info_file_read(ht, h, buf, 64, 0);
    TEST_ASSERT_EQ(n, 64, "read full 64 bytes");
    for (uint32_t i = 0; i < 64; i++) {
        char msg[48];
        snprintf(msg, sizeof(msg), "buf[%u] == %u", i, i);
        TEST_ASSERT_EQ((uint32_t)buf[i], i, msg);
    }

    /* Partial read at offset 16, size 8 -> buf[0..8] == 16..23. */
    n = ob_info_file_read(ht, h, buf, 8, 16);
    TEST_ASSERT_EQ(n, 8, "partial read 8 bytes at offset 16");
    for (uint32_t i = 0; i < 8; i++) {
        char msg[48];
        snprintf(msg, sizeof(msg), "buf[%u] == %u after offset read", i, 16 + i);
        TEST_ASSERT_EQ((uint32_t)buf[i], 16 + i, msg);
    }

    /* EOF: offset == size returns 0. */
    n = ob_info_file_read(ht, h, buf, 16, 64);
    TEST_ASSERT_EQ(n, 0, "read at offset == size returns EOF (0)");

    /* EOF: offset > size returns 0 too. */
    n = ob_info_file_read(ht, h, buf, 16, 100);
    TEST_ASSERT_EQ(n, 0, "read past size returns EOF (0)");

    /* Clamp: request 64 bytes at offset 32 -> only 32 bytes left. */
    n = ob_info_file_read(ht, h, buf, 64, 32);
    TEST_ASSERT_EQ(n, 32, "read clamps to remaining bytes");

    /* Bad handle -> -1. */
    n = ob_info_file_read(ht, (HANDLE)0xdeadbeef, buf, 16, 0);
    TEST_ASSERT_EQ(n, -1, "bad handle returns -1");

    /* NULL buf -> -1. */
    n = ob_info_file_read(ht, h, NULL, 16, 0);
    TEST_ASSERT_EQ(n, -1, "null buf returns -1");

    /* Unknown name -> INVALID_HANDLE_VALUE. */
    HANDLE bad = ob_info_file_open_handle(ht, "NoSuchPseudoFile");
    TEST_ASSERT(bad == INVALID_HANDLE_VALUE,
                "open unknown name returns INVALID_HANDLE_VALUE");

    ObpFreeHandle(ht, h);

    /* -- Cleanup -- unlink the PERMANENT test info file so leak
     * tracking sees a clean close. Same pattern as test_ob_cleanup_named
     * for \BaseNamedObjects entries: clear PERMANENT via
     * ObMakeTemporaryObject, then ObpRemoveFromDirectory drops the
     * directory's ref which lets the body free on the next deref. */
    {
        void *body = NULL;
        void *om = NULL;
        if (ObLookupObjectByName("\\ObjectManager\\TestInfoPattern",
                                 ObpInfoFileType, 0, &body) == 0 && body) {
            ObMakeTemporaryObject(body);
            if (ObLookupObjectByName("\\ObjectManager", ObpDirectoryType,
                                     0, &om) == 0 && om) {
                ObpRemoveFromDirectory(om, body);
                ObDereferenceObject(om);
            }
            ObDereferenceObject(body);
        }
    }
}

static void test_ob_framestats_pseudo_file(void)
{
    /* Manually register the FrameStats info file. In production this
     * runs from wm_init() during boot_phase3, but the kernel test phase
     * runs in phase 3 BEFORE boot_desktop_init is called, so we have to
     * drive it ourselves. Idempotent: a second register from wm_init
     * later during normal boot will no-op. */
    wm_framestats_register_info_file();

    HANDLE_TABLE *ht = &task_current()->handle_table;
    HANDLE h = ob_info_file_open_handle(ht, "FrameStats");
    TEST_ASSERT(h != INVALID_HANDLE_VALUE, "open \\ObjectManager\\FrameStats");

    /* Byte-wise read of the full struct. In the kernel test phase the
     * compositor has never run, so every counter is 0. This test is
     * about the PLUMBING (schema + read path), not compositor liveness;
     * the late-phase harness owns the "frames_presented > 0 after
     * idle" check. */
    uint8_t buf[ETW_WM_FRAME_STATS_SIZE];
    for (uint32_t i = 0; i < sizeof(buf); i++)
        buf[i] = 0xAA;  /* poison so a short read is visible */

    int32_t n = ob_info_file_read(ht, h, buf, ETW_WM_FRAME_STATS_SIZE, 0);
    TEST_ASSERT_EQ(n, (int32_t)ETW_WM_FRAME_STATS_SIZE,
                   "read returns full schema size");

    /* Schema lock-step: the C struct AND the etw.h constant are the same
     * size (static_assert in wm.c also enforces this at compile time). */
    TEST_ASSERT_EQ((uint32_t)sizeof(struct wm_frame_stats),
                   ETW_WM_FRAME_STATS_SIZE,
                   "sizeof(wm_frame_stats) == ETW_WM_FRAME_STATS_SIZE");

    /* Snapshot via the direct API and compare byte-for-byte with the
     * pseudo-file read -- they MUST produce identical output since both
     * paths share wm_get_frame_stats under the seqlock. Any drift here
     * is an ABI regression. */
    struct wm_frame_stats direct;
    wm_get_frame_stats(&direct);
    const uint8_t *direct_bytes = (const uint8_t *)&direct;
    for (uint32_t i = 0; i < ETW_WM_FRAME_STATS_SIZE; i++) {
        char msg[56];
        snprintf(msg, sizeof(msg),
                 "pseudo-file byte %u matches direct snapshot", i);
        TEST_ASSERT_EQ((uint32_t)buf[i], (uint32_t)direct_bytes[i], msg);
    }

    /* EOF at offset == size. */
    n = ob_info_file_read(ht, h, buf, 16, ETW_WM_FRAME_STATS_SIZE);
    TEST_ASSERT_EQ(n, 0, "read at EOF returns 0");

    ObpFreeHandle(ht, h);

    /* -- Cleanup -- unlink FrameStats from \ObjectManager so per-test
     * leak tracking sees a balanced close. In production boot, wm_init
     * will re-register FrameStats during phase 3 via the idempotent
     * wm_framestats_register_info_file path. */
    {
        void *body = NULL;
        void *om = NULL;
        if (ObLookupObjectByName("\\ObjectManager\\FrameStats",
                                 ObpInfoFileType, 0, &body) == 0 && body) {
            ObMakeTemporaryObject(body);
            if (ObLookupObjectByName("\\ObjectManager", ObpDirectoryType,
                                     0, &om) == 0 && om) {
                ObpRemoveFromDirectory(om, body);
                ObDereferenceObject(om);
            }
            ObDereferenceObject(body);
        }
    }
}

/* ---- Registration ---- */

void test_register_ob(void)
{
    test_suite_register_cat("OB: alloc+header roundtrip", test_ob_alloc_header_roundtrip, TEST_CAT_OB);
    test_suite_register_cat("OB: refcount lifecycle", test_ob_refcount_lifecycle, TEST_CAT_OB);
    test_suite_register_cat("OB: handle table", test_ob_handle_table, TEST_CAT_OB);
    test_suite_register_cat("OB: namespace lookup", test_ob_namespace_lookup, TEST_CAT_OB);
    test_suite_register_cat("OB: handle low-bits rejected", test_ob_handle_low_bits_rejected, TEST_CAT_OB);
    test_suite_register_cat("OB: duplicate handle", test_ob_duplicate_handle, TEST_CAT_OB);
    test_suite_register_cat("OB: duplicate access cap", test_ob_duplicate_access_cap, TEST_CAT_OB);
    test_suite_register_cat("OB: duplicate-close protected", test_ob_duplicate_close_protected, TEST_CAT_OB);
    test_suite_register_cat("OB: duplicate cap callback-proof", test_ob_duplicate_cap_callback_proof, TEST_CAT_OB);
    test_suite_register_cat("OB: dup-close no premature on_close",
                            test_ob_duplicate_close_no_premature_onclose, TEST_CAT_OB);
    test_suite_register_cat("OB: handle inherit", test_ob_handle_inherit, TEST_CAT_OB);
    test_suite_register_cat("OB: inherit none -> 0", test_ob_inherit_none, TEST_CAT_OB);
    test_suite_register_cat("OB: query directory", test_ob_query_directory, TEST_CAT_OB);
    test_suite_register_cat("OB: type stats", test_ob_type_stats, TEST_CAT_OB);
    test_suite_register_cat("OB: stat export clamp + no-handle types", test_ob_stat_export_clamp, TEST_CAT_OB);
    test_suite_register_cat("OB: callbacks", test_ob_callbacks, TEST_CAT_OB);
    test_suite_register_cat("OB: callback hardening (ceiling + stable id)", test_ob_callbacks_hardening, TEST_CAT_OB);
    test_suite_register_cat("OB: dup create-only cb skip", test_ob_callbacks_dup_create_only_unaffected, TEST_CAT_OB);
    test_suite_register_cat("OB: zero-access no-op cb", test_ob_callbacks_zero_access_noop_allowed, TEST_CAT_OB);
    test_suite_register_cat("OB: handle quota", test_ob_handle_quota, TEST_CAT_OB);
    test_suite_register_cat("OB: quota grows past old cap", test_ob_handle_quota_grows_past_old_cap, TEST_CAT_OB);
    test_suite_register_cat("OB: trace", test_ob_trace, TEST_CAT_OB);
    test_suite_register_cat("OB: trace wrap + tags", test_ob_trace_wrap_and_tags, TEST_CAT_OB);
    test_suite_register_cat("OB: trace mis-tag lifetime", test_ob_trace_mistag_lifetime, TEST_CAT_OB);
    test_suite_register_cat("OB: NT create+open directory", test_nt_create_open_directory, TEST_CAT_OB);
    test_suite_register_cat("OB: NT open directory not found", test_nt_open_directory_not_found, TEST_CAT_OB);
    test_suite_register_cat("OB: NT symlink roundtrip", test_nt_symlink_roundtrip, TEST_CAT_OB);
    test_suite_register_cat("OB: NT query symlink wrong type", test_nt_query_symlink_wrong_type, TEST_CAT_OB);
    test_suite_register_cat("OB: symlink cycle bounded", test_ob_symlink_cycle_bounded, TEST_CAT_OB);
    test_suite_register_cat("OB: NT namespace SSDT registered", test_nt_namespace_ssdt_registered, TEST_CAT_OB);
    test_suite_register_cat("OB: NT section anon roundtrip", test_nt_section_anon_roundtrip, TEST_CAT_OB);
    test_suite_register_cat("OB: NT section file-backed", test_nt_section_file_backed, TEST_CAT_OB);
    test_suite_register_cat("OB: NT section named open+query+extend",
                            test_nt_section_named_open_query_extend, TEST_CAT_OB);
    test_suite_register_cat("OB: NT section unmap bad base", test_nt_section_unmap_bad_base, TEST_CAT_OB);
    test_suite_register_cat("OB: NT section SSDT registered", test_nt_section_ssdt_registered, TEST_CAT_OB);
    test_suite_register_cat("OB: NT timer create+query", test_nt_timer_create_and_query, TEST_CAT_OB);
    test_suite_register_cat("OB: NT timer set/cancel", test_nt_timer_set_cancel, TEST_CAT_OB);
    test_suite_register_cat("OB: NT timer open existing", test_nt_timer_open_existing, TEST_CAT_OB);
    test_suite_register_cat("OB: NT timer wrong type", test_nt_timer_wrong_type, TEST_CAT_OB);
    test_suite_register_cat("OB: NT timer SSDT registered", test_nt_timer_ssdt_registered, TEST_CAT_OB);
    test_suite_register_cat("OB: NT LPC slots registered", test_nt_lpc_slots_registered, TEST_CAT_OB);
    test_suite_register_cat("OB: NT LPC pending features", test_nt_lpc_pending_features, TEST_CAT_OB);
    test_suite_register_cat("OB: NT ALPC slots registered", test_nt_alpc_slots_registered, TEST_CAT_OB);
    test_suite_register_cat("OB: NT ALPC pending features", test_nt_alpc_pending_features, TEST_CAT_OB);
    test_suite_register_cat("OB: PE ntdll exports sorted", test_pe_ntdll_exports_sorted, TEST_CAT_OB);
    test_suite_register_cat("OB: namespace locking stress", test_ob_ns_locking_stress, TEST_CAT_OB);
    test_suite_register_cat("OB: info-file register+read", test_ob_info_file_register_and_read, TEST_CAT_OB);
    test_suite_register_cat("OB: \\ObjectManager\\FrameStats pseudo-file", test_ob_framestats_pseudo_file, TEST_CAT_OB);
}

#endif /* KERNEL_TESTS */
