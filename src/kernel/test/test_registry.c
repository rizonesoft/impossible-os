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

/* Registry key lookup folds through the compiled invariant authority, so a
 * Latin-1 case pair (caf-e-acute vs CAF-E-acute) hashes into the same bucket and
 * matches -- not just ASCII A-Z. Guards the reg_fnv1a + reg_stricmp retrofit. */
static void test_registry_latin1_casefold(void)
{
    HKEY hKey = 0, hOpen = 0;
    long rc = RegCreateKeyEx(HKEY_LOCAL_MACHINE, "Software\\caf\xE9", 0, NULL, 0, 0,
                             NULL, &hKey, NULL);
    TEST_ASSERT(rc == 0, "create Software\\caf<e-acute>");
    /* Open with the UPPERCASE Latin-1 variant: the canonical fold must resolve
     * the same key (0xE9 e-acute folds to 0xC9 E-acute). */
    rc = RegOpenKeyEx(HKEY_LOCAL_MACHINE, "Software\\CAF\xC9", 0, 0, &hOpen);
    TEST_ASSERT(rc == 0, "open Software\\CAF<E-acute> resolves the same key");
    if (hOpen) RegCloseKey(hOpen);
    if (hKey) RegCloseKey(hKey);
}

/* Registration */
/* Section 1: KEY_* access-rights enforcement -- a KEY_READ handle may query
 * but not set/delete; a full handle may do both. */
static void test_registry_access_enforcement(void)
{
    HKEY hk;
    long rc;
    uint32_t val = 1, out = 0, cb;

    rc = RegCreateKeyEx(HKEY_LOCAL_MACHINE, "Software\\AccessTest", 0, NULL, 0,
                        KEY_ALL_ACCESS, NULL, &hk, NULL);
    TEST_ASSERT(rc == ERROR_SUCCESS, "create AccessTest (full access)");
    if (rc != ERROR_SUCCESS) return;
    rc = RegSetValueEx(hk, "V", 0, REG_DWORD, (const uint8_t *)&val, sizeof(val));
    TEST_ASSERT(rc == ERROR_SUCCESS, "full handle can set value");
    RegCloseKey(hk);

    /* Reopen read-only: query allowed, set/delete denied. */
    rc = RegOpenKeyEx(HKEY_LOCAL_MACHINE, "Software\\AccessTest", 0, KEY_READ, &hk);
    TEST_ASSERT(rc == ERROR_SUCCESS, "open AccessTest KEY_READ");
    if (rc != ERROR_SUCCESS) return;

    cb = sizeof(out);
    rc = RegQueryValueEx(hk, "V", NULL, NULL, (uint8_t *)&out, &cb);
    TEST_ASSERT(rc == ERROR_SUCCESS, "KEY_READ handle can query");
    TEST_ASSERT(out == 1, "queried value correct");

    val = 2;
    rc = RegSetValueEx(hk, "V", 0, REG_DWORD, (const uint8_t *)&val, sizeof(val));
    TEST_ASSERT(rc == ERROR_ACCESS_DENIED, "KEY_READ handle cannot set value");

    rc = RegDeleteValue(hk, "V");
    TEST_ASSERT(rc == ERROR_ACCESS_DENIED, "KEY_READ handle cannot delete value");

    /* RegDeleteTree from a read-only handle is denied before any deletion. */
    rc = RegDeleteTree(hk, "AnySub");
    TEST_ASSERT(rc == ERROR_ACCESS_DENIED, "KEY_READ handle cannot RegDeleteTree");

    /* No escalation: reopening the same key with KEY_ALL_ACCESS from a
     * KEY_READ handle yields a handle still capped to KEY_READ. */
    {
        HKEY hk2;
        uint32_t d = 3;
        rc = RegOpenKeyEx(hk, NULL, 0, KEY_ALL_ACCESS, &hk2);
        TEST_ASSERT(rc == ERROR_SUCCESS, "reopen from read-only handle");
        if (rc == ERROR_SUCCESS) {
            rc = RegSetValueEx(hk2, "V", 0, REG_DWORD, (const uint8_t *)&d, sizeof(d));
            TEST_ASSERT(rc == ERROR_ACCESS_DENIED,
                        "reopened handle capped to KEY_READ (no escalation)");
            RegCloseKey(hk2);
        }
    }
    RegCloseKey(hk);

    /* RegFlushKey requires KEY_QUERY_VALUE: a create-only handle is denied
     * before any persistence I/O (deterministic regardless of C: mount). */
    {
        HKEY hk3;
        rc = RegOpenKeyEx(HKEY_LOCAL_MACHINE, "Software\\AccessTest", 0,
                          KEY_CREATE_SUB_KEY, &hk3);
        if (rc == ERROR_SUCCESS) {
            rc = RegFlushKey(hk3);
            TEST_ASSERT(rc == ERROR_ACCESS_DENIED,
                        "RegFlushKey denied without KEY_QUERY_VALUE");
            RegCloseKey(hk3);
        }
    }

    /* GENERIC_READ maps to KEY_READ: the handle can query but not set. */
    {
        HKEY hk4;
        uint32_t d = 9;
        rc = RegOpenKeyEx(HKEY_LOCAL_MACHINE, "Software\\AccessTest", 0,
                          GENERIC_READ, &hk4);
        if (rc == ERROR_SUCCESS) {
            uint32_t out = 0, cb = sizeof(out);
            rc = RegQueryValueEx(hk4, "V", NULL, NULL, (uint8_t *)&out, &cb);
            TEST_ASSERT(rc == ERROR_SUCCESS, "GENERIC_READ handle can query");
            rc = RegSetValueEx(hk4, "V", 0, REG_DWORD, (const uint8_t *)&d, sizeof(d));
            TEST_ASSERT(rc == ERROR_ACCESS_DENIED, "GENERIC_READ handle cannot set");
            RegCloseKey(hk4);
        }
    }

    RegDeleteKey(HKEY_LOCAL_MACHINE, "Software\\AccessTest");
}

/* Section 1: API limits -- a key-name component over REG_MAX_KEY_NAME (255)
 * is rejected; 255 chars is accepted. */
