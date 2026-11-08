/*
 * src/kernel/test/test_loader_vars.c -- Boot Loader Interface publication.
 *
 * Runs the shipped publisher (src/boot/uefi/loader_vars.c, cross-included the
 * way test_boot_entry_kind.c includes its validator) against an in-memory UEFI
 * variable store that keeps the properties the section is about:
 *   - persistence: mv_reset() drops every variable without NON_VOLATILE, so a
 *     two-boot sequence shows what a second boot inherits;
 *   - the UEFI rewrite rule: writing an existing variable with different
 *     attributes fails with EFI_INVALID_PARAMETER, which is why the publisher
 *     deletes first;
 *   - namespaces: a variable is its name AND its 16-byte vendor GUID value.
 * Writes and deletes can be made to fail per name; every call is counted.
 */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "kernel/types.h"
#include "kernel/test/scratch.h"  /* TEST_SCRATCH_KBUF */
#include "libc/string.h"

/* The kernel test image sits at its USER_BASE ceiling (02-kernel-core/TODO-33),
 * so this file, including its copy of the publisher, is optimised for size.
 * That changes code generation only; the loader builds loader_vars.c at its
 * own -O2, from the same source. Helpers are also noinline: inlining them at
 * every assertion site cost ~6 KB of .text. */
#pragma clang attribute push (__attribute__((minsize)), apply_to = function)

#include "../../boot/uefi/loader_vars.c"

#define MV_NOINLINE __attribute__((noinline))

#define MV_MAX_VARS   16u
#define MV_POOL_BYTES 16384u   /* holds a full 64-entry LoaderEntries (6146 B) */
#define MV_DEVICE_ERROR 0x8000000000000007ull

struct mv_var {
    int used;
    unsigned short name[LOADER_VAR_NAME_MAX];
    unsigned char guid[16];
    unsigned int attrs;
    unsigned int size;
    unsigned int off;      /* into mv_store.pool */
};

struct mv_store {
    struct mv_var v[MV_MAX_VARS];
    unsigned char pool[MV_POOL_BYTES];
    unsigned int pool_used;
    unsigned int calls;
    const char *fail_set;      /* writes of this name fail */
    const char *fail_delete;   /* deletes of this name fail */
};

/* The store, payload scratch and a parse result live in a test-scoped kernel
 * buffer: the kernel image has no BSS headroom for them. */
struct mv_fixture {
    struct mv_store st;
    unsigned short scratch[BOOT_LOADER_VAR_PAYLOAD_MAX / 2u];
    boot_entries_parse_result_t parse;
    struct boot_loader_var_inputs in;
    struct boot_loader_var_report rep;
    struct boot_loader_var_ops ops;
    unsigned char guid_copy[2][16];   /* inputs point here, not at the constants */
};

static const unsigned char MV_SYSTEMD[16] = {
    0x82, 0xb0, 0x67, 0x4a, 0x4c, 0x0a, 0xcf, 0x41,
    0xb6, 0xc7, 0x44, 0x0b, 0x29, 0xbb, 0x8c, 0x4f };
static const unsigned char MV_IMPOSSIBLE[16] = {
    0xa4, 0xd3, 0x35, 0x6f, 0xe6, 0xc0, 0x82, 0x4a,
    0xb5, 0xd8, 0x7c, 0x9d, 0x2e, 0x4f, 0x8a, 0x13 };
static const unsigned short MV_VENDOR[] = { 'E', 'D', 'K', ' ', 'I', 'I', 0 };
static const unsigned char MV_GPT_GUID[16] = {
    0x78, 0x56, 0x34, 0x12, 0x34, 0x12, 0x78, 0x56,
    0x9a, 0xbc, 0xde, 0xf0, 0x12, 0x34, 0x56, 0x78 };
#define MV_GPT_TEXT "12345678-1234-5678-9abc-def012345678"
#define MV_NV (BOOT_LOADER_VAR_ATTR_NV | BOOT_LOADER_VAR_ATTR_VOLATILE)

static MV_NOINLINE int mv_name_eq(const unsigned short *a, const char *b)
{
    unsigned int i = 0;
    while (a[i] && b[i]) {
        if (a[i] != (unsigned short)(unsigned char)b[i]) return 0;
        i++;
    }
    return a[i] == 0 && b[i] == 0;
}

static MV_NOINLINE int mv_wname_eq(const unsigned short *a, const unsigned short *b)
{
    unsigned int i = 0;
    while (a[i] && a[i] == b[i]) i++;
    return a[i] == b[i];
}

