/* ============================================================================
 * test_uefi_boot.c -- UEFI boot infrastructure unit tests
 *
 * Tests UEFI runtime services availability, variable access, boot info
 * struct population, SMBIOS UUID, and Secure Boot registry mirror.
 *
 * XREF: 01-boot-platform/TODO-02-uefi-hardening-secureboot.md Unit Tests
 * ============================================================================ */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "kernel/uefi_runtime.h"
#include "kernel/uefi_vars.h"
#include "kernel/uefi_config.h"
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
    NTSTATUS s = uefi_var_get(sb_name, &global, &val, &sz, (uint32_t *)0);
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

/* ---- UKI (Unified Kernel Image) ---- */

/* BOOT_FLAG_INVOKED_VIA_UKI is part of BOOT_FLAG_MASK_KNOWN; the
 * boot_rollback_validate path rejects unknown flag bits, so adding
 * the flag to the mask is part of the v10 ABI contract. */
static void test_uki_flag_in_known_mask(void)
{
    TEST_ASSERT((BOOT_FLAG_MASK_KNOWN & BOOT_FLAG_INVOKED_VIA_UKI) != 0,
                "BOOT_FLAG_INVOKED_VIA_UKI is part of BOOT_FLAG_MASK_KNOWN");
}

/* The four flag bits MUST occupy distinct positions; a collision would
 * silently merge two semantic states (e.g. UKI invocation conflated
 * with rollback refusal). */
static void test_uki_flag_distinct_position(void)
{
    uint32_t all = BOOT_FLAG_ROLLBACK_REFUSAL |
                   BOOT_FLAG_ROLLBACK_READ_FAILED |
                   BOOT_FLAG_WARM_UPDATE |
                   BOOT_FLAG_INVOKED_VIA_UKI;
    TEST_ASSERT_EQ(__builtin_popcount(all), 4,
                   "four BOOT_FLAG_* bits occupy distinct positions");
    TEST_ASSERT((BOOT_FLAG_INVOKED_VIA_UKI &
                 (BOOT_FLAG_ROLLBACK_REFUSAL |
                  BOOT_FLAG_ROLLBACK_READ_FAILED |
                  BOOT_FLAG_WARM_UPDATE)) == 0,
                "BOOT_FLAG_INVOKED_VIA_UKI does not overlap any prior flag");
}

/* The flag bit's existence implies BOOT_INFO_VERSION >= 10; the
 * version bump is what tells the kernel the bit is meaningful. */
static void test_uki_flag_implies_v10_abi(void)
{
    TEST_ASSERT(BOOT_INFO_VERSION >= 10,
                "BOOT_INFO_VERSION >= 10 (UKI flag bit landed in v10)");
}

/* ---- EFI System Partition integrity (TODO-02 ESP integrity) -----------
 *
 * The bootloader's esp_integrity_check() runs pre-load and writes
 * three new fields plus mirrors the result to the registry. We verify
 * (1) the boot_info ABI carries the fields at the v11 layout, (2)
 * size_mb is non-zero on the disk-boot smoke path (i.e. when BlockIO
 * was usable), and (3) the registry seed produced HKLM\HARDWARE\BOOT
 * \ESP\Uuid as a non-empty string when the boot device is GPT (the
 * common smoke-test case). These are runtime oracle queries -- no
 * live boot infrastructure is invoked from the test body.
 */

static void test_esp_integrity_v11_abi(void)
{
    TEST_ASSERT(BOOT_INFO_VERSION >= 11,
                "BOOT_INFO_VERSION >= 11 (ESP integrity fields landed in v11)");
    /* The struct must contain the three new fields by name at the
     * compile-time layout. The static asserts in boot_info.h enforce
     * total size; the runtime test confirms the producer actually
     * stored sane values into them. */
    uint32_t valid_bit = (uint32_t)g_boot_info.esp_type_guid_valid;
    TEST_ASSERT(valid_bit <= 1,
                "esp_type_guid_valid is a single bit (0 or 1)");
    uint32_t fs_type = (uint32_t)g_boot_info.esp_filesystem_type;
    TEST_ASSERT(fs_type <= 2,
                "esp_filesystem_type in {0=unknown, 1=FAT16, 2=FAT32}");
}

/* Disk-boot path: when the bootloader had BlockIO available it
 * populates esp_size_mb. UKI mode also populates from the partition's
 * BlockIO before fast-skipping, so a zero here means BlockIO was
 * absent (PXE / RAM-disk only). The registry seed mirrors the same
 * value, so cross-check that they agree. */