static void test_registry_api_limits(void)
{
    HKEY hk;
    long rc;
    char name[300];
    uint32_t i;

    for (i = 0; i < 257; i++) name[i] = 'A';
    name[257] = '\0';
    rc = RegCreateKeyEx(HKEY_LOCAL_MACHINE, name, 0, NULL, 0, KEY_ALL_ACCESS,
                        NULL, &hk, NULL);
    TEST_ASSERT(rc == ERROR_INVALID_PARAMETER, "256+ char key name rejected (create)");

    /* Same limit enforced on the open path (not just create). */
    rc = RegOpenKeyEx(HKEY_LOCAL_MACHINE, name, 0, KEY_READ, &hk);
    TEST_ASSERT(rc == ERROR_INVALID_PARAMETER, "256+ char key name rejected (open)");

    name[255] = '\0';
    rc = RegCreateKeyEx(HKEY_LOCAL_MACHINE, name, 0, NULL, 0, KEY_ALL_ACCESS,
                        NULL, &hk, NULL);
    TEST_ASSERT(rc == ERROR_SUCCESS, "255-char key name accepted");
    if (rc == ERROR_SUCCESS) {
        RegCloseKey(hk);
        RegDeleteKey(HKEY_LOCAL_MACHINE, name);
    }
}

/* Section 1: reg_check_access chokepoint -- predefined roots grant implicit
 * full access; a NULL handle is invalid. */
static void test_reg_check_access_basic(void)
{
    TEST_ASSERT(reg_check_access(HKEY_LOCAL_MACHINE, KEY_ALL_ACCESS) == ERROR_SUCCESS,
                "predefined handle grants full access");
    TEST_ASSERT(reg_check_access((HKEY)0, KEY_QUERY_VALUE) == ERROR_INVALID_HANDLE,
                "NULL handle is invalid");
}

/* Section 2: RegCopyTree copies values + sub-keys recursively. */
static void test_registry_copytree(void)
{
    HKEY hsrc, hdst, hverify;
    long rc;
    uint32_t val = 77, out = 0, cb;

    rc = RegCreateKeyEx(HKEY_LOCAL_MACHINE, "Software\\CopySrc", 0, NULL, 0,
                        KEY_ALL_ACCESS, NULL, &hsrc, NULL);
    TEST_ASSERT(rc == ERROR_SUCCESS, "create CopySrc");
    if (rc != ERROR_SUCCESS) return;
    RegSetValueEx(hsrc, "SV", 0, REG_DWORD, (const uint8_t *)&val, sizeof(val));
    {
        HKEY hsub;
        if (RegCreateKeyEx(hsrc, "Child", 0, NULL, 0, KEY_ALL_ACCESS, NULL,
                           &hsub, NULL) == ERROR_SUCCESS)
            RegCloseKey(hsub);
    }

    rc = RegCreateKeyEx(HKEY_LOCAL_MACHINE, "Software\\CopyDst", 0, NULL, 0,
                        KEY_ALL_ACCESS, NULL, &hdst, NULL);
    TEST_ASSERT(rc == ERROR_SUCCESS, "create CopyDst");

    rc = RegCopyTree(hsrc, NULL, hdst);
    TEST_ASSERT(rc == ERROR_SUCCESS, "RegCopyTree src->dst");

    cb = sizeof(out);
    rc = RegQueryValueEx(hdst, "SV", NULL, NULL, (uint8_t *)&out, &cb);
    TEST_ASSERT(rc == ERROR_SUCCESS && out == 77, "copied value present");

    rc = RegOpenKeyEx(hdst, "Child", 0, KEY_READ, &hverify);
    TEST_ASSERT(rc == ERROR_SUCCESS, "copied subkey present");
    if (rc == ERROR_SUCCESS) RegCloseKey(hverify);

    /* Copying into a descendant of the source is rejected (no self-amplify). */
    {
        HKEY hdesc;
        if (RegOpenKeyEx(hsrc, "Child", 0, KEY_ALL_ACCESS, &hdesc) == ERROR_SUCCESS) {
            rc = RegCopyTree(hsrc, NULL, hdesc);
            TEST_ASSERT(rc == ERROR_INVALID_PARAMETER, "copy into own descendant rejected");
            RegCloseKey(hdesc);
        }
    }

    /* Copying a subtree into one of its ANCESTORS is rejected: a name collision
     * on the ancestor->source path would otherwise alias a source node. */
    {
        HKEY hanc, hleaf;
        if (RegCreateKeyEx(HKEY_LOCAL_MACHINE, "Software\\Anc\\Mid\\Leaf", 0, NULL,
                           0, KEY_ALL_ACCESS, NULL, &hleaf, NULL) == ERROR_SUCCESS) {
            if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, "Software\\Anc", 0, KEY_ALL_ACCESS,
                             &hanc) == ERROR_SUCCESS) {
                rc = RegCopyTree(hleaf, NULL, hanc);
                TEST_ASSERT(rc == ERROR_INVALID_PARAMETER, "copy into own ancestor rejected");
                RegCloseKey(hanc);
            }
            RegCloseKey(hleaf);
        }
        RegDeleteTree(HKEY_LOCAL_MACHINE, "Software\\Anc");
    }

    RegCloseKey(hsrc);
    RegCloseKey(hdst);
    RegDeleteTree(HKEY_LOCAL_MACHINE, "Software\\CopySrc");
    RegDeleteTree(HKEY_LOCAL_MACHINE, "Software\\CopyDst");
}

