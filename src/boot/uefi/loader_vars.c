/* loader_vars.c -- publication of the systemd Boot Loader Interface variables.
 *
 * Pure C, no UEFI types, no dynamic allocation. Compiled in two contexts:
 *   - src/boot/uefi/Makefile -> bootloader, freestanding x86_64-elf
 *   - src/kernel/test (via test_loader_vars.c) -> kernel unit-test build
 *
 * The contract (set or clear, volatile, delete first, two names left alone)
 * is documented in include/boot/loader_vars.h and docs/boot/loader-vars.md.
 */

#include "../../../include/boot/loader_vars.h"
#include "../../../include/boot/boot_smbios_parse.h"

/* Firmware info is the vendor string, a space and the revision in decimal.
 * The vendor is firmware-supplied and unbounded, so it gets a fixed cap and a
 * vendor that does not fit clears the variable instead of truncating it. */
#define LOADER_FIRMWARE_INFO_MAX_CHARS 256u

#define LOADER_IMAGE_IDENTIFIER "\\EFI\\ImpossibleOS\\BOOTX64.EFI"
#define LOADER_INFO_TEXT        "Impossible OS bootloader 1.0"

const char *const boot_loader_var_names[BOOT_LOADER_VAR_COUNT] = {
    "LoaderInfo",
    "LoaderFirmwareInfo",
    "LoaderFirmwareType",
    "LoaderImageIdentifier",
    "LoaderDevicePartUUID",
    "LoaderEntries",
    "LoaderEntrySelected",
    "LoaderTimeInitUSec",
    "LoaderTimeExecUSec",
    "LoaderFeatures",
    "ImpossibleOSLoaderFeaturesExt",
};

enum {
    LV_INFO, LV_FIRMWARE_INFO, LV_FIRMWARE_TYPE, LV_IMAGE_ID, LV_PART_UUID,
    LV_ENTRIES, LV_ENTRY_SELECTED, LV_TIME_INIT, LV_TIME_EXEC, LV_FEATURES,
    LV_IMPOSSIBLE_EXT, LV_COUNT
};
_Static_assert(LV_COUNT == BOOT_LOADER_VAR_COUNT,
               "boot_loader_var_names and the publication enum must agree");

/* The largest list a valid store produces fits the payload, so the overflow
 * check in lv_entries() is defence in depth, not a reachable truncation. */
_Static_assert(BOOT_ENTRIES_MAX_ENTRIES * (BOOT_ENTRIES_MAX_ID_LEN + 1u) + 1u
                   <= BOOT_LOADER_VAR_PAYLOAD_MAX / 2u,
               "a full LoaderEntries list must fit BOOT_LOADER_VAR_PAYLOAD_MAX");

/* Longest name above is 29 characters. */
#define LOADER_VAR_NAME_MAX 32u

struct lv_ctx {
    const struct boot_loader_var_ops *ops;
    struct boot_loader_var_report *report;
    unsigned short *buf;      /* in->scratch, BOOT_LOADER_VAR_PAYLOAD_MAX bytes */
};

static int lv_name_ucs2(unsigned int which, unsigned short out[LOADER_VAR_NAME_MAX])
{
    const char *s = boot_loader_var_names[which];
    unsigned int i = 0;
    while (s[i]) {
        if (i + 1u >= LOADER_VAR_NAME_MAX) return 0;
        out[i] = (unsigned short)(unsigned char)s[i];
        i++;
    }
    out[i] = 0;
    return 1;
}

static const void *lv_guid(unsigned int which, const struct boot_loader_var_inputs *in)
{
    return which == LV_IMPOSSIBLE_EXT ? in->impossible_guid : in->systemd_guid;
}

/* Delete any instance, volatile or persistent. attrs 0 with size 0 is the
 * delete form every firmware accepts whatever the existing attributes are;
 * NOT_FOUND means there was nothing to delete. */
static int lv_delete(struct lv_ctx *c, const unsigned short *name, const void *guid)
{
    unsigned long long s = c->ops->set(c->ops->ctx, name, guid, 0u, 0ull, (const void *)0);
    if (s != BOOT_LOADER_VAR_SUCCESS && s != BOOT_LOADER_VAR_NOT_FOUND) {
        c->report->failed++;
        return 0;
    }
    return 1;
}

static void lv_clear(struct lv_ctx *c, unsigned int which, const struct boot_loader_var_inputs *in)
{
    unsigned short name[LOADER_VAR_NAME_MAX];
    if (!lv_name_ucs2(which, name)) { c->report->failed++; return; }
    if (lv_delete(c, name, lv_guid(which, in)))
        c->report->cleared++;
}

