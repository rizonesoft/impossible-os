/* ============================================================================
 * test_registry.c -- Win32-style Registry unit tests
 * ============================================================================ */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "registry.h"
#include "kernel/types.h"

/* Test: set and get a REG_DWORD value */
static void test_registry_dword(void)
{
    HKEY hKey;
    long rc = RegOpenKeyEx(HKEY_LOCAL_MACHINE, "Software\\Test", 0, 0, &hKey);
    if (rc != 0) {
        /* Key doesn't exist -- create it */
        rc = RegCreateKeyEx(HKEY_LOCAL_MACHINE, "Software\\Test", 0, NULL, 0, 0, NULL, &hKey, NULL);
        TEST_ASSERT(rc == 0, "RegCreateKeyEx succeeds");
        if (rc != 0) return;
    }

    /* Set a DWORD value */
    uint32_t val = 42;
    rc = RegSetValueEx(hKey, "TestDword", 0, REG_DWORD,
                       (const uint8_t *)&val, sizeof(val));
    TEST_ASSERT(rc == 0, "RegSetValueEx DWORD succeeds");

    /* Read it back */
    uint32_t out = 0;
    uint32_t out_size = sizeof(out);
    rc = RegGetValue(hKey, NULL, "TestDword", RRF_RT_REG_DWORD, NULL,
                     &out, &out_size);
    TEST_ASSERT(rc == 0, "RegGetValue DWORD succeeds");
    TEST_ASSERT(out == 42, "RegGetValue DWORD returns correct value");

    /* Clean up */
    RegDeleteKey(hKey, "TestDword");
    RegCloseKey(hKey);
}

/* Test: set and get a REG_SZ string value */
static void test_registry_string(void)
{
    HKEY hKey;
    long rc = RegCreateKeyEx(HKEY_LOCAL_MACHINE, "Software\\Test", 0, NULL, 0, 0, NULL, &hKey, NULL);
    if (rc != 0) {
        TEST_ASSERT(0, "RegCreateKeyEx for string test");
        return;
    }

    const char *str = "hello";
    rc = RegSetValueEx(hKey, "TestString", 0, REG_SZ,
                       (const uint8_t *)str, 6);  /* includes NUL */
    TEST_ASSERT(rc == 0, "RegSetValueEx REG_SZ succeeds");

    char buf[32];
    uint32_t buf_size = sizeof(buf);
    rc = RegGetValue(hKey, NULL, "TestString", RRF_RT_REG_SZ, NULL,
                     buf, &buf_size);
    TEST_ASSERT(rc == 0, "RegGetValue REG_SZ succeeds");
    TEST_ASSERT(buf[0] == 'h' && buf[4] == 'o', "RegGetValue REG_SZ returns correct string");

    /* Clean up */
    RegDeleteKey(hKey, "TestString");
    RegCloseKey(hKey);
}

/* ============================================================================
 * NT-level SSDT registry tests
 * Test the NtXxx handlers via ssdt_dispatch with proper OBJECT_ATTRIBUTES.
 * ============================================================================ */

#include "kernel/nt/ssdt.h"
#include "kernel/nt/ntstatus.h"
#include "kernel/nt/nt_types.h"
#include "kernel/nt/service_numbers.h"
#include "kernel/nt/nt_registry.h"

/* Helper: build OBJECT_ATTRIBUTES + UNICODE_STRING for an NT registry path */
static void build_oa(OBJECT_ATTRIBUTES *oa, UNICODE_STRING *us, const char *path)
{
    us->Buffer = (uint16_t *)(uintptr_t)path;  /* ASCII stored as pointer */
    us->Length = 0;
    us->MaximumLength = 0;
    /* Count length */
    {
        const char *p = path;
        while (*p) { us->Length++; p++; }
        us->MaximumLength = us->Length + 1;
    }
    oa->Length = sizeof(OBJECT_ATTRIBUTES);
    oa->RootDirectory = INVALID_HANDLE_VALUE;
    oa->ObjectName = us;
    oa->Attributes = OBJ_CASE_INSENSITIVE;
    oa->_pad1 = 0;
    oa->_pad2 = 0;
    oa->SecurityDescriptor = (void *)0;
    oa->SecurityQualityOfService = (void *)0;
}