static MV_NOINLINE struct mv_var *mv_lookup(struct mv_store *st, const char *name,
                                            const unsigned char guid[16])
{
    for (unsigned int i = 0; i < MV_MAX_VARS; i++)
        if (st->v[i].used && mv_name_eq(st->v[i].name, name)
            && memcmp(st->v[i].guid, guid, 16) == 0)
            return &st->v[i];
    return (struct mv_var *)0;
}

static struct mv_var *mv_find(struct mv_store *st, const char *name)
{
    return mv_lookup(st, name, MV_SYSTEMD);
}

static MV_NOINLINE unsigned long long mv_set(void *ctx, const unsigned short *name,
                                             const void *guid, unsigned int attrs,
                                             unsigned long long size, const void *data)
{
    struct mv_store *st = (struct mv_store *)ctx;
    struct mv_var *slot = (struct mv_var *)0;
    unsigned int i;

    st->calls++;
    for (i = 0; i < MV_MAX_VARS; i++)
        if (st->v[i].used && mv_wname_eq(st->v[i].name, name)
            && memcmp(st->v[i].guid, guid, 16) == 0) { slot = &st->v[i]; break; }
    if (attrs == 0u || size == 0ull) {
        if (st->fail_delete && mv_name_eq(name, st->fail_delete)) return MV_DEVICE_ERROR;
        if (!slot) return BOOT_LOADER_VAR_NOT_FOUND;
        slot->used = 0;
        return BOOT_LOADER_VAR_SUCCESS;
    }
    if (st->fail_set && mv_name_eq(name, st->fail_set)) return BOOT_LOADER_VAR_OUT_OF_RESOURCES;
    if (slot && slot->attrs != attrs) return BOOT_LOADER_VAR_INVALID_PARAMETER;
    if (size > MV_POOL_BYTES - st->pool_used) return BOOT_LOADER_VAR_OUT_OF_RESOURCES;
    if (!slot) {
        for (i = 0; i < MV_MAX_VARS && st->v[i].used; i++) { }
        if (i == MV_MAX_VARS) return BOOT_LOADER_VAR_OUT_OF_RESOURCES;
        slot = &st->v[i];
        for (i = 0; name[i] && i + 1u < LOADER_VAR_NAME_MAX; i++) slot->name[i] = name[i];
        slot->name[i] = 0;
        memcpy(slot->guid, guid, 16);
        slot->used = 1;
    }
    slot->attrs = attrs;
    slot->size = (unsigned int)size;
    slot->off = st->pool_used;
    memcpy(st->pool + st->pool_used, data, (size_t)size);
    st->pool_used += (unsigned int)size;
    return BOOT_LOADER_VAR_SUCCESS;
}

/* A platform reset: volatile variables are gone, persistent ones stay. */
static MV_NOINLINE void mv_reset(struct mv_store *st)
{
    for (unsigned int i = 0; i < MV_MAX_VARS; i++)
        if (st->v[i].used && !(st->v[i].attrs & BOOT_LOADER_VAR_ATTR_NV))
            st->v[i].used = 0;
    st->fail_set = (const char *)0;
    st->fail_delete = (const char *)0;
}

static MV_NOINLINE unsigned int mv_count(struct mv_store *st)
{
    unsigned int n = 0;
    for (unsigned int i = 0; i < MV_MAX_VARS; i++) n += (unsigned int)st->v[i].used;
    return n;
}

/* Seed a variable the way an older loader, bootctl or another loader left it. */
static MV_NOINLINE void mv_seed(struct mv_store *st, const char *name,
                                const unsigned char guid[16], const char *text)
{
    unsigned short wname[LOADER_VAR_NAME_MAX], wtext[64];
    unsigned int i;
    for (i = 0; name[i]; i++) wname[i] = (unsigned short)(unsigned char)name[i];
    wname[i] = 0;
    for (i = 0; text[i]; i++) wtext[i] = (unsigned short)(unsigned char)text[i];
    (void)mv_set(st, wname, guid, MV_NV, i * 2u, wtext);
}

/* Does the systemd-GUID variable hold exactly this UCS-2 text (no NUL)? */
static MV_NOINLINE int mv_text_is(struct mv_store *st, const char *name, const char *text)
{
    struct mv_var *v = mv_find(st, name);
    const unsigned char *d;
    unsigned int i;
    if (!v) return 0;
    d = st->pool + v->off;
    for (i = 0; text[i]; i++) {
        if (2u * i + 1u >= v->size) return 0;
        if (d[2u * i] != (unsigned char)text[i] || d[2u * i + 1u] != 0) return 0;
    }
    return v->size == 2u * i;
}