/* Section 2: RegRenameKey (in-place) + ERROR_ALREADY_EXISTS collision. */
static void test_registry_rename(void)
{
    HKEY hk, hv;
    long rc;
    uint32_t val = 55, out = 0, cb;

    rc = RegCreateKeyEx(HKEY_LOCAL_MACHINE, "Software\\RenOld", 0, NULL, 0,
                        KEY_ALL_ACCESS, NULL, &hk, NULL);
    TEST_ASSERT(rc == ERROR_SUCCESS, "create RenOld");
    if (rc != ERROR_SUCCESS) return;
    RegSetValueEx(hk, "RV", 0, REG_DWORD, (const uint8_t *)&val, sizeof(val));
    RegCloseKey(hk);

    rc = RegRenameKey(HKEY_LOCAL_MACHINE, "Software\\RenOld", "RenNew");
    TEST_ASSERT(rc == ERROR_SUCCESS, "RegRenameKey RenOld->RenNew");

    rc = RegOpenKeyEx(HKEY_LOCAL_MACHINE, "Software\\RenOld", 0, KEY_READ, &hv);
    TEST_ASSERT(rc == ERROR_FILE_NOT_FOUND, "old name gone after rename");
    rc = RegOpenKeyEx(HKEY_LOCAL_MACHINE, "Software\\RenNew", 0, KEY_READ, &hv);
    TEST_ASSERT(rc == ERROR_SUCCESS, "new name present after rename");
    if (rc == ERROR_SUCCESS) {
        cb = sizeof(out);
        rc = RegQueryValueEx(hv, "RV", NULL, NULL, (uint8_t *)&out, &cb);
        TEST_ASSERT(rc == ERROR_SUCCESS && out == 55, "renamed key retains value");
        RegCloseKey(hv);
    }

    {
        HKEY hd;
        if (RegCreateKeyEx(HKEY_LOCAL_MACHINE, "Software\\RenDup", 0, NULL, 0,
                           KEY_ALL_ACCESS, NULL, &hd, NULL) == ERROR_SUCCESS)
            RegCloseKey(hd);
    }
    rc = RegRenameKey(HKEY_LOCAL_MACHINE, "Software\\RenNew", "RenDup");
    TEST_ASSERT(rc == ERROR_ALREADY_EXISTS, "rename to existing name -> ALREADY_EXISTS");

    /* Renaming a key to its own current name is a no-op success, not a collision. */
    rc = RegRenameKey(HKEY_LOCAL_MACHINE, "Software\\RenNew", "RenNew");
    TEST_ASSERT(rc == ERROR_SUCCESS, "same-name rename is a no-op success");

    RegDeleteKey(HKEY_LOCAL_MACHINE, "Software\\RenNew");
    RegDeleteKey(HKEY_LOCAL_MACHINE, "Software\\RenDup");
}

/* Section 2: REG_OPTION_VOLATILE create + RegFlushKey no-op success. */
static void test_registry_volatile(void)
{
    HKEY hvol, hperm, hre;
    long rc;

    rc = RegCreateKeyEx(HKEY_LOCAL_MACHINE, "Software\\VolKey", 0, NULL,
                        REG_OPTION_VOLATILE, KEY_ALL_ACCESS, NULL, &hvol, NULL);
    TEST_ASSERT(rc == ERROR_SUCCESS, "create volatile key");
    if (rc != ERROR_SUCCESS) return;

    /* RegFlushKey on a volatile key is a no-op success (no hive backing). */
    rc = RegFlushKey(hvol);
    TEST_ASSERT(rc == ERROR_SUCCESS, "RegFlushKey(volatile) -> success no-op");

    /* A non-volatile sibling created without the flag is persistent; both are
     * openable while the volatile one lives in RAM. */
    rc = RegCreateKeyEx(HKEY_LOCAL_MACHINE, "Software\\PermKey", 0, NULL,
                        REG_OPTION_NON_VOLATILE, KEY_ALL_ACCESS, NULL, &hperm, NULL);
    TEST_ASSERT(rc == ERROR_SUCCESS, "create persistent sibling");

    rc = RegOpenKeyEx(HKEY_LOCAL_MACHINE, "Software\\VolKey", 0, KEY_READ, &hre);
    TEST_ASSERT(rc == ERROR_SUCCESS, "volatile key openable this boot");
    if (rc == ERROR_SUCCESS) RegCloseKey(hre);

    RegCloseKey(hvol);
    RegCloseKey(hperm);
    RegDeleteKey(HKEY_LOCAL_MACHINE, "Software\\VolKey");
    RegDeleteKey(HKEY_LOCAL_MACHINE, "Software\\PermKey");
    /* Reboot-absence of the volatile key is a bare-metal serial-log check
     * (hive round-trip needs a mounted C: volume); excluded is verified in
     * hive_count / hive_serialize_key by construction (REG_FLAG_VOLATILE). */
}

/* Section 2: RegSaveKey / RegRestoreKey fail closed and distinguish a missing
 * privilege (ERROR_PRIVILEGE_NOT_HELD) from a handle-access failure. */
static void test_registry_save_restore_failclosed(void)
{
    HKEY hfull, hnoacc;
    long rc;

    rc = RegCreateKeyEx(HKEY_LOCAL_MACHINE, "Software\\SaveMe", 0, NULL, 0,
                        KEY_ALL_ACCESS, NULL, &hfull, NULL);
    TEST_ASSERT(rc == ERROR_SUCCESS, "create SaveMe");
    if (rc != ERROR_SUCCESS) return;

    /* Full-access handle: fails on the privilege gate, NOT on access. */
    rc = RegSaveKey(hfull, "C:\\save.hive", NULL);
    TEST_ASSERT(rc == ERROR_PRIVILEGE_NOT_HELD,
                "RegSaveKey w/ access but no SeBackup -> PRIVILEGE_NOT_HELD");
    rc = RegRestoreKey(hfull, "C:\\save.hive", 0);
    TEST_ASSERT(rc == ERROR_PRIVILEGE_NOT_HELD,
                "RegRestoreKey w/ access but no SeRestore -> PRIVILEGE_NOT_HELD");

    /* Read-only handle: RegRestoreKey needs KEY_WRITE, so the access-mask
     * branch fires first -> ACCESS_DENIED, distinct from the privilege path. */
    rc = RegOpenKeyEx(HKEY_LOCAL_MACHINE, "Software\\SaveMe", 0, KEY_READ, &hnoacc);
    TEST_ASSERT(rc == ERROR_SUCCESS, "reopen SaveMe read-only");
    if (rc == ERROR_SUCCESS) {
        rc = RegRestoreKey(hnoacc, "C:\\save.hive", 0);
        TEST_ASSERT(rc == ERROR_ACCESS_DENIED,
                    "RegRestoreKey w/o KEY_WRITE -> ACCESS_DENIED (mask branch)");
        RegCloseKey(hnoacc);
    }

    RegCloseKey(hfull);
    RegDeleteKey(HKEY_LOCAL_MACHINE, "Software\\SaveMe");
}

