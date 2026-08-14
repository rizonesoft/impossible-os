/* ============================================================================
 * config.c -- Boot argument schema and parser (kernel config plane, Section 1)
 *
 * Static descriptor table + a pure cmdline parser + a Phase-0 reconciliation
 * entry point. See config.h for the full design contract (reconciliation rule,
 * BOOTCFG-canonical-raw projection, Phase-0 no-allocation guarantees).
 * ============================================================================ */

#include "kernel/config.h"
#include "kernel/acpi.h"       /* MAX_CPUS -- bounds the S27 injection keys */
#include "kernel/boot_info.h"
#include "kernel/klog.h"
#include "kernel/boot_halt.h"
#include "kernel/barrier.h"
#include "kernel/policy_lock.h"
#include "kernel/boot_status.h"
#include "kernel/tunables.h"
#include "libc/string.h"

extern struct boot_info g_boot_info;

/* ---- ENUM value name tables (value = index) ------------------------------ */

static const char *const k_safemode_names[] = {
    "off", "minimal", "network", "dsrepair", (void *)0,
};
static const char *const k_bootstatuspolicy_names[] = {
    "DisplayAllFailures", "IgnoreAllFailures", (void *)0,
};

/* ---- Static schema table ------------------------------------------------- */
/* The single source of truth for every kernel-recognized boot key. Keys backed
 * by a struct boot_config field are projected as the BOOTCFG layer; the rest
 * are cmdline-only extension keys. */
static const boot_arg_desc_t k_arg_table[] = {
    { "debug",             BOOT_ARG_BOOL,     BOOT_ARG_CLASS_DEBUG,    BOOT_ARG_PHASE0,
      0, 1, 0, (void *)0,            "live-flush debug logging" },
    { "test",              BOOT_ARG_BOOL,     BOOT_ARG_CLASS_DEBUG,    BOOT_ARG_PHASE0,
      0, 1, 0, (void *)0,            "run unit tests then halt" },
    { "async_init",        BOOT_ARG_BOOL,     BOOT_ARG_CLASS_DEBUG,    BOOT_ARG_PHASE2,
      0, 1, 0, (void *)0,            "parallel subsystem init on APs" },
    { "safemode",          BOOT_ARG_ENUM,     BOOT_ARG_CLASS_SECURITY, BOOT_ARG_PHASE0,
      0, 3, 0, k_safemode_names,     "safe boot profile" },
    { "testsigning",       BOOT_ARG_BOOL,     BOOT_ARG_CLASS_SECURITY, BOOT_ARG_PHASE0,
      0, 1, 0, (void *)0,            "allow test-signed kernel code" },
    { "nointegritychecks", BOOT_ARG_BOOL,     BOOT_ARG_CLASS_SECURITY, BOOT_ARG_PHASE0,
      0, 1, 0, (void *)0,            "disable code-integrity enforcement" },
    { "noacpi",            BOOT_ARG_BOOL,     BOOT_ARG_CLASS_SECURITY, BOOT_ARG_PHASE0,
      0, 1, 0, (void *)0,            "skip ACPI bring-up" },
    { "nogui",             BOOT_ARG_BOOL,     BOOT_ARG_CLASS_NORMAL,   BOOT_ARG_PHASE3,
      0, 1, 0, (void *)0,            "disable the graphical shell" },
    { "bootstatuspolicy",  BOOT_ARG_ENUM,     BOOT_ARG_CLASS_NORMAL,   BOOT_ARG_PHASE3,
      0, 1, 0, k_bootstatuspolicy_names, "failed-boot display policy" },
    { "recoveryenabled",   BOOT_ARG_BOOL,     BOOT_ARG_CLASS_NORMAL,   BOOT_ARG_PHASE3,
      0, 1, 1, (void *)0,            "allow recovery environment entry" },
    { "verifier",          BOOT_ARG_CSV,      BOOT_ARG_CLASS_DEBUG,    BOOT_ARG_PHASE1,
      0, 0, 0, (void *)0,            "driver verifier flags (CSV or *)" },
    { "boot.allow_unknown", BOOT_ARG_BOOL,    BOOT_ARG_CLASS_NORMAL,   BOOT_ARG_PHASE0,
      0, 1, 0, (void *)0,            "downgrade unknown kernel.* keys to a warning" },
#ifdef KERNEL_TESTS
    /* Degraded-configuration fault injection (TODO-10 S27). Every automated leg
     * this repo runs boots HEALTHY, where the live CPU count equals the present
     * count -- so the two-count split and the quiescence rules that depend on it
     * had no live coverage of the case they exist for. These two keys produce
     * those configurations on demand. Both the branches they gate AND these
     * schema rows compile out at KERNEL_TESTS=off, so a released kernel carries
     * neither the injection code nor the configuration surface to reach it.
     * A CPU index of 0 means "off": the BSP is never an AP and never an async
     * worker, so it is not a legal target either way.
     *
     * REACHED VIA `cmdline=`, NOT as a bare boot.conf line. Only the typed
     * struct boot_config fields are projected into the arg table
     * (boot_args_init below); an extension key written directly into boot.conf
     * is consumed by the bootloader's own key parser and never arrives here --
     * and it is not reported as unknown either, so the mistake is silent and
     * reads as "the injection did not fire". Reproduce with:
     *
     *   bash scripts/patch-boot-conf.sh cmdline "test_park_cpu=1"
     *   bash scripts/patch-boot-conf.sh async_init 1 cmdline "test_hold_async_cpu=1"
     *
     * then boot build/system-disk.img directly: test-smoke.sh rebuilds the
     * image and would overwrite the patch before QEMU ever sees it. */
    { "test_abandon_ap",   BOOT_ARG_INT,      BOOT_ARG_CLASS_DEBUG,    BOOT_ARG_PHASE2,
      0, MAX_CPUS - 1, 0, (void *)0, "abandon this AP slot at bringup (0 = off)" },
    { "test_hold_async_cpu", BOOT_ARG_INT,    BOOT_ARG_CLASS_DEBUG,    BOOT_ARG_PHASE2,
      0, MAX_CPUS - 1, 0, (void *)0, "hold this CPU inside its async step (0 = off)" },
    { "test_park_cpu",     BOOT_ARG_INT,      BOOT_ARG_CLASS_DEBUG,    BOOT_ARG_PHASE2,
      0, MAX_CPUS - 1, 0, (void *)0, "park this CPU after bringup (0 = off)" },
#endif
};
#define K_ARG_COUNT (sizeof(k_arg_table) / sizeof(k_arg_table[0]))

