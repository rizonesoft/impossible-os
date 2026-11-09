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

/* UCS-2 string payload WITH its terminator: the Boot Loader Interface
 * formats every string variable as a NUL-terminated UTF-16 string, and
 * systemd-boot writes strsize16(), which counts the NUL. Returns the byte
 * count, 0 when the text and its NUL do not fit (the caller clears instead of
 * publishing a truncated value). */
static unsigned int lv_ascii_payload(struct lv_ctx *c, const char *s)
{
    unsigned int i = 0;
    if (!s || !s[0]) return 0;
    while (s[i]) {
        if (i + 1u >= BOOT_LOADER_VAR_PAYLOAD_MAX / 2u) return 0;
        c->buf[i] = (unsigned short)(unsigned char)s[i];
        i++;
    }
    c->buf[i++] = 0;
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

/* LoaderImageIdentifier: the loader's own ESP-relative path as firmware
 * reports it (systemd-boot reads LoadedImage FilePath the same way), so a
 * copy launched as \EFI\BOOT\BOOTX64.EFI says so. Unknown is cleared. */
static void lv_image_path(struct lv_ctx *c, const struct boot_loader_var_inputs *in)
{
    const unsigned short *v = in->image_path;
    unsigned int p = 0;
    if (!v || !v[0]) { lv_clear(c, LV_IMAGE_ID, in); return; }
    while (v[p]) {
        if (p + 1u >= BOOT_LOADER_VAR_PAYLOAD_MAX / 2u) { lv_clear(c, LV_IMAGE_ID, in); return; }
        c->buf[p] = v[p];
        p++;
    }
    c->buf[p++] = 0;  /* NUL-terminated, like every BLI string */
    lv_set(c, LV_IMAGE_ID, in, c->buf, (unsigned long long)p * 2u);
}

static void lv_set_ascii(struct lv_ctx *c, unsigned int which,
                         const struct boot_loader_var_inputs *in, const char *text)
{
    unsigned int bytes = lv_ascii_payload(c, text);
    if (bytes == 0) { lv_clear(c, which, in); return; }
    lv_set(c, which, in, c->buf, bytes);
}

/* "<major>.<minor>" of a UEFI revision word, the minor padded to two digits
 * as systemd-boot prints it ("%u.%02u"): 0x00020046 is "2.70", 0x00010000
 * is "1.00". Returns the length, 0 if it does not fit. */
static unsigned int lv_revision(unsigned int rev, char *out, unsigned int cap)
{
    unsigned int p = lv_decimal((rev >> 16) & 0xFFFFu, out, cap);
    unsigned int minor = rev & 0xFFFFu, m;
    if (p == 0 || p + 4u > cap) return 0;
    out[p++] = '.';
    if (minor < 10u) out[p++] = '0';
    m = lv_decimal(minor, out + p, cap - p);
    if (m == 0) return 0;
    return p + m;
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
    rn = lv_revision(in->firmware_revision, rev, sizeof(rev));
    c->buf[p++] = (unsigned short)' ';
    for (i = 0; i < rn; i++) c->buf[p++] = (unsigned short)(unsigned char)rev[i];
    c->buf[p++] = 0;  /* NUL-terminated, like every BLI string */
    lv_set(c, LV_FIRMWARE_INFO, in, c->buf, (unsigned long long)p * 2u);
}

static void lv_firmware_type(struct lv_ctx *c, const struct boot_loader_var_inputs *in)
{
    char text[32] = { 'U', 'E', 'F', 'I', ' ', 0 };
    unsigned int p = 5;
    if (!in->have_firmware) { lv_clear(c, LV_FIRMWARE_TYPE, in); return; }
    p += lv_revision(in->uefi_revision, text + p, sizeof(text) - p);
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
    boot_entries_bls_sort(parse, in->entry_bad, order, n);
    for (oi = 0; oi < n; oi++) {
        const boot_entry_envelope_t *e = &parse->entries[order[oi]];
        unsigned int j = 0, len = 0;
        if (!lv_entry_visible(e)) continue;
        /* Bounded by the field: the parser NUL-terminates every id, and an
         * id that is not is refused whole rather than published cut short.
         * An empty id would end the list early, so it is skipped. */
        while (len < sizeof(e->id) && e->id[len]) len++;
        if (len == sizeof(e->id)) { lv_clear(c, LV_ENTRIES, in); return; }
        if (len == 0) continue;
        for (j = 0; j < len; j++) {
            if (p + 2u >= cap) { lv_clear(c, LV_ENTRIES, in); return; }
            c->buf[p++] = (unsigned short)(unsigned char)e->id[j];
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

int boot_loader_vars_publish_timing(const struct boot_loader_var_ops *ops,
                                    const struct boot_loader_var_inputs *in,
                                    struct boot_loader_var_report *report)
{
    struct lv_ctx c;
    if (!lv_begin(&c, ops, in, report))
        return 0;
    lv_time(&c, LV_TIME_INIT, in, in->init_usec);
    lv_time(&c, LV_TIME_EXEC, in, in->exec_usec);
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
    lv_image_path(&c, in);

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

static void lv_ucs2_name(const char *a, unsigned short *out)
{
    unsigned int i = 0;
    while (a[i]) { out[i] = (unsigned short)(unsigned char)a[i]; i++; }
    out[i] = 0;
}

/* Read one one-shot into `buf` (`chars` CHAR16). Returns the number of
 * CHAR16 before the first NUL, or -1 when absent, -2 when present but too
 * large or filling the buffer, -3 when the read itself failed (both still
 * deleted). */
static int lv_one_shot_read(const struct boot_loader_var_ops *ops, const unsigned short *name,
                            const void *guid, unsigned short *buf, unsigned int chars)
{
    unsigned int attrs = 0, n = 0;
    unsigned long long size = (unsigned long long)chars * 2u;
    unsigned long long s = ops->get(ops->ctx, name, guid, &attrs, &size, buf);
    if (s == BOOT_LOADER_VAR_BUFFER_TOO_SMALL) return -2;
    if (s == BOOT_LOADER_VAR_NOT_FOUND) return -1;
    if (s != BOOT_LOADER_VAR_SUCCESS) return -3;
    /* Writers may or may not count the NUL (systemd-boot does), so a value
     * that fills the buffer, leaving no room for one, is refused rather than
     * guessed at. */
    if (size == 0 || size >= (unsigned long long)chars * 2u || (size & 1u)) return -2;
    while (n < size / 2u && buf[n]) n++;
    return (int)n;
}

static int lv_one_shot_delete(const struct boot_loader_var_ops *ops, const unsigned short *name,
                              const void *guid)
{
    unsigned long long s = ops->set(ops->ctx, name, guid, 0u, 0ull, (const void *)0);
    return s == BOOT_LOADER_VAR_SUCCESS || s == BOOT_LOADER_VAR_NOT_FOUND;
}

/* Does text[0..n) spell the ASCII word w exactly? */
static int lv_text_is(const unsigned short *text, unsigned int n, const char *w)
{
    unsigned int i = 0;
    while (w[i]) {
        if (i >= n || text[i] != (unsigned short)(unsigned char)w[i])
            return 0;
        i++;
    }
    return i == n;
}

unsigned int boot_loader_timeout_parse(const unsigned short *text, unsigned int n,
                                       unsigned int *seconds)
{
    unsigned int acc = 0, i;
    if (seconds)
        *seconds = 0;
    if (!text || n == 0u)
        return 0u;
    if (lv_text_is(text, n, "menu-force"))    return BOOT_LOADER_TIMEOUT_FORCE;
    if (lv_text_is(text, n, "menu-hidden"))   return BOOT_LOADER_TIMEOUT_HIDDEN;
    if (lv_text_is(text, n, "menu-disabled")) return BOOT_LOADER_TIMEOUT_DISABLED;
    /* Cap checked before the multiply, so no digit string can wrap. */
    for (i = 0; i < n; i++) {
        unsigned int d = (unsigned int)text[i] - (unsigned int)'0';
        if (text[i] < '0' || text[i] > '9' || acc > (BOOT_LOADER_ONE_SHOT_TIMEOUT_MAX - d) / 10u)
            return 0u;
        acc = acc * 10u + d;
    }
    if (seconds)
        *seconds = acc;
    return BOOT_LOADER_TIMEOUT_SECONDS;
}

int boot_loader_one_shot_consume(const struct boot_loader_var_ops *ops, const void *systemd_guid,
                                 char *entry_id, unsigned int entry_cap,
                                 struct boot_loader_one_shot *out)
{
    unsigned short name[LOADER_VAR_NAME_MAX];
    unsigned short ebuf[BOOT_LOADER_ONE_SHOT_ID_CHARS];
    unsigned short tbuf[BOOT_LOADER_ONE_SHOT_TIMEOUT_CHARS];
    int n, i, ok = 1;

    if (!out) return 0;
    out->have_entry = out->have_timeout = 0;
    out->timeout_kind = 0;
    out->timeout_seconds = 0;
    out->entry_malformed = out->timeout_malformed = 0;
    out->entry_refused = out->timeout_refused = 0;
    out->entry_unreadable = out->timeout_unreadable = 0;
    if (!ops || !ops->get || !ops->set || !systemd_guid || !entry_id || entry_cap < 2u)
        return 0;
    entry_id[0] = 0;

    lv_ucs2_name("LoaderEntryOneShot", name);
    n = lv_one_shot_read(ops, name, systemd_guid, ebuf, BOOT_LOADER_ONE_SHOT_ID_CHARS);
    if (n != -1) {
        int good = n > 0 && (unsigned int)n < entry_cap;
        for (i = 0; good && i < n; i++)
            good = ebuf[i] >= 0x20u && ebuf[i] <= 0x7Eu;
        if (n == -3) {
            out->entry_unreadable = 1;
            ok = 0;
            good = 0;
        }
        if (!lv_one_shot_delete(ops, name, systemd_guid)) {
            out->entry_refused = 1;
            ok = 0;
        } else if (good) {
            for (i = 0; i < n; i++) entry_id[i] = (char)ebuf[i];
            entry_id[n] = 0;
            out->have_entry = 1;
        } else if (n != -3) {
            out->entry_malformed = 1;
        }
    }

    lv_ucs2_name("LoaderConfigTimeoutOneShot", name);
    n = lv_one_shot_read(ops, name, systemd_guid, tbuf, BOOT_LOADER_ONE_SHOT_TIMEOUT_CHARS);
    if (n != -1) {
        unsigned int acc = 0;
        unsigned int kind = n > 0 ? boot_loader_timeout_parse(tbuf, (unsigned int)n, &acc) : 0u;
        int good = kind != 0u;
        if (n == -3) {
            out->timeout_unreadable = 1;
            ok = 0;
            good = 0;
        }
        if (!lv_one_shot_delete(ops, name, systemd_guid)) {
            out->timeout_refused = 1;
            ok = 0;
        } else if (good) {
            out->timeout_kind = kind;
            out->timeout_seconds = acc;
            out->have_timeout = 1;
        } else if (n != -3) {
            out->timeout_malformed = 1;
        }
    }
    return ok;
}

int boot_loader_entry_default_read(const struct boot_loader_var_ops *ops, const void *systemd_guid,
                                   char *id, unsigned int cap)
{
    unsigned short name[LOADER_VAR_NAME_MAX];
    unsigned short buf[BOOT_LOADER_ONE_SHOT_ID_CHARS];
    if (id && cap)
        id[0] = 0;
    if (!ops || !ops->get || !systemd_guid || !id || cap < 2u)
        return BOOT_LOADER_DEFAULT_UNUSABLE;
    lv_ucs2_name("LoaderEntryDefault", name);
    int n = lv_one_shot_read(ops, name, systemd_guid, buf, BOOT_LOADER_ONE_SHOT_ID_CHARS);
    if (n == -1)
        return BOOT_LOADER_DEFAULT_ABSENT;
    if (n <= 0 || (unsigned int)n >= cap)
        return BOOT_LOADER_DEFAULT_UNUSABLE;
    for (int i = 0; i < n; i++)
        if (buf[i] < 0x20u || buf[i] > 0x7Eu)
            return BOOT_LOADER_DEFAULT_UNUSABLE;
    for (int i = 0; i < n; i++)
        id[i] = (char)buf[i];
    id[n] = 0;
    return BOOT_LOADER_DEFAULT_PRESENT;
}

int boot_loader_entry_default_write(const struct boot_loader_var_ops *ops, const void *systemd_guid,
                                    const char *id)
{
    unsigned short name[LOADER_VAR_NAME_MAX];
    unsigned short buf[BOOT_LOADER_ONE_SHOT_ID_CHARS];
    if (!ops || !ops->set || !systemd_guid)
        return 0;
    lv_ucs2_name("LoaderEntryDefault", name);
    if (!id || !id[0])
        return lv_one_shot_delete(ops, name, systemd_guid);
    unsigned int n = 0;
    /* The reader refuses a value that fills its buffer, so the writer stops
     * one short of it: whatever is written can be read back. */
    while (id[n]) {
        if (n + 1u >= BOOT_LOADER_ONE_SHOT_ID_CHARS - 1u ||
            (unsigned char)id[n] < 0x20u || (unsigned char)id[n] > 0x7Eu)
            return 0;
        buf[n] = (unsigned short)(unsigned char)id[n];
        n++;
    }
    buf[n] = 0;
    unsigned long long s = ops->set(ops->ctx, name, systemd_guid, BOOT_LOADER_DEFAULT_ATTRS,
                                    (unsigned long long)(n + 1u) * 2u, buf);
    return s == BOOT_LOADER_VAR_SUCCESS;
}

int boot_loader_config_timeout_read(const struct boot_loader_var_ops *ops, const void *systemd_guid,
                                    unsigned int *kind, unsigned int *seconds)
{
    unsigned short name[LOADER_VAR_NAME_MAX];
    unsigned short buf[BOOT_LOADER_ONE_SHOT_TIMEOUT_CHARS];
    unsigned int k, v = 0;
    if (kind) *kind = 0;
    if (seconds) *seconds = 0;
    if (!ops || !ops->get || !systemd_guid || !kind || !seconds)
        return BOOT_LOADER_DEFAULT_UNUSABLE;
    lv_ucs2_name("LoaderConfigTimeout", name);
    int n = lv_one_shot_read(ops, name, systemd_guid, buf, BOOT_LOADER_ONE_SHOT_TIMEOUT_CHARS);
    if (n == -1)
        return BOOT_LOADER_DEFAULT_ABSENT;
    if (n <= 0)
        return BOOT_LOADER_DEFAULT_UNUSABLE;
    k = boot_loader_timeout_parse(buf, (unsigned int)n, &v);
    if (k == 0u)
        return BOOT_LOADER_DEFAULT_UNUSABLE;
    *kind = k;
    *seconds = v;
    return BOOT_LOADER_DEFAULT_PRESENT;
}

int boot_loader_config_timeout_write(const struct boot_loader_var_ops *ops, const void *systemd_guid,
                                     unsigned int kind, unsigned int seconds)
{
    unsigned short name[LOADER_VAR_NAME_MAX];
    unsigned short buf[BOOT_LOADER_ONE_SHOT_TIMEOUT_CHARS];
    char text[BOOT_LOADER_ONE_SHOT_TIMEOUT_CHARS];
    const char *w = (const char *)0;
    unsigned int n = 0;
    if (!ops || !ops->set || !systemd_guid)
        return 0;
    switch (kind) {
    case BOOT_LOADER_TIMEOUT_FORCE:    w = "menu-force";    break;
    case BOOT_LOADER_TIMEOUT_HIDDEN:   w = "menu-hidden";   break;
    case BOOT_LOADER_TIMEOUT_DISABLED: w = "menu-disabled"; break;
    case BOOT_LOADER_TIMEOUT_SECONDS:
        if (seconds > BOOT_LOADER_ONE_SHOT_TIMEOUT_MAX)
            return 0;
        n = lv_decimal(seconds, text, sizeof(text));
        if (n == 0u)
            return 0;
        text[n] = 0;
        w = text;
        break;
    default:
        return 0;
    }
    n = 0;
    while (w[n]) {
        buf[n] = (unsigned short)(unsigned char)w[n];
        n++;
    }
    buf[n] = 0;
    lv_ucs2_name("LoaderConfigTimeout", name);
    unsigned long long s = ops->set(ops->ctx, name, systemd_guid, BOOT_LOADER_DEFAULT_ATTRS,
                                    (unsigned long long)(n + 1u) * 2u, buf);
    return s == BOOT_LOADER_VAR_SUCCESS;
}
