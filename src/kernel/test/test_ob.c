/* ============================================================================
 * test_ob.c -- Object Manager unit tests
 *
 * Tests object allocation, reference counting, handle table, namespace
 * lookup, handle duplication, inheritance, and directory enumeration.
 *
 * XREF: 00-infrastructure/TODO-02 §3
 * ============================================================================ */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "kernel/atomic.h"
#include "kernel/ob/ob.h"
#include "kernel/ob/ob_type.h"
#include "kernel/ob/ob_callback.h"
#include "kernel/ob/ob_trace.h"
#include "kernel/ob/ob_ns.h"
#include "kernel/ob/handle_table.h"

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
}

/* ---- ob_handle_table_inherit ---- */

static void test_ob_handle_inherit(void)
{
    HANDLE_TABLE parent, child;
    ob_handle_table_init(&parent);
    ob_handle_table_init(&child);

    void *body = ob_alloc_object(&test_type);
    HANDLE h = ObpAllocateHandle(&parent, body, 0x1F01FF, OBJ_INHERIT);

    uint32_t inherited = ob_handle_table_inherit(&parent, &child);
    TEST_ASSERT(inherited >= 1, "at least 1 handle inherited");

    HANDLE_TABLE_ENTRY *child_entry = ObpLookupHandle(&child, h);
    TEST_ASSERT(child_entry != (void *)0 && child_entry->object == body,
                "inherited handle at same index points to same object");

    ObpFreeHandle(&parent, h);
    ObpFreeHandle(&child, h);
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
}

/* ---- Per-type object and handle statistics (§12) ---- */

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

    /* Free 1 handle and retry -- should succeed */
    ObpFreeHandle(&ht, h0);
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
}

/* ---- Registration ---- */

void test_register_ob(void)
{
    test_suite_register_cat("OB: alloc+header roundtrip", test_ob_alloc_header_roundtrip, TEST_CAT_OB);
    test_suite_register_cat("OB: refcount lifecycle", test_ob_refcount_lifecycle, TEST_CAT_OB);
    test_suite_register_cat("OB: handle table", test_ob_handle_table, TEST_CAT_OB);
    test_suite_register_cat("OB: namespace lookup", test_ob_namespace_lookup, TEST_CAT_OB);
    test_suite_register_cat("OB: duplicate handle", test_ob_duplicate_handle, TEST_CAT_OB);
    test_suite_register_cat("OB: handle inherit", test_ob_handle_inherit, TEST_CAT_OB);
    test_suite_register_cat("OB: query directory", test_ob_query_directory, TEST_CAT_OB);
    test_suite_register_cat("OB: type stats", test_ob_type_stats, TEST_CAT_OB);
    test_suite_register_cat("OB: callbacks", test_ob_callbacks, TEST_CAT_OB);
    test_suite_register_cat("OB: handle quota", test_ob_handle_quota, TEST_CAT_OB);
    test_suite_register_cat("OB: trace", test_ob_trace, TEST_CAT_OB);
}

#endif /* KERNEL_TESTS */