const boot_arg_desc_t *boot_arg_table(uint32_t *out_count)
{
    if (out_count)
        *out_count = (uint32_t)K_ARG_COUNT;
    return k_arg_table;
}

const boot_arg_desc_t *boot_arg_find(const char *name)
{
    if (!name)
        return (void *)0;
    for (uint32_t i = 0; i < K_ARG_COUNT; i++) {
        if (strcmp(k_arg_table[i].name, name) == 0)
            return &k_arg_table[i];
    }
    return (void *)0;
}

/* ---- Small pure scalar helpers ------------------------------------------- */

static int ci_eq(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb) return 0;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

/* Parse a signed decimal integer. Returns 0 on success and writes *out; -1 on
 * any malformed input (empty, non-digit, lone '-'). */
static int parse_i64(const char *s, int64_t *out)
{
    if (!s || !*s) return -1;
    int neg = 0;
    if (*s == '-') { neg = 1; s++; if (!*s) return -1; }
    int64_t v = 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') return -1;
        int d = *s - '0';
        /* Reject before the multiply-add can wrap int64 (untrusted cmdline). */
        if (v > (9223372036854775807LL - d) / 10) return -1;
        v = v * 10 + d;
    }
    *out = neg ? -v : v;
    return 0;
}

/* Split off the trailing alpha suffix of a number+unit token. Copies the
 * leading digits into num[] (bounded) and returns a pointer to the suffix
 * (possibly empty). Returns NULL when the digit run overflows num[] or no digit
 * is present. */
static const char *split_unit(const char *s, char *num, uint32_t numcap)
{
    uint32_t n = 0;
    int saw_digit = 0;
    if (*s == '-') { if (n + 1 >= numcap) return (void *)0; num[n++] = *s++; }
    while (*s >= '0' && *s <= '9') {
        saw_digit = 1;
        if (n + 1 >= numcap) return (void *)0;
        num[n++] = *s++;
    }
    if (!saw_digit) return (void *)0;
    num[n] = 0;
    return s;
}

/* ---- Per-type value validation ------------------------------------------- */

/* On success writes desc-typed result into v->ival / v->sval and returns
 * BOOT_ARGS_OK. Returns BOOT_ARGS_ERR_BAD_VALUE / _OVERFLOW otherwise. `val`
 * is the value string (may be empty for a bare BOOL token, handled by caller). */