static MV_NOINLINE int mv_u64_is(struct mv_store *st, const char *name,
                                 const unsigned char guid[16], unsigned long long want)
{
    struct mv_var *v = mv_lookup(st, name, guid);
    unsigned long long got = 0;
    if (!v || v->size != 8u) return 0;
    for (unsigned int i = 0; i < 8u; i++)
        got |= (unsigned long long)st->pool[v->off + i] << (8u * i);
    return got == want;
}

/* Wipe the fixture; inputs know nothing beyond the loader's own constants.
 * The GUIDs are copies, so a store keyed by address rather than by value
 * would fail the namespace checks. */
static MV_NOINLINE void mv_begin(struct mv_fixture *fx)
{
    memset(fx, 0, sizeof(*fx));
    memcpy(fx->guid_copy[0], MV_SYSTEMD, 16);
    memcpy(fx->guid_copy[1], MV_IMPOSSIBLE, 16);
    fx->ops.set = mv_set;
    fx->ops.ctx = &fx->st;
    fx->in.systemd_guid = fx->guid_copy[0];
    fx->in.impossible_guid = fx->guid_copy[1];
    fx->in.scratch = fx->scratch;
    fx->in.scratch_bytes = sizeof(fx->scratch);
    fx->in.loader_features = 0x1Aull;
    fx->in.impossible_features = 0x0Full;
}

/* Start the next boot: the store keeps its contents, the inputs reset. */
static MV_NOINLINE void mv_next_boot(struct mv_fixture *fx)
{
    mv_reset(&fx->st);
    memset(&fx->parse, 0, sizeof(fx->parse));
    memset(&fx->in, 0, sizeof(fx->in));
    fx->in.systemd_guid = fx->guid_copy[0];
    fx->in.impossible_guid = fx->guid_copy[1];
    fx->in.scratch = fx->scratch;
    fx->in.scratch_bytes = sizeof(fx->scratch);
    fx->in.loader_features = 0x1Aull;
    fx->in.impossible_features = 0x0Full;
}

static MV_NOINLINE void mv_set_id(char *dst, const char *src)
{
    unsigned int i;
    for (i = 0; src[i]; i++) dst[i] = src[i];
    dst[i] = 0;
}

/* Everything known: a GPT boot with a two-entry store and measured timing. */
static MV_NOINLINE void mv_known(struct mv_fixture *fx)
{
    boot_entries_parse_result_t *p = &fx->parse;
    p->reject_code = BOOT_ENTRIES_OK;
    p->entry_count = 2u;
    mv_set_id(p->entries[0].id, "slot-a");
    mv_set_id(p->entries[0].sort_key, "2");
    mv_set_id(p->entries[1].id, "slot-b");
    mv_set_id(p->entries[1].sort_key, "1");
    fx->in.have_firmware = 1;
    fx->in.firmware_vendor = MV_VENDOR;
    fx->in.firmware_revision = 65536u;
    fx->in.uefi_revision = (2u << 16) | 70u;
    fx->in.partition_style = 2u;
    memcpy(fx->in.partition_guid, MV_GPT_GUID, 16);
    fx->in.parse = p;
    fx->in.selected_entry_id = "slot-b";
    fx->in.timing_known = 1;
    fx->in.init_usec = 1234u;
    fx->in.exec_usec = 5678u;
}

static MV_NOINLINE int mv_publish(struct mv_fixture *fx)
{
    return boot_loader_vars_publish(&fx->ops, &fx->in, &fx->rep);
}

#define MV_FIXTURE() \
    TEST_SCRATCH_KBUF(fxbuf, sizeof(struct mv_fixture)); \
    struct mv_fixture *fx = (struct mv_fixture *)fxbuf; \
    struct mv_store *mv = &fx->st; \
    mv_begin(fx)

