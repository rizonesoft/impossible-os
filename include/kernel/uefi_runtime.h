/* ============================================================================
 * uefi_runtime.h -- UEFI Runtime Services kernel interface
 *
 * After ExitBootServices(), the UEFI firmware still provides runtime
 * services (GetTime, GetVariable, ResetSystem, etc.) through function
 * pointers in the EFI_RUNTIME_SERVICES table.  This module calls
 * SetVirtualAddressMap() to remap firmware runtime memory into the
 * kernel's address space, then exposes runtime service wrappers.
 *
 * All runtime service calls are serialized with a spinlock because
 * UEFI firmware is not reentrant.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"
#include "kernel/boot_info.h"

/* UEFI calling convention -- Microsoft x64 ABI (rcx, rdx, r8, r9) */
#define UEFI_EFIAPI __attribute__((ms_abi))

/* ---- EFI status codes (for kernel callers) ---- */
#define UEFI_SUCCESS            0ULL
#define UEFI_UNSUPPORTED        (3ULL | (1ULL << 63))

/* ---- EFI_RT_PROPERTIES_TABLE supported-service bitmask ---- */
#define EFI_RT_SUPPORTED_GET_TIME                0x0001
#define EFI_RT_SUPPORTED_SET_TIME                0x0002
#define EFI_RT_SUPPORTED_GET_WAKEUP_TIME         0x0004
#define EFI_RT_SUPPORTED_SET_WAKEUP_TIME         0x0008
#define EFI_RT_SUPPORTED_GET_VARIABLE            0x0010
#define EFI_RT_SUPPORTED_GET_NEXT_VARIABLE_NAME  0x0020
#define EFI_RT_SUPPORTED_SET_VARIABLE            0x0040
#define EFI_RT_SUPPORTED_SET_VIRTUAL_ADDRESS_MAP  0x0080
#define EFI_RT_SUPPORTED_CONVERT_POINTER         0x0100
#define EFI_RT_SUPPORTED_GET_NEXT_HIGH_MONO      0x0200
#define EFI_RT_SUPPORTED_RESET_SYSTEM            0x0400
#define EFI_RT_SUPPORTED_UPDATE_CAPSULE          0x0800
#define EFI_RT_SUPPORTED_QUERY_CAPSULE_CAP       0x1000
#define EFI_RT_SUPPORTED_QUERY_VARIABLE_INFO     0x2000

/* EFI_RT_PROPERTIES_TABLE -- found via Configuration Table GUID lookup */
struct uefi_rt_properties_table {
    uint16_t version;
    uint16_t length;
    uint32_t runtime_services_supported;  /* bitmask of EFI_RT_SUPPORTED_* */
};

/* ---- Kernel API ---- */

/* Initialize UEFI runtime services.
 * Calls SetVirtualAddressMap() (identity mapping: virt = phys) and reads
 * EFI_RT_PROPERTIES_TABLE to determine supported services.
 * Must be called ONCE, early in kernel init, after uefi_config_init().
 * Returns BOOT_OK on success or BOOT_DEGRADED if runtime unavailable. */
#include "kernel/boot_init.h"
boot_result_t uefi_runtime_init(void);

/* Returns 1 if runtime services are usable. */
int uefi_rt_available(void);

/* Returns bitmask of supported services (EFI_RT_SUPPORTED_*).
 * If EFI_RT_PROPERTIES_TABLE is absent, all services are assumed supported. */
uint32_t uefi_rt_supported(void);

/* ---- EFI status codes (extended) ---- */
#define UEFI_NOT_FOUND            (14ULL | (1ULL << 63))
#define UEFI_BUFFER_TOO_SMALL     (5ULL  | (1ULL << 63))
#define UEFI_INVALID_PARAMETER    (2ULL  | (1ULL << 63))
#define UEFI_OUT_OF_RESOURCES     (9ULL  | (1ULL << 63))
#define UEFI_DEVICE_ERROR         (7ULL  | (1ULL << 63))
#define UEFI_WRITE_PROTECTED      (8ULL  | (1ULL << 63))
#define UEFI_SECURITY_VIOLATION   (26ULL | (1ULL << 63))

/* ---- Shared EFI -> NTSTATUS mapper (single source of truth) ---- */
#include "kernel/nt/ntstatus.h"