static boot_args_status_t validate_value(const boot_arg_desc_t *d,
                                         const char *val, boot_arg_value_t *v)
{
    switch (d->type) {
    case BOOT_ARG_BOOL:
        if (ci_eq(val, "1") || ci_eq(val, "on") || ci_eq(val, "yes") ||
            ci_eq(val, "true"))   { v->ival = 1; return BOOT_ARGS_OK; }
        if (ci_eq(val, "0") || ci_eq(val, "off") || ci_eq(val, "no") ||
            ci_eq(val, "false"))  { v->ival = 0; return BOOT_ARGS_OK; }
        return BOOT_ARGS_ERR_BAD_VALUE;
    case BOOT_ARG_INT: {
        int64_t n;
        if (parse_i64(val, &n) != 0)            return BOOT_ARGS_ERR_BAD_VALUE;
        if (n < d->min_val || n > d->max_val)   return BOOT_ARGS_ERR_BAD_VALUE;
        v->ival = n; return BOOT_ARGS_OK;
    }
    case BOOT_ARG_ENUM: {
        if (!d->enum_names) return BOOT_ARGS_ERR_BAD_VALUE;
        for (int64_t i = 0; d->enum_names[i]; i++) {
            if (ci_eq(val, d->enum_names[i])) { v->ival = i; return BOOT_ARGS_OK; }
        }
        return BOOT_ARGS_ERR_BAD_VALUE;
    }
    case BOOT_ARG_STRING:
        if (strlen(val) > BOOT_ARG_STRVAL_CAP)  return BOOT_ARGS_ERR_OVERFLOW;
        strncpy(v->sval, val, BOOT_ARG_STRVAL_CAP);
        v->sval[BOOT_ARG_STRVAL_CAP] = 0;
        return BOOT_ARGS_OK;
    case BOOT_ARG_DURATION: {
        char num[BOOT_ARG_RAWVAL_MAX];
        const char *suf = split_unit(val, num, sizeof(num));
        if (!suf) return BOOT_ARGS_ERR_BAD_VALUE;
        int64_t n;
        if (parse_i64(num, &n) != 0 || n < 0) return BOOT_ARGS_ERR_BAD_VALUE;
        int64_t mul;
        if (ci_eq(suf, "ms") || *suf == 0) mul = 1;
        else if (ci_eq(suf, "s"))          mul = 1000;
        else if (ci_eq(suf, "m"))          mul = 60000;
        else return BOOT_ARGS_ERR_BAD_VALUE;
        if (mul != 0 && n > 9223372036854775807LL / mul)  /* reject pre-multiply wrap */
            return BOOT_ARGS_ERR_BAD_VALUE;
        int64_t ms = n * mul;
        if (d->max_val > 0 && (ms < d->min_val || ms > d->max_val))
            return BOOT_ARGS_ERR_BAD_VALUE;
        v->ival = ms; return BOOT_ARGS_OK;
    }
    case BOOT_ARG_BYTESIZE: {
        char num[BOOT_ARG_RAWVAL_MAX];
        const char *suf = split_unit(val, num, sizeof(num));
        if (!suf) return BOOT_ARGS_ERR_BAD_VALUE;
        int64_t n;
        if (parse_i64(num, &n) != 0 || n < 0) return BOOT_ARGS_ERR_BAD_VALUE;
        int64_t mul;
        if (*suf == 0)                      mul = 1;
        else if (ci_eq(suf, "k"))           mul = 1024;
        else if (ci_eq(suf, "m"))           mul = 1024 * 1024;
        else if (ci_eq(suf, "g"))           mul = 1024 * 1024 * 1024;
        else return BOOT_ARGS_ERR_BAD_VALUE;
        if (mul != 0 && n > 9223372036854775807LL / mul)  /* reject pre-multiply wrap */
            return BOOT_ARGS_ERR_BAD_VALUE;
        v->ival = n * mul;
        if (d->max_val > 0 && (v->ival < d->min_val || v->ival > d->max_val))
            return BOOT_ARGS_ERR_BAD_VALUE;
        return BOOT_ARGS_OK;
    }
    case BOOT_ARG_CSV: {
        /* "*" is the wildcard-all sentinel; otherwise count comma-separated
         * elements and copy the joined list (already comma-separated) verbatim
         * after the length + element-count checks. */
        if (strlen(val) > BOOT_ARG_STRVAL_CAP) return BOOT_ARGS_ERR_OVERFLOW;
        uint8_t elems = 1;
        for (const char *p = val; *p; p++) {
            if (*p == ',') {
                elems++;
                if (elems > BOOT_ARG_CSV_MAX) return BOOT_ARGS_ERR_OVERFLOW;
            }
        }
        strncpy(v->sval, val, BOOT_ARG_STRVAL_CAP);
        v->sval[BOOT_ARG_STRVAL_CAP] = 0;
        v->csv_count = (uint8_t)(ci_eq(val, "*") ? 1 : elems);
        return BOOT_ARGS_OK;
    }
    }
    return BOOT_ARGS_ERR_BAD_VALUE;
}

/* ---- Result accessors + insertion ---------------------------------------- */

const boot_arg_value_t *boot_args_get(const boot_args_t *args, const char *name)
{
    if (!args || !name) return (void *)0;
    const boot_arg_desc_t *d = boot_arg_find(name);
    if (!d) return (void *)0;
    for (uint32_t i = 0; i < args->count; i++) {
        if (args->values[i].desc == d) return &args->values[i];
    }
    return (void *)0;
}

/* Find the mutable slot for `d`, allocating a new one if absent. Returns NULL
 * when the result is already at BOOT_ARGS_TOKEN_MAX distinct keys. */
static boot_arg_value_t *result_slot(boot_args_t *out, const boot_arg_desc_t *d)
{
    for (uint32_t i = 0; i < out->count; i++) {
        if (out->values[i].desc == d) return &out->values[i];
    }
    if (out->count >= BOOT_ARGS_TOKEN_MAX) return (void *)0;
    boot_arg_value_t *v = &out->values[out->count++];
    memset(v, 0, sizeof(*v));
    v->desc = d;
    return v;
}

static void record_unknown(boot_args_t *out, const char *key)
{
    if (out->unknown_count < BOOT_ARGS_UNKNOWN_MAX) {
        strncpy(out->unknown[out->unknown_count], key, BOOT_ARG_NAME_CAP);
        out->unknown[out->unknown_count][BOOT_ARG_NAME_CAP] = 0;
        out->unknown_count++;
    }
}