/* Section 2: KCB cache -- a close/reopen tight loop must resolve via the cache
 * on > 90% of hops after warmup. */
static void test_registry_kcb_hitrate(void)
{
    HKEY hbase, hk;
    long rc;
    uint64_t h0 = 0, m0 = 0, h1 = 0, m1 = 0, hits, misses;
    int i;

    /* Open a stable base handle, then repeatedly open/close a single-component
     * child under it -- the hot HKLM\SYSTEM\Display-style pattern. */
    rc = RegCreateKeyEx(HKEY_LOCAL_MACHINE, "Software\\KcbBase", 0, NULL, 0,
                        KEY_ALL_ACCESS, NULL, &hbase, NULL);
    TEST_ASSERT(rc == ERROR_SUCCESS, "create KcbBase");
    if (rc != ERROR_SUCCESS) return;
    if (RegCreateKeyEx(hbase, "Hot", 0, NULL, 0, KEY_ALL_ACCESS, NULL, &hk, NULL)
        == ERROR_SUCCESS)
        RegCloseKey(hk);

    /* Warm the cache once so the first cold miss is not counted. */
    if (RegOpenKeyEx(hbase, "Hot", 0, KEY_READ, &hk) == ERROR_SUCCESS)
        RegCloseKey(hk);

    reg_kcb_get_stats(&h0, &m0);
    for (i = 0; i < 1000; i++) {
        if (RegOpenKeyEx(hbase, "Hot", 0, KEY_READ, &hk) == ERROR_SUCCESS)
            RegCloseKey(hk);
    }
    reg_kcb_get_stats(&h1, &m1);

    hits   = h1 - h0;
    misses = m1 - m0;
    TEST_ASSERT(hits + misses >= 1000, "KCB counters advanced over the loop");
    /* > 90% hit rate: hits*10 > (hits+misses)*9. */
    TEST_ASSERT(hits * 10 > (hits + misses) * 9,
                "KCB hot-key hit rate exceeds 90%");

    RegCloseKey(hbase);
    RegDeleteTree(HKEY_LOCAL_MACHINE, "Software\\KcbBase");
}

/* Section 2: RegRenameKey rejects a new name containing a path separator (a
 * literal '\' would corrupt the namespace -- unreachable/undeletable by name). */
static void test_registry_rename_separator(void)
{
    HKEY hk;
    long rc;

    rc = RegCreateKeyEx(HKEY_LOCAL_MACHINE, "Software\\SepRen", 0, NULL, 0,
                        KEY_ALL_ACCESS, NULL, &hk, NULL);
    TEST_ASSERT(rc == ERROR_SUCCESS, "create SepRen");
    if (rc == ERROR_SUCCESS) RegCloseKey(hk);

    rc = RegRenameKey(HKEY_LOCAL_MACHINE, "Software\\SepRen", "Bad\\Name");
    TEST_ASSERT(rc == ERROR_INVALID_PARAMETER, "rename to name with '\\' rejected");

    /* A hive root (direct child of HKLM such as SYSTEM/SOFTWARE) cannot be
     * renamed -- the hive table maps it by fixed name. */
    rc = RegRenameKey(HKEY_LOCAL_MACHINE, "SYSTEM", "SYSTEM_RENAMED");
    TEST_ASSERT(rc == ERROR_ACCESS_DENIED, "rename of hive root SYSTEM rejected");

    /* The HKCR backing key HKLM\SOFTWARE\Classes cannot be renamed either --
     * reg_resolve_hkcr finds it by name, so a rename would break HKCR. */
    rc = RegRenameKey(HKEY_LOCAL_MACHINE, "SOFTWARE\\Classes", "Classes2");
    TEST_ASSERT(rc == ERROR_ACCESS_DENIED, "rename of HKLM\\SOFTWARE\\Classes rejected");
    {
        /* HKCR still resolves after the rejected rename. */
        HKEY hcr;
        rc = RegCreateKeyEx(HKEY_CLASSES_ROOT, "SepRenHkcrProbe", 0, NULL, 0,
                            KEY_ALL_ACCESS, NULL, &hcr, NULL);
        TEST_ASSERT(rc == ERROR_SUCCESS, "HKCR still resolves after Classes rename attempt");
        if (rc == ERROR_SUCCESS) RegCloseKey(hcr);
        RegDeleteTree(HKEY_CLASSES_ROOT, "SepRenHkcrProbe");
    }

    RegDeleteKey(HKEY_LOCAL_MACHINE, "Software\\SepRen");
}

/* Section 2: NtRenameKey reports a sibling-name collision as a collision
 * (STATUS_OBJECT_NAME_COLLISION), not STATUS_ACCESS_DENIED. */
static void test_nt_rename_key_collision(void)
{
    OBJECT_ATTRIBUTES oa;
    UNICODE_STRING us;
    UNICODE_STRING new_name_us;
    HANDLE ha = INVALID_HANDLE_VALUE, hb = INVALID_HANDLE_VALUE;
    NTSTATUS status;
    const char *new_name = "NtRenColB";

    build_oa(&oa, &us, "\\Registry\\Machine\\Software\\NtRenColA");
    status = ssdt_dispatch(SSDT_NtCreateKey, (uint64_t)(uintptr_t)&ha,
                           (uint64_t)KEY_ALL_ACCESS, (uint64_t)(uintptr_t)&oa, 0, 0, 0);
    TEST_ASSERT(NT_SUCCESS(status), "NtCreateKey A");
    if (!NT_SUCCESS(status)) return;

    build_oa(&oa, &us, "\\Registry\\Machine\\Software\\NtRenColB");
    status = ssdt_dispatch(SSDT_NtCreateKey, (uint64_t)(uintptr_t)&hb,
                           (uint64_t)KEY_ALL_ACCESS, (uint64_t)(uintptr_t)&oa, 0, 0, 0);
    TEST_ASSERT(NT_SUCCESS(status), "NtCreateKey B");

    /* Rename A -> "NtRenColB": a different sibling already holds that name. */
    new_name_us.Buffer = (uint16_t *)(uintptr_t)new_name;
    new_name_us.Length = 9;
    new_name_us.MaximumLength = 10;
    status = ssdt_dispatch(SSDT_NtRenameKey, (uint64_t)(uintptr_t)ha,
                           (uint64_t)(uintptr_t)&new_name_us, 0, 0, 0, 0);
    TEST_ASSERT(status == STATUS_OBJECT_NAME_COLLISION,
                "NtRenameKey sibling collision -> OBJECT_NAME_COLLISION");

    if (ha != INVALID_HANDLE_VALUE)
        ssdt_dispatch(SSDT_NtDeleteKey, (uint64_t)(uintptr_t)ha, 0, 0, 0, 0, 0);
    if (hb != INVALID_HANDLE_VALUE)
        ssdt_dispatch(SSDT_NtDeleteKey, (uint64_t)(uintptr_t)hb, 0, 0, 0, 0, 0);
}

