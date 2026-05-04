/* ============================================================================
 * test_boot_device.c -- Boot device discovery unit tests
 *
 * Tests boot_info fields populated by the UEFI bootloader for boot device
 * identification (TODO-03 S1-S12). All tests are read-only checks on
 * g_boot_info -- no live boot infrastructure calls.
 *
 * XREF: 01-boot-platform/TODO-05-boot-device-discovery.md Unit Tests
 * ============================================================================ */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "kernel/boot_info.h"
#include "registry.h"

/* ---- Boot device type (S3, S4) ---- */

static void test_boot_device_type_valid(void)
{
    TEST_ASSERT(g_boot_info.boot_device_type <= 4,
                "boot_device_type is a valid enum value (0--4)");
}

static void test_boot_device_path_nonempty(void)
{
    TEST_ASSERT(g_boot_info.boot_device_path[0] != '\0',
                "boot_device_path is non-empty (at least 1 character)");
}

static void test_boot_device_type_known(void)
{
    /* QEMU always has SATA -- type should not be 0 (UNKNOWN) */
    TEST_ASSERT_NEQ(g_boot_info.boot_device_type, 0,
                    "boot_device_type != UNKNOWN on QEMU (always has SATA)");
}

/* ---- Partition info (S7) ---- */

static void test_partition_style_gpt(void)
{
    /* QEMU boots from a GPT disk image */
    TEST_ASSERT_EQ(g_boot_info.boot_partition_style, 2,
                   "boot_partition_style == 2 (GPT) on QEMU");
}

static void test_partition_guid_nonzero(void)
{
    /* GPT partition GUID must not be all-zero */
    int nonzero = 0;
    int i;
    for (i = 0; i < 16; i++) {
        if (g_boot_info.boot_partition_guid[i] != 0) {
            nonzero = 1;
            break;
        }
    }
    TEST_ASSERT(nonzero, "boot_partition_guid is non-zero on GPT");
}

/* ---- Removable media (S8) ---- */

static void test_removable_sata(void)
{
    /* SATA boot on QEMU: not removable */
    if (g_boot_info.boot_device_type == 1) /* SATA */
        TEST_ASSERT_EQ(g_boot_info.boot_device_removable, 0,
                       "boot_device_removable == 0 on SATA boot");
    else
        TEST_SKIP("Not a SATA boot -- skip removable check");
}

static void test_media_present(void)
{
    TEST_ASSERT_EQ(g_boot_info.boot_media_present, 1,
                   "boot_media_present == 1 (booted from this device)");
}

/* ---- Boot variables (S6) ---- */

static void test_boot_order_count_bounded(void)
{
    TEST_ASSERT(g_boot_info.uefi_boot_order_count <= 16,
                "uefi_boot_order_count <= 16 (array bounds)");
}

/* ---- Registry (S9) ---- */

static void test_registry_boot_device_type(void)
{
    HKEY hKey = (HKEY)0;
    long rc = RegOpenKeyEx(HKEY_LOCAL_MACHINE, "SYSTEM\\Boot\\Device", 0,
                           KEY_READ, &hKey);
    /* Phase 2 contract: registry_populate_defaults() unconditionally
     * calls boot_device_populate_registry(), which MUST create this
     * key. Absence is a regression, not an unsupported platform. */
    TEST_ASSERT(rc == 0,
                "HKLM\\SYSTEM\\Boot\\Device key exists "
                "(populated in Phase 2)");
    if (rc != 0) return;

    uint32_t reg_type = 0;
    rc = RegGetDword(hKey, "Type", &reg_type);
    TEST_ASSERT(rc == 0, "RegGetDword(Type) succeeds");
    TEST_ASSERT_EQ(reg_type, (uint32_t)g_boot_info.boot_device_type,
                   "Registry Type matches boot_info.boot_device_type");
    RegCloseKey(hKey);
}

/* ---- Registration ---- */

void test_register_boot_device(void)
{
    test_suite_register_cat("Boot device: type valid",
        test_boot_device_type_valid, TEST_CAT_BOOT);
    test_suite_register_cat("Boot device: path non-empty",
        test_boot_device_path_nonempty, TEST_CAT_BOOT);
    test_suite_register_cat("Boot device: type known (not UNKNOWN)",
        test_boot_device_type_known, TEST_CAT_BOOT);
    test_suite_register_cat("Boot device: partition style GPT",
        test_partition_style_gpt, TEST_CAT_BOOT);
    test_suite_register_cat("Boot device: partition GUID non-zero",
        test_partition_guid_nonzero, TEST_CAT_BOOT);
    test_suite_register_cat("Boot device: SATA not removable",
        test_removable_sata, TEST_CAT_BOOT);
    test_suite_register_cat("Boot device: media present",
        test_media_present, TEST_CAT_BOOT);
    test_suite_register_cat("Boot device: boot order count bounded",
        test_boot_order_count_bounded, TEST_CAT_BOOT);
    test_suite_register_cat("Boot device: Registry Type matches boot_info",
        test_registry_boot_device_type, TEST_CAT_BOOT);
}

#endif /* KERNEL_TESTS */