static void test_loader_vars_all_known_are_volatile(void)
{
    MV_FIXTURE();
    static const char expect[] = { 's','l','o','t','-','b',0,'s','l','o','t','-','a',0,0 };
    static const char *const texts[][2] = {
        { "LoaderDevicePartUUID", MV_GPT_TEXT }, { "LoaderEntrySelected", "slot-b" },
        { "LoaderTimeInitUSec", "1234" }, { "LoaderTimeExecUSec", "5678" },
        { "LoaderFirmwareInfo", "EDK II 65536" }, { "LoaderFirmwareType", "UEFI 2.70" },
        { "LoaderInfo", "Impossible OS bootloader 1.0" },
        { "LoaderImageIdentifier", "\\EFI\\ImpossibleOS\\BOOTX64.EFI" } };
    struct mv_var *e;
    int ok = 1;

    mv_known(fx);
    TEST_ASSERT_EQ(mv_publish(fx), 1, "every firmware call succeeded");
    TEST_ASSERT_EQ(fx->rep.published, BOOT_LOADER_VAR_COUNT, "a fully known boot sets every variable");
    TEST_ASSERT_EQ(fx->rep.cleared, 0u, "and clears none");
    TEST_ASSERT_EQ(mv_count(mv), BOOT_LOADER_VAR_COUNT, "one store entry per published variable");
    for (unsigned int i = 0; i < MV_MAX_VARS; i++)
        if (mv->v[i].used && mv->v[i].attrs != BOOT_LOADER_VAR_ATTR_VOLATILE) ok = 0;
    TEST_ASSERT(ok, "every published variable is BS|RT, never NON_VOLATILE");
    /* Every text variable, exact UCS-2 content with no trailing NUL. */
    ok = 1;
    for (unsigned int i = 0; i < sizeof(texts) / sizeof(texts[0]); i++)
        if (!mv_text_is(mv, texts[i][0], texts[i][1])) ok = 0;
    TEST_ASSERT(ok, "partition UUID, selection, both timings, firmware info and type, identity, image path");
    TEST_ASSERT(mv_u64_is(mv, "LoaderFeatures", MV_SYSTEMD, 0x1Aull),
                "LoaderFeatures: 8 bytes under the systemd GUID");
    TEST_ASSERT(mv_u64_is(mv, "ImpossibleOSLoaderFeaturesExt", MV_IMPOSSIBLE, 0x0Full),
                "the extension bitmap is under the Impossible OS GUID");
    TEST_ASSERT_NULL(mv_lookup(mv, "ImpossibleOSLoaderFeaturesExt", MV_SYSTEMD),
                     "and not under the systemd GUID");
    e = mv_find(mv, "LoaderEntries");
    ok = e && e->size == sizeof(expect) * 2u;
    for (unsigned int i = 0; ok && i < sizeof(expect); i++)
        ok = mv->pool[e->off + 2u * i] == (unsigned char)expect[i]
          && mv->pool[e->off + 2u * i + 1u] == 0;
    TEST_ASSERT(ok, "LoaderEntries is the ids in menu order, NUL separated, double-NUL ended");
    mv_reset(mv);
    TEST_ASSERT_EQ(mv_count(mv), 0u, "a reset leaves none of this boot's variables behind");
}

/* The section's user-visible bug: a boot that knows its partition followed by
 * one that does not must not report the first boot's partition. */
static void test_loader_vars_unknown_boot_after_known_boot(void)
{
    MV_FIXTURE();
    static const char *const absent[] = {
        "LoaderDevicePartUUID", "LoaderEntries", "LoaderEntrySelected", "LoaderTimeInitUSec",
        "LoaderTimeExecUSec", "LoaderFirmwareInfo", "LoaderFirmwareType" };
    int ok = 1;

    mv_known(fx);
    (void)mv_publish(fx);
    TEST_ASSERT(mv_find(mv, "LoaderDevicePartUUID") != (struct mv_var *)0, "boot 1 publishes it");
    mv_next_boot(fx);
    /* And persistent copies of every unknown fact, as the old loader left them. */
    for (unsigned int i = 0; i < sizeof(absent) / sizeof(absent[0]); i++)
        mv_seed(mv, absent[i], MV_SYSTEMD, "old");
    for (unsigned int i = 0; i < sizeof(absent) / sizeof(absent[0]); i++)
        if (!mv_find(mv, absent[i])) ok = 0;
    TEST_ASSERT(ok, "boot 2 starts with all seven old values present");
    TEST_ASSERT_EQ(mv_publish(fx), 1, "boot 2: every call succeeded");
    for (unsigned int i = 0; i < sizeof(absent) / sizeof(absent[0]); i++)
        if (mv_find(mv, absent[i])) ok = 0;
    TEST_ASSERT(ok, "boot 2 reports none of the earlier facts");
    TEST_ASSERT_EQ(fx->rep.published, 4u, "only the loader's own constants are set");
    TEST_ASSERT_EQ(fx->rep.cleared, 7u, "every unknown variable is cleared");
}