/* ---- Section 3: change-notification callback engine ---- */

static uint32_t s_notify_fires;
static uint32_t s_notify_last_change;
static char     s_notify_last_value[64];

static void notify_test_cb(const char *key_path, uint32_t change_type,
                           const char *value_name, void *ctx)
{
    (void)key_path; (void)ctx;
    s_notify_fires++;
    s_notify_last_change = change_type;
    s_notify_last_value[0] = '\0';
    if (value_name) {
        uint32_t i = 0;
        while (value_name[i] && i < sizeof(s_notify_last_value) - 1) {
            s_notify_last_value[i] = value_name[i];
            i++;
        }
        s_notify_last_value[i] = '\0';
    }
}

static int notify_streq(const char *a, const char *b)
{
    uint32_t i = 0;
    while (a[i] && b[i]) { if (a[i] != b[i]) return 0; i++; }
    return a[i] == b[i];
}

/* Basic value-change notification + value_name delivery + unregister. */
static void test_registry_notify_basic(void)
{
    HKEY hk;
    uint64_t id; uint32_t val = 1920;
    long rc;

    rc = RegCreateKeyEx(HKEY_LOCAL_MACHINE, "Software\\NotifyBasic", 0, NULL, 0,
                        KEY_ALL_ACCESS, NULL, &hk, NULL);
    TEST_ASSERT(rc == ERROR_SUCCESS, "create NotifyBasic");
    if (rc != ERROR_SUCCESS) return;

    s_notify_fires = 0;
    id = reg_notify_register(hk, REG_NOTIFY_CHANGE_LAST_SET, 0, notify_test_cb, NULL, 0);
    TEST_ASSERT(id != 0, "reg_notify_register returns a valid id");

    RegSetValueEx(hk, "Width", 0, REG_DWORD, (const uint8_t *)&val, sizeof(val));
    TEST_ASSERT(s_notify_fires == 1, "LAST_SET watcher fired once on RegSetValueEx");
    TEST_ASSERT(s_notify_last_change == REG_NOTIFY_CHANGE_LAST_SET, "change_type is LAST_SET");
    TEST_ASSERT(notify_streq(s_notify_last_value, "Width"), "value_name delivered as Width");

    /* A NAME-only filter must NOT fire on a value change. */
    rc = RegUnregisterNotify(id);
    TEST_ASSERT(rc == ERROR_SUCCESS, "RegUnregisterNotify succeeds");
    s_notify_fires = 0;
    RegSetValueEx(hk, "Width", 0, REG_DWORD, (const uint8_t *)&val, sizeof(val));
    TEST_ASSERT(s_notify_fires == 0, "no fire after unregister");

    RegCloseKey(hk);
    RegDeleteTree(HKEY_LOCAL_MACHINE, "Software\\NotifyBasic");
}

/* Subtree vs non-subtree scope; reg_is_descendant primitive. */
static void test_registry_notify_subtree(void)
{
    HKEY hpar, hchild;
    uint64_t idsub, idflat; uint32_t val = 7;

    if (RegCreateKeyEx(HKEY_LOCAL_MACHINE, "Software\\NotifyPar", 0, NULL, 0,
                       KEY_ALL_ACCESS, NULL, &hpar, NULL) != ERROR_SUCCESS)
        return;
    if (RegCreateKeyEx(hpar, "Child", 0, NULL, 0, KEY_ALL_ACCESS, NULL,
                       &hchild, NULL) != ERROR_SUCCESS) { RegCloseKey(hpar); return; }

    idsub  = reg_notify_register(hpar, REG_NOTIFY_CHANGE_LAST_SET, 1, notify_test_cb, NULL, 0);
    /* A second, non-subtree watcher on the parent must not fire for a child change. */
    idflat = reg_notify_register(hpar, REG_NOTIFY_CHANGE_LAST_SET, 0, notify_test_cb, NULL, 0);
    TEST_ASSERT(idsub != 0 && idflat != 0, "both watchers registered");

    s_notify_fires = 0;
    RegSetValueEx(hchild, "V", 0, REG_DWORD, (const uint8_t *)&val, sizeof(val));
    TEST_ASSERT(s_notify_fires == 1, "only the subtree watcher fires for a child change");

    RegUnregisterNotify(idsub);
    RegUnregisterNotify(idflat);
    RegCloseKey(hchild);
    RegCloseKey(hpar);
    RegDeleteTree(HKEY_LOCAL_MACHINE, "Software\\NotifyPar");
}

