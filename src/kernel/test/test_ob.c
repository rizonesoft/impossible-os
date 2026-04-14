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

/* ============================================================================
 * §17 NT namespace syscall tests (SSDT dispatch path)
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

/* Test: all 6 SSDT slots are registered (not stubs) */
static void test_nt_namespace_ssdt_registered(void)
{
    const SSDT_TABLE *tbl = ssdt_get_table(SSDT_TABLE_MAIN);
    extern NTSTATUS ssdt_stub_not_implemented(uint64_t, uint64_t, uint64_t,
                                              uint64_t, uint64_t, uint64_t);
    uint32_t slots[] = {
        SSDT_NtCreateDirectoryObject, SSDT_NtOpenDirectoryObject,
        SSDT_NtQueryDirectoryObject,  SSDT_NtCreateSymbolicLinkObject,
        SSDT_NtOpenSymbolicLinkObject, SSDT_NtQuerySymbolicLinkObject,
    };
    uint32_t i;
    TEST_ASSERT(tbl != (const SSDT_TABLE *)0, "SSDT main table exists");
    if (!tbl) return;
    for (i = 0; i < 6; i++) {
        uint32_t idx = slots[i] & 0xFFF;
        TEST_ASSERT(tbl->handlers[idx] != ssdt_stub_not_implemented,
                    "Namespace SSDT slot is registered");
    }
}


/* ============================================================================
 * §18 NT section / mapped file syscall tests
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
    uint32_t slots[] = {
        SSDT_NtCreateSection, SSDT_NtOpenSection, SSDT_NtMapViewOfSection,
        SSDT_NtUnmapViewOfSection, SSDT_NtExtendSection, SSDT_NtQuerySection,
        SSDT_NtAreMappedFilesTheSame,
    };
    uint32_t i;
    TEST_ASSERT(tbl != (const SSDT_TABLE *)0, "SSDT main table exists");
    if (!tbl) return;
    for (i = 0; i < sizeof(slots) / sizeof(slots[0]); i++) {
        uint32_t idx = slots[i] & 0xFFF;
        TEST_ASSERT(tbl->handlers[idx] != ssdt_stub_not_implemented,
                    "Section SSDT slot registered");
    }
}

/* ============================================================================
 * §19 NT timer syscall tests
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
    uint32_t slots[] = {
        SSDT_NtCreateTimer, SSDT_NtOpenTimer,  SSDT_NtSetTimer,
        SSDT_NtCancelTimer, SSDT_NtQueryTimer, SSDT_NtSetTimerEx,
    };
    uint32_t i;
    TEST_ASSERT(tbl != (const SSDT_TABLE *)0, "SSDT main table exists");
    if (!tbl) return;
    for (i = 0; i < sizeof(slots) / sizeof(slots[0]); i++) {
        uint32_t idx = slots[i] & 0xFFF;
        TEST_ASSERT(tbl->handlers[idx] != ssdt_stub_not_implemented,
                    "Timer SSDT slot registered");
    }
}

/* ============================================================================
 * §20 NT legacy LPC SSDT stub-wiring tests
 * ============================================================================ */

#include "kernel/nt/nt_lpc.h"

static void test_nt_lpc_slots_registered(void)
{
    const SSDT_TABLE *tbl = ssdt_get_table(SSDT_TABLE_MAIN);
    extern NTSTATUS ssdt_stub_not_implemented(uint64_t, uint64_t, uint64_t,
                                              uint64_t, uint64_t, uint64_t);
    uint32_t slots[] = {
        SSDT_NtCreatePort,              SSDT_NtCreateWaitablePort,
        SSDT_NtConnectPort,             SSDT_NtSecureConnectPort,
        SSDT_NtAcceptConnectPort,       SSDT_NtCompleteConnectPort,
        SSDT_NtListenPort,              SSDT_NtReplyPort,
        SSDT_NtReplyWaitReceivePort,    SSDT_NtReplyWaitReceivePortEx,
        SSDT_NtRequestPort,             SSDT_NtRequestWaitReplyPort,
        SSDT_NtImpersonateClientOfPort, SSDT_NtReadRequestData,
        SSDT_NtWriteRequestData,
    };
    uint32_t i;
    TEST_ASSERT(tbl != (const SSDT_TABLE *)0, "SSDT main table exists");
    if (!tbl) return;
    for (i = 0; i < sizeof(slots) / sizeof(slots[0]); i++) {
        uint32_t idx = slots[i] & 0xFFF;
        TEST_ASSERT(tbl->handlers[idx] != ssdt_stub_not_implemented,
                    "LPC SSDT slot registered (not default stub)");
    }
}