/* Delete first, then create volatile. The delete migrates a persistent copy
 * a pre-2026-11-08 loader left (a write with different attributes would be
 * refused), and if the write then fails the variable is absent rather than
 * holding an older boot's value. */
static void lv_set(struct lv_ctx *c, unsigned int which, const struct boot_loader_var_inputs *in,
                   const void *data, unsigned long long size)
{
    unsigned short name[LOADER_VAR_NAME_MAX];
    const void *guid = lv_guid(which, in);
    unsigned long long s;

    if (!lv_name_ucs2(which, name)) { c->report->failed++; return; }
    if (!lv_delete(c, name, guid))
        return;
    s = c->ops->set(c->ops->ctx, name, guid, BOOT_LOADER_VAR_ATTR_VOLATILE, size, data);
    if (s != BOOT_LOADER_VAR_SUCCESS) {
        c->report->failed++;
        return;
    }
    c->report->published++;
}

/* UCS-2 string payload without its terminator, the systemd-boot convention
 * for string variables. Returns the byte count, 0 when the text does not
 * fit (the caller clears instead of publishing a truncated value). */
static unsigned int lv_ascii_payload(struct lv_ctx *c, const char *s)
{
    unsigned int i = 0;
    if (!s || !s[0]) return 0;
    while (s[i]) {
        if (i >= BOOT_LOADER_VAR_PAYLOAD_MAX / 2u) return 0;
        c->buf[i] = (unsigned short)(unsigned char)s[i];
        i++;
    }
    return i * 2u;
}