/* Coalescing suppresses a rapid second fire within the window. */
static void test_registry_notify_coalesce(void)
{
    HKEY hk;
    uint64_t id; uint32_t val = 0;

    if (RegCreateKeyEx(HKEY_LOCAL_MACHINE, "Software\\NotifyCoal", 0, NULL, 0,
                       KEY_ALL_ACCESS, NULL, &hk, NULL) != ERROR_SUCCESS)
        return;
    /* Very large window so the two back-to-back writes land in one window. */
    id = reg_notify_register(hk, REG_NOTIFY_CHANGE_LAST_SET, 0, notify_test_cb, NULL,
                             1000000000ULL);
    TEST_ASSERT(id != 0, "coalescing watcher registered");
    s_notify_fires = 0;
    RegSetValueEx(hk, "A", 0, REG_DWORD, (const uint8_t *)&val, sizeof(val));
    RegSetValueEx(hk, "A", 0, REG_DWORD, (const uint8_t *)&val, sizeof(val));
    TEST_ASSERT(s_notify_fires == 1, "coalesced: two rapid writes fire once");

    RegUnregisterNotify(id);
    RegCloseKey(hk);
    RegDeleteTree(HKEY_LOCAL_MACHINE, "Software\\NotifyCoal");
}

/* NAME notification on sub-key create/delete; cleanup on delete leaves no fire. */
static void test_registry_notify_name(void)
{
    HKEY hpar, hchild;
    uint64_t id;

    if (RegCreateKeyEx(HKEY_LOCAL_MACHINE, "Software\\NotifyName", 0, NULL, 0,
                       KEY_ALL_ACCESS, NULL, &hpar, NULL) != ERROR_SUCCESS)
        return;
    id = reg_notify_register(hpar, REG_NOTIFY_CHANGE_NAME, 0, notify_test_cb, NULL, 0);
    TEST_ASSERT(id != 0, "NAME watcher registered");

    s_notify_fires = 0;
    if (RegCreateKeyEx(hpar, "Sub", 0, NULL, 0, KEY_ALL_ACCESS, NULL, &hchild, NULL)
        == ERROR_SUCCESS)
        RegCloseKey(hchild);
    TEST_ASSERT(s_notify_fires == 1, "NAME watcher fires on sub-key create");
    TEST_ASSERT(s_notify_last_change == REG_NOTIFY_CHANGE_NAME, "change_type is NAME");

    s_notify_fires = 0;
    RegDeleteKey(hpar, "Sub");
    TEST_ASSERT(s_notify_fires == 1, "NAME watcher fires on sub-key delete");

    RegUnregisterNotify(id);
    RegCloseKey(hpar);
    RegDeleteTree(HKEY_LOCAL_MACHINE, "Software\\NotifyName");
}

/* Deleting the watched key tears down its watchers (no fire, no stale slot). */
static void test_registry_notify_cleanup_on_delete(void)
{
    HKEY hpar, hk;
    uint64_t id; uint32_t val = 1;

    if (RegCreateKeyEx(HKEY_LOCAL_MACHINE, "Software\\NotifyDel", 0, NULL, 0,
                       KEY_ALL_ACCESS, NULL, &hpar, NULL) != ERROR_SUCCESS)
        return;
    if (RegCreateKeyEx(hpar, "Victim", 0, NULL, 0, KEY_ALL_ACCESS, NULL, &hk, NULL)
        != ERROR_SUCCESS) { RegCloseKey(hpar); return; }

    id = reg_notify_register(hk, REG_NOTIFY_CHANGE_LAST_SET, 0, notify_test_cb, NULL, 0);
    TEST_ASSERT(id != 0, "victim watcher registered");
    RegCloseKey(hk);

    RegDeleteKey(hpar, "Victim");
    /* The watcher was torn down by the delete; unregister now reports not-found. */
    TEST_ASSERT(RegUnregisterNotify(id) == ERROR_FILE_NOT_FOUND,
                "watcher slot freed by key delete");

    RegCloseKey(hpar);
    RegDeleteTree(HKEY_LOCAL_MACHINE, "Software\\NotifyDel");
    (void)val;
}

/* Deep-path notification: a deep key must build its callback path iteratively
 * (no per-level recursion) without exhausting the kernel stack. */
static void test_registry_notify_deep_path(void)
{
    HKEY hk;
    uint64_t id; uint32_t val = 3;
    /* ~40 components -- a recursive path builder would risk the kernel stack. */
    const char *deep =
        "Software\\d0\\d1\\d2\\d3\\d4\\d5\\d6\\d7\\d8\\d9\\d10\\d11\\d12\\d13"
        "\\d14\\d15\\d16\\d17\\d18\\d19\\d20\\d21\\d22\\d23\\d24\\d25\\d26\\d27"
        "\\d28\\d29\\d30\\d31\\d32\\d33\\d34\\d35\\d36\\d37\\d38";

    if (RegCreateKeyEx(HKEY_LOCAL_MACHINE, deep, 0, NULL, 0, KEY_ALL_ACCESS,
                       NULL, &hk, NULL) != ERROR_SUCCESS)
        return;
    id = reg_notify_register(hk, REG_NOTIFY_CHANGE_LAST_SET, 0, notify_test_cb, NULL, 0);
    TEST_ASSERT(id != 0, "deep-key watcher registered");
    s_notify_fires = 0;
    RegSetValueEx(hk, "V", 0, REG_DWORD, (const uint8_t *)&val, sizeof(val));
    TEST_ASSERT(s_notify_fires == 1, "deep-path notification fires without stack overflow");

    RegUnregisterNotify(id);
    RegCloseKey(hk);
    RegDeleteTree(HKEY_LOCAL_MACHINE, "Software\\d0");
}

/* Re-entrant callback that writes the watched key -- must terminate (bounded by
 * the dispatch depth guard), never recurse the stack to death. */
static HKEY  s_reentrant_key;
static void notify_reentrant_cb(const char *key_path, uint32_t change_type,
                                const char *value_name, void *ctx)
{
    uint32_t v = 1;
    (void)key_path; (void)change_type; (void)value_name; (void)ctx;
    s_notify_fires++;
    /* Write the watched key again -- this re-enters reg_dispatch_notify. */
    RegSetValueEx(s_reentrant_key, "R", 0, REG_DWORD, (const uint8_t *)&v, sizeof(v));
}

