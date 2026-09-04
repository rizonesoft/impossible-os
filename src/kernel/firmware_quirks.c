/* ============================================================================
 * firmware_quirks.c -- Firmware quirk database (SMBIOS-keyed)
 * ============================================================================ */

#include "kernel/firmware_quirks.h"
#include "kernel/smbios.h"
#include "kernel/boot_info.h"
#include "kernel/klog.h"

extern struct boot_info g_boot_info;

/* ---- Quirk descriptor table -------------------------------------------- */

struct quirk_desc {
    uint32_t    bit;            /* FW_QUIRK_* */
    const char *name;           /* canonical lowercase name */
    const char *vendor_substr;  /* NULL = no vendor constraint */
    const char *product_substr; /* NULL = no product constraint */
    const char *bios_substr;    /* NULL = no BIOS-version constraint */
};

/* Bit + canonical name come from the shared X-macro; per-quirk SMBIOS
 * predicate substrings stay local to this file (they are kernel policy,
 * not bootloader ABI). The s_match_<id> entries name the synthetic
 * TestVendor strings used by unit tests; real-world predicates land
 * here as known-bad firmware combinations are confirmed. */
struct quirk_match {
    const char *vendor_substr;
    const char *product_substr;
    const char *bios_substr;
};

static const struct quirk_match s_match_BROKEN_FPDT           = { "TestVendor", "BrokenFPDT", 0 };
static const struct quirk_match s_match_BAD_MADT_CHECKSUM     = { "TestVendor", "BadMadt",    0 };
static const struct quirk_match s_match_GOP_PITCH_LIES        = { "TestVendor", "GopLies",    0 };
static const struct quirk_match s_match_BOGUS_MAT             = { "TestVendor", "BogusMat",   0 };
static const struct quirk_match s_match_USB_HANDOFF_BLACKLIST = { "TestVendor", "BadUsb",     0 };
/* Some firmware publishes the ECDT with EC_CONTROL and EC_DATA transposed, so
 * an OS trusting the table writes command bytes into the data register. The
 * predicate stays synthetic per this table's stated convention: a real model
 * string lands only once confirmed on that hardware, and a guessed one is
 * either inert or transposes the ports on an innocent machine. */
static const struct quirk_match s_match_EC_ECDT_PORTS_SWAPPED = { "TestVendor", "EcPortsSwapped", 0 };

static const struct quirk_desc s_quirks[] = {
#define FW_QUIRK_DEF(id, bit, name) \
    { (uint32_t)(bit), (name), \
      s_match_##id.vendor_substr, \
      s_match_##id.product_substr, \
      s_match_##id.bios_substr },
#include "kernel/firmware_quirks_table.inc"
#undef FW_QUIRK_DEF
};

#define QUIRK_COUNT (sizeof(s_quirks) / sizeof(s_quirks[0]))

_Static_assert(QUIRK_COUNT == FW_QUIRK_COUNT,
    "FW_QUIRK_COUNT must match the descriptor table size");

/* ---- Module state ------------------------------------------------------ */

static uint32_t s_active_mask;
static int      s_initialized;

/* ---- Helpers ----------------------------------------------------------- */

static int str_contains(const char *hay, const char *needle)
{
    if (!hay || !needle) return 0;
    if (!*needle)        return 1;
    for (const char *h = hay; *h; h++) {
        const char *p = h;
        const char *n = needle;
        while (*p && *n && *p == *n) { p++; n++; }
        if (!*n) return 1;
    }
    return 0;
}

static int predicate_match(const struct quirk_desc *d,
                           const struct smbios_system_info *info)
{
    if (!info || !info->valid) return 0;
    if (d->vendor_substr  && !str_contains(info->sys_manufacturer, d->vendor_substr))
        return 0;
    if (d->product_substr && !str_contains(info->sys_product, d->product_substr))
        return 0;
    if (d->bios_substr    && !str_contains(info->bios_version, d->bios_substr))
        return 0;
    return 1;
}

/* ---- Public API -------------------------------------------------------- */

void firmware_quirks_init(void)
{
    if (s_initialized) return;
    s_active_mask = 0;

    const struct smbios_system_info *info = smbios_get_info();
    /* If SMBIOS is not yet valid, do NOT latch s_initialized -- a later
     * post-SMBIOS call must still get a chance to populate the mask.
     * The header contract says early calls leave the mask at 0; this
     * guard ensures that invariant survives a retryable boot path. */
    if (!info || !info->valid) return;
    s_initialized = 1;

    uint32_t disable = (uint32_t)g_boot_info.config.firmware_quirk_disable;

    for (uint32_t i = 0; i < QUIRK_COUNT; i++) {
        const struct quirk_desc *d = &s_quirks[i];
        if (predicate_match(d, info))
            s_active_mask |= d->bit;
    }
    s_active_mask &= ~disable;

    /* Single LOG_INFO summary; per-quirk detail goes to JSON + Registry. */
    if (s_active_mask == 0) {
        klog(LOG_INFO, "FW", "quirks: 0 active");
        return;
    }
    /* Emit names in canonical bit order. */
    char buf[256];
    uint32_t pos = 0;
    uint32_t count = 0;
    for (uint32_t i = 0; i < QUIRK_COUNT; i++) {
        if (!(s_active_mask & s_quirks[i].bit)) continue;
        const char *n = s_quirks[i].name;
        if (count > 0 && pos < sizeof(buf) - 1) buf[pos++] = ',';
        while (*n && pos < sizeof(buf) - 1) buf[pos++] = *n++;
        count++;
    }
    buf[pos] = 0;
    klog(LOG_INFO, "FW", "quirks: %u active: %s", count, buf);
}

uint32_t firmware_quirks_active_mask(void)
{
    return s_active_mask;
}

int firmware_quirks_is_active(uint32_t bit)
{
    if (bit == 0 || (bit & (bit - 1)) != 0) return 0;
    return (s_active_mask & bit) != 0;
}

uint32_t firmware_quirks_active_count(void)
{
    uint32_t m = s_active_mask;
    uint32_t c = 0;
    while (m) { c += (m & 1u); m >>= 1; }
    return c;
}

const char *firmware_quirks_name(uint32_t bit)
{
    if (bit == 0 || (bit & (bit - 1)) != 0) return 0;
    for (uint32_t i = 0; i < QUIRK_COUNT; i++)
        if (s_quirks[i].bit == bit) return s_quirks[i].name;
    return 0;
}

uint32_t firmware_quirks_iter_next(uint32_t prev_bit)
{
    for (uint32_t i = 0; i < QUIRK_COUNT; i++) {
        uint32_t b = s_quirks[i].bit;
        if (b <= prev_bit) continue;
        if (s_active_mask & b) return b;
    }
    return 0;
}

#include "kernel/firmware_quirks_parse.inc"

uint32_t firmware_quirks_parse_disable(const char *list)
{
    return (uint32_t)firmware_quirks_parse_disable_inline(list);
}