static void fail(boot_args_t *out, boot_args_status_t st, const char *key)
{
    const char *k = key ? key : "?";
    if (out->status == BOOT_ARGS_OK) {     /* first failure wins (status/err_key) */
        out->status = st;
        strncpy(out->err_key, k, BOOT_ARG_NAME_CAP);
        out->err_key[BOOT_ARG_NAME_CAP] = 0;
    }
    /* Separately remember the first hard (non-downgradeable) error so the
     * end-of-parse allow_unknown downgrade can promote it instead of erasing a
     * real malformed-value failure. */
    if ((st == BOOT_ARGS_ERR_BAD_VALUE || st == BOOT_ARGS_ERR_OVERFLOW) &&
        out->hard_status == BOOT_ARGS_OK) {
        out->hard_status = st;
        strncpy(out->hard_key, k, BOOT_ARG_NAME_CAP);
        out->hard_key[BOOT_ARG_NAME_CAP] = 0;
    }
}

/* ---- Pure cmdline parser ------------------------------------------------- */

/* Convenience cap for the unbounded wrapper used by NUL-terminated literals
 * (unit tests). The live Phase-0 path passes BOOT_CONF_CMDLINE_MAX instead. */
#define BOOT_ARG_CMDLINE_SCAN_MAX 1024u

boot_args_status_t boot_args_parse_cmdline(const char *cmdline, boot_args_t *out)
{
    return boot_args_parse_cmdline_n(cmdline, BOOT_ARG_CMDLINE_SCAN_MAX, out);
}

boot_args_status_t boot_args_parse_cmdline_n(const char *cmdline, uint32_t max_len,
                                             boot_args_t *out)
{
    memset(out, 0, sizeof(*out));
    out->status = BOOT_ARGS_OK;
    if (!cmdline || max_len == 0) return BOOT_ARGS_OK;

    /* Bound the untrusted handoff: require a NUL terminator within max_len so
     * the parse loop cannot read past the fixed-size cmdline field into
     * adjacent boot_info bytes. A corrupt/unterminated field is rejected. */
    uint32_t len = 0;
    while (len < max_len && cmdline[len]) len++;
    if (len == max_len) { fail(out, BOOT_ARGS_ERR_OVERFLOW, "?"); return out->status; }

    const char *p = cmdline;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;     /* skip separators */
        if (!*p) break;

        /* Lift one whitespace-delimited token. */
        char tok[BOOT_ARG_NAME_MAX + BOOT_ARG_RAWVAL_MAX + 2];
        uint32_t tn = 0;
        while (*p && *p != ' ' && *p != '\t') {
            if (tn + 1 >= sizeof(tok)) { fail(out, BOOT_ARGS_ERR_OVERFLOW, "?"); goto done; }
            tok[tn++] = *p++;
        }
        tok[tn] = 0;

        /* Normalize `/key` alias form to `key`. */
        char *t = tok;
        if (t[0] == '/') t++;

        /* Split key=value (bare token -> implicit BOOL "1"). */
        char key[BOOT_ARG_NAME_MAX];
        const char *val;
        char *eq = strchr(t, '=');
        uint32_t klen = eq ? (uint32_t)(eq - t) : (uint32_t)strlen(t);
        if (klen > BOOT_ARG_NAME_CAP) { fail(out, BOOT_ARGS_ERR_OVERFLOW, "?"); goto done; }
        memcpy(key, t, klen);
        key[klen] = 0;
        val = eq ? eq + 1 : "1";
        if (strlen(val) > BOOT_ARG_RAWVAL_CAP) { fail(out, BOOT_ARGS_ERR_OVERFLOW, key); goto done; }

        const boot_arg_desc_t *d = boot_arg_find(key);
        if (!d) {
            /* Unknown key. kernel.* without boot.allow_unknown is the fatal
             * class; everything else outside the schema is ignored. */
            if (strncmp(key, "kernel.", 7) == 0) {
                record_unknown(out, key);
                fail(out, BOOT_ARGS_ERR_UNKNOWN_KEY, key);
            }
            continue;
        }

        boot_arg_value_t *v = result_slot(out, d);
        if (!v) { fail(out, BOOT_ARGS_ERR_OVERFLOW, key); goto done; }
        boot_args_status_t vs = validate_value(d, val, v);
        if (vs != BOOT_ARGS_OK) { fail(out, vs, key); continue; }
        v->source = BOOT_ARG_SRC_CMDLINE;
        v->present = 1;
        strncpy(v->raw, val, BOOT_ARG_RAWVAL_CAP);
        v->raw[BOOT_ARG_RAWVAL_CAP] = 0;
        if (d == boot_arg_find("boot.allow_unknown") && v->ival)
            out->allow_unknown = 1;
    }

done:
    /* Single finalization point -- every post-parse-start exit (normal end or an
     * early overflow `goto done`) runs this so the downgrade/promotion is never
     * bypassed. allow_unknown downgrades a leading UNKNOWN_KEY, but only to a
     * clean OK when NO hard error occurred; a malformed known value/overflow is
     * promoted to the reported status so the reject-invalid-arg contract holds.
     * With allow_unknown absent the first failure is kept verbatim. */
    if (out->status == BOOT_ARGS_ERR_UNKNOWN_KEY && out->allow_unknown) {
        if (out->hard_status != BOOT_ARGS_OK) {
            out->status = out->hard_status;
            strncpy(out->err_key, out->hard_key, BOOT_ARG_NAME_CAP);
            out->err_key[BOOT_ARG_NAME_CAP] = 0;
        } else {
            out->status = BOOT_ARGS_OK;
        }
    }
    return out->status;
}

