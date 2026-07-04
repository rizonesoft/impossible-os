/* ============================================================================
 * nls_locale.c -- Locale/LCID metadata records + registry policy binding
 *
 * See nls_locale.h for the contract. Design points from the pre-code review:
 *  - All three policy surfaces (system / user / UI language) are registry-backed
 *    and observable, not just the system locale.
 *  - Locale records carry ANSI/OEM code pages (validated against a code-page
 *    provider) so a future CP_THREAD_ACP can derive a thread ACP without a
 *    schema break; the CP_THREAD_ACP WIRING itself is deferred.
 *  - The privileged NtSetDefaultLocale/UILanguage path stays fail-closed (no SRM
 *    privilege check yet); this layer binds only the QUERY path.
 * ============================================================================ */

#include "kernel/types.h"
#include "libc/string.h"           /* strcmp */
#include "kernel/nt/nls_locale.h"
#include "kernel/nt/nls_cp.h"      /* nls_cp_get_provider (ANSI/OEM validation) */
#include "kernel/nt/nt_misc.h"     /* nt_locale_set_default / nt_locale_set_ui_language */
#include "kernel/klog.h"
#include "registry.h"

/* ---- Compiled locale records --------------------------------------------- */
/* first_day_of_week: LOCALE_IFIRSTDAYOFWEEK (0=Monday .. 6=Sunday). */
static const nls_locale_t s_locales[] = {
    { NLS_LCID_INVARIANT, "",      "Invariant",   "Invariant",       1252, 437,
      ".", ",", "M/d/yyyy",  "HH:mm:ss",    "",   6 },   /* invariant: no locale-specific currency */
    { 0x0409, "en-US", "English", "United States", 1252, 437,
      ".", ",", "M/d/yyyy",  "h:mm:ss tt",  "$",  6 },
    { 0x0809, "en-GB", "English", "United Kingdom", 1252, 850,
      ".", ",", "dd/MM/yyyy", "HH:mm:ss",   "GBP", 0 },
    { 0x0407, "de-DE", "German",  "Germany",        1252, 850,
      ",", ".", "dd.MM.yyyy", "HH:mm:ss",   "EUR", 0 },
    { 0x040C, "fr-FR", "French",  "France",         1252, 850,
      ",", " ", "dd/MM/yyyy", "HH:mm:ss",   "EUR", 0 },
    { 0x0C0A, "es-ES", "Spanish", "Spain",          1252, 850,
      ",", ".", "dd/MM/yyyy", "HH:mm:ss",   "EUR", 0 },
};
#define NLS_LOCALE_COUNT (sizeof(s_locales) / sizeof(s_locales[0]))

/* ---- Policy caches (boot-published, acquire-read; see nls_cp.c) ----------- */
static uint32_t s_system_lcid = 0;   /* 0 == uncached -> compiled default */
static uint32_t s_user_lcid   = 0;
static uint32_t s_ui_langid   = 0;

/* ---- Record lookup ------------------------------------------------------- */

const nls_locale_t *nls_locale_by_lcid(uint32_t lcid)
{
    uint32_t i;
    for (i = 0; i < NLS_LOCALE_COUNT; i++)
        if (s_locales[i].lcid == lcid)
            return &s_locales[i];
    return 0;
}

const nls_locale_t *nls_locale_by_bcp47(const char *name)
{
    uint32_t i;
    if (!name)
        return 0;
    /* The invariant locale's Windows name is the empty string; round-trip it. */
    if (name[0] == '\0')
        return nls_locale_by_lcid(NLS_LCID_INVARIANT);
    for (i = 0; i < NLS_LOCALE_COUNT; i++)
        if (s_locales[i].bcp47[0] && strcmp(s_locales[i].bcp47, name) == 0)
            return &s_locales[i];
    return 0;
}

/* ---- Policy accessors ---------------------------------------------------- */

uint32_t nls_locale_get_system(void)
{
    uint32_t v = __atomic_load_n(&s_system_lcid, __ATOMIC_ACQUIRE);
    return v ? v : NLS_LANGID_ENUS;
}

uint32_t nls_locale_get_user(void)
{
    uint32_t v = __atomic_load_n(&s_user_lcid, __ATOMIC_ACQUIRE);
    return v ? v : NLS_LANGID_ENUS;
}

uint16_t nls_locale_get_ui_language(void)
{
    uint32_t v = __atomic_load_n(&s_ui_langid, __ATOMIC_ACQUIRE);
    return (uint16_t)(v ? v : NLS_LANGID_ENUS);
}

