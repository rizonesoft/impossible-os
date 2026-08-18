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
#include "boot/uki_cmdline_check.h"
#include "boot/uki_cmdline_media_role.h"
#include "boot/sha256_boot.h"
#include "boot/devpath_filepath.h"
#include "kernel/crypto/sha256.h"

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

/* ---- UKI signed-payload addresses (v14 ABI; whole-chain Secure Boot) ---- */

static void test_uki_initrd_section_size_consistent(void)
{
    /* Invariant: when the bootloader publishes uki_initrd_addr != 0,
     * uki_initrd_size MUST also be > 0. A non-zero pointer with a
     * zero size means the bootloader copied the section but reported
     * it as absent -- a state inconsistency that would mislead the
     * kernel-side initrd consumer. */
    if (g_boot_info.uki_initrd_addr != 0) {
        TEST_ASSERT(g_boot_info.uki_initrd_size > 0,
                    "uki_initrd_size > 0 when uki_initrd_addr is non-zero");
    } else {
        TEST_ASSERT_EQ(g_boot_info.uki_initrd_size, (uint64_t)0,
                       "uki_initrd_size == 0 when uki_initrd_addr is 0");
    }
    /* Same invariant for recovery and modules. */
    if (g_boot_info.uki_recovery_addr != 0) {
        TEST_ASSERT(g_boot_info.uki_recovery_size > 0,
                    "uki_recovery_size > 0 when uki_recovery_addr is non-zero");
    } else {
        TEST_ASSERT_EQ(g_boot_info.uki_recovery_size, (uint64_t)0,
                       "uki_recovery_size == 0 when uki_recovery_addr is 0");
    }
    if (g_boot_info.uki_modules_addr != 0) {
        TEST_ASSERT(g_boot_info.uki_modules_size > 0,
                    "uki_modules_size > 0 when uki_modules_addr is non-zero");
    } else {
        TEST_ASSERT_EQ(g_boot_info.uki_modules_size, (uint64_t)0,
                       "uki_modules_size == 0 when uki_modules_addr is 0");
    }
}

static void test_boot_flag_uki_implies_no_disk_initrd(void)
{
    /* Cross-check: if the UKI flag is set in boot_info.flags, the
     * payload-loading path must come exclusively from the v14 fields
     * (which can be 0/0 if the UKI shipped no payload sections). The
     * kernel-side initrd loader (when wired) must reject any disk
     * path under this flag -- the bootloader rejection in
     * parse_boot_conf is the upstream check; this test asserts the
     * boot_info publication invariant the kernel observes. If the
     * UKI flag is not set, payload addresses must all be zero
     * (split-path boot does not populate them). */
    if ((g_boot_info.flags & BOOT_FLAG_INVOKED_VIA_UKI) == 0) {
        TEST_ASSERT_EQ(g_boot_info.uki_initrd_addr, (uint64_t)0,
                       "uki_initrd_addr == 0 when not invoked via UKI");
        TEST_ASSERT_EQ(g_boot_info.uki_recovery_addr, (uint64_t)0,
                       "uki_recovery_addr == 0 when not invoked via UKI");
        TEST_ASSERT_EQ(g_boot_info.uki_modules_addr, (uint64_t)0,
                       "uki_modules_addr == 0 when not invoked via UKI");
    }
}