/* Test: NtCreateKey creates a key under \Registry\Machine\Software */
static void test_nt_create_key(void)
{
    OBJECT_ATTRIBUTES oa;
    UNICODE_STRING us;
    HANDLE key_handle = INVALID_HANDLE_VALUE;
    NTSTATUS status;

    build_oa(&oa, &us, "\\Registry\\Machine\\Software\\NtTest");

    status = ssdt_dispatch(SSDT_NtCreateKey,
                           (uint64_t)(uintptr_t)&key_handle,
                           (uint64_t)KEY_ALL_ACCESS,
                           (uint64_t)(uintptr_t)&oa,
                           0, 0, 0);

    TEST_ASSERT(NT_SUCCESS(status), "NtCreateKey returns STATUS_SUCCESS");
    TEST_ASSERT(key_handle != INVALID_HANDLE_VALUE, "NtCreateKey returns valid handle");

    /* Clean up: delete the key via NtDeleteKey */
    if (key_handle != INVALID_HANDLE_VALUE) {
        ssdt_dispatch(SSDT_NtDeleteKey,
                      (uint64_t)(uintptr_t)key_handle,
                      0, 0, 0, 0, 0);
    }
}

/* Test: NtOpenKey opens an existing key */
static void test_nt_open_key(void)
{
    OBJECT_ATTRIBUTES oa;
    UNICODE_STRING us;
    HANDLE key_handle = INVALID_HANDLE_VALUE;
    NTSTATUS status;

    /* Software key should exist (created by registry_init) */
    build_oa(&oa, &us, "\\Registry\\Machine\\SOFTWARE");

    status = ssdt_dispatch(SSDT_NtOpenKey,
                           (uint64_t)(uintptr_t)&key_handle,
                           (uint64_t)KEY_ALL_ACCESS,
                           (uint64_t)(uintptr_t)&oa,
                           0, 0, 0);

    TEST_ASSERT(NT_SUCCESS(status), "NtOpenKey returns STATUS_SUCCESS for SOFTWARE");
    TEST_ASSERT(key_handle != INVALID_HANDLE_VALUE, "NtOpenKey returns valid handle");
}

/* Test: NtOpenKey fails for non-existent key */
static void test_nt_open_key_not_found(void)
{
    OBJECT_ATTRIBUTES oa;
    UNICODE_STRING us;
    HANDLE key_handle = INVALID_HANDLE_VALUE;
    NTSTATUS status;

    build_oa(&oa, &us, "\\Registry\\Machine\\NonExistentKey12345");

    status = ssdt_dispatch(SSDT_NtOpenKey,
                           (uint64_t)(uintptr_t)&key_handle,
                           (uint64_t)KEY_ALL_ACCESS,
                           (uint64_t)(uintptr_t)&oa,
                           0, 0, 0);

    TEST_ASSERT(status == STATUS_OBJECT_NAME_NOT_FOUND,
                "NtOpenKey returns STATUS_OBJECT_NAME_NOT_FOUND for missing key");
}

