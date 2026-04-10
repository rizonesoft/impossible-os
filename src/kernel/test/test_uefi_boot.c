/* ============================================================================
 * test_uefi_boot.c -- UEFI boot infrastructure unit tests
 *
 * Tests UEFI runtime services availability, variable access, boot info
 * struct population, SMBIOS UUID, and Secure Boot registry mirror.
 *
 * XREF: 01-boot-platform/TODO-01-uefi-hardening-secureboot.md Unit Tests
 * ============================================================================ */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "kernel/uefi_runtime.h"
#include "kernel/uefi_vars.h"
#include "kernel/boot_info.h"
#include "kernel/smbios.h"
#include "kernel/nt/ntstatus.h"
#include "registry.h"

/* ---- UEFI Runtime Services ---- */

static void test_uefi_rt_available(void)
{
    int avail = uefi_rt_available();
    TEST_ASSERT_EQ(avail, 1,
                   "uefi_rt_available() returns 1 (runtime services preserved)");
}

/* ---- UEFI Variable Access ---- */

static void test_uefi_var_get_secureboot(void)
{
    static const uint16_t sb_name[] = {
        'S','e','c','u','r','e','B','o','o','t', 0
    };
    efi_guid_t global = EFI_GLOBAL_VARIABLE_GUID_INIT;
    uint8_t val = 0;
    size_t sz = sizeof(val);
    NTSTATUS s = uefi_var_get(sb_name, &global, &val, &sz);
    /* Must return SUCCESS (variable exists) or NOT_FOUND (no SB on this platform) */
    TEST_ASSERT(s == STATUS_SUCCESS || s == STATUS_NOT_FOUND,
                "uefi_var_get(SecureBoot) returns SUCCESS or NOT_FOUND");
}

static void test_uefi_var_u32_roundtrip(void)
{
    /* Use the Impossible OS vendor GUID to avoid touching firmware variables */
    static const uint16_t test_name[] = {
        'T','e','s','t','V','a','r','3','2', 0
    };
    efi_guid_t vendor = IMPOSSIBLE_OS_VENDOR_GUID_INIT;
    uint32_t write_val = 0xDEADBEEF;
    uint32_t read_val = 0;
    NTSTATUS s_set, s_get;

    s_set = uefi_var_set_u32(test_name, &vendor, write_val);
    if (s_set != STATUS_SUCCESS) {
        /* Some firmware (QEMU) may not support SetVariable -- skip */
        TEST_SKIP("uefi_var_set_u32 not supported on this platform");
        return;
    }
    s_get = uefi_var_get_u32(test_name, &vendor, &read_val);
    TEST_ASSERT_EQ(s_get, STATUS_SUCCESS,
                   "uefi_var_get_u32 returns SUCCESS after set");
    TEST_ASSERT_EQ(read_val, write_val,
                   "uefi_var_get_u32 reads back written value");
    /* Clean up: delete the test variable (size=0) */
    uefi_var_set(test_name, &vendor, (void *)0, 0, 0);
}

/* ---- Boot Info Struct ---- */

static void test_boot_info_framebuffer(void)
{
    TEST_ASSERT(g_boot_info.fb.width > 0,
                "boot_info.fb.width > 0 (GOP negotiated)");
    TEST_ASSERT(g_boot_info.fb.height > 0,
                "boot_info.fb.height > 0 (GOP negotiated)");
}

static void test_boot_info_hidpi(void)
{
    if (g_boot_info.fb.width >= 2560) {
        TEST_ASSERT_EQ(g_boot_info.hidpi, 1,
                       "hidpi == 1 when fb.width >= 2560");
    } else {
        TEST_ASSERT_EQ(g_boot_info.hidpi, 0,
                       "hidpi == 0 when fb.width < 2560");
    }
}

/* ---- SMBIOS UUID ---- */

static void test_smbios_uuid(void)
{
    uint8_t uuid[16];
    int found = smbios_get_system_uuid(uuid);
    /* On QEMU, SMBIOS exists but UUID may be all zeros.
     * On real hardware, UUID should be non-zero. Just verify no crash. */
    TEST_ASSERT(found == 0 || found == 1,
                "smbios_get_system_uuid returns 0 or 1 (no crash)");
}

/* ---- Secure Boot State Consistency ---- */

static void test_secureboot_state_consistency(void)
{
    /* boot_info.secure_boot_enabled should match the UEFI SecureBoot variable.
     * We can't read the raw variable in test context without calling uefi_get_variable
     * (which is a live firmware call -- allowed since it's read-only). Instead,
     * verify consistency with the uefi_secureboot_enabled() accessor. */
    int sb_api = uefi_secureboot_enabled();
    TEST_ASSERT_EQ(g_boot_info.secure_boot_enabled, (uint8_t)sb_api,
                   "boot_info.secure_boot_enabled matches uefi_secureboot_enabled()");
}

/* ---- Registry BIOS Vendor ---- */

static void test_registry_bios_vendor(void)
{
    HKEY hKey;
    long err = RegOpenKeyEx(HKEY_LOCAL_MACHINE, "HARDWARE\\BIOS", 0,
                            KEY_READ, &hKey);
    if (err != ERROR_SUCCESS) {
        TEST_SKIP("HKLM\\HARDWARE\\BIOS key not found");
        return;
    }
    {
        char buf[128];
        uint32_t size = sizeof(buf);
        uint32_t type = 0;
        err = RegQueryValueEx(hKey, "BIOSVendor", (void *)0, &type,
                              (uint8_t *)buf, &size);
        if (err != ERROR_SUCCESS) {
            RegCloseKey(hKey);
            TEST_SKIP("BIOSVendor value not found");
            return;
        }
        TEST_ASSERT(size > 1,
                    "HKLM\\HARDWARE\\BIOS\\BIOSVendor is non-empty");
    }
    RegCloseKey(hKey);
}