static void test_registry_notify_reentrant(void)
{
    HKEY hk;
    uint64_t id; uint32_t val = 1;

    if (RegCreateKeyEx(HKEY_LOCAL_MACHINE, "Software\\NotifyReent", 0, NULL, 0,
                       KEY_ALL_ACCESS, NULL, &hk, NULL) != ERROR_SUCCESS)
        return;
    s_reentrant_key = hk;
    id = reg_notify_register(hk, REG_NOTIFY_CHANGE_LAST_SET, 0, notify_reentrant_cb, NULL, 0);
    TEST_ASSERT(id != 0, "reentrant watcher registered");
    s_notify_fires = 0;
    RegSetValueEx(hk, "R", 0, REG_DWORD, (const uint8_t *)&val, sizeof(val));
    /* Bounded by REG_NOTIFY_MAX_DEPTH (4) -- terminated, did not hang. */
    TEST_ASSERT(s_notify_fires >= 1 && s_notify_fires <= 4,
                "reentrant callback terminates within the depth guard");

    RegUnregisterNotify(id);
    RegCloseKey(hk);
    RegDeleteTree(HKEY_LOCAL_MACHINE, "Software\\NotifyReent");
}

/* Multi-component create: a NAME watcher on an EXISTING parent must see the
 * first new component created directly under it (not only the deepest parent). */
static void test_registry_notify_multicomponent(void)
{
    HKEY hpar, hk;
    uint64_t id;

    if (RegCreateKeyEx(HKEY_LOCAL_MACHINE, "Software\\NotifyMC", 0, NULL, 0,
                       KEY_ALL_ACCESS, NULL, &hpar, NULL) != ERROR_SUCCESS)
        return;
    id = reg_notify_register(hpar, REG_NOTIFY_CHANGE_NAME, 0, notify_test_cb, NULL, 0);
    TEST_ASSERT(id != 0, "multi-component NAME watcher registered");
    s_notify_fires = 0;
    /* Creates A (under NotifyMC), then B under A, then C under B. */
    if (RegCreateKeyEx(hpar, "A\\B\\C", 0, NULL, 0, KEY_ALL_ACCESS, NULL, &hk, NULL)
        == ERROR_SUCCESS)
        RegCloseKey(hk);
    TEST_ASSERT(s_notify_fires >= 1,
                "parent NAME watcher sees the first new component of a deep create");

    RegUnregisterNotify(id);
    RegCloseKey(hpar);
    RegDeleteTree(HKEY_LOCAL_MACHINE, "Software\\NotifyMC");
}

/* RegDeleteTree (recursive delete API) must fire NAME on the affected parent. */
static void test_registry_notify_deletetree(void)
{
    HKEY hpar, hk;
    uint64_t idsub, idflat;

    if (RegCreateKeyEx(HKEY_LOCAL_MACHINE, "Software\\NotifyDT", 0, NULL, 0,
                       KEY_ALL_ACCESS, NULL, &hpar, NULL) != ERROR_SUCCESS)
        return;
    if (RegCreateKeyEx(hpar, "Sub\\Leaf", 0, NULL, 0, KEY_ALL_ACCESS, NULL, &hk, NULL)
        == ERROR_SUCCESS)
        RegCloseKey(hk);

    /* Subtree watcher on the parent sees a RegDeleteTree(parent, "Sub"). */
    idsub = reg_notify_register(hpar, REG_NOTIFY_CHANGE_NAME, 1, notify_test_cb, NULL, 0);
    TEST_ASSERT(idsub != 0, "deletetree subtree watcher registered");
    s_notify_fires = 0;
    RegDeleteTree(hpar, "Sub");
    TEST_ASSERT(s_notify_fires >= 1, "RegDeleteTree(parent, subkey) fires NAME");
    RegUnregisterNotify(idsub);

    /* Non-subtree watcher on the parent sees a RegDeleteTree(parent, NULL) clear. */
    if (RegCreateKeyEx(hpar, "C1", 0, NULL, 0, KEY_ALL_ACCESS, NULL, &hk, NULL)
        == ERROR_SUCCESS)
        RegCloseKey(hk);
    idflat = reg_notify_register(hpar, REG_NOTIFY_CHANGE_NAME, 0, notify_test_cb, NULL, 0);
    s_notify_fires = 0;
    RegDeleteTree(hpar, NULL);
    TEST_ASSERT(s_notify_fires >= 1, "RegDeleteTree(parent, NULL) fires NAME on the clear");
    RegUnregisterNotify(idflat);

    RegCloseKey(hpar);
    RegDeleteTree(HKEY_LOCAL_MACHINE, "Software\\NotifyDT");
}

/* Rename fires NAME on the parent. */
static void test_registry_notify_rename(void)
{
    HKEY hpar, hk;
    uint64_t id;

    if (RegCreateKeyEx(HKEY_LOCAL_MACHINE, "Software\\NotifyRen", 0, NULL, 0,
                       KEY_ALL_ACCESS, NULL, &hpar, NULL) != ERROR_SUCCESS)
        return;
    if (RegCreateKeyEx(hpar, "Old", 0, NULL, 0, KEY_ALL_ACCESS, NULL, &hk, NULL)
        == ERROR_SUCCESS)
        RegCloseKey(hk);
    id = reg_notify_register(hpar, REG_NOTIFY_CHANGE_NAME, 0, notify_test_cb, NULL, 0);
    TEST_ASSERT(id != 0, "rename NAME watcher registered");
    s_notify_fires = 0;
    RegRenameKey(hpar, "Old", "New");
    TEST_ASSERT(s_notify_fires >= 1, "RegRenameKey fires NAME on the parent");

    RegUnregisterNotify(id);
    RegCloseKey(hpar);
    RegDeleteTree(HKEY_LOCAL_MACHINE, "Software\\NotifyRen");
}

/* A callback that unregisters the next watcher AND re-registers a replacement on
 * the same key (pool-slot reuse) must not corrupt the walk or spin. */
static uint64_t s_mutation_victim;
static HKEY     s_mutation_key;
static uint64_t s_mutation_readd;
static void notify_mutation_cb(const char *key_path, uint32_t change_type,
                               const char *value_name, void *ctx)
{
    (void)key_path; (void)change_type; (void)value_name; (void)ctx;
    s_notify_fires++;
    if (s_mutation_victim) {
        /* Unregister the later sibling, then re-register on the SAME key -- the
         * new watcher reuses the freed pool slot but gets a new watcher_id. */
        RegUnregisterNotify(s_mutation_victim);
        s_mutation_victim = 0;
        s_mutation_readd = reg_notify_register(s_mutation_key, REG_NOTIFY_CHANGE_LAST_SET,
                                               0, notify_test_cb, NULL, 0);
    }
}

