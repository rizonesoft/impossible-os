/*
 * src/kernel/main/loader_vars_readback.c -- post-boot check that the Boot
 * Loader Interface variables the loader published survived the handoff and
 * agree with boot_info (contract: include/kernel/loader_vars_readback.h).
 *
 * Runs once in Phase 3, on one CPU, with no shared state: every buffer is on
 * the stack and the firmware call is the only side effect. A mismatch is a
 * warning, never a halt; the boot is already past the point where the loader
 * could have done anything about it, and the line is what a user or a test
 * reads to learn that firmware dropped or kept the wrong value.
 */

#include "kernel/loader_vars_readback.h"
#include "kernel/boot_info.h"
#include "kernel/klog.h"
#include "kernel/uefi_runtime.h"
#include "kernel/uefi_vars.h"

#define LOADER_READBACK_ATTRS \
    (EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS)

/* LoaderInfo is "Impossible OS bootloader 1.0"; any readback longer than this
 * many UCS-2 units is not ours. */
#define LOADER_READBACK_INFO_CHARS 128u

static const char *const s_verdict_names[LOADER_READBACK_VERDICT_COUNT] = {
    "match", "missing", "unexpected", "mismatch", "bad attributes", "unreadable",
};
_Static_assert(sizeof(s_verdict_names) / sizeof(s_verdict_names[0]) ==
               LOADER_READBACK_VERDICT_COUNT,
               "every loader readback verdict needs a name");

const char *loader_readback_verdict_name(enum loader_readback_verdict v)
{
    if ((unsigned int)v >= LOADER_READBACK_VERDICT_COUNT) return "invalid";
    return s_verdict_names[v];
}

/* A UCS-2 value of exactly `bytes` that ends in its only NUL: returns the
 * number of characters before it, or -1 when the shape is wrong. */
static int ucs2_string_len(const uint16_t *data, size_t bytes)
{
    if (!data || bytes < sizeof(uint16_t) || (bytes % sizeof(uint16_t)) != 0)
        return -1;
    size_t n = bytes / sizeof(uint16_t);
    if (data[n - 1u] != 0) return -1;
    for (size_t i = 0; i + 1u < n; i++)
        if (data[i] == 0) return -1;
    return (int)(n - 1u);
}

enum loader_readback_verdict
loader_readback_classify_selected(NTSTATUS st, uint32_t attrs,
                                  const uint16_t *data, size_t bytes,
                                  const char *want_id)
{
    int want_none = (!want_id || want_id[0] == '\0');

    if (st == STATUS_NOT_FOUND)
        return want_none ? LOADER_READBACK_MATCH : LOADER_READBACK_MISSING;
    if (st != STATUS_SUCCESS)
        return LOADER_READBACK_UNREADABLE;
    if (want_none)
        return LOADER_READBACK_UNEXPECTED;
    if (attrs != LOADER_READBACK_ATTRS)
        return LOADER_READBACK_BAD_ATTRS;

    int len = ucs2_string_len(data, bytes);
    if (len <= 0) return LOADER_READBACK_MISMATCH;
    for (int i = 0; i < len; i++) {
        /* Entry ids are printable ASCII, so a wider unit can never match. */
        if (want_id[i] == '\0' || data[i] != (uint16_t)(unsigned char)want_id[i])
            return LOADER_READBACK_MISMATCH;
    }
    return want_id[len] == '\0' ? LOADER_READBACK_MATCH : LOADER_READBACK_MISMATCH;
}

enum loader_readback_verdict
loader_readback_classify_info(NTSTATUS st, uint32_t attrs,
                              const uint16_t *data, size_t bytes)
{
    if (st == STATUS_NOT_FOUND) return LOADER_READBACK_MISSING;
    if (st != STATUS_SUCCESS) return LOADER_READBACK_UNREADABLE;
    if (attrs != LOADER_READBACK_ATTRS) return LOADER_READBACK_BAD_ATTRS;
    return ucs2_string_len(data, bytes) > 0 ? LOADER_READBACK_MATCH
                                            : LOADER_READBACK_MISMATCH;
}

unsigned int loader_vars_readback(void)
{
    static const uint16_t info_name[] = u"LoaderInfo";
    static const uint16_t selected_name[] = u"LoaderEntrySelected";
    efi_guid_t guid = LOADER_INTERFACE_GUID_INIT;

    /* boot_info's id is a fixed 64-byte field; read it only up to its NUL. */
    char want[sizeof(g_boot_info.selected_entry_id)];
    size_t w = 0;
    while (w + 1u < sizeof(want) && g_boot_info.selected_entry_id[w] != '\0') {
        want[w] = g_boot_info.selected_entry_id[w];
        w++;
    }
    want[w] = '\0';

    uint16_t info[LOADER_READBACK_INFO_CHARS];
    size_t info_bytes = sizeof(info);
    uint32_t info_attrs = 0;
    NTSTATUS st = uefi_var_get(info_name, &guid, info, &info_bytes, &info_attrs);
    enum loader_readback_verdict vi =
        loader_readback_classify_info(st, info_attrs, info, info_bytes);

    uint16_t sel[LOADER_READBACK_ID_CHARS];
    size_t sel_bytes = sizeof(sel);
    uint32_t sel_attrs = 0;
    st = uefi_var_get(selected_name, &guid, sel, &sel_bytes, &sel_attrs);
    enum loader_readback_verdict vs =
        loader_readback_classify_selected(st, sel_attrs, sel, sel_bytes, want);

    unsigned int bad = (vi != LOADER_READBACK_MATCH) + (vs != LOADER_READBACK_MATCH);
    if (bad == 0) {
        klog(LOG_INFO, "BOOT", "loader-vars: readback matches boot_info (selected=%s)",
             (uint64_t)(w ? want : "none"));
    } else {
        klog(LOG_WARN, "BOOT",
             "loader-vars: readback differs from boot_info: LoaderInfo %s, "
             "LoaderEntrySelected %s (selected=%s, loader degraded=%u)",
             (uint64_t)loader_readback_verdict_name(vi),
             (uint64_t)loader_readback_verdict_name(vs),
             (uint64_t)(w ? want : "none"),
             (uint64_t)(g_boot_info.loader_vars_degraded ? 1u : 0u));
    }
    return bad;
}