/* SCOPE-GAP-ALLOWED: STATUS_NOT_IMPLEMENTED is a legitimate NTSTATUS mapping for EFI_UNSUPPORTED */
static inline NTSTATUS efi_status_to_ntstatus(uint64_t s)
{
    if (s == UEFI_SUCCESS)            return STATUS_SUCCESS;
    if (s == UEFI_NOT_FOUND)          return STATUS_NOT_FOUND;
    if (s == UEFI_BUFFER_TOO_SMALL)   return STATUS_BUFFER_TOO_SMALL;
    if (s == UEFI_UNSUPPORTED)        return STATUS_NOT_IMPLEMENTED;
    if (s == UEFI_INVALID_PARAMETER)  return STATUS_INVALID_PARAMETER;
    if (s == UEFI_DEVICE_ERROR)       return STATUS_IO_DEVICE_ERROR;
    if (s == UEFI_WRITE_PROTECTED)    return STATUS_MEDIA_WRITE_PROTECTED;
    if (s == UEFI_SECURITY_VIOLATION) return STATUS_ACCESS_DENIED;
    if (s == UEFI_OUT_OF_RESOURCES)   return STATUS_INSUFFICIENT_RESOURCES;
    return STATUS_UNSUCCESSFUL;
}

/* ---- UEFI Variable Attributes ---- */
#define EFI_VARIABLE_NON_VOLATILE                          0x00000001
#define EFI_VARIABLE_BOOTSERVICE_ACCESS                    0x00000002
#define EFI_VARIABLE_RUNTIME_ACCESS                        0x00000004
#define EFI_VARIABLE_HARDWARE_ERROR_RECORD                 0x00000008
#define EFI_VARIABLE_AUTHENTICATED_WRITE_ACCESS            0x00000010  /* deprecated */
#define EFI_VARIABLE_TIME_BASED_AUTHENTICATED_WRITE_ACCESS 0x00000020
#define EFI_VARIABLE_APPEND_WRITE                          0x00000040

/* EFI Global Variable GUID: {8BE4DF61-93CA-11D2-AA0D-00E098032B8C} */
#define EFI_GLOBAL_VARIABLE_GUID \
    ((struct boot_uefi_guid){ 0x8be4df61, 0x93ca, 0x11d2, \
        { 0xaa, 0x0d, 0x00, 0xe0, 0x98, 0x03, 0x2b, 0x8c } })

/* ---- Variable Services API ---- */

/* Get a UEFI variable by GUID and name (UCS-2 string).
 * data: output buffer, data_size: in/out size.
 * Returns EFI status code (UEFI_SUCCESS, UEFI_NOT_FOUND, etc.) */
uint64_t uefi_get_variable(const struct boot_uefi_guid *guid,
                           const uint16_t *name,
                           uint32_t *attributes,
                           uint64_t *data_size,
                           void *data);

/* Set a UEFI variable. data_size=0 deletes the variable.
 * Returns EFI status code. */
uint64_t uefi_set_variable(const struct boot_uefi_guid *guid,
                           const uint16_t *name,
                           uint32_t attributes,
                           uint64_t data_size,
                           const void *data);

/* Advance the variable enumeration cursor by one step.
 * On the first call: name[0] must be 0 and *guid must be all-zero.
 * name_size: on entry = buffer byte capacity; on exit = actual name bytes.
 * Returns UEFI_SUCCESS to continue, UEFI_NOT_FOUND when done,
 *         UEFI_BUFFER_TOO_SMALL if name buffer is too small (retry with
 *         updated name_size), UEFI_UNSUPPORTED if service unavailable. */
uint64_t uefi_get_next_variable_name(uint64_t *name_size, uint16_t *name,
                                     struct boot_uefi_guid *guid);

/* Query NVRAM capacity for variables with the given attributes.
 * max_storage: maximum storage in bytes for this attribute set.
 * remaining: remaining storage in bytes.
 * max_var_size: maximum size of a single variable.
 * Returns EFI status code. */
uint64_t uefi_query_variable_info(uint32_t attributes,
                                  uint64_t *max_storage,
                                  uint64_t *remaining,
                                  uint64_t *max_var_size);

/* Initialize variable services -- enumerate + log summary.
 * Must be called after uefi_runtime_init().
 * Returns BOOT_OK on success, BOOT_DEGRADED if unavailable. */
boot_result_t uefi_vars_init(void);

/* ---- System Reset API ---- */

