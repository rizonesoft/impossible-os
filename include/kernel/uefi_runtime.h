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