/* ---- BOOTCFG projection -------------------------------------------------- */

/* Project one boot_config-backed key as the BOOTCFG layer: canonical raw,
 * present=1, source=BOOTCFG. Does nothing when the key is already CMDLINE-set
 * (cmdline wins per Section 3 precedence) -- but counts the override. */
static void project(boot_args_t *out, const char *name, int64_t ival,
                    const char *raw)
{
    const boot_arg_desc_t *d = boot_arg_find(name);
    if (!d) return;
    boot_arg_value_t *v = result_slot(out, d);
    if (!v) return;
    if (v->present && v->source == BOOT_ARG_SRC_CMDLINE) {
        out->overridden_count++;     /* cmdline already won; record the conflict */
        return;
    }
    v->ival = ival;
    v->source = BOOT_ARG_SRC_BOOTCFG;
    v->present = 1;
    strncpy(v->raw, raw, BOOT_ARG_RAWVAL_CAP);
    v->raw[BOOT_ARG_RAWVAL_CAP] = 0;
}

/* ---- Phase-0 entry point + provenance ------------------------------------ */

static boot_args_t g_boot_args;     /* published parsed result (several KB) */
static int         g_boot_args_ready;

boot_result_t boot_args_init(const struct boot_config *cfg)
{
    if (!cfg)
        return BOOT_FATAL;

    /* CMDLINE layer first so projection can detect cmdline overrides. Bounded
     * to the fixed handoff-field size -- a corrupt/unterminated cmdline is
     * rejected, never over-read. */
    boot_args_parse_cmdline_n(cfg->cmdline, BOOT_CONF_CMDLINE_MAX, &g_boot_args);

    /* BOOTCFG layer: canonical projection of the authoritative struct fields
     * that have a schema descriptor. boot_mode 0/1/2 -> safemode off/minimal/
     * recovery; only minimal maps to a non-zero safe profile here. */
    project(&g_boot_args, "debug",      cfg->debug ? 1 : 0,      cfg->debug ? "1" : "0");
    project(&g_boot_args, "test",       cfg->test ? 1 : 0,       cfg->test ? "1" : "0");
    project(&g_boot_args, "async_init", cfg->async_init ? 1 : 0, cfg->async_init ? "1" : "0");
    if (cfg->boot_mode == 1)
        project(&g_boot_args, "safemode", 1, "minimal");

    /* RELEASE, because the reader is cross-CPU. boot_arg_resolved_ival()
     * ACQUIRE-loads this flag from an AP inside the async IPI handler, and a
     * plain store here would leave that acquire nothing to pair with: the
     * several-KB g_boot_args table filled above could be visible after the
     * flag. x86 TSO hides it today; the planned ARM64 port would not. */
    __atomic_store_n(&g_boot_args_ready, 1, __ATOMIC_RELEASE);

    if (g_boot_args.overridden_count)
        klog(LOG_INFO, "CONF", "[CONF] %u cmdline override(s) of boot.conf keys",
             (uint32_t)g_boot_args.overridden_count);

    if (g_boot_args.status == BOOT_ARGS_ERR_UNKNOWN_KEY) {
        klog(LOG_ERROR, "CONF", "[CONF] unknown boot key: %s", g_boot_args.err_key);
        return BOOT_FATAL;     /* caller halts -- Phase 0 has no degraded path */
    }
    if (g_boot_args.status == BOOT_ARGS_ERR_BAD_VALUE ||
        g_boot_args.status == BOOT_ARGS_ERR_OVERFLOW) {
        klog(LOG_ERROR, "CONF", "[CONF] bad boot arg: %s", g_boot_args.err_key);
        return BOOT_FATAL;
    }

    klog(LOG_INFO, "CONF", "[CONF] parsed boot args: %u keys (%u unknown ignored)",
         g_boot_args.count, (uint32_t)g_boot_args.unknown_count);
    return BOOT_OK;
}

const boot_args_t *boot_args_parsed(void)
{
    return g_boot_args_ready ? &g_boot_args : (void *)0;
}

const char *boot_args_cmdline(void)
{
    return g_boot_info.config.cmdline;
}

const char *boot_args_entry_id(void)
{
    return g_boot_info.selected_entry_id;
}

uint32_t boot_args_selection_reason(void)
{
    /* The boot-ENTRY ladder reason (enum boot_selection_reason), distinct from
     * boot_reason (the policy reason). The accessor name promises this field. */
    return g_boot_info.selection_reason;
}

/* ---- Section 2: immutable kernel_config_t snapshot ----------------------- */

static kernel_config_t g_kernel_config;
static int             g_kernel_config_ready;   /* release-published last */

/* Resolved value of a boot-arg key: the parsed value if present, else the
 * descriptor default. Flattening every key through this means consumers of
 * kernel_config_t never re-derive a default or re-interpret an enum. */