/* EFI_RESET_TYPE values */
#define EFI_RESET_COLD              0  /* Full hardware power cycle */
#define EFI_RESET_WARM              1  /* CPU reset, no power cycle */
#define EFI_RESET_SHUTDOWN          2  /* Power off */
#define EFI_RESET_PLATFORM_SPECIFIC 3  /* Platform-defined (recovery, etc.) */

/* Reset the system via UEFI ResetSystem().
 * This function does NOT return on success.
 * Falls back to keyboard controller reset (0x64/0xFE) if UEFI unavailable. */
void uefi_reset(uint32_t reset_type);

/* Convenience wrappers */
static inline void uefi_reboot(void)   { uefi_reset(EFI_RESET_COLD); }
static inline void uefi_shutdown(void) { uefi_reset(EFI_RESET_SHUTDOWN); }

/* ---- RTC Time Services API ---- */

/* EFI_TIME structure -- returned by GetTime() */
struct efi_time {
    uint16_t year;        /* 1900–9999 */
    uint8_t  month;       /* 1–12 */
    uint8_t  day;         /* 1–31 */
    uint8_t  hour;        /* 0–23 */
    uint8_t  minute;      /* 0–59 */
    uint8_t  second;      /* 0–59 */
    uint8_t  pad1;
    uint32_t nanosecond;  /* 0–999,999,999 */
    int16_t  timezone;    /* minutes from UTC (-1440 to 1440) */
    uint8_t  daylight;    /* daylight savings flags */
    uint8_t  pad2;
};

/* EFI_TIME_CAPABILITIES -- RTC resolution and accuracy */
struct efi_time_capabilities {
    uint32_t resolution;  /* ticks per second (1 = 1-sec resolution) */
    uint32_t accuracy;    /* error in parts per million */
    uint8_t  sets_to_zero; /* TRUE if time is reset on SetTime() */
};

/* Timezone constants */
#define EFI_UNSPECIFIED_TIMEZONE  0x07FF  /* timezone not specified */

/* Daylight savings flags */
#define EFI_TIME_ADJUST_DAYLIGHT  0x01  /* time is affected by DST */
#define EFI_TIME_IN_DAYLIGHT      0x02  /* currently in DST */

/* Get current time via UEFI GetTime().
 * Returns EFI status code. caps may be NULL. */
uint64_t uefi_get_time(struct efi_time *time,
                       struct efi_time_capabilities *caps);

/* Set time via UEFI SetTime().
 * Returns EFI status code. */
uint64_t uefi_set_time(const struct efi_time *time);

/* Get RTC wakeup alarm state.
 * Returns EFI status code. */
uint64_t uefi_get_wakeup_time(uint8_t *enabled, uint8_t *pending,
                              struct efi_time *time);

/* Initialize time services -- read and log current time.
 * Returns BOOT_OK on success, BOOT_DEGRADED if unavailable. */
boot_result_t uefi_time_init(void);

/* ---- Secure Boot State Detection API ---- */

/* Initialize Secure Boot state by reading UEFI NVRAM variables.
 * Must be called after uefi_runtime_init() + uefi_vars_init().
 * Returns BOOT_OK on success, BOOT_DEGRADED if unavailable. */
boot_result_t uefi_secureboot_init(void);

/* Write HKLM\SYSTEM\SecureBoot\State to the registry.
 * Called from registry_populate_defaults() after registry_init(). */
void uefi_secureboot_populate_registry(void);

/* Surface QueryVariableInfo() results to HKLM\SYSTEM\SecureBoot\Vars
 * so post-boot tools can see the firmware variable storage quota
 * (production firmware enforces the documented per-machine cap).
 * Writes VarsValid (DWORD) always; size fields (QWORD MaxStorageSize,
 * RemainingSize, MaxVariableSize) ONLY on successful query. Consumers
 * MUST check VarsValid before reading sizes. Called from
 * registry_populate_defaults() after uefi_secureboot_populate_registry. */
void uefi_runtime_populate_vars_registry(void);

/* Returns 1 if Secure Boot is enabled by firmware. */
int uefi_secureboot_enabled(void);

/* Returns 1 if firmware is in Setup Mode (no Platform Key enrolled). */
int uefi_secureboot_setup_mode(void);

/* Returns 1 if a Platform Key (PK) is enrolled. */
int uefi_secureboot_pk_present(void);

/* Returns 1 if a Key Exchange Key (KEK) is enrolled. */
int uefi_secureboot_kek_present(void);

/* ---- Secure Boot Key Management API ---- */