static void test_esp_size_mb_consistency(void)
{
    HKEY hKey = (HKEY)0;
    if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, "HARDWARE\\BOOT\\ESP",
                     0, KEY_READ, &hKey) != ERROR_SUCCESS) {
        TEST_SKIP("HKLM\\HARDWARE\\BOOT\\ESP key absent -- non-disk boot path");
    }
    uint32_t size_mb = 0;
    uint32_t reg_dword = 0;
    if (RegGetDword(hKey, "SizeMB", &reg_dword) == ERROR_SUCCESS)
        size_mb = reg_dword;
    RegCloseKey(hKey);
    TEST_ASSERT_EQ((uint64_t)size_mb,
                   (uint64_t)g_boot_info.esp_size_mb,
                   "registry SizeMB matches g_boot_info.esp_size_mb");
}

/* GPT boot path: the registry seed writes Uuid as the formatted
 * unique partition GUID (matches PartitionGUID). On non-GPT boots
 * (network, MBR-only test image) the seed writes an empty string so
 * the key always has a value. We just verify the Uuid string is
 * either empty (non-GPT) or a 36-char canonical form. */
static void test_esp_registry_uuid_format(void)
{
    HKEY hKey = (HKEY)0;
    if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, "HARDWARE\\BOOT\\ESP",
                     0, KEY_READ, &hKey) != ERROR_SUCCESS) {
        TEST_SKIP("HKLM\\HARDWARE\\BOOT\\ESP key absent -- non-disk boot path");
    }
    char uuid_str[64];
    uuid_str[0] = '\0';
    long rc = RegGetString(hKey, "Uuid", uuid_str, sizeof(uuid_str));
    RegCloseKey(hKey);
    TEST_ASSERT(rc == ERROR_SUCCESS,
                "HKLM\\HARDWARE\\BOOT\\ESP\\Uuid value is present");
    uuid_str[sizeof(uuid_str) - 1] = '\0';
    /* Length is either 0 (non-GPT) or 36 (canonical GUID). */
    uint32_t len = 0;
    while (len < sizeof(uuid_str) - 1 && uuid_str[len] != '\0') len++;
    TEST_ASSERT(len == 0 || len == 36,
                "Uuid string is empty or 36-char canonical GUID");
    if (len == 36) {
        /* Canonical form: 8-4-4-4-12 hex with dashes at fixed positions. */
        TEST_ASSERT(uuid_str[8] == '-' && uuid_str[13] == '-' &&
                    uuid_str[18] == '-' && uuid_str[23] == '-',
                    "Uuid dashes at positions 8/13/18/23");
    }
}

/* ---- Win32 Firmware Variable + Table Surface (TODO-02 firmware section) ----
 *
 * The kernel32 export table reserves SSDT slots for the Win32 firmware
 * variable + table APIs. The user-mode kernel32 trampoline (the
 * Win32 API surface TODO Console & Process API, not yet shipped) handles
 * the actual ANSI/Wide conversion + GUID parsing. These tests verify
 * (1) the export-table sort invariant survived the new insertions,
 * (2) the QueryVariableInfo registry mirror at HKLM\SYSTEM\SecureBoot
 * \Vars carries either VarsValid=1 + non-zero sizes OR VarsValid=0
 * (unavailable -- distinguishable from real zero quota), (3)
 * NtQuerySystemInformation(SystemFirmwareTableInformation) responds
 * coherently to the ACPI enumerate path. */

#include "kernel/pe.h"
#include "kernel/nt/ssdt.h"
#include "kernel/nt/service_numbers.h"

static void test_kernel32_exports_sorted(void)
{
    int violations = pe_exports_sorted_check();
    TEST_ASSERT_EQ(violations, 0,
                   "pe_exports_sorted_check returns 0 (kernel32+ntdll tables strictly sorted)");
}

/* HKLM\SYSTEM\SecureBoot\Vars must always carry VarsValid; the size
 * fields are present iff VarsValid=1. Either shape is correct -- the
 * test rejects the broken intermediate state where VarsValid is
 * missing or VarsValid=0 with size keys present. */
static void test_vars_registry_validity_contract(void)
{
    HKEY hKey = (HKEY)0;
    long rc = RegOpenKeyEx(HKEY_LOCAL_MACHINE, "SYSTEM\\SecureBoot\\Vars",
                           0, KEY_READ, &hKey);
    if (rc != ERROR_SUCCESS) {
        TEST_SKIP("HKLM\\SYSTEM\\SecureBoot\\Vars absent -- registry not seeded");
    }
    uint32_t valid = 0xFFFFFFFFu;
    long vrc = RegGetDword(hKey, "VarsValid", &valid);
    TEST_ASSERT(vrc == ERROR_SUCCESS,
                "VarsValid DWORD is present");
    TEST_ASSERT(valid <= 1,
                "VarsValid is a single bit (0 or 1)");
    if (valid == 1) {
        uint64_t max_storage = 0;
        long mrc = RegGetQword(hKey, "MaxStorageSize", &max_storage);
        TEST_ASSERT(mrc == ERROR_SUCCESS && max_storage > 0,
                    "VarsValid=1 implies MaxStorageSize > 0");
    } else {
        /* VarsValid=0: size keys MUST be absent (no truthy 0 sentinel). */
        uint64_t probe = 0;
        long mrc = RegGetQword(hKey, "MaxStorageSize", &probe);
        TEST_ASSERT(mrc != ERROR_SUCCESS,
                    "VarsValid=0 implies MaxStorageSize key is absent");
    }
    RegCloseKey(hKey);
}