static int64_t resolved_ival(const boot_args_t *a, const char *name)
{
    const boot_arg_value_t *v = a ? boot_args_get(a, name) : (void *)0;
    if (v)
        return v->ival;
    const boot_arg_desc_t *d = boot_arg_find(name);
    return d ? d->default_val : 0;
}

#ifdef KERNEL_TESTS
/* Resolved value of a boot-arg key that has no kernel_config_t field of its own
 * (TODO-10 S27 injection keys). Answers 0 before the Phase-0 reconciliation has
 * published g_boot_args, so an injection site that runs early reads "off"
 * rather than a half-parsed table. Test-flavor only: at KERNEL_TESTS=off there
 * are no extension keys of this shape and no caller. */
int64_t boot_arg_resolved_ival(const char *name)
{
    if (!name || !__atomic_load_n(&g_boot_args_ready, __ATOMIC_ACQUIRE))
        return 0;
    return resolved_ival(&g_boot_args, name);
}
#endif

void safe_mode_resolve(uint8_t boot_mode, uint8_t sm_arg, int operator_set,
                       uint8_t *out_level, uint8_t *out_reason)
{
    /* boot_mode: 0=normal, 1=safe, 2=recovery. Any OTHER value is a corrupt or
     * unrecognized handoff -- fail SAFE (floor to MINIMAL), never fail open to
     * normal mode. recovery(2)/safe(1)/unknown all floor the level to MINIMAL;
     * the operator may escalate above the floor but a lower-precedence
     * safemode=off can never drop below it. */
    int is_recovery = (boot_mode == 2);
    int is_safe_policy = (boot_mode != 0);   /* 1, 2, or any unknown value */
    uint8_t floor = is_safe_policy
                        ? (uint8_t)SAFE_MODE_MINIMAL : (uint8_t)SAFE_MODE_OFF;
    uint8_t level = sm_arg > floor ? sm_arg : floor;
    /* A corrupt safemode= arg can exceed the highest defined level; clamp to
     * DSREPAIR (most restrictive) so gating + name-table stay in range. */
    if (level >= (uint8_t)SAFE_MODE_COUNT)
        level = (uint8_t)SAFE_MODE_DSREPAIR;
    uint8_t reason;
    if (level == SAFE_MODE_OFF)
        reason = (uint8_t)SAFE_REASON_NONE;
    else if (is_recovery)
        reason = (uint8_t)SAFE_REASON_RECOVERY;
    else if (operator_set && sm_arg > floor)
        reason = (uint8_t)SAFE_REASON_OPERATOR;   /* operator ESCALATED above the floor */
    else
        reason = (uint8_t)SAFE_REASON_BOOT_POLICY; /* boot policy forced the effective level */
    if (out_level)  *out_level = level;
    if (out_reason) *out_reason = reason;
}