/* Security database GUID -- used for db, dbx, dbt variables */
#define EFI_IMAGE_SECURITY_DATABASE_GUID \
    ((struct boot_uefi_guid){ 0xd719b2cb, 0x3d3a, 0x4596, \
        { 0xa3, 0xbc, 0xda, 0xd0, 0x0e, 0x67, 0x65, 0x6f } })

/* Signature type GUIDs */
#define EFI_CERT_SHA256_GUID \
    ((struct boot_uefi_guid){ 0xc1c41626, 0x504c, 0x4092, \
        { 0xac, 0xa9, 0x41, 0xf9, 0x36, 0x93, 0x43, 0x28 } })

#define EFI_CERT_X509_GUID \
    ((struct boot_uefi_guid){ 0xa5c059a1, 0x94e4, 0x4aa7, \
        { 0x87, 0xb5, 0xab, 0x15, 0x5c, 0x2b, 0xf0, 0x72 } })

#define EFI_CERT_RSA2048_GUID \
    ((struct boot_uefi_guid){ 0x3c5766e8, 0x269c, 0x4e34, \
        { 0xaa, 0x14, 0xed, 0x77, 0x6e, 0x85, 0xb3, 0xb6 } })

/* EFI_SIGNATURE_LIST header */
struct efi_signature_list {
    struct boot_uefi_guid signature_type;
    uint32_t signature_list_size;   /* total size including header + data */
    uint32_t signature_header_size; /* size of optional header (usually 0) */
    uint32_t signature_size;        /* size of each EFI_SIGNATURE_DATA */
};

/* EFI_SIGNATURE_DATA (one per entry in a signature list) */
struct efi_signature_data {
    struct boot_uefi_guid signature_owner;
    /* followed by signature_size - 16 bytes of signature data */
};

/* Parsed Secure Boot database summary */
struct secureboot_db_info {
    uint32_t db_entries;    /* trusted certs/hashes in db */
    uint32_t dbx_entries;   /* revoked certs/hashes in dbx */
    uint32_t dbt_entries;   /* timestamp entries in dbt */
    uint32_t db_x509_count; /* X.509 certificates in db */
    uint32_t db_sha256_count; /* SHA-256 hashes in db */
    uint32_t dbx_sha256_count; /* SHA-256 hashes in dbx */
};

/* Initialize Secure Boot key enumeration -- read and parse db/dbx/dbt.
 * Must be called after uefi_secureboot_init(). */
void secureboot_keys_init(void);

/* Returns parsed database summary (valid after secureboot_keys_init). */
const struct secureboot_db_info *secureboot_get_db_info(void);

/* ---- Crypto Agility API (§5.3 -- UEFI 2.10) ----
 *
 * UEFI 2.10 introduces three variables for dynamic algorithm negotiation:
 *   - CryptoIndicationsSupported (firmware-owned): bitmask of all algorithms
 *     the firmware can use for Secure Boot signature verification.
 *   - CryptoIndications (OS-owned): bitmask the OS writes to request
 *     specific algorithms for next boot. Enables algorithm upgrade
 *     without firmware reflash.
 *   - CryptoIndicationsActivated (firmware-owned): bitmask of algorithms
 *     actually active for this boot. Firmware confirms what it's using.
 *
 * Critical for the 2026 Microsoft Secure Boot certificate rollover:
 * - Current: SHA-256 + RSA-2048 (PKCS#1 v1.5)
 * - 2026+:   SHA-384 + RSA-3072/4096 or ECDSA P-384
 *
 * STATUS: Reader implementation. Writes to CryptoIndications deferred
 * until crypto policy engine is built.
 * ---- */

/* EFI_CRYPTO_INDICATION bitmask values (UEFI 2.10 §32.4.3) */
#define CRYPTO_IND_RSA_2048_SHA256   (1U << 0)  /* RSA-2048, PKCS#1 v1.5 */
#define CRYPTO_IND_RSA_3072_SHA384   (1U << 1)  /* RSA-3072, PKCS#1 v1.5 */
#define CRYPTO_IND_RSA_4096_SHA512   (1U << 2)  /* RSA-4096, PKCS#1 v1.5 */
#define CRYPTO_IND_RSA_2048_PSS      (1U << 3)  /* RSA-2048, PSS */
#define CRYPTO_IND_RSA_3072_PSS      (1U << 4)  /* RSA-3072, PSS */
#define CRYPTO_IND_RSA_4096_PSS      (1U << 5)  /* RSA-4096, PSS */
#define CRYPTO_IND_ECDSA_P256        (1U << 6)  /* ECDSA P-256, SHA-256 */
#define CRYPTO_IND_ECDSA_P384        (1U << 7)  /* ECDSA P-384, SHA-384 */
#define CRYPTO_IND_SHA256            (1U << 8)  /* SHA-256 hash */
#define CRYPTO_IND_SHA384            (1U << 9)  /* SHA-384 hash */
#define CRYPTO_IND_SHA512            (1U << 10) /* SHA-512 hash */