/* ---- Secure Boot DB Registry Mirror (S9) ---- */

static void test_secureboot_db_registry_mirror(void)
{
    const struct secureboot_db_info *dbi = secureboot_get_db_info();
    HKEY hKey;
    long err;

    if (!dbi) {
        TEST_SKIP("secureboot_get_db_info() returned NULL");
        return;
    }

    err = RegOpenKeyEx(HKEY_LOCAL_MACHINE, "SYSTEM\\SecureBoot", 0,
                       KEY_READ, &hKey);
    if (err != ERROR_SUCCESS) {
        TEST_SKIP("HKLM\\SYSTEM\\SecureBoot key not found");
        return;
    }

    {
        uint32_t reg_db = 0, reg_dbx = 0;
        uint32_t size, type;

        size = sizeof(reg_db);
        type = 0;
        err = RegQueryValueEx(hKey, "DbEntries", (void *)0, &type,
                              (uint8_t *)&reg_db, &size);
        if (err == ERROR_SUCCESS) {
            TEST_ASSERT_EQ(reg_db, dbi->db_entries,
                           "DbEntries matches secureboot_get_db_info()->db_entries");
        }

        size = sizeof(reg_dbx);
        type = 0;
        err = RegQueryValueEx(hKey, "DbxEntries", (void *)0, &type,
                              (uint8_t *)&reg_dbx, &size);
        if (err == ERROR_SUCCESS) {
            TEST_ASSERT_EQ(reg_dbx, dbi->dbx_entries,
                           "DbxEntries matches secureboot_get_db_info()->dbx_entries");
        }
    }
    RegCloseKey(hKey);
}

/* ---- Serial Detection (S10: SPCR Auto-Detection) ---- */

static void test_serial_source_valid(void)
{
    /* serial_source must be 0 (none), 1 (SPCR), or 2 (I/O probe) */
    TEST_ASSERT(g_boot_info.serial_source <= 2,
                "serial_source is 0, 1, or 2");
}

static void test_serial_source_matches_port(void)
{
    /* If serial_port is set, serial_source must be non-zero */
    if (g_boot_info.serial_port != 0) {
        TEST_ASSERT(g_boot_info.serial_source > 0,
                    "serial_source > 0 when serial_port is set");
    }
}

static void test_serial_baud_valid(void)
{
    /* If serial_source is non-zero, baud must be a standard rate or 0 */
    if (g_boot_info.serial_source > 0) {
        uint32_t baud = g_boot_info.serial_baud;
        TEST_ASSERT(baud == 9600 || baud == 19200 || baud == 38400 ||
                    baud == 57600 || baud == 115200 || baud == 0,
                    "serial_baud is a standard rate (or 0 for default)");
    }
}

static void test_serial_port_standard(void)
{
    /* If serial_port is set, it should be COM1 or COM2 */
    if (g_boot_info.serial_port != 0) {
        TEST_ASSERT(g_boot_info.serial_port == 0x3F8 ||
                    g_boot_info.serial_port == 0x2F8,
                    "serial_port is COM1 (0x3F8) or COM2 (0x2F8)");
    }
}

/* ---- Registration ---- */

void test_register_uefi_boot(void)
{
    test_suite_register_cat("UEFI: runtime available",
                            test_uefi_rt_available, TEST_CAT_BOOT);
    test_suite_register_cat("UEFI: var_get SecureBoot",
                            test_uefi_var_get_secureboot, TEST_CAT_BOOT);
    test_suite_register_cat("UEFI: var_u32 roundtrip",
                            test_uefi_var_u32_roundtrip, TEST_CAT_BOOT);
    test_suite_register_cat("UEFI: boot_info framebuffer",
                            test_boot_info_framebuffer, TEST_CAT_BOOT);
    test_suite_register_cat("UEFI: boot_info HiDPI",
                            test_boot_info_hidpi, TEST_CAT_BOOT);
    test_suite_register_cat("UEFI: SMBIOS UUID",
                            test_smbios_uuid, TEST_CAT_BOOT);
    test_suite_register_cat("UEFI: Secure Boot state",
                            test_secureboot_state_consistency, TEST_CAT_BOOT);
    test_suite_register_cat("UEFI: registry BIOS vendor",
                            test_registry_bios_vendor, TEST_CAT_BOOT);
    test_suite_register_cat("UEFI: SecureBoot DB mirror",
                            test_secureboot_db_registry_mirror, TEST_CAT_BOOT);
    test_suite_register_cat("UEFI: serial_source valid",
                            test_serial_source_valid, TEST_CAT_BOOT);
    test_suite_register_cat("UEFI: serial_source matches port",
                            test_serial_source_matches_port, TEST_CAT_BOOT);
    test_suite_register_cat("UEFI: serial_baud valid",
                            test_serial_baud_valid, TEST_CAT_BOOT);
    test_suite_register_cat("UEFI: serial_port standard",
                            test_serial_port_standard, TEST_CAT_BOOT);
}

#endif /* KERNEL_TESTS */
