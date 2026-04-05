/* ============================================================================
 * test_ob.c -- Object Manager unit tests
 *
 * Tests object allocation, reference counting, handle table, namespace
 * lookup, handle duplication, inheritance, and directory enumeration.
 *
 * XREF: 00-infrastructure/TODO-03 §3
 * ============================================================================ */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "kernel/atomic.h"
#include "kernel/ob/ob.h"
#include "kernel/ob/ob_type.h"
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
}

#endif /* KERNEL_TESTS */
