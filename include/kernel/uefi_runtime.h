/* ============================================================================
 * uefi_runtime.h — UEFI Runtime Services kernel interface
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

/* UEFI calling convention — Microsoft x64 ABI (rcx, rdx, r8, r9) */
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

/* EFI_RT_PROPERTIES_TABLE — found via Configuration Table GUID lookup */
struct uefi_rt_properties_table {
    uint16_t version;
    uint16_t length;
    uint32_t runtime_services_supported;  /* bitmask of EFI_RT_SUPPORTED_* */
};

/* ---- Kernel API ---- */

/* Initialize UEFI runtime services.
 * Calls SetVirtualAddressMap() (identity mapping: virt = phys) and reads
 * EFI_RT_PROPERTIES_TABLE to determine supported services.
 * Must be called ONCE, early in kernel init, after uefi_config_init(). */
void uefi_runtime_init(void);

/* Returns 1 if runtime services are usable. */
int uefi_rt_available(void);

/* Returns bitmask of supported services (EFI_RT_SUPPORTED_*).
 * If EFI_RT_PROPERTIES_TABLE is absent, all services are assumed supported. */
uint32_t uefi_rt_supported(void);

/* ---- EFI status codes (extended) ---- */
#define UEFI_NOT_FOUND          (14ULL | (1ULL << 63))
#define UEFI_BUFFER_TOO_SMALL   (5ULL | (1ULL << 63))
#define UEFI_OUT_OF_RESOURCES   (9ULL | (1ULL << 63))
#define UEFI_DEVICE_ERROR       (7ULL | (1ULL << 63))
#define UEFI_SECURITY_VIOLATION (26ULL | (1ULL << 63))

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

/* Enumerate all variables (for logging/debugging).
 * Returns the total number of variables found. */
uint32_t uefi_enumerate_variables(void);

/* Initialize variable services — enumerate + log summary.
 * Must be called after uefi_runtime_init(). */
void uefi_vars_init(void);

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

/* EFI_TIME structure — returned by GetTime() */
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

/* EFI_TIME_CAPABILITIES — RTC resolution and accuracy */
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

/* Initialize time services — read and log current time. */
void uefi_time_init(void);