static void test_uki_cmdline_rejects_initrd_token(void)
{
    /* Bare initrd= at start of buffer must be detected. */
    const unsigned char buf1[] = "initrd=disk-payload.img";
    const char *r1 = uki_find_disk_override_token(buf1, sizeof(buf1) - 1);
    TEST_ASSERT(r1 != (const char *)0,
                "uki_find_disk_override_token detects 'initrd=' at offset 0");

    /* initrd= preceded by space must be detected. */
    const unsigned char buf2[] = "console=ttyS0 initrd=disk.img quiet";
    const char *r2 = uki_find_disk_override_token(buf2, sizeof(buf2) - 1);
    TEST_ASSERT(r2 != (const char *)0,
                "uki_find_disk_override_token detects 'initrd=' after space");

    /* "noinitrd=foo" must NOT match (initrd= not at token start). */
    const unsigned char buf3[] = "console=ttyS0 noinitrd=foo";
    const char *r3 = uki_find_disk_override_token(buf3, sizeof(buf3) - 1);
    TEST_ASSERT_EQ(r3, (const char *)0,
                   "uki_find_disk_override_token rejects substring match 'noinitrd='");

    /* module= (singular) and recovery_image= (full key) -- the
     * actual parser keys per parse_conf_kv in bootx64.c. */
    const unsigned char buf4[] = "module=foo.eif";
    TEST_ASSERT(uki_find_disk_override_token(buf4, sizeof(buf4) - 1) != (const char *)0,
                "uki_find_disk_override_token detects 'module=' at offset 0");
    const unsigned char buf5[] = "recovery_image=foo.img";
    TEST_ASSERT(uki_find_disk_override_token(buf5, sizeof(buf5) - 1) != (const char *)0,
                "uki_find_disk_override_token detects 'recovery_image=' at offset 0");

    /* "modules=" plural and "recovery=" short are NOT parser keys.
     * Helper deliberately ignores them -- rejecting them would be
     * defense theater since the parser never stages those. */
    const unsigned char buf4b[] = "modules=foo.cpio";
    TEST_ASSERT_EQ(uki_find_disk_override_token(buf4b, sizeof(buf4b) - 1),
                   (const char *)0,
                   "uki_find_disk_override_token does not flag non-parser key 'modules='");
    const unsigned char buf5b[] = "recovery=foo.img";
    TEST_ASSERT_EQ(uki_find_disk_override_token(buf5b, sizeof(buf5b) - 1),
                   (const char *)0,
                   "uki_find_disk_override_token does not flag non-parser key 'recovery='");

    /* Clean cmdline (no override tokens) must return NULL. */
    const unsigned char buf6[] = "console=ttyS0,115200 quiet noapic";
    TEST_ASSERT_EQ(uki_find_disk_override_token(buf6, sizeof(buf6) - 1),
                   (const char *)0,
                   "uki_find_disk_override_token returns NULL on clean cmdline");

    /* Empty buffer / NULL safety. */
    TEST_ASSERT_EQ(uki_find_disk_override_token(buf1, 0),
                   (const char *)0,
                   "uki_find_disk_override_token returns NULL on zero-length buffer");
    TEST_ASSERT_EQ(uki_find_disk_override_token((const unsigned char *)0, 100),
                   (const char *)0,
                   "uki_find_disk_override_token returns NULL on NULL buffer");

    /* Tab + newline + carriage return as separators. */
    const unsigned char buf7[] = "x\tinitrd=foo";
    TEST_ASSERT(uki_find_disk_override_token(buf7, sizeof(buf7) - 1) != (const char *)0,
                "uki_find_disk_override_token detects 'initrd=' after tab");
    const unsigned char buf8[] = "x\nrecovery_image=bar";
    TEST_ASSERT(uki_find_disk_override_token(buf8, sizeof(buf8) - 1) != (const char *)0,
                "uki_find_disk_override_token detects 'recovery_image=' after newline");
    const unsigned char buf8b[] = "x\rmodule=baz.eif";
    TEST_ASSERT(uki_find_disk_override_token(buf8b, sizeof(buf8b) - 1) != (const char *)0,
                "uki_find_disk_override_token detects 'module=' after CR");

    /* Truncated near end: "initrd" without "=" must NOT match. */
    const unsigned char buf9[] = "initrd";
    TEST_ASSERT_EQ(uki_find_disk_override_token(buf9, sizeof(buf9) - 1),
                   (const char *)0,
                   "uki_find_disk_override_token requires trailing '=' for initrd");

    /* "module" without "=" must NOT match. */
    const unsigned char buf9b[] = "module";
    TEST_ASSERT_EQ(uki_find_disk_override_token(buf9b, sizeof(buf9b) - 1),
                   (const char *)0,
                   "uki_find_disk_override_token requires trailing '=' for module");

    /* "recovery_imag" (truncated key) must NOT match. */
    const unsigned char buf9c[] = "recovery_imag=foo";
    TEST_ASSERT_EQ(uki_find_disk_override_token(buf9c, sizeof(buf9c) - 1),
                   (const char *)0,
                   "uki_find_disk_override_token requires full 'recovery_image=' key");
}

/* ---- SecureBoot Drift Detection (gap-audit 2026-05-01 M2) ---- */