/* Test: NtSetValueKey + NtQueryValueKey round-trip */
static void test_nt_set_query_value(void)
{
    OBJECT_ATTRIBUTES oa;
    UNICODE_STRING us;
    UNICODE_STRING vname_us;
    HANDLE key_handle = INVALID_HANDLE_VALUE;
    NTSTATUS status;
    uint32_t test_val = 0xDEADBEEF;
    uint8_t query_buf[64];
    uint32_t result_len = 0;

    /* Create a test key */
    build_oa(&oa, &us, "\\Registry\\Machine\\Software\\NtValueTest");
    status = ssdt_dispatch(SSDT_NtCreateKey,
                           (uint64_t)(uintptr_t)&key_handle,
                           (uint64_t)KEY_ALL_ACCESS,
                           (uint64_t)(uintptr_t)&oa,
                           0, 0, 0);
    TEST_ASSERT(NT_SUCCESS(status), "NtCreateKey for value test");
    if (!NT_SUCCESS(status)) return;

    /* Set a DWORD value */
    vname_us.Buffer = (uint16_t *)(uintptr_t)"TestVal";
    vname_us.Length = 7;
    vname_us.MaximumLength = 8;

    status = ssdt_dispatch(SSDT_NtSetValueKey,
                           (uint64_t)(uintptr_t)key_handle,
                           (uint64_t)(uintptr_t)&vname_us,
                           0,
                           (uint64_t)REG_DWORD,
                           (uint64_t)(uintptr_t)&test_val,
                           (uint64_t)sizeof(test_val));
    TEST_ASSERT(NT_SUCCESS(status), "NtSetValueKey DWORD succeeds");

    /* Query it back using KeyValuePartialInformation */
    status = ssdt_dispatch(SSDT_NtQueryValueKey,
                           (uint64_t)(uintptr_t)key_handle,
                           (uint64_t)(uintptr_t)&vname_us,
                           (uint64_t)KeyValuePartialInformation,
                           (uint64_t)(uintptr_t)query_buf,
                           (uint64_t)sizeof(query_buf),
                           (uint64_t)(uintptr_t)&result_len);
    TEST_ASSERT(NT_SUCCESS(status), "NtQueryValueKey succeeds");

    if (NT_SUCCESS(status)) {
        KEY_VALUE_PARTIAL_INFORMATION *info = (KEY_VALUE_PARTIAL_INFORMATION *)query_buf;
        TEST_ASSERT_EQ(info->Type, REG_DWORD, "NtQueryValueKey returns REG_DWORD");
        TEST_ASSERT_EQ(info->DataLength, sizeof(uint32_t), "NtQueryValueKey data length is 4");
        if (info->DataLength == sizeof(uint32_t)) {
            uint32_t *val = (uint32_t *)info->Data;
            TEST_ASSERT_EQ(*val, 0xDEADBEEF, "NtQueryValueKey round-trip value matches");
        }
    }

    /* Delete value */
    ssdt_dispatch(SSDT_NtDeleteValueKey,
                  (uint64_t)(uintptr_t)key_handle,
                  (uint64_t)(uintptr_t)&vname_us,
                  0, 0, 0, 0);

    /* Delete key */
    ssdt_dispatch(SSDT_NtDeleteKey,
                  (uint64_t)(uintptr_t)key_handle,
                  0, 0, 0, 0, 0);
}