static unsigned int lv_decimal(unsigned long long v, char *out, unsigned int cap)
{
    char tmp[24];
    unsigned int n = 0, i = 0;
    do {
        tmp[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    } while (v);
    if (n + 1u > cap) return 0;
    while (n) out[i++] = tmp[--n];
    out[i] = 0;
    return i;
}

static void lv_set_ascii(struct lv_ctx *c, unsigned int which,
                         const struct boot_loader_var_inputs *in, const char *text)
{
    unsigned int bytes = lv_ascii_payload(c, text);
    if (bytes == 0) { lv_clear(c, which, in); return; }
    lv_set(c, which, in, c->buf, bytes);
}

static void lv_firmware_info(struct lv_ctx *c, const struct boot_loader_var_inputs *in)
{
    char rev[24];
    unsigned int p = 0, i, rn;
    const unsigned short *v = in->firmware_vendor;

    if (!in->have_firmware || !v || !v[0]) { lv_clear(c, LV_FIRMWARE_INFO, in); return; }
    while (v[p]) {
        if (p >= LOADER_FIRMWARE_INFO_MAX_CHARS) { lv_clear(c, LV_FIRMWARE_INFO, in); return; }
        c->buf[p] = v[p];
        p++;
    }
    rn = lv_decimal(in->firmware_revision, rev, sizeof(rev));
    c->buf[p++] = (unsigned short)' ';
    for (i = 0; i < rn; i++) c->buf[p++] = (unsigned short)(unsigned char)rev[i];
    lv_set(c, LV_FIRMWARE_INFO, in, c->buf, (unsigned long long)p * 2u);
}

static void lv_firmware_type(struct lv_ctx *c, const struct boot_loader_var_inputs *in)
{
    char text[32] = { 'U', 'E', 'F', 'I', ' ', 0 };
    unsigned int p = 5;
    if (!in->have_firmware) { lv_clear(c, LV_FIRMWARE_TYPE, in); return; }
    p += lv_decimal((in->uefi_revision >> 16) & 0xFFFFu, text + p, sizeof(text) - p);
    text[p++] = '.';
    p += lv_decimal(in->uefi_revision & 0xFFFFu, text + p, sizeof(text) - p);
    text[p] = 0;
    lv_set_ascii(c, LV_FIRMWARE_TYPE, in, text);
}

int boot_loader_part_uuid_text(unsigned char style, const unsigned char guid[16], char out[37])
{
    out[0] = 0;
    if (style != 2u || !guid)
        return 0;
    return boot_smbios_format_uuid(guid, out);
}

static int lv_entry_visible(const boot_entry_envelope_t *e)
{
    return !e->kind_skipped && !(e->flags & BOOT_ENTRY_FLAG_HIDDEN);
}

/* NUL-separated ids in menu order with a double-NUL terminator, visible
 * entries only. Cleared for a rejected store or one with nothing visible:
 * advertising entries the loader refused to offer is wrong, not unknown. */
static void lv_entries(struct lv_ctx *c, const struct boot_loader_var_inputs *in)
{
    const boot_entries_parse_result_t *parse = in->parse;
    unsigned int order[BOOT_ENTRIES_MAX_ENTRIES];
    unsigned int n, oi, p = 0, shown = 0;
    const unsigned int cap = BOOT_LOADER_VAR_PAYLOAD_MAX / 2u;

    if (!parse || parse->reject_code != BOOT_ENTRIES_OK || parse->entry_count == 0) {
        lv_clear(c, LV_ENTRIES, in);
        return;
    }
    n = (unsigned int)parse->entry_count;
    if (n > BOOT_ENTRIES_MAX_ENTRIES) n = BOOT_ENTRIES_MAX_ENTRIES;
    for (oi = 0; oi < n; oi++) order[oi] = oi;
    boot_entries_bls_sort(parse, order, n);
    for (oi = 0; oi < n; oi++) {
        const boot_entry_envelope_t *e = &parse->entries[order[oi]];
        unsigned int j = 0;
        if (!lv_entry_visible(e)) continue;
        while (e->id[j]) {
            if (p + 2u >= cap) { lv_clear(c, LV_ENTRIES, in); return; }
            c->buf[p++] = (unsigned short)(unsigned char)e->id[j++];
        }
        c->buf[p++] = 0;
        shown++;
    }
    if (shown == 0) { lv_clear(c, LV_ENTRIES, in); return; }
    c->buf[p++] = 0;
    lv_set(c, LV_ENTRIES, in, c->buf, (unsigned long long)p * 2u);
}

static void lv_time(struct lv_ctx *c, unsigned int which,
                    const struct boot_loader_var_inputs *in, unsigned long long usec)
{
    char text[24];
    /* systemd rejects a zero ExecUSec, so 0 is never a measurement. */
    if (!in->timing_known || usec == 0 || lv_decimal(usec, text, sizeof(text)) == 0) {
        lv_clear(c, which, in);
        return;
    }
    lv_set_ascii(c, which, in, text);
}

static int lv_begin(struct lv_ctx *c, const struct boot_loader_var_ops *ops,
                    const struct boot_loader_var_inputs *in,
                    struct boot_loader_var_report *report)
{
    report->published = 0;
    report->cleared = 0;
    report->failed = 0;
    if (!ops || !ops->set || !in || !in->scratch
        || in->scratch_bytes < BOOT_LOADER_VAR_PAYLOAD_MAX) {
        report->failed = 1;
        return 0;
    }
    c->ops = ops;
    c->report = report;
    c->buf = in->scratch;
    return 1;
}

int boot_loader_vars_clear_all(const struct boot_loader_var_ops *ops,
                               const struct boot_loader_var_inputs *in,
                               struct boot_loader_var_report *report)
{
    struct lv_ctx c;
    if (!lv_begin(&c, ops, in, report))
        return 0;
    for (unsigned int i = 0; i < LV_COUNT; i++)
        lv_clear(&c, i, in);
    return report->failed == 0;
}

int boot_loader_vars_publish(const struct boot_loader_var_ops *ops,
                             const struct boot_loader_var_inputs *in,
                             struct boot_loader_var_report *report)
{
    struct lv_ctx c;
    char uuid[37];

    if (!lv_begin(&c, ops, in, report))
        return 0;

    lv_set_ascii(&c, LV_INFO, in, LOADER_INFO_TEXT);
    lv_firmware_info(&c, in);
    lv_firmware_type(&c, in);
    lv_set_ascii(&c, LV_IMAGE_ID, in, LOADER_IMAGE_IDENTIFIER);

    if (boot_loader_part_uuid_text(in->partition_style, in->partition_guid, uuid))
        lv_set_ascii(&c, LV_PART_UUID, in, uuid);
    else
        lv_clear(&c, LV_PART_UUID, in);

    lv_entries(&c, in);

    if (in->selected_entry_id && in->selected_entry_id[0])
        lv_set_ascii(&c, LV_ENTRY_SELECTED, in, in->selected_entry_id);
    else
        lv_clear(&c, LV_ENTRY_SELECTED, in);

    lv_time(&c, LV_TIME_INIT, in, in->init_usec);
    lv_time(&c, LV_TIME_EXEC, in, in->exec_usec);

    lv_set(&c, LV_FEATURES, in, &in->loader_features, sizeof(in->loader_features));
    lv_set(&c, LV_IMPOSSIBLE_EXT, in, &in->impossible_features, sizeof(in->impossible_features));

    return report->failed == 0;
}