/* Each way of not knowing the partition or the timing deletes an old value. */
static void test_loader_vars_unknown_inputs_delete_old_values(void)
{
    MV_FIXTURE();
    static const unsigned char ff[16] = {
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
    int ok = 1;

    for (unsigned int c = 0; c < 4u; c++) {      /* MBR, unknown style, zero, all-FF */
        mv_begin(fx);
        mv_known(fx);
        mv_seed(mv, "LoaderDevicePartUUID", MV_SYSTEMD, "old");
        if (c == 0) fx->in.partition_style = 1u;
        if (c == 1) fx->in.partition_style = 0u;
        if (c == 2) memset(fx->in.partition_guid, 0, 16);
        if (c == 3) memcpy(fx->in.partition_guid, ff, 16);
        if (mv_publish(fx) != 1 || mv_find(mv, "LoaderDevicePartUUID")
            || fx->rep.published != 10u || fx->rep.cleared != 1u) ok = 0;
    }
    TEST_ASSERT(ok, "MBR, unknown style, zero and all-FF GUIDs each delete an old partition UUID");

    for (unsigned int c = 0; c < 3u; c++) {      /* unmeasured, InitUSec 0, ExecUSec 0 */
        mv_begin(fx);
        mv_known(fx);
        mv_seed(mv, "LoaderTimeInitUSec", MV_SYSTEMD, "1");
        mv_seed(mv, "LoaderTimeExecUSec", MV_SYSTEMD, "2");
        if (c == 0) fx->in.timing_known = 0;
        if (c == 1) fx->in.init_usec = 0u;
        if (c == 2) fx->in.exec_usec = 0u;
        if (mv_publish(fx) != 1
            || (mv_find(mv, "LoaderTimeInitUSec") == (struct mv_var *)0) != (c != 2)
            || (mv_find(mv, "LoaderTimeExecUSec") == (struct mv_var *)0) != (c != 1)) ok = 0;
    }
    TEST_ASSERT(ok, "each timing variable is deleted exactly when it is unknown");

    mv_begin(fx);                                 /* no system table */
    mv_known(fx);
    mv_seed(mv, "LoaderFirmwareInfo", MV_SYSTEMD, "old");
    mv_seed(mv, "LoaderFirmwareType", MV_SYSTEMD, "old");
    fx->in.have_firmware = 0;
    ok = mv_publish(fx) == 1 && !mv_find(mv, "LoaderFirmwareInfo")
      && !mv_find(mv, "LoaderFirmwareType") && fx->rep.cleared == 2u;
    TEST_ASSERT(ok, "no system table: both old firmware variables deleted, nothing else cleared");
}

/* The pre-2026-11-08 loader wrote these NON_VOLATILE. Its copies are replaced
 * on the first boot of this loader, or cleared when this boot has nothing to
 * say, under both vendor GUIDs. */
static void test_loader_vars_migrates_persistent_copies(void)
{
    MV_FIXTURE();
    struct mv_var *v;

    mv_seed(mv, "LoaderDevicePartUUID", MV_SYSTEMD, "00000000-old0-0000-0000-000000000000");
    mv_seed(mv, "LoaderEntrySelected", MV_SYSTEMD, "old-entry");
    mv_seed(mv, "LoaderInfo", MV_SYSTEMD, "old loader");
    mv_seed(mv, "ImpossibleOSLoaderFeaturesExt", MV_IMPOSSIBLE, "old");
    fx->in.partition_style = 2u;
    memcpy(fx->in.partition_guid, MV_GPT_GUID, 16);

    TEST_ASSERT_EQ(mv_publish(fx), 1, "a persistent copy does not make the volatile write fail");
    v = mv_find(mv, "LoaderDevicePartUUID");
    TEST_ASSERT(mv_text_is(mv, "LoaderDevicePartUUID", MV_GPT_TEXT), "old UUID replaced");
    TEST_ASSERT(v && v->attrs == BOOT_LOADER_VAR_ATTR_VOLATILE, "and the copy is now volatile");
    TEST_ASSERT_NULL(mv_find(mv, "LoaderEntrySelected"), "old selection deleted, nothing selected");
    TEST_ASSERT(mv_text_is(mv, "LoaderInfo", "Impossible OS bootloader 1.0"), "LoaderInfo rewritten");
    TEST_ASSERT(mv_u64_is(mv, "ImpossibleOSLoaderFeaturesExt", MV_IMPOSSIBLE, 0x0Full),
                "the extension's old copy is replaced under its own GUID");
    mv_reset(mv);
    TEST_ASSERT_EQ(mv_count(mv), 0u, "nothing persistent is left after reset");
}

/* LoaderConfigTimeout and LoaderEntryDefault belong to bootctl or another
 * loader. Neither a publish nor a chainload clear writes or deletes them. */
static void test_loader_vars_leaves_user_settings(void)
{
    MV_FIXTURE();
    struct mv_var *t, *d;

    mv_seed(mv, "LoaderConfigTimeout", MV_SYSTEMD, "7");
    mv_seed(mv, "LoaderEntryDefault", MV_SYSTEMD, "user-choice");
    mv_known(fx);
    (void)mv_publish(fx);
    mv_next_boot(fx);
    (void)mv_publish(fx);
    (void)boot_loader_vars_clear_all(&fx->ops, &fx->in, &fx->rep);
    mv_reset(mv);
    t = mv_find(mv, "LoaderConfigTimeout");
    d = mv_find(mv, "LoaderEntryDefault");
    TEST_ASSERT(mv_text_is(mv, "LoaderConfigTimeout", "7") && t && t->attrs == MV_NV,
                "the user's timeout setting is untouched");
    TEST_ASSERT(mv_text_is(mv, "LoaderEntryDefault", "user-choice") && d && d->attrs == MV_NV,
                "the user's default entry setting is untouched");
}

/* A store that offers nothing deletes the list an older boot left, rather
 * than only declining to write one. */
static void test_loader_vars_store_without_entries_deletes_old_list(void)
{
    MV_FIXTURE();
    for (unsigned int c = 0; c < 4u; c++) {
        mv_begin(fx);
        mv_seed(mv, "LoaderEntries", MV_SYSTEMD, "old-list");
        mv_known(fx);
        if (c == 0) fx->parse.reject_code = BOOT_ENTRIES_REJECT_FILE_TOO_LARGE;
        if (c == 1) fx->parse.entry_count = 0u;
        if (c == 2) fx->in.parse = (const boot_entries_parse_result_t *)0;
        if (c == 3) {
            fx->parse.entries[0].flags = BOOT_ENTRY_FLAG_HIDDEN;
            fx->parse.entries[1].kind_skipped = 1;
        }
        TEST_ASSERT_EQ(mv_publish(fx), 1, "rejected, empty, absent, all-hidden: calls succeed");
        TEST_ASSERT_NULL(mv_find(mv, "LoaderEntries"), "and the old list is deleted");
        TEST_ASSERT_EQ(fx->rep.cleared, 1u, "LoaderEntries is the one variable cleared");
    }
    /* A refused delete is a reported failure; the old list is not overwritten. */
    mv_begin(fx);
    mv_seed(mv, "LoaderEntries", MV_SYSTEMD, "old-list");
    mv_known(fx);
    fx->parse.reject_code = BOOT_ENTRIES_REJECT_JSON_PARSE;
    mv->fail_delete = "LoaderEntries";
    TEST_ASSERT_EQ(mv_publish(fx), 0, "a refused delete is reported");
    TEST_ASSERT_EQ(fx->rep.failed, 1u, "as exactly one failure");
    TEST_ASSERT(mv_text_is(mv, "LoaderEntries", "old-list"), "best effort: the old list stays");
}

/* The largest valid store: 64 entries of 47-character ids. */
static void test_loader_vars_full_entry_list(void)
{
    MV_FIXTURE();
    struct mv_var *e;
    unsigned int i, j, k = 0;
    int ok;

    mv_known(fx);
    fx->parse.entry_count = BOOT_ENTRIES_MAX_ENTRIES;
    for (i = 0; i < BOOT_ENTRIES_MAX_ENTRIES; i++) {
        char *id = fx->parse.entries[i].id;
        for (j = 0; j < BOOT_ENTRIES_MAX_ID_LEN; j++) id[j] = 'a';
        id[0] = (char)('A' + i / 26u);
        id[1] = (char)('a' + i % 26u);
        id[BOOT_ENTRIES_MAX_ID_LEN] = 0;
        fx->parse.entries[i].sort_key[0] = id[0];
        fx->parse.entries[i].sort_key[1] = id[1];
        fx->parse.entries[i].sort_key[2] = 0;
        fx->parse.entries[i].flags = 0;
        fx->parse.entries[i].kind_skipped = 0;
    }
    TEST_ASSERT_EQ(mv_publish(fx), 1, "a full store publishes");
    e = mv_find(mv, "LoaderEntries");
    ok = e && e->size == (BOOT_ENTRIES_MAX_ENTRIES * (BOOT_ENTRIES_MAX_ID_LEN + 1u) + 1u) * 2u;
    /* Every UCS-2 unit, both bytes: each id in full, its separator, then the
     * final terminator. Ids sort by sort_key, which is their own prefix. */
    for (i = 0; ok && i < BOOT_ENTRIES_MAX_ENTRIES; i++) {
        for (j = 0; ok && j <= BOOT_ENTRIES_MAX_ID_LEN; j++, k++) {
            unsigned char want = j == BOOT_ENTRIES_MAX_ID_LEN ? 0
                               : j == 0 ? (unsigned char)('A' + i / 26u)
                               : j == 1 ? (unsigned char)('a' + i % 26u) : (unsigned char)'a';
            ok = mv->pool[e->off + 2u * k] == want && mv->pool[e->off + 2u * k + 1u] == 0;
        }
    }
    ok = ok && mv->pool[e->off + 2u * k] == 0 && mv->pool[e->off + 2u * k + 1u] == 0;
    TEST_ASSERT(ok, "all 64 ids, in order, whole, double-NUL ended, every byte checked");
}

static void test_loader_vars_firmware_failures_are_reported(void)
{
    MV_FIXTURE();

    /* A write that fails after the delete leaves the variable absent. */
    mv_seed(mv, "LoaderDevicePartUUID", MV_SYSTEMD, "old");
    fx->in.partition_style = 2u;
    memcpy(fx->in.partition_guid, MV_GPT_GUID, 16);
    mv->fail_set = "LoaderDevicePartUUID";
    TEST_ASSERT_EQ(mv_publish(fx), 0, "a failed write is reported");
    TEST_ASSERT_EQ(fx->rep.failed, 1u, "exactly one failure");
    TEST_ASSERT_NULL(mv_find(mv, "LoaderDevicePartUUID"), "and no older value is left standing");

    /* A delete that fails is reported; the write is not attempted over it. */
    mv->fail_set = (const char *)0;
    mv_seed(mv, "LoaderDevicePartUUID", MV_SYSTEMD, "old");
    mv->fail_delete = "LoaderDevicePartUUID";
    TEST_ASSERT_EQ(mv_publish(fx), 0, "a failed delete is reported");
    TEST_ASSERT_EQ(fx->rep.failed, 1u, "exactly one failure");
    TEST_ASSERT_EQ(fx->rep.published, 4u, "the other variables still publish");
    TEST_ASSERT(mv_text_is(mv, "LoaderDevicePartUUID", "old"), "the refused delete left it in place");

    /* Unusable arguments touch no firmware and report a failure. */
    for (unsigned int c = 0; c < 5u; c++) {
        struct boot_loader_var_ops ops = fx->ops;
        struct boot_loader_var_inputs in = fx->in;
        const struct boot_loader_var_ops *op = &ops;
        const struct boot_loader_var_inputs *ip = &in;
        if (c == 0) op = (const struct boot_loader_var_ops *)0;
        if (c == 1) ops.set = 0;
        if (c == 2) ip = (const struct boot_loader_var_inputs *)0;
        if (c == 3) in.scratch = (unsigned short *)0;
        if (c == 4) in.scratch_bytes = BOOT_LOADER_VAR_PAYLOAD_MAX - 1u;
        mv->calls = 0;
        TEST_ASSERT_EQ(boot_loader_vars_publish(op, ip, &fx->rep), 0, "bad arguments fail");
        TEST_ASSERT_EQ(boot_loader_vars_clear_all(op, ip, &fx->rep), 0, "for clear_all too");
        TEST_ASSERT_EQ(mv->calls, 0u, "without a single firmware call");
    }
}

/* A chainload hands the machine to another loader: every owned variable
 * is gone before it runs (old persistent copies are covered by migration). A
 * refused delete is reported and does not stop the loop. */
static void test_loader_vars_chainload_clears_everything(void)
{
    MV_FIXTURE();
    int ok;

    mv_known(fx);
    (void)mv_publish(fx);
    TEST_ASSERT_EQ(mv_count(mv), BOOT_LOADER_VAR_COUNT, "all 11 present before the chainload");
    mv->fail_delete = "LoaderInfo";
    mv->calls = 0;
    ok = boot_loader_vars_clear_all(&fx->ops, &fx->in, &fx->rep) == 0
      && fx->rep.failed == 1u && fx->rep.cleared == BOOT_LOADER_VAR_COUNT - 1u
      && fx->rep.published == 0u && mv->calls == BOOT_LOADER_VAR_COUNT;
    TEST_ASSERT(ok, "a refused delete is one reported failure; the other ten are still cleared");
    TEST_ASSERT(mv_count(mv) == 1u && mv_find(mv, "LoaderInfo"), "only the refused variable remains");
    mv->fail_delete = (const char *)0;
    ok = boot_loader_vars_clear_all(&fx->ops, &fx->in, &fx->rep) == 1
      && fx->rep.cleared == BOOT_LOADER_VAR_COUNT && fx->rep.published == 0u;
    TEST_ASSERT(ok, "a full clear reports all 11 cleared, none published");
    TEST_ASSERT_EQ(mv_count(mv), 0u, "nothing of this loader's is left for the chained one");
}

static void test_loader_vars_edge_values(void)
{
    MV_FIXTURE();
    char text[37];
    unsigned short long_vendor[300];
    static const unsigned char zero[16];

    /* A zero measurement is not a measurement. */
    mv_known(fx);
    fx->in.exec_usec = 0u;
    (void)mv_publish(fx);
    TEST_ASSERT_NULL(mv_find(mv, "LoaderTimeExecUSec"), "ExecUSec 0 is cleared");
    TEST_ASSERT(mv_text_is(mv, "LoaderTimeInitUSec", "1234"), "InitUSec still published");

    /* A vendor string too long to publish whole is cleared, not truncated. */
    for (unsigned int i = 0; i < 299u; i++) long_vendor[i] = 'V';
    long_vendor[299] = 0;
    fx->in.firmware_vendor = long_vendor;
    (void)mv_publish(fx);
    TEST_ASSERT_NULL(mv_find(mv, "LoaderFirmwareInfo"), "an oversize vendor clears firmware info");
    TEST_ASSERT(mv_text_is(mv, "LoaderFirmwareType", "UEFI 2.70"), "firmware type is independent");

    /* Only a GPT GUID is a partition UUID. */
    TEST_ASSERT_EQ(boot_loader_part_uuid_text(2u, MV_GPT_GUID, text), 1, "GPT formats");
    TEST_ASSERT_EQ(boot_loader_part_uuid_text(1u, MV_GPT_GUID, text), 0, "an MBR signature never does");
    TEST_ASSERT_EQ(boot_loader_part_uuid_text(0u, MV_GPT_GUID, text), 0, "unknown style never does");
    TEST_ASSERT_EQ(boot_loader_part_uuid_text(2u, zero, text), 0, "an all-zero GUID is unknown");
}

void test_register_loader_vars(void)
{
    test_suite_register_cat("loader-vars: fully known boot sets every variable volatile",
        test_loader_vars_all_known_are_volatile, TEST_CAT_BOOT);
    test_suite_register_cat("loader-vars: unknown boot after a known one inherits nothing",
        test_loader_vars_unknown_boot_after_known_boot, TEST_CAT_BOOT);
    test_suite_register_cat("loader-vars: persistent copies from the old loader migrate",
        test_loader_vars_migrates_persistent_copies, TEST_CAT_BOOT);
    test_suite_register_cat("loader-vars: user timeout and default entry left alone",
        test_loader_vars_leaves_user_settings, TEST_CAT_BOOT);
    test_suite_register_cat("loader-vars: a store with no entries deletes the old list",
        test_loader_vars_store_without_entries_deletes_old_list, TEST_CAT_BOOT);
    test_suite_register_cat("loader-vars: a full 64-entry list publishes whole",
        test_loader_vars_full_entry_list, TEST_CAT_BOOT);
    test_suite_register_cat("loader-vars: firmware failures and bad arguments reported",
        test_loader_vars_firmware_failures_are_reported, TEST_CAT_BOOT);
    test_suite_register_cat("loader-vars: chainload clears every owned variable, past a refusal",
        test_loader_vars_chainload_clears_everything, TEST_CAT_BOOT);
    test_suite_register_cat("loader-vars: each unknown input deletes an old value",
        test_loader_vars_unknown_inputs_delete_old_values, TEST_CAT_BOOT);
    test_suite_register_cat("loader-vars: zero timing, oversize vendor, MBR",
        test_loader_vars_edge_values, TEST_CAT_BOOT);
}

#pragma clang attribute pop

#endif /* KERNEL_TESTS */
