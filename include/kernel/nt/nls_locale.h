/* ============================================================================
 * nls_locale.h -- Locale / LCID metadata records + registry policy binding
 *
 * The locale-metadata layer of the atom/NLS/locale subsystem. A compiled table
 * of locale records (LCID, BCP-47 name, language/region, ANSI/OEM code page,
 * number/date/time formats, currency, first day of week), a BCP-47 <-> LCID
 * round-trip, an ordered MUI UI-language fallback chain, and the registry-backed
 * three-way locale policy (system / user / UI language).
 *
 * Scope: kernel-resident records + the QUERY-path binding for the existing
 * NtQueryDefaultLocale / NtQueryDefaultUILanguage handlers (nt_misc.c). The
 * PRIVILEGED NtSetDefaultLocale / NtSetDefaultUILanguage path stays fail-closed
 * until the security reference monitor's privilege check lands; true per-user
 * (HKCU) locale routing is deferred until per-user hives exist -- the user
 * locale is a single machine-wide policy value for now.
 * ============================================================================ */
#ifndef KERNEL_NT_NLS_LOCALE_H
#define KERNEL_NT_NLS_LOCALE_H

#include "kernel/types.h"

#define NLS_LCID_INVARIANT   0x007Fu   /* LOCALE_INVARIANT */
#define NLS_LANGID_ENUS      0x0409u

/* A compiled locale record. ANSI/OEM code pages let a future SetThreadLocale
 * derive a thread ACP (the CP_THREAD_ACP wiring is deferred; the schema carries
 * the fields now so no format break is needed later). */
typedef struct nls_locale {
    uint32_t    lcid;
    const char *bcp47;             /* e.g. "en-US"; invariant is "" */
    const char *language;          /* English display name of the language */
    const char *region;            /* English display name of the region */
    uint32_t    ansi_code_page;    /* CP for CP_THREAD_ACP (validated vs a provider) */
    uint32_t    oem_code_page;
    const char *decimal_sep;       /* "." */
    const char *thousand_sep;      /* "," */
    const char *short_date;        /* e.g. "M/d/yyyy" */
    const char *long_time;         /* e.g. "h:mm:ss tt" */
    const char *currency;          /* currency symbol, e.g. "$" */
    uint8_t     first_day_of_week; /* LOCALE_IFIRSTDAYOFWEEK: 0=Mon .. 6=Sun */
} nls_locale_t;

/* Record lookup. Returns NULL for an unknown LCID / name; the invariant record
 * is always resolvable. */
const nls_locale_t *nls_locale_by_lcid(uint32_t lcid);
const nls_locale_t *nls_locale_by_bcp47(const char *name);

/* Registry-backed three-way policy (machine-wide; per-user routing deferred).
 * Read the boot cache (acquire); fall back to the compiled default when
 * unset/unsupported. */
uint32_t nls_locale_get_system(void);     /* system default LCID (non-Unicode default) */
uint32_t nls_locale_get_user(void);       /* user default LCID */
uint16_t nls_locale_get_ui_language(void);/* UI LANGID */

/* Ordered MUI UI-language fallback chain for `langid`: the specific LANGID, then
 * its neutral (primary-language) form, then an en-US backstop, de-duplicated.
 * Writes up to `cap` entries into out[]; returns the number produced. */
uint32_t nls_locale_ui_fallback(uint16_t langid, uint16_t *out, uint32_t cap);

/* Seed HKLM\SYSTEM\Nls SystemLocale/UserLocale/UILanguage (seed-if-missing),
 * boot-cache the validated values, and sync the nt_misc.c locale globals so
 * NtQueryDefaultLocale / NtQueryDefaultUILanguage return the registry-selected
 * policy. Called from registry_populate_defaults. */
void nls_locale_register_defaults(void);

#endif /* KERNEL_NT_NLS_LOCALE_H */