/* Test: NtEnumerateKey iterates subkeys correctly */
static void test_nt_enumerate_key(void)
{
    OBJECT_ATTRIBUTES oa;
    UNICODE_STRING us;
    HANDLE parent_handle = INVALID_HANDLE_VALUE;
    HANDLE child1 = INVALID_HANDLE_VALUE;
    HANDLE child2 = INVALID_HANDLE_VALUE;
    NTSTATUS status;
    uint8_t enum_buf[256];
    uint32_t result_len = 0;
    uint32_t found = 0;

    /* Create parent key */
    build_oa(&oa, &us, "\\Registry\\Machine\\Software\\NtEnumTest");
    status = ssdt_dispatch(SSDT_NtCreateKey,
                           (uint64_t)(uintptr_t)&parent_handle,
                           (uint64_t)KEY_ALL_ACCESS,
                           (uint64_t)(uintptr_t)&oa, 0, 0, 0);
    TEST_ASSERT(NT_SUCCESS(status), "NtCreateKey parent for enum test");
    if (!NT_SUCCESS(status)) return;

    /* Create two child keys */
    build_oa(&oa, &us, "\\Registry\\Machine\\Software\\NtEnumTest\\ChildA");
    ssdt_dispatch(SSDT_NtCreateKey,
                  (uint64_t)(uintptr_t)&child1,
                  (uint64_t)KEY_ALL_ACCESS,
                  (uint64_t)(uintptr_t)&oa, 0, 0, 0);

    build_oa(&oa, &us, "\\Registry\\Machine\\Software\\NtEnumTest\\ChildB");
    ssdt_dispatch(SSDT_NtCreateKey,
                  (uint64_t)(uintptr_t)&child2,
                  (uint64_t)KEY_ALL_ACCESS,
                  (uint64_t)(uintptr_t)&oa, 0, 0, 0);

    /* Enumerate index 0 */
    status = ssdt_dispatch(SSDT_NtEnumerateKey,
                           (uint64_t)(uintptr_t)parent_handle,
                           0,
                           (uint64_t)KeyBasicInformation,
                           (uint64_t)(uintptr_t)enum_buf,
                           (uint64_t)sizeof(enum_buf),
                           (uint64_t)(uintptr_t)&result_len);
    if (NT_SUCCESS(status)) found++;

    /* Enumerate index 1 */
    status = ssdt_dispatch(SSDT_NtEnumerateKey,
                           (uint64_t)(uintptr_t)parent_handle,
                           1,
                           (uint64_t)KeyBasicInformation,
                           (uint64_t)(uintptr_t)enum_buf,
                           (uint64_t)sizeof(enum_buf),
                           (uint64_t)(uintptr_t)&result_len);
    if (NT_SUCCESS(status)) found++;

    /* Enumerate index 2 should fail */
    status = ssdt_dispatch(SSDT_NtEnumerateKey,
                           (uint64_t)(uintptr_t)parent_handle,
                           2,
                           (uint64_t)KeyBasicInformation,
                           (uint64_t)(uintptr_t)enum_buf,
                           (uint64_t)sizeof(enum_buf),
                           (uint64_t)(uintptr_t)&result_len);
    TEST_ASSERT(status == STATUS_NO_MORE_ENTRIES,
                "NtEnumerateKey index 2 returns STATUS_NO_MORE_ENTRIES");

    TEST_ASSERT_EQ(found, 2, "NtEnumerateKey found 2 subkeys");

    /* Clean up: delete children then parent */
    if (child1 != INVALID_HANDLE_VALUE)
        ssdt_dispatch(SSDT_NtDeleteKey, (uint64_t)(uintptr_t)child1, 0, 0, 0, 0, 0);
    if (child2 != INVALID_HANDLE_VALUE)
        ssdt_dispatch(SSDT_NtDeleteKey, (uint64_t)(uintptr_t)child2, 0, 0, 0, 0, 0);
    ssdt_dispatch(SSDT_NtDeleteKey, (uint64_t)(uintptr_t)parent_handle, 0, 0, 0, 0, 0);
}

/* Test: NtQueryKey returns key metadata */
static void test_nt_query_key(void)
{
    OBJECT_ATTRIBUTES oa;
    UNICODE_STRING us;
    HANDLE key_handle = INVALID_HANDLE_VALUE;
    NTSTATUS status;
    uint8_t buf[128];
    uint32_t result_len = 0;

    /* Open SOFTWARE which has known subkeys (Classes, etc.) */
    build_oa(&oa, &us, "\\Registry\\Machine\\SOFTWARE");
    status = ssdt_dispatch(SSDT_NtOpenKey,
                           (uint64_t)(uintptr_t)&key_handle,
                           (uint64_t)KEY_ALL_ACCESS,
                           (uint64_t)(uintptr_t)&oa, 0, 0, 0);
    TEST_ASSERT(NT_SUCCESS(status), "NtOpenKey for query test");
    if (!NT_SUCCESS(status)) return;

    /* Query key full information */
    status = ssdt_dispatch(SSDT_NtQueryKey,
                           (uint64_t)(uintptr_t)key_handle,
                           (uint64_t)KeyFullInformation,
                           (uint64_t)(uintptr_t)buf,
                           (uint64_t)sizeof(buf),
                           (uint64_t)(uintptr_t)&result_len,
                           0);
    TEST_ASSERT(NT_SUCCESS(status), "NtQueryKey(KeyFullInformation) succeeds");

    if (NT_SUCCESS(status)) {
        KEY_FULL_INFORMATION *info = (KEY_FULL_INFORMATION *)buf;
        /* SOFTWARE should have at least Classes as a subkey */
        TEST_ASSERT(info->SubKeys >= 1, "NtQueryKey reports at least 1 subkey");
    }
}

