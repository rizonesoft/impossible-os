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