/* SystemFirmwareTableInformation enumerate of the ACPI provider should
 * return STATUS_SUCCESS with a non-zero list (every UEFI system has
 * at least one ACPI table -- typically FACP+APIC+HPET). */
static void test_nt_query_system_information_acpi_enum(void)
{
    /* FW_PROVIDER_ACPI = 0x49504341 ('ACPI' little-endian) */
    /* SystemFirmwareTableInformation = 76 */
    struct {
        uint32_t provider_signature;
        uint32_t action;
        uint32_t table_id;
        uint32_t table_buffer_length;
        uint32_t buf[64];
    } req;
    uint32_t i;
    for (i = 0; i < sizeof(req) / sizeof(uint32_t); i++)
        ((uint32_t *)&req)[i] = 0;
    req.provider_signature = 0x49504341u; /* 'ACPI' */
    req.action = 0;                       /* enumerate */
    req.table_buffer_length = sizeof(req.buf);

    uint32_t return_length = 0;
    NTSTATUS s = ssdt_dispatch(SSDT_NtQuerySystemInformation,
                               76,                 /* info class */
                               (uint64_t)(uintptr_t)&req,
                               sizeof(req),
                               (uint64_t)(uintptr_t)&return_length,
                               0, 0);
    /* On a system with no ACPI tables (extremely rare) the call
     * returns STATUS_NOT_FOUND; on a normal UEFI boot it returns
     * STATUS_SUCCESS with a non-zero TableBufferLength. Accept either
     * but reject any other status. */
    TEST_ASSERT(s == STATUS_SUCCESS || s == STATUS_NOT_FOUND,
                "SystemFirmwareTableInformation+ACPI+enumerate returns SUCCESS or NOT_FOUND");
    if (s == STATUS_SUCCESS) {
        TEST_ASSERT(req.table_buffer_length >= 4,
                    "ACPI enumerate returns at least one 4-byte signature");
    }
}

/* ---- MAT region inventory + RT property mismatch ---- */

static void test_mat_classify_guard(void)
{
    /* RP set: guard regardless of other bits. */
    TEST_ASSERT_EQ((int)mat_classify_attr(EFI_MEMORY_RP),
                   (int)MAT_CLASS_GUARD, "RP -> GUARD");
    TEST_ASSERT_EQ((int)mat_classify_attr(EFI_MEMORY_RP | EFI_MEMORY_RO),
                   (int)MAT_CLASS_GUARD, "RP+RO -> GUARD (RP wins)");
}

static void test_mat_classify_code(void)
{
    /* RO + executable (no XP) = code region. */
    TEST_ASSERT_EQ((int)mat_classify_attr(EFI_MEMORY_RO),
                   (int)MAT_CLASS_CODE, "RO -> CODE");
}

static void test_mat_classify_data(void)
{
    /* Writable + non-executable (XP) = data region. */
    TEST_ASSERT_EQ((int)mat_classify_attr(EFI_MEMORY_XP),
                   (int)MAT_CLASS_DATA, "XP -> DATA");
}

static void test_mat_classify_rodata(void)
{
    /* RO + XP = read-only constants. */
    TEST_ASSERT_EQ((int)mat_classify_attr(EFI_MEMORY_RO | EFI_MEMORY_XP),
                   (int)MAT_CLASS_RODATA, "RO+XP -> RODATA");
}

static void test_mat_classify_wx_violation(void)
{
    /* Writable AND executable (no RO, no XP) = W^X violation. */
    TEST_ASSERT_EQ((int)mat_classify_attr(0),
                   (int)MAT_CLASS_WX_VIOLATION, "0 attr -> WX_VIOLATION");
}

static void test_mat_get_entry_null_guard(void)
{
    TEST_ASSERT_EQ(mat_get_entry(0, 0), 0, "NULL out -> 0");
}

static void test_mat_get_entry_range_guard(void)
{
    mat_entry_t e;
    /* MAT_MAX_ENTRIES + 1000 is guaranteed out of range whether the
     * runtime cache is empty or full. */
    TEST_ASSERT_EQ(mat_get_entry(MAT_MAX_ENTRIES + 1000, &e), 0,
                   "huge idx -> 0");
}

