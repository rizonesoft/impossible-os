/* loader_vars.h -- publication of the systemd Boot Loader Interface variables.
 *
 * The bootloader reports this boot's facts to the OS through the Loader*
 * variables (vendor GUID 4a67b082-0a4c-41cf-b6c7-440b29bb8c4f) and its own
 * ImpossibleOSLoaderFeaturesExt. Every one of them describes THIS boot, so
 * every one is VOLATILE (BOOTSERVICE_ACCESS | RUNTIME_ACCESS): firmware drops
 * it at reset and no value can outlive the boot it describes. That matches
 * systemd-boot, which writes these with no NON_VOLATILE flag, and it is what
 * keeps a later boot (ours or another loader's) from inheriting a stale
 * value.
 *
 * Each variable has exactly two outcomes per boot: SET to this boot's value,
 * or CLEARED because this boot cannot determine it. A value is never left
 * standing by accident. Before every write the variable is deleted first:
 * the loader wrote these NON_VOLATILE until 2026-11-08, and UEFI refuses to
 * rewrite an existing variable with different attributes, so the delete is
 * also what migrates an old persistent copy.
 *
 * Two names are NOT published: LoaderConfigTimeout and LoaderEntryDefault
 * are persistent settings the OS (bootctl) or another loader owns, so the
 * per-boot publisher never overwrites or deletes them. LoaderEntryDefault
 * is honoured (LoaderFeatures bit 2) through its own read and write calls
 * below, which only the boot menu's d key writes.
 *
 * Pure C with no UEFI types, so the kernel boot tests run this exact code
 * against an in-memory variable store. All firmware access goes through
 * boot_loader_var_ops.
 */
#ifndef BOOT_LOADER_VARS_H
#define BOOT_LOADER_VARS_H

#include "boot_entries_parser.h"

/* UEFI variable attributes and statuses this module uses (UEFI 2.10
 * section 8.2, Appendix D). Mirrored here because the kernel test build
 * cannot include efi.h. */
#define BOOT_LOADER_VAR_ATTR_NV       0x00000001u
#define BOOT_LOADER_VAR_ATTR_BS       0x00000002u
#define BOOT_LOADER_VAR_ATTR_RT       0x00000004u
#define BOOT_LOADER_VAR_ATTR_VOLATILE (BOOT_LOADER_VAR_ATTR_BS | BOOT_LOADER_VAR_ATTR_RT)

#define BOOT_LOADER_VAR_SUCCESS            0x0000000000000000ull
#define BOOT_LOADER_VAR_BUFFER_TOO_SMALL   0x8000000000000005ull
#define BOOT_LOADER_VAR_INVALID_PARAMETER  0x8000000000000002ull
#define BOOT_LOADER_VAR_OUT_OF_RESOURCES   0x8000000000000009ull
#define BOOT_LOADER_VAR_NOT_FOUND          0x800000000000000Eull

/* Largest payload any variable here carries: LoaderEntries, up to 64 ids of
 * 47 characters in UCS-2 with separators and the double-NUL terminator. */
#define BOOT_LOADER_VAR_PAYLOAD_MAX  8192u

/* Firmware access. `set` has SetVariable semantics: attrs 0 with size 0
 * deletes, and the return is an EFI_STATUS value. `get` has GetVariable
 * semantics (in: *size is the buffer size; out: the value's size, with
 * BUFFER_TOO_SMALL when it does not fit); only the one-shot consumer uses it.
 * `guid` points at an EFI_GUID. */
struct boot_loader_var_ops {
    unsigned long long (*set)(void *ctx, const unsigned short *name, const void *guid,
                              unsigned int attrs, unsigned long long size, const void *data);
    void *ctx;
    unsigned long long (*get)(void *ctx, const unsigned short *name, const void *guid,
                              unsigned int *attrs, unsigned long long *size, void *data);
};

/* What this boot knows. A field the loader could not determine is marked
 * unknown here rather than given a placeholder value. */
struct boot_loader_var_inputs {
    const void *systemd_guid;              /* 4a67b082-... */
    const void *impossible_guid;           /* ImpossibleOSLoaderFeaturesExt's vendor */

    int have_firmware;                     /* the system table was available */
    const unsigned short *firmware_vendor; /* NUL-terminated UCS-2, may be NULL */
    unsigned int firmware_revision;        /* gST->FirmwareRevision */
    unsigned int uefi_revision;            /* gST->Hdr.Revision */

    unsigned char partition_style;         /* boot_info: 0 unknown, 1 MBR, 2 GPT */
    unsigned char partition_guid[16];      /* GPT partition GUID, on-disk byte order */

    const unsigned short *image_path;      /* loader's ESP path, UCS-2, NULL if unknown */

    const boot_entries_parse_result_t *parse; /* NULL when the store was not read */
    const unsigned char *entry_bad;        /* per parse->entries[]: 1 = boot counting marked it
                                            * bad, so LoaderEntries lists it last; NULL = none */
    const char *selected_entry_id;         /* "" or NULL when nothing was selected */

    /* Microseconds since firmware start (the TSC counts from reset), as the
     * Boot Loader Interface defines them: Init at loader entry, Exec just
     * before the kernel runs. */
    int timing_known;                      /* init_usec and exec_usec were measured */
    unsigned long long init_usec;
    unsigned long long exec_usec;

    /* Payload scratch, at least BOOT_LOADER_VAR_PAYLOAD_MAX bytes and 2-byte
     * aligned. Caller-owned so the module adds no static storage to either
     * image (the kernel test image has no BSS headroom for it). */
    unsigned short *scratch;
    unsigned int scratch_bytes;

    unsigned long long loader_features;    /* LoaderFeatures bitmap */
    unsigned long long impossible_features;/* ImpossibleOSLoaderFeaturesExt bitmap */
};