/* Test: SSDT slots are registered (not stubs) */
static void test_nt_registry_ssdt_registered(void)
{
    const SSDT_TABLE *tbl = ssdt_get_table(SSDT_TABLE_MAIN);
    TEST_ASSERT(tbl != (const SSDT_TABLE *)0, "SSDT main table exists");
    if (!tbl) return;

    /* Verify all 10 registry slots are not the default stub */
    extern NTSTATUS ssdt_stub_not_implemented(uint64_t, uint64_t, uint64_t,
                                              uint64_t, uint64_t, uint64_t);
    extern int snprintf(char *buf, size_t size, const char *fmt, ...);
    static const struct { uint32_t svc; const char *name; } slots[] = {
        { SSDT_NtCreateKey,        "NtCreateKey" },
        { SSDT_NtOpenKey,          "NtOpenKey" },
        { SSDT_NtOpenKeyEx,        "NtOpenKeyEx" },
        { SSDT_NtDeleteKey,        "NtDeleteKey" },
        { SSDT_NtSetValueKey,      "NtSetValueKey" },
        { SSDT_NtQueryValueKey,    "NtQueryValueKey" },
        { SSDT_NtDeleteValueKey,   "NtDeleteValueKey" },
        { SSDT_NtEnumerateKey,     "NtEnumerateKey" },
        { SSDT_NtEnumerateValueKey,"NtEnumerateValueKey" },
        { SSDT_NtQueryKey,         "NtQueryKey" },
    };
    uint32_t i;
    char msg[96];
    for (i = 0; i < sizeof(slots) / sizeof(slots[0]); i++) {
        uint32_t idx = slots[i].svc & 0xFFF;
        snprintf(msg, sizeof(msg), "%s (0x%x) registered",
                 slots[i].name, (uint64_t)slots[i].svc);
        TEST_ASSERT(tbl->handlers[idx] != ssdt_stub_not_implemented, msg);
    }
}

/* ============================================================================
 * Advanced registry tests (flush, rename, unload, SSDT registration)
 * ============================================================================ */

/* Test: NtFlushKey accepts a valid handle and returns success */
static void test_nt_flush_key(void)
{
    OBJECT_ATTRIBUTES oa;
    UNICODE_STRING us;
    HANDLE key_handle = INVALID_HANDLE_VALUE;
    NTSTATUS status;

    /* Create a test key with a value so there's something dirty */
    build_oa(&oa, &us, "\\Registry\\Machine\\Software\\NtFlushTest");
    status = ssdt_dispatch(SSDT_NtCreateKey,
                           (uint64_t)(uintptr_t)&key_handle,
                           (uint64_t)KEY_ALL_ACCESS,
                           (uint64_t)(uintptr_t)&oa, 0, 0, 0);
    TEST_ASSERT(NT_SUCCESS(status), "NtCreateKey for flush test");
    if (!NT_SUCCESS(status)) return;

    /* NtFlushKey should succeed */
    status = ssdt_dispatch(SSDT_NtFlushKey,
                           (uint64_t)(uintptr_t)key_handle, 0, 0, 0, 0, 0);
    TEST_ASSERT(NT_SUCCESS(status), "NtFlushKey returns STATUS_SUCCESS");

    /* NtFlushKey with NULL handle should fail */
    status = ssdt_dispatch(SSDT_NtFlushKey, 0, 0, 0, 0, 0, 0);
    TEST_ASSERT(status == STATUS_INVALID_HANDLE,
                "NtFlushKey rejects NULL handle");

    /* Clean up */
    ssdt_dispatch(SSDT_NtDeleteKey, (uint64_t)(uintptr_t)key_handle,
                  0, 0, 0, 0, 0);
}