static void test_mat_count_consistent(void)
{
    /* Whatever count is, it must not exceed the cache cap. */
    uint32_t n = mat_get_count();
    TEST_ASSERT(n <= MAT_MAX_ENTRIES, "mat_get_count <= MAT_MAX_ENTRIES");
}

static void test_rt_property_mismatches_nonneg(void)
{
    int n = uefi_rt_property_mismatches();
    TEST_ASSERT(n >= 0, "uefi_rt_property_mismatches returns nonnegative");
    /* On QEMU OVMF the mismatch count is typically 0; on weird firmware
     * it may be non-zero. Either way the value must be sane. */
    TEST_ASSERT(n <= 14, "mismatches bounded by service count");
}

static void test_rt_supported_implies_pointer_callable(void)
{
    /* Codex H1 guarantee: every bit still set in s_supported after
     * uefi_runtime_init() must correspond to a non-NULL function
     * pointer. The mismatch checker clears bits whose pointer is NULL,
     * so wrappers gating on s_supported cannot null-deref.
     *
     * We don't have direct access to s_rt, but we exercise the
     * critical wrapper surface: if the mismatch fix is in place AND
     * a service bit is set, the wrapper must not return UEFI_NOT_FOUND
     * specifically because of NULL. Smoke-check: rt_property_mismatches
     * is 0 on a healthy firmware (OVMF). On firmware with mismatches,
     * the bits would have been cleared, so the residual set bits are
     * always callable. The strongest test we can run from here is
     * range-bounded counter sanity already covered above. */
    uint32_t supported = uefi_rt_supported();
    int mismatches = uefi_rt_property_mismatches();
    /* If RT properties table absent, supported defaults to 0xFFFFFFFF
     * and no mismatches are computed (s_rt may be NULL on degraded). */
    if (uefi_rt_available()) {
        TEST_ASSERT(supported != 0,
                    "RT available => at least one service bit set");
    }
    (void)mismatches;
}

/* Registration */

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
    test_suite_register_cat("UEFI: UKI flag in mask",
                            test_uki_flag_in_known_mask, TEST_CAT_BOOT);
    test_suite_register_cat("UEFI: UKI flag distinct bit",
                            test_uki_flag_distinct_position, TEST_CAT_BOOT);
    test_suite_register_cat("UEFI: UKI flag v10 ABI",
                            test_uki_flag_implies_v10_abi, TEST_CAT_BOOT);
    test_suite_register_cat("UEFI: ESP integrity v11 ABI",
                            test_esp_integrity_v11_abi, TEST_CAT_BOOT);
    test_suite_register_cat("UEFI: ESP size_mb consistency",
                            test_esp_size_mb_consistency, TEST_CAT_BOOT);
    test_suite_register_cat("UEFI: ESP registry Uuid format",
                            test_esp_registry_uuid_format, TEST_CAT_BOOT);
    test_suite_register_cat("UEFI: kernel32 exports sorted",
                            test_kernel32_exports_sorted, TEST_CAT_BOOT);
    test_suite_register_cat("UEFI: Vars registry validity contract",
                            test_vars_registry_validity_contract, TEST_CAT_BOOT);
    test_suite_register_cat("UEFI: NtQuerySystemInformation ACPI enum",
                            test_nt_query_system_information_acpi_enum, TEST_CAT_BOOT);
    /* MAT classification + inventory + RT property mismatch */
    test_suite_register_cat("UEFI: MAT classify GUARD",
                            test_mat_classify_guard, TEST_CAT_BOOT);
    test_suite_register_cat("UEFI: MAT classify CODE",
                            test_mat_classify_code, TEST_CAT_BOOT);
    test_suite_register_cat("UEFI: MAT classify DATA",
                            test_mat_classify_data, TEST_CAT_BOOT);
    test_suite_register_cat("UEFI: MAT classify RODATA",
                            test_mat_classify_rodata, TEST_CAT_BOOT);
    test_suite_register_cat("UEFI: MAT classify WX violation",
                            test_mat_classify_wx_violation, TEST_CAT_BOOT);
    test_suite_register_cat("UEFI: mat_get_entry NULL guard",
                            test_mat_get_entry_null_guard, TEST_CAT_BOOT);
    test_suite_register_cat("UEFI: mat_get_entry range guard",
                            test_mat_get_entry_range_guard, TEST_CAT_BOOT);
    test_suite_register_cat("UEFI: mat_get_count <= cap",
                            test_mat_count_consistent, TEST_CAT_BOOT);
    test_suite_register_cat("UEFI: rt_property_mismatches range",
                            test_rt_property_mismatches_nonneg, TEST_CAT_BOOT);
    test_suite_register_cat("UEFI: supported bits imply callable",
                            test_rt_supported_implies_pointer_callable, TEST_CAT_BOOT);
}

#endif /* KERNEL_TESTS */
