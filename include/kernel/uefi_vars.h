/* ============================================================================
 * uefi_vars.h -- NTSTATUS-returning UEFI variable services for kernel use
 *
 * Thin abstraction layer over uefi_runtime.c primitives.  Translates EFI
 * status codes to NTSTATUS so kernel subsystems consume a consistent error
 * type regardless of the underlying firmware interface.
 *
 * Caller contract:
 *   - Must call uefi_runtime_init() + uefi_vars_init() before using this API.
 *   - All functions silently return STATUS_NOT_IMPLEMENTED when runtime
 *     services are unavailable (uefi_rt_available() == 0).
 *
 * NTSTATUS is defined here as a temporary home.  When TODO-05 (Native API
 * Layer) scopes the full Win32 API surface it will be moved to a central
 * kernel/ntstatus.h; a forwarding include will remain here.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"
#include "kernel/boot_info.h"
#include "kernel/uefi_runtime.h"

/* ---- NTSTATUS type ---- */

typedef int32_t NTSTATUS;

#define STATUS_SUCCESS              ((NTSTATUS)0x00000000)
#define STATUS_UNSUCCESSFUL         ((NTSTATUS)0xC0000001)
#define STATUS_NOT_IMPLEMENTED      ((NTSTATUS)0xC0000002)
#define STATUS_INVALID_PARAMETER    ((NTSTATUS)0xC000000D)
#define STATUS_BUFFER_TOO_SMALL     ((NTSTATUS)0xC0000023)
#define STATUS_NOT_FOUND            ((NTSTATUS)0xC0000225)

/* Returns non-zero when status indicates success (high bit clear). */
#define NT_SUCCESS(s)   (((NTSTATUS)(s)) >= 0)

/* ---- EFI GUID type alias ---- */

typedef struct boot_uefi_guid efi_guid_t;

/* ---- Well-known GUIDs ---- */

/* EFI Global Variable GUID: {8BE4DF61-93CA-11D2-AA0D-00E098032B8C} */
#define EFI_GLOBAL_VARIABLE_GUID_INIT \
    ((efi_guid_t){ 0x8be4df61, 0x93ca, 0x11d2, \
        { 0xaa, 0x0d, 0x00, 0xe0, 0x98, 0x03, 0x2b, 0x8c } })

/* EFI Image Security Database GUID: {D719B2CB-3D3A-4596-A3BC-DAD00E67656F} */
#define EFI_IMAGE_SECURITY_DATABASE_GUID_INIT \
    ((efi_guid_t){ 0xd719b2cb, 0x3d3a, 0x4596, \
        { 0xa3, 0xbc, 0xda, 0xd0, 0x0e, 0x67, 0x65, 0x6f } })

/* Impossible OS vendor GUID: {6F35D3A4-C0E6-4A82-B5D8-7C9D2E4F8A13}
 * Used for all OS-owned NVRAM variables (A/B slot state, boot counters,
 * OS-specific settings).  Reserve this namespace exclusively for
 * Impossible OS use -- never share it with third-party code. */
#define IMPOSSIBLE_OS_VENDOR_GUID_INIT \
    ((efi_guid_t){ 0x6f35d3a4, 0xc0e6, 0x4a82, \
        { 0xb5, 0xd8, 0x7c, 0x9d, 0x2e, 0x4f, 0x8a, 0x13 } })

/* Convenience shorthand -- combined attribute set for persistent OS variables */
#define UEFI_VAR_NV_BOOT_RUNTIME \
    (EFI_VARIABLE_NON_VOLATILE | \
     EFI_VARIABLE_BOOTSERVICE_ACCESS | \
     EFI_VARIABLE_RUNTIME_ACCESS)

/* ============================================================================
 * Kernel variable services API
 * ============================================================================ */

/* Get a UEFI variable.
 * name:  UCS-2 (UTF-16LE) variable name.
 * guid:  namespace GUID.
 * buf:   output buffer (may be NULL to query required size).
 * size:  in/out -- caller sets to buffer capacity; firmware sets to actual
 *        data size.  Updated even on STATUS_BUFFER_TOO_SMALL.
 *
 * Returns:
 *   STATUS_SUCCESS           -- data copied into buf, *size updated.
 *   STATUS_NOT_FOUND         -- variable does not exist.
 *   STATUS_BUFFER_TOO_SMALL  -- buf too small; *size holds required bytes.
 *   STATUS_NOT_IMPLEMENTED   -- runtime services unavailable.
 *   STATUS_UNSUCCESSFUL      -- firmware error. */
NTSTATUS uefi_var_get(const uint16_t *name, const efi_guid_t *guid,
                      void *buf, size_t *size);

/* Set (or delete) a UEFI variable.
 * size == 0 and buf == NULL deletes the variable.
 * attrs: combination of EFI_VARIABLE_* flags.
 *        Use UEFI_VAR_NV_BOOT_RUNTIME for persistent OS variables.
 *
 * Returns:
 *   STATUS_SUCCESS         -- variable written.
 *   STATUS_NOT_IMPLEMENTED -- runtime services unavailable or unsupported.
 *   STATUS_UNSUCCESSFUL    -- firmware error. */
NTSTATUS uefi_var_set(const uint16_t *name, const efi_guid_t *guid,
                      const void *buf, size_t size, uint32_t attrs);

/* Read a uint32_t UEFI variable.
 * Returns STATUS_BUFFER_TOO_SMALL if the variable exists but is not 4 bytes. */
NTSTATUS uefi_var_get_u32(const uint16_t *name, const efi_guid_t *guid,
                          uint32_t *out);

/* Write a uint32_t UEFI variable (non-volatile, boot + runtime access). */
NTSTATUS uefi_var_set_u32(const uint16_t *name, const efi_guid_t *guid,
                          uint32_t val);

/* Enumerate all UEFI variables, calling callback once per variable.
 * callback: receives UCS-2 name, GUID, and caller-supplied ctx pointer.
 * ctx:      passed through unchanged; may be NULL.
 * Iteration stops when the firmware signals end-of-list.
 * No-op if runtime services are unavailable. */
void uefi_var_enumerate(
    void (*callback)(const uint16_t *name, const efi_guid_t *guid, void *ctx),
    void *ctx);