/* Test: NtRenameKey changes a key's name and keeps it addressable */
static void test_nt_rename_key(void)
{
    OBJECT_ATTRIBUTES oa;
    UNICODE_STRING us;
    UNICODE_STRING new_name_us;
    HANDLE key_handle = INVALID_HANDLE_VALUE;
    HANDLE verify_handle = INVALID_HANDLE_VALUE;
    NTSTATUS status;
    const char *new_name = "Renamed";

    /* Create key with original name */
    build_oa(&oa, &us, "\\Registry\\Machine\\Software\\NtRenameTest");
    status = ssdt_dispatch(SSDT_NtCreateKey,
                           (uint64_t)(uintptr_t)&key_handle,
                           (uint64_t)KEY_ALL_ACCESS,
                           (uint64_t)(uintptr_t)&oa, 0, 0, 0);
    TEST_ASSERT(NT_SUCCESS(status), "NtCreateKey for rename test");
    if (!NT_SUCCESS(status)) return;

    /* Rename it */
    new_name_us.Buffer = (uint16_t *)(uintptr_t)new_name;
    new_name_us.Length = 7;
    new_name_us.MaximumLength = 8;
    status = ssdt_dispatch(SSDT_NtRenameKey,
                           (uint64_t)(uintptr_t)key_handle,
                           (uint64_t)(uintptr_t)&new_name_us,
                           0, 0, 0, 0);
    TEST_ASSERT(NT_SUCCESS(status), "NtRenameKey succeeds");

    /* Verify new name is reachable */
    build_oa(&oa, &us, "\\Registry\\Machine\\Software\\Renamed");
    status = ssdt_dispatch(SSDT_NtOpenKey,
                           (uint64_t)(uintptr_t)&verify_handle,
                           (uint64_t)KEY_ALL_ACCESS,
                           (uint64_t)(uintptr_t)&oa, 0, 0, 0);
    TEST_ASSERT(NT_SUCCESS(status), "Renamed key reachable by new name");

    /* Verify old name is NOT reachable */
    {
        HANDLE dead = INVALID_HANDLE_VALUE;
        OBJECT_ATTRIBUTES oa2;
        UNICODE_STRING us2;
        build_oa(&oa2, &us2, "\\Registry\\Machine\\Software\\NtRenameTest");
        status = ssdt_dispatch(SSDT_NtOpenKey,
                               (uint64_t)(uintptr_t)&dead,
                               (uint64_t)KEY_ALL_ACCESS,
                               (uint64_t)(uintptr_t)&oa2, 0, 0, 0);
        TEST_ASSERT(status == STATUS_OBJECT_NAME_NOT_FOUND,
                    "Old name no longer resolves after rename");
    }

    /* Clean up: delete the renamed key */
    if (verify_handle != INVALID_HANDLE_VALUE)
        ssdt_dispatch(SSDT_NtDeleteKey, (uint64_t)(uintptr_t)verify_handle,
                      0, 0, 0, 0, 0);
}

/* Test: NtNotifyChangeKey returns STATUS_NOT_IMPLEMENTED (blocked on watcher roadmap) */
static void test_nt_notify_change_key_pending(void)
{
    NTSTATUS status = ssdt_dispatch(SSDT_NtNotifyChangeKey,
                                    0, 0, 0, 0, 0, 0);
    TEST_PENDING(status == STATUS_NOT_IMPLEMENTED,
                 "NtNotifyChangeKey: no change-notification engine yet");
}

