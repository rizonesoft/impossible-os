/* ============================================================================
 * uefi_vars.c -- NTSTATUS-returning UEFI variable services
 *
 * Wraps the raw EFI-status-returning primitives from uefi_runtime.c with
 * NTSTATUS translation so kernel subsystems use a consistent error type.
 * ============================================================================ */

#include "kernel/uefi_vars.h"
#include "kernel/uefi_runtime.h"

/* ---- EFI status → NTSTATUS translation ---- */

static NTSTATUS efi_to_ntstatus(uint64_t efi_status)
{
    if (efi_status == UEFI_SUCCESS)           return STATUS_SUCCESS;
    if (efi_status == UEFI_NOT_FOUND)         return STATUS_NOT_FOUND;
    if (efi_status == UEFI_BUFFER_TOO_SMALL)  return STATUS_BUFFER_TOO_SMALL;
    if (efi_status == UEFI_UNSUPPORTED)       return STATUS_NOT_IMPLEMENTED;
    return STATUS_UNSUCCESSFUL;
}

/* ============================================================================
 * Core get/set wrappers
 * ============================================================================ */

NTSTATUS uefi_var_get(const uint16_t *name, const efi_guid_t *guid,
                      void *buf, size_t *size)
{
    uint32_t attrs = 0;
    uint64_t sz = *size;

    uint64_t status = uefi_get_variable(guid, name, &attrs, &sz, buf);

    /* Always update size -- firmware sets it even on BUFFER_TOO_SMALL */
    *size = (size_t)sz;

    return efi_to_ntstatus(status);
}

NTSTATUS uefi_var_set(const uint16_t *name, const efi_guid_t *guid,
                      const void *buf, size_t size, uint32_t attrs)
{
    uint64_t status = uefi_set_variable(guid, name, attrs,
                                        (uint64_t)size, buf);
    return efi_to_ntstatus(status);
}

/* ============================================================================
 * Convenience wrappers
 * ============================================================================ */

NTSTATUS uefi_var_get_u32(const uint16_t *name, const efi_guid_t *guid,
                          uint32_t *out)
{
    uint32_t tmp = 0;
    size_t sz = sizeof(tmp);
    NTSTATUS s = uefi_var_get(name, guid, &tmp, &sz);
    if (s == STATUS_SUCCESS && sz != sizeof(uint32_t))
        return STATUS_BUFFER_TOO_SMALL;
    if (s == STATUS_SUCCESS)
        *out = tmp;
    return s;
}

NTSTATUS uefi_var_set_u32(const uint16_t *name, const efi_guid_t *guid,
                          uint32_t val)
{
    return uefi_var_set(name, guid, &val, sizeof(val),
                        UEFI_VAR_NV_BOOT_RUNTIME);
}

/* ============================================================================
 * Callback-based enumeration
 * ============================================================================ */

void uefi_var_enumerate(
    void (*callback)(const uint16_t *name, const efi_guid_t *guid, void *ctx),
    void *ctx)
{
    if (!callback) return;

    /* UEFI spec: variable name buffer must be at least 1024 bytes (512 × UCS-2) */
    uint16_t name_buf[512];
    efi_guid_t guid;

    /* Zero-initialize GUID for first call */
    uint32_t i;
    uint8_t *gp = (uint8_t *)&guid;
    for (i = 0; i < (uint32_t)sizeof(guid); i++)
        gp[i] = 0;

    /* Start with empty name */
    name_buf[0] = 0;

    for (;;) {
        uint64_t name_size = sizeof(name_buf);

        uint64_t status = uefi_get_next_variable_name(
            &name_size, name_buf, &guid);

        if (status != UEFI_SUCCESS)
            break;

        callback(name_buf, &guid, ctx);
    }
}