void kernel_config_publish(const struct boot_config *cfg)
{
    const boot_args_t *a = boot_args_parsed();
    kernel_config_t *k = &g_kernel_config;
    memset(k, 0, sizeof(*k));
    k->magic   = KERNEL_CONFIG_MAGIC;
    k->version = (uint16_t)KERNEL_CONFIG_VERSION;
    k->size    = (uint16_t)sizeof(*k);

    k->boot_mode        = cfg ? cfg->boot_mode : 0;
    {
        uint8_t sm_arg = (uint8_t)resolved_ival(a, "safemode");
        const boot_arg_value_t *sm_v = a ? boot_args_get(a, "safemode") : (void *)0;
        int operator_set = sm_v && sm_v->source == BOOT_ARG_SRC_CMDLINE && sm_v->present;
        safe_mode_resolve(cfg ? cfg->boot_mode : 0, sm_arg, operator_set,
                          &k->safe_mode, &k->safe_mode_reason);
    }
    k->async_init       = (uint8_t)resolved_ival(a, "async_init");
    k->deferred_init    = cfg ? cfg->deferred : 0;   /* not a schema key; raw cfg field */
    k->debug_enabled    = (uint8_t)resolved_ival(a, "debug");
    k->serial_debug     = cfg ? cfg->serial_debug : 0;
    k->test_mode        = (uint8_t)resolved_ival(a, "test");
    k->testsigning      = (uint8_t)resolved_ival(a, "testsigning");
    k->nointegritychecks = (uint8_t)resolved_ival(a, "nointegritychecks");
    k->noacpi           = (uint8_t)resolved_ival(a, "noacpi");
    k->nogui            = (uint8_t)resolved_ival(a, "nogui");

    /* Graphics mode from the validated framebuffer handoff. */
    k->fb_valid     = (uint8_t)(g_boot_info.fb_available && g_boot_info.fb.addr ? 1 : 0);
    k->pixel_format = g_boot_info.fb.pixel_format;
    k->fb_width     = g_boot_info.fb.width;
    k->fb_height    = g_boot_info.fb.height;

    /* Boot-decision provenance (valid: caller publishes after boot_decision_validate). */
    k->boot_reason      = g_boot_info.boot_reason;
    k->selection_reason = g_boot_info.selection_reason;
    strncpy(k->entry_id, g_boot_info.selected_entry_id, sizeof(k->entry_id) - 1);
    k->entry_id[sizeof(k->entry_id) - 1] = 0;

    const boot_arg_value_t *vf = a ? boot_args_get(a, "verifier") : (void *)0;
    if (vf) {
        strncpy(k->verifier, vf->sval, BOOT_ARG_STRVAL_CAP);
        k->verifier[BOOT_ARG_STRVAL_CAP] = 0;
    }
    k->cmdline_override_count = a ? a->overridden_count : 0;

    /* Publish: every field is written above; the ready flag is RELEASE-stored
     * last so a concurrent reader (AP / late consumer) that observes ready via
     * an acquire load is guaranteed to see the fully-written snapshot. A C
     * atomic release/acquire (not a plain int + smp_mb) is the sound, repo-
     * standard publication contract. */
    __atomic_store_n(&g_kernel_config_ready, 1, __ATOMIC_RELEASE);

    klog(LOG_INFO, "CONF", "[CONF] snapshot ready: version=%u phase=0",
         (uint32_t)k->version);

    /* Surface the resolved safe-mode policy so the boot serial log shows the
     * effective level + why it is in effect (test-checkpoint observable). */
    if (k->safe_mode != SAFE_MODE_OFF) {
        static const char *const sm_name[] = {
            "off", "minimal", "network", "dsrepair" };
        static const char *const rsn_name[] = {
            "none", "operator", "boot-policy", "recovery" };
        /* Name tables must stay length-synced with the enums they serialize;
         * an added enum value fails the build instead of aliasing a string. */
        _Static_assert(sizeof(sm_name) / sizeof(sm_name[0]) == SAFE_MODE_COUNT,
            "sm_name[] out of sync with safe_mode_t");
        _Static_assert(sizeof(rsn_name) / sizeof(rsn_name[0]) == SAFE_REASON_COUNT,
            "rsn_name[] out of sync with safe_mode_reason_t");
        const char *sm = k->safe_mode < SAFE_MODE_COUNT
                             ? sm_name[k->safe_mode] : "invalid";
        const char *rsn = k->safe_mode_reason < SAFE_REASON_COUNT
                              ? rsn_name[k->safe_mode_reason] : "invalid";
        klog(LOG_INFO, "CONF", "[CONF] safe_mode=%s reason=%s", sm, rsn);
    }
}

const kernel_config_t *kernel_config_get(void)
{
    if (!__atomic_load_n(&g_kernel_config_ready, __ATOMIC_ACQUIRE))
        return (void *)0;
    return &g_kernel_config;
}

/* ---- Section 5: Safe Mode policy accessors ------------------------------- */

safe_mode_t kernel_safe_mode(void)
{
    const kernel_config_t *k = kernel_config_get();
    return k ? (safe_mode_t)k->safe_mode : SAFE_MODE_OFF;
}

safe_mode_reason_t kernel_safe_mode_reason(void)
{
    const kernel_config_t *k = kernel_config_get();
    return k ? (safe_mode_reason_t)k->safe_mode_reason : SAFE_REASON_NONE;
}

int safe_mode_component_allowed(safe_mode_t level, safe_mode_component_t component)
{
    if (level == SAFE_MODE_OFF)
        return 1;                       /* normal boot -- nothing gated */
    switch (component) {
    case SAFE_COMP_NETWORK:
        return level == SAFE_MODE_NETWORK ? 1 : 0;  /* networking only at the network level */
    case SAFE_COMP_GUI:                  /* basic graphics only; full shell disabled */
    case SAFE_COMP_THIRD_PARTY_MODULE:   /* core modules only */
    case SAFE_COMP_NONESSENTIAL_SVC:     /* essential services only */
    case SAFE_COMP_CI_RELAX:             /* CI relaxation never auto-allowed in safe mode */
        return 0;
    }
    return 0;
}

int kernel_safe_mode_allows(safe_mode_component_t component)
{
    return safe_mode_component_allowed(kernel_safe_mode(), component);
}

/* ---- config_dump (operator/support diagnostic) -------------------------- */

/* Small paginated scratch in .bss, NOT on the caller stack (~88 bytes/row would
 * consume an 8 KiB kernel stack at full capacity, and a full-capacity static
 * pushed kernel BSS into the user base). config_dump streams the whole tunable
 * registry through this chunk via kernel_tunable_dump()'s start index. */
#define CONFIG_DUMP_CHUNK 8u
static tunable_snapshot_t s_dump_rows[CONFIG_DUMP_CHUNK];
/* Non-reentrant guard for the shared scratch (atomic, not a spinlock, so no
 * lock is held across the klog serial writes -- a concurrent dumper backs off). */
static volatile int s_dump_busy;

static const char *const k_dump_safe_names[] = {
    "off", "minimal", "network", "dsrepair",
};
_Static_assert(sizeof(k_dump_safe_names) / sizeof(k_dump_safe_names[0]) == SAFE_MODE_COUNT,
    "safe-mode dump-name table must match safe_mode_t (enum/name-table drift)");