/* Test: NtUnloadKey rejects a root key and unknown paths */
static void test_nt_unload_key_invalid(void)
{
    OBJECT_ATTRIBUTES oa;
    UNICODE_STRING us;
    NTSTATUS status;

    /* Unloading a root directly returns ACCESS_DENIED */
    build_oa(&oa, &us, "\\Registry\\Machine");
    status = ssdt_dispatch(SSDT_NtUnloadKey,
                           (uint64_t)(uintptr_t)&oa, 0, 0, 0, 0, 0);
    TEST_ASSERT(status == STATUS_ACCESS_DENIED,
                "NtUnloadKey rejects bare root (no subpath)");

    /* Unknown path returns OBJECT_NAME_NOT_FOUND */
    build_oa(&oa, &us, "\\Registry\\Machine\\NoSuchHive12345");
    status = ssdt_dispatch(SSDT_NtUnloadKey,
                           (uint64_t)(uintptr_t)&oa, 0, 0, 0, 0, 0);
    TEST_ASSERT(status == STATUS_OBJECT_NAME_NOT_FOUND,
                "NtUnloadKey rejects unknown key");
}

/* Test: all 10 advanced SSDT slots are registered (not stubs) */
static void test_nt_registry_advanced_registered(void)
{
    const SSDT_TABLE *tbl = ssdt_get_table(SSDT_TABLE_MAIN);
    extern NTSTATUS ssdt_stub_not_implemented(uint64_t, uint64_t, uint64_t,
                                              uint64_t, uint64_t, uint64_t);
    extern int snprintf(char *buf, size_t size, const char *fmt, ...);
    static const struct { uint32_t svc; const char *name; } slots[] = {
        { SSDT_NtFlushKey,        "NtFlushKey" },
        { SSDT_NtNotifyChangeKey, "NtNotifyChangeKey" },
        { SSDT_NtRenameKey,       "NtRenameKey" },
        { SSDT_NtSaveKey,         "NtSaveKey" },
        { SSDT_NtSaveKeyEx,       "NtSaveKeyEx" },
        { SSDT_NtRestoreKey,      "NtRestoreKey" },
        { SSDT_NtLoadKey,         "NtLoadKey" },
        { SSDT_NtLoadKeyEx,       "NtLoadKeyEx" },
        { SSDT_NtUnloadKey,       "NtUnloadKey" },
        { SSDT_NtUnloadKeyEx,     "NtUnloadKeyEx" },
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

/* Registration */
void test_register_registry(void)
{
    test_suite_register_cat("Registry: DWORD", test_registry_dword, TEST_CAT_ABI);
    test_suite_register_cat("Registry: string", test_registry_string, TEST_CAT_ABI);
    test_suite_register_cat("Registry: NtCreateKey", test_nt_create_key, TEST_CAT_ABI);
    test_suite_register_cat("Registry: NtOpenKey", test_nt_open_key, TEST_CAT_ABI);
    test_suite_register_cat("Registry: NtOpenKey not found", test_nt_open_key_not_found, TEST_CAT_ABI);
    test_suite_register_cat("Registry: NtSetValueKey+NtQueryValueKey", test_nt_set_query_value, TEST_CAT_ABI);
    test_suite_register_cat("Registry: NtEnumerateKey", test_nt_enumerate_key, TEST_CAT_ABI);
    test_suite_register_cat("Registry: NtQueryKey", test_nt_query_key, TEST_CAT_ABI);
    test_suite_register_cat("Registry: SSDT slots registered", test_nt_registry_ssdt_registered, TEST_CAT_ABI);
    test_suite_register_cat("Registry: NtFlushKey", test_nt_flush_key, TEST_CAT_ABI);
    test_suite_register_cat("Registry: NtRenameKey", test_nt_rename_key, TEST_CAT_ABI);
    test_suite_register_cat("Registry: NtNotifyChangeKey pending", test_nt_notify_change_key_pending, TEST_CAT_ABI);
    test_suite_register_cat("Registry: NtUnloadKey invalid", test_nt_unload_key_invalid, TEST_CAT_ABI);
    test_suite_register_cat("Registry: advanced SSDT registered", test_nt_registry_advanced_registered, TEST_CAT_ABI);
}

#endif /* KERNEL_TESTS */
