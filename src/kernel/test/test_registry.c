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

/* Registration */
void test_register_registry(void)
{
    test_suite_register_cat("Registry: DWORD", test_registry_dword, TEST_CAT_ABI);
    test_suite_register_cat("Registry: string", test_registry_string, TEST_CAT_ABI);
}

#endif /* KERNEL_TESTS */