void config_dump(void)
{
    int expected = 0;
    if (!__atomic_compare_exchange_n(&s_dump_busy, &expected, 1, 0,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        klog(LOG_INFO, "CONF", "[CONF] config_dump: already in progress -- skipped");
        return;
    }

    const kernel_config_t *c = kernel_config_get();
    if (!c) {
        klog(LOG_INFO, "CONF", "[CONF] config_dump: snapshot not yet published");
    } else {
        safe_mode_t sm = kernel_safe_mode();
        klog(LOG_INFO, "CONF",
             "[CONF] dump: boot_mode=%u safe_mode=%s(reason=%u) debug=%u testsigning=%u nointegrity=%u nogui=%u",
             (uint32_t)c->boot_mode,
             (sm < SAFE_MODE_COUNT) ? k_dump_safe_names[sm] : "?",
             (uint32_t)kernel_safe_mode_reason(),
             (uint32_t)c->debug_enabled, (uint32_t)c->testsigning,
             (uint32_t)c->nointegritychecks, (uint32_t)c->nogui);
        klog(LOG_INFO, "CONF",
             "[CONF] dump: boot_reason=%u selection_reason=%u entry_id=\"%s\" cmdline_overrides=%u",
             (uint32_t)c->boot_reason, (uint32_t)c->selection_reason,
             c->entry_id, (uint32_t)c->cmdline_override_count);
    }

    /* Policy-lock plane (lockless getters). */
    klog(LOG_INFO, "CONF",
         "[CONF] dump: lockdown=%u lock_phase=%u policies=%u tamper=%lu",
         (uint32_t)kernel_lockdown_level_get(),
         (uint32_t)kernel_policy_lock_phase_get(),
         kernel_policy_count(), (uint64_t)kernel_policy_tamper_count());

    /* Boot-status policy + acceptance ledger. */
    const boot_status_policy_t *bs = boot_status_policy_get();
    if (bs)
        klog(LOG_INFO, "CONF",
             "[CONF] dump: boot_status accept=%s display=%u recovery=%u stage=%s",
             boot_status_stage_name((boot_accept_stage_t)bs->accept_stage),
             (uint32_t)bs->failure_display, (uint32_t)bs->recovery_enabled,
             boot_status_stage_name(boot_status_stage()));
    else
        klog(LOG_INFO, "CONF", "[CONF] dump: boot_status not yet initialized");

    /* Runtime tunable registry. Privileged tunables are the secret class: their
     * current value is redacted so a shared dump / support bundle cannot leak
     * sensitive tuning. Name, bounds, flags, and source stay visible. */
    klog(LOG_INFO, "CONF", "[CONF] dump: %u tunables", kernel_tunable_count());
    /* Stream the whole registry through the small chunk (no truncation). The
     * cursor is a raw slot index, stable if the registry mutates mid-dump. */
    uint32_t cursor = 0, got;
    do {
        got = kernel_tunable_dump(s_dump_rows, CONFIG_DUMP_CHUNK, &cursor);
        for (uint32_t i = 0; i < got; i++) {
            const tunable_snapshot_t *t = &s_dump_rows[i];
            if (t->flags & TUNABLE_PRIVILEGED)
                klog(LOG_INFO, "CONF",
                     "[CONF]   %s = **** (privileged; def/min/max redacted) flags=0x%x src=%u",
                     t->name, (uint32_t)t->flags, (uint32_t)t->source);
            else
                klog(LOG_INFO, "CONF",
                     "[CONF]   %s = %ld (def=%ld min=%ld max=%ld) flags=0x%x src=%u",
                     t->name, (int64_t)t->cur, (int64_t)t->def,
                     (int64_t)t->min, (int64_t)t->max,
                     (uint32_t)t->flags, (uint32_t)t->source);
        }
    } while (got == CONFIG_DUMP_CHUNK);

    /* Boot-arg schema: every recognized key with its phase, default, range, and
     * its effective parsed value + provenance. This is the operator-facing
     * "supported keys / defaults / ranges / lock phases" surface the section
     * promises, so a key's effective-vs-default and source are auditable from
     * the dump without reverse-engineering the parser. */
    uint32_t ac = 0;
    const boot_arg_desc_t *at = boot_arg_table(&ac);
    const boot_args_t *pa = boot_args_parsed();
    klog(LOG_INFO, "CONF", "[CONF] dump: %u boot-arg keys", ac);
    for (uint32_t i = 0; at && i < ac; i++) {
        const boot_arg_desc_t *d = &at[i];
        const boot_arg_value_t *v = pa ? boot_args_get(pa, d->name) : (const boot_arg_value_t *)0;
        klog(LOG_INFO, "CONF",
             "[CONF]   %s: phase=%u class=%u def=%ld range=[%ld,%ld] value=%ld src=%u present=%u",
             d->name, (uint32_t)d->phase, (uint32_t)d->security_class,
             (int64_t)d->default_val, (int64_t)d->min_val, (int64_t)d->max_val,
             v ? (int64_t)v->ival : (int64_t)d->default_val,
             v ? (uint32_t)v->source : (uint32_t)BOOT_ARG_SRC_DEFAULT,
             v ? (uint32_t)v->present : 0u);
    }

    __atomic_store_n(&s_dump_busy, 0, __ATOMIC_RELEASE);
}