/* Counts for the one-line serial summary and for the tests. */
struct boot_loader_var_report {
    unsigned int published;
    unsigned int cleared;
    unsigned int failed;    /* writes or deletes the firmware refused */
};

/* The variables this module owns, in publication order. */
#define BOOT_LOADER_VAR_COUNT 11u
extern const char *const boot_loader_var_names[BOOT_LOADER_VAR_COUNT];

/* Publish every owned variable for this boot: each is SET or CLEARED.
 * Returns 1 when every firmware call succeeded, 0 when any failed (the
 * caller marks boot_info.loader_vars_degraded). */
int boot_loader_vars_publish(const struct boot_loader_var_ops *ops,
                             const struct boot_loader_var_inputs *in,
                             struct boot_loader_var_report *report);

/* Set or clear only LoaderTimeInitUSec and LoaderTimeExecUSec. The loader
 * calibrates its clock after the main publication, so it reports timing in a
 * second, late call just before the kernel handoff. Same return and report;
 * only the GUIDs, scratch and timing fields of `in` are read. */
int boot_loader_vars_publish_timing(const struct boot_loader_var_ops *ops,
                                    const struct boot_loader_var_inputs *in,
                                    struct boot_loader_var_report *report);

/* Clear every owned variable, for a boot whose facts this loader will not
 * report: a chainloaded loader reports its own boot, and must not find this
 * loader's (or an older build's persistent) values standing. Same return and
 * report as boot_loader_vars_publish(); only the GUIDs and scratch in `in`
 * are read. */
int boot_loader_vars_clear_all(const struct boot_loader_var_ops *ops,
                               const struct boot_loader_var_inputs *in,
                               struct boot_loader_var_report *report);

/* One-shot overrides the OS left for this boot (LoaderEntryOneShot,
 * LoaderConfigTimeoutOneShot, systemd GUID). Each present variable is
 * deleted whether or not its value is usable, so it fires at most once. A
 * value is honoured only when it is well formed AND its delete succeeded: a
 * one-shot the firmware will not delete would otherwise apply on every boot. */
#define BOOT_LOADER_ONE_SHOT_ID_CHARS      64u     /* LoaderEntryOneShot buffer, CHAR16 */
#define BOOT_LOADER_ONE_SHOT_TIMEOUT_CHARS 16u
#define BOOT_LOADER_ONE_SHOT_TIMEOUT_MAX   3600u   /* seconds */

struct boot_loader_one_shot {
    int have_entry;            /* entry_id holds a printable-ASCII id to honour */
    int have_timeout;
    unsigned int timeout_seconds;
    int entry_malformed;       /* present but unusable: deleted, not honoured */
    int timeout_malformed;
    int entry_refused;         /* the firmware refused the delete: not honoured */
    int timeout_refused;
    int entry_unreadable;      /* GetVariable failed (not NOT_FOUND): deleted, not honoured */
    int timeout_unreadable;
};

/* `entry_id` receives the id, NUL-terminated, `entry_cap` bytes (an id that
 * does not fit is malformed, never truncated). A read error other than
 * NOT_FOUND is not taken as absence: the variable is deleted unread so it
 * cannot fire on a later boot, and the boot is reported degraded. Returns 1
 * when every read and delete succeeded, 0 otherwise or when the arguments are
 * unusable (the caller marks boot_info.loader_vars_degraded). */
int boot_loader_one_shot_consume(const struct boot_loader_var_ops *ops, const void *systemd_guid,
                                 char *entry_id, unsigned int entry_cap,
                                 struct boot_loader_one_shot *out);

/* LoaderDevicePartUUID's text: lowercase RFC 4122 form of a GPT partition
 * GUID. Returns 0 (clear the variable) unless the style is GPT and the GUID
 * is neither all-zero nor all-FF. An MBR disk signature is not a partition
 * UUID and is never published. */
int boot_loader_part_uuid_text(unsigned char style, const unsigned char guid[16], char out[37]);

/* The saved default: LoaderEntryDefault (systemd GUID), the entry the OS or
 * the menu's d key chose to boot when nothing more specific applies. It is a
 * persistent setting, so it is written NV|BS|RT and never cleared by the
 * per-boot publisher.
 *
 * boot_loader_entry_default_read() copies a printable-ASCII id of fewer than
 * `cap` characters into `id`. Returns BOOT_LOADER_DEFAULT_PRESENT,
 * BOOT_LOADER_DEFAULT_ABSENT, or BOOT_LOADER_DEFAULT_UNUSABLE (malformed,
 * too long, or unreadable; `id` is ""). The value is the operator's, so an
 * unusable one is reported and left in place, never deleted.
 *
 * boot_loader_entry_default_write() stores `id` (UCS-2, NUL-terminated), or
 * deletes the variable when `id` is NULL or "". Returns 1 on success, 0 when
 * the firmware refused or `id` is not a printable-ASCII id that fits. */
#define BOOT_LOADER_DEFAULT_ABSENT   0
#define BOOT_LOADER_DEFAULT_PRESENT  1
#define BOOT_LOADER_DEFAULT_UNUSABLE 2
#define BOOT_LOADER_DEFAULT_ATTRS \
    (BOOT_LOADER_VAR_ATTR_NV | BOOT_LOADER_VAR_ATTR_BS | BOOT_LOADER_VAR_ATTR_RT)

int boot_loader_entry_default_read(const struct boot_loader_var_ops *ops, const void *systemd_guid,
                                   char *id, unsigned int cap);
int boot_loader_entry_default_write(const struct boot_loader_var_ops *ops, const void *systemd_guid,
                                    const char *id);

#endif /* BOOT_LOADER_VARS_H */
