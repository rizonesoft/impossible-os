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
}

#endif /* KERNEL_TESTS */