/* ---- MUI UI-language fallback chain -------------------------------------- */

uint32_t nls_locale_ui_fallback(uint16_t langid, uint16_t *out, uint32_t cap)
{
    /* Build the chain in a fixed local (at most 3: specific -> neutral primary
     * -> en-US backstop), de-dup against the LOCAL (never the caller buffer),
     * then copy min(count, cap) to out. Returns the full logical count so a
     * caller can size a second pass; a small/zero cap never over-reads or
     * over-writes out[]. */
    uint16_t cand[3];
    uint16_t chain[3];
    uint32_t count = 0, i, j, n;

    cand[0] = langid;
    cand[1] = (uint16_t)(langid & 0x03FFu);            /* PRIMARYLANGID (neutral) */
    cand[2] = (uint16_t)NLS_LANGID_ENUS;               /* en-US backstop */
    for (i = 0; i < 3; i++) {
        int dup = 0;
        for (j = 0; j < count; j++)
            if (chain[j] == cand[i]) { dup = 1; break; }
        if (!dup)
            chain[count++] = cand[i];
    }
    if (out) {
        n = count < cap ? count : cap;
        for (i = 0; i < n; i++)
            out[i] = chain[i];
    }
    return count;
}

/* ---- Registry policy binding --------------------------------------------- */

/* Read a policy DWORD; fall back to `deflt` when absent/malformed OR when the
 * value does not name a supported locale record (so a bad policy never yields an
 * unknown LCID to consumers). */
static uint32_t nls_locale_snapshot(HKEY k, const char *value, uint32_t deflt)
{
    uint32_t v;
    if (RegGetDword(k, value, &v) != 0)
        return deflt;
    return nls_locale_by_lcid(v) ? v : deflt;
}

void nls_locale_register_defaults(void)
{
    HKEY k;
    uint32_t disp, v, sys, usr, ui;

    if (RegCreateKeyEx(HKEY_LOCAL_MACHINE, "SYSTEM\\Nls", 0, (const char *)0, 0,
                       KEY_ALL_ACCESS, (void *)0, &k, &disp) != 0) {
        klog(LOG_WARN, "nls", "cannot open HKLM\\SYSTEM\\Nls -- locale policy defaults");
        __atomic_store_n(&s_system_lcid, (uint32_t)NLS_LANGID_ENUS, __ATOMIC_RELEASE);
        __atomic_store_n(&s_user_lcid,   (uint32_t)NLS_LANGID_ENUS, __ATOMIC_RELEASE);
        __atomic_store_n(&s_ui_langid,   (uint32_t)NLS_LANGID_ENUS, __ATOMIC_RELEASE);
        return;
    }
    /* Seed each value only when absent/malformed -- never clobber existing policy. */
    if (RegGetDword(k, "SystemLocale", &v) != 0)
        RegSetDword(k, "SystemLocale", NLS_LANGID_ENUS);
    if (RegGetDword(k, "UserLocale", &v) != 0)
        RegSetDword(k, "UserLocale", NLS_LANGID_ENUS);
    if (RegGetDword(k, "UILanguage", &v) != 0)
        RegSetDword(k, "UILanguage", NLS_LANGID_ENUS);

    sys = nls_locale_snapshot(k, "SystemLocale", NLS_LANGID_ENUS);
    usr = nls_locale_snapshot(k, "UserLocale",   NLS_LANGID_ENUS);
    ui  = nls_locale_snapshot(k, "UILanguage",   NLS_LANGID_ENUS);
    RegCloseKey(k);

    __atomic_store_n(&s_system_lcid, sys, __ATOMIC_RELEASE);
    __atomic_store_n(&s_user_lcid,   usr, __ATOMIC_RELEASE);
    __atomic_store_n(&s_ui_langid,   ui,  __ATOMIC_RELEASE);

    /* Bind the query path: sync the nt_misc.c globals that NtQueryDefaultLocale
     * (UserProfile=TRUE) and NtQueryDefaultUILanguage read. The system-locale
     * (UserProfile=FALSE) query reads nls_locale_get_system() directly. */
    nt_locale_set_default(usr);
    nt_locale_set_ui_language((uint16_t)ui);
    klog(LOG_INFO, "nls", "locale policy: system=0x%x user=0x%x ui=0x%x",
         (uint64_t)sys, (uint64_t)usr, (uint64_t)ui);
}