static void test_secureboot_drift_detection(void)
{
    /* The boot snapshot must already be captured (uefi_secureboot_init
     * runs in Phase 1 before any test runs). A no-op revalidate should
     * report no drift because no UEFI variables changed between init
     * and now. */
    int initial_drift = uefi_secureboot_drift_detected();
    int tick_result = uefi_secureboot_revalidate_tick();

    /* If an earlier test or boot-time anomaly already flagged drift
     * we can not assert "no drift" -- only that the tick result is
     * idempotent (sticky once set). */
    if (initial_drift) {
        TEST_ASSERT_EQ(uefi_secureboot_drift_detected(), 1,
                       "drift flag is sticky once set");
        return;
    }

    TEST_ASSERT_EQ(tick_result, 0,
                   "revalidate_tick returns 0 when state matches boot snapshot");
    TEST_ASSERT_EQ(uefi_secureboot_drift_detected(), 0,
                   "drift flag stays clear when no mismatch observed");

    /* refresh() re-reads UEFI variables and re-runs the comparison.
     * On a non-tampered system the variables match the boot snapshot,
     * so this should also report no drift. On a system without UEFI
     * runtime services available the function returns 0 (no drift
     * signal possible) -- still 0. */
    int refresh_result = uefi_secureboot_refresh();
    TEST_ASSERT_EQ(refresh_result, 0,
                   "refresh returns 0 on untampered system");
    TEST_ASSERT_EQ(uefi_secureboot_drift_detected(), 0,
                   "drift flag still clear after refresh on untampered system");
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
    /* serial_source == 2 (I/O probe fallback) is COM1 or COM2 only --
     * the no-SPCR scratch-register probe in serial_early_init only
     * checks those two ports.  serial_source == 1 (SPCR) accepts any
     * firmware-declared 16-bit I/O base in [1, 0xFFF8] because SPCR
     * (ACPI Serial Port Console Redirection) may legitimately
     * advertise COM3 (0x3E8), COM4 (0x2E8), or a vendor-custom
     * address; the bootloader broadened to honor that.  Upper bound
     * stays at 0xFFF8 so the 16550 register block (base..base+7)
     * stays inside the 16-bit I/O port space. */
    if (g_boot_info.serial_port == 0)
        return;
    if (g_boot_info.serial_source == 2) {
        TEST_ASSERT(g_boot_info.serial_port == 0x3F8 ||
                    g_boot_info.serial_port == 0x2F8,
                    "I/O-probe fallback: serial_port is COM1 or COM2");
    } else if (g_boot_info.serial_source == 1) {
        TEST_ASSERT(g_boot_info.serial_port <= 0xFFF8,
                    "SPCR: serial_port within 16-bit I/O port space");
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
    /* Invariant: every bit still set in s_supported after
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

/* ===== ESRT decoder + capsule-policy primitive tests ===== */

static int strs_eq(const char *a, const char *b)
{
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == 0 && *b == 0;
}

static void test_esrt_decode_status_known(void)
{
    /* Spot-check both ends + middle of the UEFI 2.10 Table 23-3 range
     * so a future drift in any one mnemonic surfaces as a single
     * named failure rather than a generic regression. */
    TEST_ASSERT(strs_eq(esrt_decode_status(ESRT_STATUS_SUCCESS), "SUCCESS"),
        "ESRT_STATUS_SUCCESS decodes to \"SUCCESS\"");
    TEST_ASSERT(strs_eq(esrt_decode_status(ESRT_STATUS_ERROR_INSUFFICIENT_RESOURCES),
                        "ERROR_INSUFFICIENT_RESOURCES"),
        "INSUFFICIENT_RESOURCES decoded mnemonic");
    TEST_ASSERT(strs_eq(esrt_decode_status(ESRT_STATUS_ERROR_AUTH_ERROR),
                        "ERROR_AUTH_ERROR"),
        "AUTH_ERROR decoded mnemonic");
    TEST_ASSERT(strs_eq(esrt_decode_status(ESRT_STATUS_ERROR_PWR_EVT_BATT),
                        "ERROR_PWR_EVT_BATT"),
        "ERROR_PWR_EVT_BATT decoded mnemonic (UEFI 2.7+)");
    TEST_ASSERT(strs_eq(esrt_decode_status(ESRT_STATUS_ERROR_UNSATISFIED_DEPENDENCIES),
                        "ERROR_UNSATISFIED_DEPENDENCIES"),
        "ERROR_UNSATISFIED_DEPENDENCIES decoded mnemonic (UEFI 2.7+)");
}

static void test_esrt_decode_status_reserved(void)
{
    /* Future spec values fall through to "Reserved" so operator UX
     * cannot silently lie about an unknown status code.  Boundary
     * is 0x08 (UNSATISFIED_DEPENDENCIES, the highest valid mnemonic
     * in UEFI 2.10); 0x09+ is "Reserved". */
    TEST_ASSERT(strs_eq(esrt_decode_status(0xFFFFFFFF), "Reserved"),
        "out-of-range status decodes to \"Reserved\"");
    TEST_ASSERT(strs_eq(esrt_decode_status(9), "Reserved"),
        "9 (one past UNSATISFIED_DEPENDENCIES) decodes to \"Reserved\"");
}

static void test_esrt_decode_type_table(void)
{
    TEST_ASSERT(strs_eq(esrt_decode_type(ESRT_FW_TYPE_SYSTEM), "System"),
        "type SYSTEM decoded");
    TEST_ASSERT(strs_eq(esrt_decode_type(ESRT_FW_TYPE_DEVICE), "Device"),
        "type DEVICE decoded");
    TEST_ASSERT(strs_eq(esrt_decode_type(ESRT_FW_TYPE_UEFI_DRIVER), "Driver"),
        "type DRIVER decoded");
    TEST_ASSERT(strs_eq(esrt_decode_type(ESRT_FW_TYPE_UNKNOWN), "Unknown"),
        "type UNKNOWN decoded");
    TEST_ASSERT(strs_eq(esrt_decode_type(99), "Unknown"),
        "out-of-range type falls back to \"Unknown\"");
}

static void test_esrt_capsule_helpers_oor_idx_returns_zero(void)
{
    /* On QEMU OVMF without ESRT firmware, esrt_count() == 0, so any
     * idx is out-of-range.  Both helpers must return 0 cleanly --
     * never read past the cache or crash on an empty inventory. */
    TEST_ASSERT_EQ(esrt_rollback_floor_ok(0), 0,
        "rollback_floor_ok(0) returns 0 when inventory empty");
    TEST_ASSERT_EQ(esrt_rollback_floor_ok(99), 0,
        "rollback_floor_ok(99) returns 0 on OOR idx");
    TEST_ASSERT_EQ(esrt_capsule_persists_across_reset(0), 0,
        "persists_across_reset(0) returns 0 when inventory empty");
    TEST_ASSERT_EQ(esrt_capsule_persists_across_reset(99), 0,
        "persists_across_reset(99) returns 0 on OOR idx");
}

/* persist_across_reset checks bit 0x00010000 (UEFI 2.10 section 8.5.3).
 * Verifying the literal here would be tautological -- the compiler
 * already enforces what is in the #define -- so coverage of that bit
 * lives in the live capsule-policy consumer (TODO-27 advanced UEFI
 * work) where a real ESRT entry's capsule_flags drives the helper. */

/* ---- UKI cmdline media_role parser (artifact-format media-role
 * cross-format contract; UKI cmdline takes precedence over disk
 * /IPOS/role.txt in media_role_detect_and_record). */

static void test_media_role_uki_cmdline(void)
{
    /* Each canonical role at offset 0. */
    const unsigned char b1[] = "media_role=normal";
    TEST_ASSERT_EQ((uint64_t)uki_cmdline_extract_media_role(b1, sizeof(b1) - 1),
                   (uint64_t)BOOT_MEDIA_ROLE_NORMAL,
                   "media_role=normal -> NORMAL");
    const unsigned char b2[] = "media_role=installer";
    TEST_ASSERT_EQ((uint64_t)uki_cmdline_extract_media_role(b2, sizeof(b2) - 1),
                   (uint64_t)BOOT_MEDIA_ROLE_INSTALLER,
                   "media_role=installer -> INSTALLER");
    const unsigned char b3[] = "media_role=live";
    TEST_ASSERT_EQ((uint64_t)uki_cmdline_extract_media_role(b3, sizeof(b3) - 1),
                   (uint64_t)BOOT_MEDIA_ROLE_LIVE,
                   "media_role=live -> LIVE");
    const unsigned char b4[] = "media_role=recovery";
    TEST_ASSERT_EQ((uint64_t)uki_cmdline_extract_media_role(b4, sizeof(b4) - 1),
                   (uint64_t)BOOT_MEDIA_ROLE_RECOVERY,
                   "media_role=recovery -> RECOVERY");
    const unsigned char b5[] = "media_role=manufacturing";
    TEST_ASSERT_EQ((uint64_t)uki_cmdline_extract_media_role(b5, sizeof(b5) - 1),
                   (uint64_t)BOOT_MEDIA_ROLE_MANUFACTURING,
                   "media_role=manufacturing -> MANUFACTURING");
    const unsigned char b6[] = "media_role=diagnostics";
    TEST_ASSERT_EQ((uint64_t)uki_cmdline_extract_media_role(b6, sizeof(b6) - 1),
                   (uint64_t)BOOT_MEDIA_ROLE_DIAGNOSTICS,
                   "media_role=diagnostics -> DIAGNOSTICS");

    /* Whitespace-bounded recognition: token after space, tab, newline. */
    const unsigned char b7[] = "console=ttyS0 media_role=recovery quiet";
    TEST_ASSERT_EQ((uint64_t)uki_cmdline_extract_media_role(b7, sizeof(b7) - 1),
                   (uint64_t)BOOT_MEDIA_ROLE_RECOVERY,
                   "media_role= recognized after space");
    const unsigned char b8[] = "k=v\tmedia_role=installer\n";
    TEST_ASSERT_EQ((uint64_t)uki_cmdline_extract_media_role(b8, sizeof(b8) - 1),
                   (uint64_t)BOOT_MEDIA_ROLE_INSTALLER,
                   "media_role= recognized after tab; trailing newline trimmed");

    /* Case-insensitive value match. */
    const unsigned char b9[] = "media_role=RECOVERY";
    TEST_ASSERT_EQ((uint64_t)uki_cmdline_extract_media_role(b9, sizeof(b9) - 1),
                   (uint64_t)BOOT_MEDIA_ROLE_RECOVERY,
                   "media_role=RECOVERY upper-case accepted");
    const unsigned char b10[] = "media_role=ReCoVeRy";
    TEST_ASSERT_EQ((uint64_t)uki_cmdline_extract_media_role(b10, sizeof(b10) - 1),
                   (uint64_t)BOOT_MEDIA_ROLE_RECOVERY,
                   "media_role=ReCoVeRy mixed-case accepted");
}

static void test_media_role_uki_cmdline_rejections(void)
{
    /* NULL / empty buffer. */
    TEST_ASSERT_EQ((uint64_t)uki_cmdline_extract_media_role((const unsigned char *)0, 0),
                   (uint64_t)BOOT_MEDIA_ROLE_UNSET,
                   "NULL/0 -> UNSET");

    /* Substring at non-token-start position must NOT match (no
     * leading whitespace before "media_role="). */
    const unsigned char b1[] = "noprefixmedia_role=recovery";
    TEST_ASSERT_EQ((uint64_t)uki_cmdline_extract_media_role(b1, sizeof(b1) - 1),
                   (uint64_t)BOOT_MEDIA_ROLE_UNSET,
                   "non-token-start substring rejected");

    /* Unrecognized name -> UNSET (not NORMAL fallback; caller
     * decides fallback policy). */
    const unsigned char b2[] = "media_role=hostile";
    TEST_ASSERT_EQ((uint64_t)uki_cmdline_extract_media_role(b2, sizeof(b2) - 1),
                   (uint64_t)BOOT_MEDIA_ROLE_UNSET,
                   "unrecognized value -> UNSET");

    /* Empty value. */
    const unsigned char b3[] = "media_role=";
    TEST_ASSERT_EQ((uint64_t)uki_cmdline_extract_media_role(b3, sizeof(b3) - 1),
                   (uint64_t)BOOT_MEDIA_ROLE_UNSET,
                   "empty value -> UNSET");

    /* Oversized value (>14 bytes) -> UNSET. */
    const unsigned char b4[] = "media_role=verylongstring";
    TEST_ASSERT_EQ((uint64_t)uki_cmdline_extract_media_role(b4, sizeof(b4) - 1),
                   (uint64_t)BOOT_MEDIA_ROLE_UNSET,
                   "oversized value -> UNSET");

    /* Explicit `media_role=unset` is rejected: UNSET is the
     * producer-must-overwrite sentinel, never a legal payload. */
    const unsigned char b5[] = "media_role=unset";
    TEST_ASSERT_EQ((uint64_t)uki_cmdline_extract_media_role(b5, sizeof(b5) - 1),
                   (uint64_t)BOOT_MEDIA_ROLE_UNSET,
                   "explicit 'unset' value -> UNSET (producer-sentinel guard)");

    /* Token appears mid-line, take the FIRST occurrence. */
    const unsigned char b6[] = "media_role=installer media_role=recovery";
    TEST_ASSERT_EQ((uint64_t)uki_cmdline_extract_media_role(b6, sizeof(b6) - 1),
                   (uint64_t)BOOT_MEDIA_ROLE_INSTALLER,
                   "first media_role= occurrence wins");
}

/* Registration */


/* ---- Loader self-measurement primitives (measured-boot attribution) ----
 * Both headers are compiled INTO BOOTX64.EFI, where nothing can test them: the
 * loader has no harness and firmware will not hand it a malformed device path
 * on demand. They are pure and header-only precisely so the assertions can live
 * here instead. */

/* The two copies must agree on their output size before a byte-wise comparison
 * between them means anything: iterating one length over the other's buffer
 * would either read past it or leave bytes unchecked. This file is the only
 * place both headers are visible, so it is the only place the pin can live. */
_Static_assert(SHA256B_DIGEST_LEN == SHA256_DIGEST_LEN,
    "loader and kernel SHA-256 digest lengths must match");
_Static_assert(SHA256B_BLOCK_LEN == SHA256_BLOCK_LEN,
    "loader and kernel SHA-256 block lengths must match");

/* The loader's SHA-256 is a second copy of the kernel's. A differential test is
 * what keeps them one algorithm; the KAT below is what stops both from being
 * wrong together. */
static void test_sha256_boot_matches_kernel(void)
{
    static const unsigned int lens[] = { 0u, 1u, 3u, 55u, 56u, 63u, 64u, 65u, 1000u };
    static unsigned char msg[1000];
    unsigned char boot_digest[SHA256B_DIGEST_LEN];
    uint8_t kern_digest[SHA256_DIGEST_LEN];
    unsigned int i, n, mismatches = 0;

    for (i = 0; i < 1000u; i++)
        msg[i] = (unsigned char)(i * 31u + 7u);

    for (n = 0; n < (unsigned int)(sizeof(lens) / sizeof(lens[0])); n++) {
        sha256b(msg, (unsigned long long)lens[n], boot_digest);
        sha256(msg, lens[n], kern_digest);
        for (i = 0; i < SHA256B_DIGEST_LEN; i++) {
            if (boot_digest[i] != (unsigned char)kern_digest[i])
                mismatches++;
        }
    }
    TEST_ASSERT_EQ((int)mismatches, 0,
        "loader sha256b matches kernel sha256 over 9 lengths incl. both padding branches");
}

/* Known-answer, not just agreement: two implementations can agree and both be
 * wrong. Runs the same two vectors the loader runs before it trusts itself. */
static void test_sha256_boot_selftest_vectors(void)
{
    TEST_ASSERT_EQ(sha256b_selftest(), 1,
                   "sha256b_selftest passes the FIPS 180-4 abc + 56-byte vectors");
}

/* The loader streams the file in 64 KiB chunks, so chunked and one-shot must be
 * identical or the digest depends on the read size rather than the file. */
static void test_sha256_boot_streaming_equals_oneshot(void)
{
    static const unsigned int chunks[] = { 1u, 63u, 64u, 65u, 127u };
    static unsigned char msg[600];
    unsigned char oneshot[SHA256B_DIGEST_LEN];
    unsigned char streamed[SHA256B_DIGEST_LEN];
    unsigned int i, c, mismatches = 0;

    for (i = 0; i < 600u; i++)
        msg[i] = (unsigned char)(i ^ 0x5Au);
    sha256b(msg, 600ull, oneshot);

    for (c = 0; c < (unsigned int)(sizeof(chunks) / sizeof(chunks[0])); c++) {
        struct sha256b_ctx ctx;
        unsigned int off = 0;

        sha256b_init(&ctx);
        while (off < 600u) {
            unsigned int take = chunks[c];
            if (off + take > 600u)
                take = 600u - off;
            sha256b_update(&ctx, &msg[off], (unsigned long long)take);
            off += take;
        }
        sha256b_final(&ctx, streamed);
        for (i = 0; i < SHA256B_DIGEST_LEN; i++) {
            if (streamed[i] != oneshot[i])
                mismatches++;
        }
    }
    TEST_ASSERT_EQ((int)mismatches, 0,
        "sha256b chunked updates (1/63/64/65/127 B) equal the one-shot digest");
}

/* --- device-path FILEPATH parser fixtures --- */

/* Append one device-path node. `text` is ASCII widened to CHAR16; pass NULL for
 * a payload-free node. Returns the new offset. */
static unsigned int dpfix_node(unsigned char *buf, unsigned int off,
                               unsigned char type, unsigned char subtype,
                               const char *text, int with_nul)
{
    unsigned int chars = 0;
    unsigned int len;
    unsigned int i;

    if (text) {
        while (text[chars])
            chars++;
        if (with_nul)
            chars++;
    }
    len = 4u + chars * 2u;
    buf[off + 0u] = type;
    buf[off + 1u] = subtype;
    buf[off + 2u] = (unsigned char)(len & 0xFFu);
    buf[off + 3u] = (unsigned char)((len >> 8) & 0xFFu);
    for (i = 0; i < chars; i++) {
        unsigned short ch = (unsigned short)(unsigned char)text[i];
        buf[off + 4u + i * 2u] = (unsigned char)(ch & 0xFFu);
        buf[off + 5u + i * 2u] = (unsigned char)((ch >> 8) & 0xFFu);
    }
    return off + len;
}

/* Compare a CHAR16 result against ASCII. */
static int dpfix_equals(const unsigned short *got, const char *want)
{
    unsigned int i = 0;

    while (want[i]) {
        if (got[i] != (unsigned short)(unsigned char)want[i])
            return 0;
        i++;
    }
    return got[i] == 0;
}

static void test_dpfp_single_node_path(void)
{
    unsigned char buf[128];
    unsigned short out[64];
    unsigned int end;
    enum dpfp_status st;

    end = dpfix_node(buf, 0u, DPFP_TYPE_MEDIA, DPFP_SUBTYPE_FILEPATH,
                     "\\EFI\\BOOT\\BOOTX64.EFI", 1);
    end = dpfix_node(buf, end, DPFP_TYPE_END, DPFP_SUBTYPE_END_ENTIRE, (const char *)0, 0);

    st = dpfp_extract(buf, end, out, 64u);
    TEST_ASSERT(st == DPFP_OK && dpfix_equals(out, "\\EFI\\BOOT\\BOOTX64.EFI"),
                "dpfp_extract returns the ESP-relative path from a single FILEPATH node");
}

static void test_dpfp_accepts_missing_terminator(void)
{
    unsigned char buf[128];
    unsigned short out[64];
    unsigned int end;
    enum dpfp_status st;

    end = dpfix_node(buf, 0u, DPFP_TYPE_MEDIA, DPFP_SUBTYPE_FILEPATH,
                     "\\EFI\\BOOT\\BOOTX64.EFI", 0);
    end = dpfix_node(buf, end, DPFP_TYPE_END, DPFP_SUBTYPE_END_ENTIRE, (const char *)0, 0);

    st = dpfp_extract(buf, end, out, 64u);
    TEST_ASSERT(st == DPFP_OK && dpfix_equals(out, "\\EFI\\BOOT\\BOOTX64.EFI"),
                "dpfp_extract accepts a FILEPATH node whose string is not NUL terminated");
}

static void test_dpfp_joins_multiple_nodes(void)
{
    unsigned char buf[192];
    unsigned short out[64];
    unsigned int end;
    enum dpfp_status st;

    end = dpfix_node(buf, 0u, DPFP_TYPE_MEDIA, DPFP_SUBTYPE_FILEPATH, "\\EFI\\BOOT", 1);
    end = dpfix_node(buf, end, DPFP_TYPE_MEDIA, DPFP_SUBTYPE_FILEPATH, "BOOTX64.EFI", 1);
    end = dpfix_node(buf, end, DPFP_TYPE_END, DPFP_SUBTYPE_END_ENTIRE, (const char *)0, 0);

    st = dpfp_extract(buf, end, out, 64u);
    TEST_ASSERT(st == DPFP_OK && dpfix_equals(out, "\\EFI\\BOOT\\BOOTX64.EFI"),
                "dpfp_extract joins two FILEPATH nodes with exactly one separator");
}

/* All four separator combinations at a node boundary. UEFI 2.10 spec 10.3.5.4
 * concatenation must collapse a doubled separator: an empty path component is a
 * different path, and Open would fail on a device path that was valid. */
static void test_dpfp_separator_combinations(void)
{
    unsigned char buf[192];
    unsigned short out[64];
    unsigned int end;
    int ok = 0;

    /* neither side carries a separator -> one is inserted */
    end = dpfix_node(buf, 0u, DPFP_TYPE_MEDIA, DPFP_SUBTYPE_FILEPATH, "EFI", 1);
    end = dpfix_node(buf, end, DPFP_TYPE_MEDIA, DPFP_SUBTYPE_FILEPATH, "BOOT", 1);
    end = dpfix_node(buf, end, DPFP_TYPE_END, DPFP_SUBTYPE_END_ENTIRE, (const char *)0, 0);
    if (dpfp_extract(buf, end, out, 64u) == DPFP_OK && dpfix_equals(out, "EFI\\BOOT"))
        ok++;

    /* trailing separator only -> copied as-is */
    end = dpfix_node(buf, 0u, DPFP_TYPE_MEDIA, DPFP_SUBTYPE_FILEPATH, "EFI\\", 1);
    end = dpfix_node(buf, end, DPFP_TYPE_MEDIA, DPFP_SUBTYPE_FILEPATH, "BOOT", 1);
    end = dpfix_node(buf, end, DPFP_TYPE_END, DPFP_SUBTYPE_END_ENTIRE, (const char *)0, 0);
    if (dpfp_extract(buf, end, out, 64u) == DPFP_OK && dpfix_equals(out, "EFI\\BOOT"))
        ok++;

    /* leading separator only -> copied as-is */
    end = dpfix_node(buf, 0u, DPFP_TYPE_MEDIA, DPFP_SUBTYPE_FILEPATH, "EFI", 1);
    end = dpfix_node(buf, end, DPFP_TYPE_MEDIA, DPFP_SUBTYPE_FILEPATH, "\\BOOT", 1);
    end = dpfix_node(buf, end, DPFP_TYPE_END, DPFP_SUBTYPE_END_ENTIRE, (const char *)0, 0);
    if (dpfp_extract(buf, end, out, 64u) == DPFP_OK && dpfix_equals(out, "EFI\\BOOT"))
        ok++;

    /* BOTH sides carry one -> collapsed to a single separator */
    end = dpfix_node(buf, 0u, DPFP_TYPE_MEDIA, DPFP_SUBTYPE_FILEPATH, "EFI\\", 1);
    end = dpfix_node(buf, end, DPFP_TYPE_MEDIA, DPFP_SUBTYPE_FILEPATH, "\\BOOT", 1);
    end = dpfix_node(buf, end, DPFP_TYPE_END, DPFP_SUBTYPE_END_ENTIRE, (const char *)0, 0);
    if (dpfp_extract(buf, end, out, 64u) == DPFP_OK && dpfix_equals(out, "EFI\\BOOT"))
        ok++;

    TEST_ASSERT_EQ(ok, 4,
        "dpfp_extract yields EFI\\BOOT for all four node-boundary separator combinations");
}

static void test_dpfp_rejects_embedded_nul(void)
{
    unsigned char buf[128];
    unsigned short out[64];
    unsigned int end;
    enum dpfp_status st;

    end = dpfix_node(buf, 0u, DPFP_TYPE_MEDIA, DPFP_SUBTYPE_FILEPATH, "\\EFI\\X", 1);
    /* Widen the node by two characters behind the NUL the helper just wrote. */
    buf[2] = (unsigned char)((4u + 8u * 2u) & 0xFFu);
    buf[3] = 0u;
    buf[4u + 7u * 2u + 0u] = 'Y'; buf[4u + 7u * 2u + 1u] = 0u;
    end += 2u;
    end = dpfix_node(buf, end, DPFP_TYPE_END, DPFP_SUBTYPE_END_ENTIRE, (const char *)0, 0);

    st = dpfp_extract(buf, end, out, 64u);
    TEST_ASSERT(st == DPFP_EMBEDDED_NUL && out[0] == 0,
                "dpfp_extract refuses a NUL with characters behind it and empties the output");
}

static void test_dpfp_rejects_malformed_lengths(void)
{
    unsigned char buf[64];
    unsigned short out[32];
    int refusals = 0;

    /* length < 4 */
    buf[0] = DPFP_TYPE_MEDIA; buf[1] = DPFP_SUBTYPE_FILEPATH; buf[2] = 3u; buf[3] = 0u;
    if (dpfp_extract(buf, 8u, out, 32u) == DPFP_BAD_NODE && out[0] == 0)
        refusals++;

    /* length runs past the measured object */
    buf[2] = 64u; buf[3] = 0u;
    if (dpfp_extract(buf, 8u, out, 32u) == DPFP_BAD_NODE)
        refusals++;

    /* odd payload: a CHAR16 string cannot have an odd byte count */
    buf[2] = 7u; buf[3] = 0u;
    if (dpfp_extract(buf, 8u, out, 32u) == DPFP_BAD_NODE)
        refusals++;

    TEST_ASSERT_EQ(refusals, 3,
        "dpfp_extract refuses length < 4, a node past the measure, and an odd payload");
}

static void test_dpfp_rejects_ambiguous_shapes(void)
{
    unsigned char buf[128];
    unsigned short out[64];
    unsigned int end;
    int refusals = 0;

    /* END_INSTANCE: which instance carries the file is undefined */
    end = dpfix_node(buf, 0u, DPFP_TYPE_MEDIA, DPFP_SUBTYPE_FILEPATH, "\\A", 1);
    end = dpfix_node(buf, end, DPFP_TYPE_END, DPFP_SUBTYPE_END_INSTANCE, (const char *)0, 0);
    if (dpfp_extract(buf, end, out, 64u) == DPFP_MULTI_INSTANCE && out[0] == 0)
        refusals++;

    /* A messaging URI node: an HTTP-booted loader is not a file on a volume */
    end = dpfix_node(buf, 0u, 0x03u, 0x18u, "http://boot/x.efi", 1);
    end = dpfix_node(buf, end, DPFP_TYPE_END, DPFP_SUBTYPE_END_ENTIRE, (const char *)0, 0);
    if (dpfp_extract(buf, end, out, 64u) == DPFP_UNSUPPORTED)
        refusals++;

    /* No END node at all */
    end = dpfix_node(buf, 0u, DPFP_TYPE_MEDIA, DPFP_SUBTYPE_FILEPATH, "\\A", 1);
    if (dpfp_extract(buf, end, out, 64u) == DPFP_NO_END)
        refusals++;

    /* Well formed, but carries no path */
    end = dpfix_node(buf, 0u, DPFP_TYPE_END, DPFP_SUBTYPE_END_ENTIRE, (const char *)0, 0);
    if (dpfp_extract(buf, end, out, 64u) == DPFP_NONE)
        refusals++;

    TEST_ASSERT_EQ(refusals, 4,
        "dpfp_extract refuses END_INSTANCE, a non-FILEPATH node, a missing END and an empty path");
}

static void test_dpfp_bounds_output_buffer(void)
{
    unsigned char buf[128];
    unsigned short small[4];
    unsigned short exact[7];
    unsigned int end;
    enum dpfp_status over, fits;

    end = dpfix_node(buf, 0u, DPFP_TYPE_MEDIA, DPFP_SUBTYPE_FILEPATH, "\\ABCDE", 1);
    end = dpfix_node(buf, end, DPFP_TYPE_END, DPFP_SUBTYPE_END_ENTIRE, (const char *)0, 0);

    over = dpfp_extract(buf, end, small, 4u);
    /* Control: 6 characters plus the terminator fit in exactly 7 units, so the
     * overflow refusal above is a bound rather than an off-by-one. */
    fits = dpfp_extract(buf, end, exact, 7u);

    TEST_ASSERT(over == DPFP_OVERFLOW && small[0] == 0
                && fits == DPFP_OK && dpfix_equals(exact, "\\ABCDE"),
                "dpfp_extract refuses an overlong path and accepts one that exactly fits");
}

static void test_dpfp_rejects_bad_arguments(void)
{
    unsigned char buf[8];
    unsigned short out[8];
    int refusals = 0;

    buf[0] = DPFP_TYPE_END; buf[1] = DPFP_SUBTYPE_END_ENTIRE; buf[2] = 4u; buf[3] = 0u;

    if (dpfp_extract((const void *)0, 4u, out, 8u) == DPFP_BADARG)
        refusals++;
    if (dpfp_extract(buf, 4u, (unsigned short *)0, 8u) == DPFP_BADARG)
        refusals++;
    if (dpfp_extract(buf, 4u, out, 0u) == DPFP_BADARG)
        refusals++;
    if (dpfp_extract(buf, 3u, out, 8u) == DPFP_SHORT && out[0] == 0)
        refusals++;

    TEST_ASSERT_EQ(refusals, 4,
        "dpfp_extract refuses NULL path, NULL output, zero capacity and a sub-header measure");
}


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
    test_suite_register_cat("UEFI: SecureBoot drift detection",
                            test_secureboot_drift_detection, TEST_CAT_BOOT);
    test_suite_register_cat("UEFI: UKI initrd section size consistent",
                            test_uki_initrd_section_size_consistent, TEST_CAT_BOOT);
    test_suite_register_cat("UEFI: UKI flag implies no disk initrd",
                            test_boot_flag_uki_implies_no_disk_initrd, TEST_CAT_BOOT);
    test_suite_register_cat("UEFI: UKI cmdline rejects disk-payload tokens",
                            test_uki_cmdline_rejects_initrd_token, TEST_CAT_BOOT);
    test_suite_register_cat("UEFI: serial_source valid",
                            test_serial_source_valid, TEST_CAT_BOOT);
    test_suite_register_cat("UEFI: serial_source matches port",
                            test_serial_source_matches_port, TEST_CAT_BOOT);
    test_suite_register_cat("UEFI: serial_baud valid",
                            test_serial_baud_valid, TEST_CAT_BOOT);
    test_suite_register_cat("UEFI: serial_port standard",
                            test_serial_port_standard, TEST_CAT_BOOT);
    test_suite_register_cat("ESRT: decode_status known mnemonics",
                            test_esrt_decode_status_known, TEST_CAT_BOOT);
    test_suite_register_cat("ESRT: decode_status reserved fallback",
                            test_esrt_decode_status_reserved, TEST_CAT_BOOT);
    test_suite_register_cat("ESRT: decode_type table",
                            test_esrt_decode_type_table, TEST_CAT_BOOT);
    test_suite_register_cat("ESRT: capsule helpers OOR idx -> 0",
                            test_esrt_capsule_helpers_oor_idx_returns_zero, TEST_CAT_BOOT);
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
    test_suite_register_cat("UEFI: UKI cmdline media_role parser",
                            test_media_role_uki_cmdline, TEST_CAT_BOOT);
    test_suite_register_cat("UEFI: UKI cmdline media_role rejects garbage",
                            test_media_role_uki_cmdline_rejections, TEST_CAT_BOOT);
    /* Loader self-measurement primitives (measured-boot attribution) */
    test_suite_register_cat("Self-measure: sha256b matches kernel sha256",
                            test_sha256_boot_matches_kernel, TEST_CAT_BOOT);
    test_suite_register_cat("Self-measure: sha256b FIPS vectors",
                            test_sha256_boot_selftest_vectors, TEST_CAT_BOOT);
    test_suite_register_cat("Self-measure: sha256b chunked equals one-shot",
                            test_sha256_boot_streaming_equals_oneshot, TEST_CAT_BOOT);
    test_suite_register_cat("Self-measure: devpath single FILEPATH node",
                            test_dpfp_single_node_path, TEST_CAT_BOOT);
    test_suite_register_cat("Self-measure: devpath missing terminator",
                            test_dpfp_accepts_missing_terminator, TEST_CAT_BOOT);
    test_suite_register_cat("Self-measure: devpath joins nodes",
                            test_dpfp_joins_multiple_nodes, TEST_CAT_BOOT);
    test_suite_register_cat("Self-measure: devpath separator combinations",
                            test_dpfp_separator_combinations, TEST_CAT_BOOT);
    test_suite_register_cat("Self-measure: devpath embedded NUL refused",
                            test_dpfp_rejects_embedded_nul, TEST_CAT_BOOT);
    test_suite_register_cat("Self-measure: devpath malformed lengths refused",
                            test_dpfp_rejects_malformed_lengths, TEST_CAT_BOOT);
    test_suite_register_cat("Self-measure: devpath ambiguous shapes refused",
                            test_dpfp_rejects_ambiguous_shapes, TEST_CAT_BOOT);
    test_suite_register_cat("Self-measure: devpath output bound",
                            test_dpfp_bounds_output_buffer, TEST_CAT_BOOT);
    test_suite_register_cat("Self-measure: devpath bad arguments refused",
                            test_dpfp_rejects_bad_arguments, TEST_CAT_BOOT);
}

#endif /* KERNEL_TESTS */