static void test_registry_notify_callback_mutation(void)
{
    HKEY hk;
    uint64_t idvictim, idmut; uint32_t val = 1;

    if (RegCreateKeyEx(HKEY_LOCAL_MACHINE, "Software\\NotifyMut", 0, NULL, 0,
                       KEY_ALL_ACCESS, NULL, &hk, NULL) != ERROR_SUCCESS)
        return;
    /* Register the victim FIRST, the mutating watcher SECOND: head-insertion
     * makes the mutating watcher fire first and remove/replace the victim. */
    idvictim = reg_notify_register(hk, REG_NOTIFY_CHANGE_LAST_SET, 0, notify_test_cb, NULL, 0);
    idmut    = reg_notify_register(hk, REG_NOTIFY_CHANGE_LAST_SET, 0, notify_mutation_cb, NULL, 0);
    TEST_ASSERT(idvictim != 0 && idmut != 0, "mutation-test watchers registered");
    s_mutation_victim = idvictim;
    s_mutation_key    = hk;
    s_mutation_readd  = 0;
    s_notify_fires    = 0;
    RegSetValueEx(hk, "V", 0, REG_DWORD, (const uint8_t *)&val, sizeof(val));
    /* Exactly one fire: only the mutating watcher runs; the removed victim never
     * fires, and the same-key replacement (new id) is NOT followed/fired during
     * this dispatch -- the walk stops safely at the reused slot. */
    TEST_ASSERT(s_notify_fires == 1,
                "callback unregister+same-key re-register fires once, no wrong/duplicate watcher");

    if (s_mutation_readd) RegUnregisterNotify(s_mutation_readd);
    RegUnregisterNotify(idmut);
    RegCloseKey(hk);
    RegDeleteTree(HKEY_LOCAL_MACHINE, "Software\\NotifyMut");
}

/* RegDeleteTree(parent, NULL) on a value-only base fires LAST_SET. */
static void test_registry_notify_deletetree_values(void)
{
    HKEY hk;
    uint64_t id; uint32_t val = 9;

    if (RegCreateKeyEx(HKEY_LOCAL_MACHINE, "Software\\NotifyDTV", 0, NULL, 0,
                       KEY_ALL_ACCESS, NULL, &hk, NULL) != ERROR_SUCCESS)
        return;
    RegSetValueEx(hk, "OnlyVal", 0, REG_DWORD, (const uint8_t *)&val, sizeof(val));
    id = reg_notify_register(hk, REG_NOTIFY_CHANGE_LAST_SET, 0, notify_test_cb, NULL, 0);
    TEST_ASSERT(id != 0, "value-clear watcher registered");
    s_notify_fires = 0;
    RegDeleteTree(hk, NULL);   /* clears the value-only base */
    TEST_ASSERT(s_notify_fires >= 1, "RegDeleteTree(NULL) value-only clear fires LAST_SET");

    RegUnregisterNotify(id);
    RegCloseKey(hk);
    RegDeleteTree(HKEY_LOCAL_MACHINE, "Software\\NotifyDTV");
}

void test_register_registry(void)
{
    test_suite_register_cat("Registry: CopyTree", test_registry_copytree, TEST_CAT_ABI);
    test_suite_register_cat("Registry: RenameKey", test_registry_rename, TEST_CAT_ABI);
    test_suite_register_cat("Registry: RenameKey separator", test_registry_rename_separator, TEST_CAT_ABI);
    test_suite_register_cat("Registry: NtRenameKey collision", test_nt_rename_key_collision, TEST_CAT_ABI);
    test_suite_register_cat("Registry: volatile keys", test_registry_volatile, TEST_CAT_ABI);
    test_suite_register_cat("Registry: save/restore fail-closed", test_registry_save_restore_failclosed, TEST_CAT_ABI);
    test_suite_register_cat("Registry: KCB hit rate", test_registry_kcb_hitrate, TEST_CAT_ABI);
    test_suite_register_cat("Registry: notify basic", test_registry_notify_basic, TEST_CAT_ABI);
    test_suite_register_cat("Registry: notify subtree", test_registry_notify_subtree, TEST_CAT_ABI);
    test_suite_register_cat("Registry: notify coalesce", test_registry_notify_coalesce, TEST_CAT_ABI);
    test_suite_register_cat("Registry: notify name", test_registry_notify_name, TEST_CAT_ABI);
    test_suite_register_cat("Registry: notify cleanup on delete", test_registry_notify_cleanup_on_delete, TEST_CAT_ABI);
    test_suite_register_cat("Registry: notify deep path", test_registry_notify_deep_path, TEST_CAT_ABI);
    test_suite_register_cat("Registry: notify reentrant", test_registry_notify_reentrant, TEST_CAT_ABI);
    test_suite_register_cat("Registry: notify multi-component", test_registry_notify_multicomponent, TEST_CAT_ABI);
    test_suite_register_cat("Registry: notify deletetree", test_registry_notify_deletetree, TEST_CAT_ABI);
    test_suite_register_cat("Registry: notify rename", test_registry_notify_rename, TEST_CAT_ABI);
    test_suite_register_cat("Registry: notify callback mutation", test_registry_notify_callback_mutation, TEST_CAT_ABI);
    test_suite_register_cat("Registry: notify deletetree values", test_registry_notify_deletetree_values, TEST_CAT_ABI);
    test_suite_register_cat("Registry: access enforcement", test_registry_access_enforcement, TEST_CAT_ABI);
    test_suite_register_cat("Registry: API limits", test_registry_api_limits, TEST_CAT_ABI);
    test_suite_register_cat("Registry: reg_check_access", test_reg_check_access_basic, TEST_CAT_ABI);
    test_suite_register_cat("Registry: Latin-1 casefold", test_registry_latin1_casefold, TEST_CAT_ABI);
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
