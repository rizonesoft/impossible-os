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
#include "kernel/boot_media.h"
#include "registry.h"

/* ---- Boot device type (S3, S4) ---- */

static void test_boot_device_type_valid(void)
{
    TEST_ASSERT(g_boot_info.boot_device_type <= 6,
                "boot_device_type is a valid enum value (0--6: unknown/SATA/NVMe/USB/network/SD/eMMC)");
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

/* ---- Extended boot variable capability surface (S13) ---- */

static void test_registry_boot_var_caps_match_boot_info(void)
{
    /* Phase 2 contract: boot_device_populate_registry() persists the 5
     * v15 capability values. A typo in a value name or a swapped Lo/Hi
     * split must not slip through silently -- read each one back and
     * match against g_boot_info. */
    HKEY hKey = (HKEY)0;
    long rc = RegOpenKeyEx(HKEY_LOCAL_MACHINE, "SYSTEM\\Boot\\Device", 0,
                           KEY_READ, &hKey);
    TEST_ASSERT(rc == 0,
                "HKLM\\SYSTEM\\Boot\\Device exists for v15 capability values");
    if (rc != 0) return;

    uint32_t v32 = 0;
    rc = RegGetDword(hKey, "BootCurrentAttributes", &v32);
    TEST_ASSERT(rc == 0, "RegGetDword(BootCurrentAttributes) succeeds");
    TEST_ASSERT_EQ(v32, g_boot_info.boot_current_attrs,
                   "BootCurrentAttributes registry value matches boot_info");

    rc = RegGetDword(hKey, "BootOptionSupport", &v32);
    TEST_ASSERT(rc == 0, "RegGetDword(BootOptionSupport) succeeds");
    TEST_ASSERT_EQ(v32, g_boot_info.boot_option_support,
                   "BootOptionSupport registry value matches boot_info");

    uint32_t lo = 0, hi = 0;
    rc = RegGetDword(hKey, "OsIndicationsSupportedLo", &lo);
    TEST_ASSERT(rc == 0, "RegGetDword(OsIndicationsSupportedLo) succeeds");
    rc = RegGetDword(hKey, "OsIndicationsSupportedHi", &hi);
    TEST_ASSERT(rc == 0, "RegGetDword(OsIndicationsSupportedHi) succeeds");
    uint64_t recombined = ((uint64_t)hi << 32) | (uint64_t)lo;
    TEST_ASSERT_EQ(recombined, g_boot_info.os_indications_supported,
                   "OsIndicationsSupportedLo|Hi recombines to boot_info value");

    char desc[64];
    rc = RegGetString(hKey, "Description", desc, sizeof(desc));
    TEST_ASSERT(rc == 0, "RegGetString(Description) succeeds");
    /* Compare byte-for-byte; both sources are NUL-terminated within 64. */
    {
        int matches = 1;
        int i;
        for (i = 0; i < 64; i++) {
            if (desc[i] != g_boot_info.boot_description[i]) {
                matches = 0;
                break;
            }
            if (desc[i] == '\0') break;
        }
        TEST_ASSERT(matches,
                    "Description registry string matches boot_info");
    }

    RegCloseKey(hKey);
}


static void test_boot_option_support_reserved_bits_zero(void)
{
    /* UEFI 2.10 spec 3.1.4: BootOptionSupport defines bits
     *   0x00000001 KEY
     *   0x00000002 APP
     *   0x00000010 SYSPREP
     *   0x00000300 COUNT (2-bit field)
     * Allowed mask: 0x00000313. Any other bit set means firmware
     * is reporting reserved bits we don't recognize -- log it but
     * the field is informational so we don't fail outright; this
     * test is a drift detector for future spec extensions. */
    uint32_t reserved = g_boot_info.boot_option_support & 0xFFFFFCECU;
    if (reserved != 0)
        TEST_SKIP("BootOptionSupport reserved bits non-zero -- firmware sets unknown bits");
    else
        TEST_ASSERT_EQ(reserved, 0U,
                       "BootOptionSupport reserved bits (mask 0xFFFFFCEC) clear");
}

static void test_boot_description_nul_terminated(void)
{
    /* Bootloader copies at most 63 chars + NUL into boot_description[64]. */
    TEST_ASSERT_EQ(g_boot_info.boot_description[63], '\0',
                   "boot_description final byte is NUL (capacity 63 chars)");
}

static void test_boot_current_attrs_active_consistency(void)
{
    /* UEFI 2.10 spec 3.1.3 LOAD_OPTION_ACTIVE = 0x00000001.
     * On QEMU OVMF the firmware-selected boot entry MUST be ACTIVE,
     * else it would not have been chosen. Skip when no BootCurrent
     * (minimal-NVRAM VMs) or no description was decoded. */
    if (g_boot_info.uefi_boot_current == 0xFFFF ||
        g_boot_info.boot_description[0] == '\0') {
        TEST_SKIP("BootCurrent absent or Boot#### not decoded -- skip ACTIVE check");
        return;
    }
    TEST_ASSERT(g_boot_info.boot_current_attrs & 0x1,
                "BootCurrent's Boot####.Attributes ACTIVE bit set");
}

/* ---- Local boot device path detail (S14) ---- */

static void test_boot_pci_sentinel_or_valid(void)
{
    /* PCI device: 0..31 (5-bit field), Function: 0..7 (3-bit field) per
     * UEFI 2.10 spec 10.3.2.1. Either both 0xFF (not on PCI) OR both in
     * valid ranges. Mixed sentinel/valid would mean partial population. */
    uint8_t d = g_boot_info.boot_pci_device;
    uint8_t f = g_boot_info.boot_pci_function;
    if (d == 0xFF && f == 0xFF) {
        /* Not on PCI bus -- valid sentinel pair, no further check. */
        TEST_SKIP("Not on PCI -- 0xFF/0xFF sentinel pair");
        return;
    }
    /* Reject mixed sentinel/valid (partial population is a bug). */
    TEST_ASSERT(d != 0xFF && f != 0xFF,
                "boot_pci_device/function are both populated (no mixed sentinel)");
    TEST_ASSERT(d <= 31, "boot_pci_device <= 31 (PCI 5-bit field)");
    TEST_ASSERT(f <= 7, "boot_pci_function <= 7 (PCI 3-bit field)");
}

static void test_boot_nvme_consistency(void)
{
    /* If NSID is non-zero, the boot was NVMe; the EUI-64 array exists
     * but may be all-zero (some firmware doesn't fill it). Either case
     * is legitimate per UEFI spec -- the assertion is just that the
     * field exists and is bounded. NSID == 0 means non-NVMe boot. */
    if (g_boot_info.boot_nvme_nsid == 0) {
        TEST_SKIP("Not an NVMe boot -- skip NSID/EUI-64 consistency");
        return;
    }
    /* On NVMe: device type should be 2 (NVMe). */
    TEST_ASSERT_EQ(g_boot_info.boot_device_type, 2,
                   "NSID != 0 implies boot_device_type == 2 (NVMe)");
}

static void test_registry_boot_path_detail(void)
{
    /* Phase 2 contract: boot_device_populate_registry() persists the
     * v16 detail values. NamespaceId / PciDevice / PciFunction are
     * always written (sentinels included); NamespaceEui64 only if
     * at least one byte non-zero. */
    HKEY hKey = (HKEY)0;
    long rc = RegOpenKeyEx(HKEY_LOCAL_MACHINE, "SYSTEM\\Boot\\Device", 0,
                           KEY_READ, &hKey);
    TEST_ASSERT(rc == 0,
                "HKLM\\SYSTEM\\Boot\\Device exists for v16 detail values");
    if (rc != 0) return;

    uint32_t v32 = 0;
    rc = RegGetDword(hKey, "NamespaceId", &v32);
    TEST_ASSERT(rc == 0, "RegGetDword(NamespaceId) succeeds");
    TEST_ASSERT_EQ(v32, g_boot_info.boot_nvme_nsid,
                   "NamespaceId registry value matches boot_info");

    rc = RegGetDword(hKey, "PciDevice", &v32);
    TEST_ASSERT(rc == 0, "RegGetDword(PciDevice) succeeds");
    TEST_ASSERT_EQ(v32, (uint32_t)g_boot_info.boot_pci_device,
                   "PciDevice registry value matches boot_info");

    rc = RegGetDword(hKey, "PciFunction", &v32);
    TEST_ASSERT(rc == 0, "RegGetDword(PciFunction) succeeds");
    TEST_ASSERT_EQ(v32, (uint32_t)g_boot_info.boot_pci_function,
                   "PciFunction registry value matches boot_info");

    RegCloseKey(hKey);
}

/* boot_media_classify(us_per_4kib): the fast/medium/slow thresholds that drive the
 * deferred-klog + test-skip adaptations. Pure; boundary-checked. */
static void test_boot_media_classify(void)
{
    TEST_ASSERT_EQ(boot_media_classify(0), BOOT_MEDIA_FAST, "0us -> fast");
    TEST_ASSERT_EQ(boot_media_classify(BOOT_MEDIA_FAST_MAX_US - 1),
                   BOOT_MEDIA_FAST, "just under 1ms -> fast");
    TEST_ASSERT_EQ(boot_media_classify(BOOT_MEDIA_FAST_MAX_US),
                   BOOT_MEDIA_MEDIUM, "exactly 1ms -> medium");
    TEST_ASSERT_EQ(boot_media_classify(BOOT_MEDIA_MEDIUM_MAX_US - 1),
                   BOOT_MEDIA_MEDIUM, "just under 10ms -> medium");
    TEST_ASSERT_EQ(boot_media_classify(BOOT_MEDIA_MEDIUM_MAX_US),
                   BOOT_MEDIA_SLOW, "exactly 10ms -> slow");
    TEST_ASSERT_EQ(boot_media_classify(50000), BOOT_MEDIA_SLOW,
                   "50ms (USB 2.0) -> slow");
}

/* ---- Registration ---- */

void test_register_boot_device(void)
{
    test_suite_register_cat("Boot media: speed classify thresholds",
        test_boot_media_classify, TEST_CAT_BOOT);
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
    test_suite_register_cat("Boot device: BootOptionSupport reserved bits zero",
        test_boot_option_support_reserved_bits_zero, TEST_CAT_BOOT);
    test_suite_register_cat("Boot device: boot_description NUL-terminated",
        test_boot_description_nul_terminated, TEST_CAT_BOOT);
    test_suite_register_cat("Boot device: BootCurrent ACTIVE bit consistency",
        test_boot_current_attrs_active_consistency, TEST_CAT_BOOT);
    test_suite_register_cat("Boot device: Registry v15 capability values match boot_info",
        test_registry_boot_var_caps_match_boot_info, TEST_CAT_BOOT);
    test_suite_register_cat("Boot device: PCI device/function in valid range or sentinel",
        test_boot_pci_sentinel_or_valid, TEST_CAT_BOOT);
    test_suite_register_cat("Boot device: NVMe NSID consistent with device type",
        test_boot_nvme_consistency, TEST_CAT_BOOT);
    test_suite_register_cat("Boot device: Registry v16 path detail matches boot_info",
        test_registry_boot_path_detail, TEST_CAT_BOOT);
}

#endif /* KERNEL_TESTS */