static void test_nt_lpc_returns_deferred_status(void)
{
    /* Deterministic STATUS_NOT_IMPLEMENTED contract: user-mode callers
     * must see a real NTSTATUS, not a dispatch-miss path. Dispatch ALL
     * 15 slots -- mis-registration to another non-stub handler in any
     * slot would otherwise ship undetected. */
    uint32_t slots[] = {
        SSDT_NtCreatePort,              SSDT_NtCreateWaitablePort,
        SSDT_NtConnectPort,             SSDT_NtSecureConnectPort,
        SSDT_NtAcceptConnectPort,       SSDT_NtCompleteConnectPort,
        SSDT_NtListenPort,              SSDT_NtReplyPort,
        SSDT_NtReplyWaitReceivePort,    SSDT_NtReplyWaitReceivePortEx,
        SSDT_NtRequestPort,             SSDT_NtRequestWaitReplyPort,
        SSDT_NtImpersonateClientOfPort, SSDT_NtReadRequestData,
        SSDT_NtWriteRequestData,
    };
    uint32_t i;
    for (i = 0; i < sizeof(slots) / sizeof(slots[0]); i++) {
        NTSTATUS st = ssdt_dispatch(slots[i], 0, 0, 0, 0, 0, 0);
        TEST_ASSERT(st == STATUS_NOT_IMPLEMENTED,
                    "LPC slot returns STATUS_NOT_IMPLEMENTED");
    }
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
    test_suite_register_cat("OB: NT create+open directory", test_nt_create_open_directory, TEST_CAT_OB);
    test_suite_register_cat("OB: NT open directory not found", test_nt_open_directory_not_found, TEST_CAT_OB);
    test_suite_register_cat("OB: NT symlink roundtrip", test_nt_symlink_roundtrip, TEST_CAT_OB);
    test_suite_register_cat("OB: NT query symlink wrong type", test_nt_query_symlink_wrong_type, TEST_CAT_OB);
    test_suite_register_cat("OB: NT namespace SSDT registered", test_nt_namespace_ssdt_registered, TEST_CAT_OB);
    test_suite_register_cat("OB: NT section anon roundtrip", test_nt_section_anon_roundtrip, TEST_CAT_OB);
    test_suite_register_cat("OB: NT section file-backed", test_nt_section_file_backed, TEST_CAT_OB);
    test_suite_register_cat("OB: NT section named open+query+extend", test_nt_section_named_open_query_extend, TEST_CAT_OB);
    test_suite_register_cat("OB: NT section unmap bad base", test_nt_section_unmap_bad_base, TEST_CAT_OB);
    test_suite_register_cat("OB: NT section SSDT registered", test_nt_section_ssdt_registered, TEST_CAT_OB);
    test_suite_register_cat("OB: NT timer create+query", test_nt_timer_create_and_query, TEST_CAT_OB);
    test_suite_register_cat("OB: NT timer set/cancel", test_nt_timer_set_cancel, TEST_CAT_OB);
    test_suite_register_cat("OB: NT timer open existing", test_nt_timer_open_existing, TEST_CAT_OB);
    test_suite_register_cat("OB: NT timer wrong type", test_nt_timer_wrong_type, TEST_CAT_OB);
    test_suite_register_cat("OB: NT timer SSDT registered", test_nt_timer_ssdt_registered, TEST_CAT_OB);
    test_suite_register_cat("OB: NT LPC slots registered", test_nt_lpc_slots_registered, TEST_CAT_OB);
    test_suite_register_cat("OB: NT LPC returns deferred", test_nt_lpc_returns_deferred_status, TEST_CAT_OB);
}

#endif /* KERNEL_TESTS */