/* Parsed crypto agility state */
struct crypto_agility_info {
    uint32_t supported;     /* firmware-supported algorithms (bitmask) */
    uint32_t requested;     /* OS-requested algorithms (bitmask) */
    uint32_t activated;     /* currently active algorithms (bitmask) */
    uint8_t  available;     /* 1 if firmware supports CryptoIndications */
    uint8_t  pad[3];
};

/* Initialize crypto agility -- read all three CryptoIndication variables.
 * Must be called after uefi_runtime_init(). Graceful if firmware lacks
 * UEFI 2.10 support (available=0). */
void uefi_crypto_agility_init(void);

/* Returns parsed crypto agility state. */
const struct crypto_agility_info *uefi_crypto_agility_info(void);

/* ---- Capsule Firmware Update API ----
 *
 * ╔══════════════════════════════════════════════════════════════════════╗
 * ║  ⚠️  DANGER: UpdateCapsule() CAN PERMANENTLY BRICK YOUR HARDWARE  ⚠️  ║
 * ╠══════════════════════════════════════════════════════════════════════╣
 * ║                                                                    ║
 * ║  UpdateCapsule() writes DIRECTLY to firmware flash (SPI NOR).      ║
 * ║  A wrong capsule, corrupted data, or power loss during flash       ║
 * ║  will permanently brick the motherboard.  There is NO recovery     ║
 * ║  short of a hardware flash programmer (SPI clip + CH341A).         ║
 * ║                                                                    ║
 * ║  BEFORE implementing UpdateCapsule():                              ║
 * ║    1. Capsule must be cryptographically signed by the OEM          ║
 * ║    2. Signature must be verified (RSA-2048/SHA-256 minimum)        ║
 * ║    3. CapsuleGuid must match ESRT FwClass exactly                  ║
 * ║    4. Version must be >= LowestSupportedVersion from ESRT          ║
 * ║    5. System must be on AC power (not battery)                     ║
 * ║    6. User must explicitly confirm ("This will update firmware")   ║
 * ║    7. A full crypto stack (PKCS#7/CMS) must exist in the kernel    ║
 * ║                                                                    ║
 * ║  This API exposes ONLY QueryCapsuleCapabilities() -- read-only.    ║
 * ║  UpdateCapsule() is intentionally NOT exposed.                     ║
 * ╚══════════════════════════════════════════════════════════════════════╝
 * ---- */

/* EFI_CAPSULE_HEADER (UEFI Spec §8.5.3) */
struct efi_capsule_header {
    struct boot_uefi_guid capsule_guid;
    uint32_t header_size;
    uint32_t flags;
    uint32_t capsule_image_size;
};

/* Capsule flags */
#define CAPSULE_FLAGS_PERSIST_ACROSS_RESET   0x00010000
#define CAPSULE_FLAGS_POPULATE_SYSTEM_TABLE  0x00020000
#define CAPSULE_FLAGS_INITIATE_RESET         0x00040000

/* Capsule capability query result */
struct capsule_capability_info {
    uint64_t max_capsule_size;   /* largest capsule firmware will accept */
    uint32_t reset_type;         /* reset type needed to apply (0=cold, 1=warm) */
    uint8_t  supported;          /* 1 if firmware supports capsule updates */
    uint8_t  pad[3];
};

/* Initialize capsule subsystem -- query firmware capabilities.
 * Read-only: does NOT call UpdateCapsule() or modify firmware.
 * Must be called after uefi_runtime_init(). */
void uefi_capsule_init(void);

/* Returns 1 if firmware supports capsule updates. */
int uefi_capsule_supported(void);

/* Returns capsule capability info (valid after uefi_capsule_init). */
const struct capsule_capability_info *uefi_capsule_info(void);

/* Register NtQuerySystemEnvironmentValue[Ex] / NtSetSystemEnvironmentValue[Ex]
 * in the SSDT. Call after ssdt_init(). */
void uefi_register_ssdt(void);
