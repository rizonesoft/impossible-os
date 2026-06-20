/* ============================================================================
 * feature.c -- Feature Flag Gates and Experiment Cohorts
 *
 * See include/kernel/feature.h for the contract. SMP-safe via one irqsave
 * spinlock; each feature resolves once and caches its decision for the boot.
 * ============================================================================ */
#include "kernel/feature.h"
#include "kernel/boot_info.h"
#include "kernel/smbios.h"
#include "kernel/crypto/sha256.h"
#include "kernel/klog.h"
#include "kernel/sched/spinlock.h"
#include "libc/string.h"

extern struct boot_info g_boot_info;

#define FEATURE_MAX   64u

typedef struct {
    char     name[FEATURE_NAME_CAP];
    uint8_t  default_enabled;
    uint8_t  cur_state;
    uint8_t  resolved;
    uint8_t  flags;
    uint8_t  rollout_percent;
    uint8_t  owner;
    uint8_t  source;
    uint8_t  used;
} feature_entry_t;

static feature_entry_t s_features[FEATURE_MAX];
static uint32_t        s_count;
static DEFINE_SPINLOCK(s_lock);

/* ---- helpers ------------------------------------------------------------ */

/* Returns 1 if name (measured length nl) is in a reserved namespace AND has at
 * least one character after the prefix. Bounded by nl -- never reads past the
 * string, and rejects an anonymous prefix-only flag like "feature.". */
static int name_in_namespace(const char *name, uint32_t nl)
{
    static const char p1[] = "feature.";      /* len 8 */
    static const char p2[] = "experiment.";   /* len 11 */
    if (nl > 8) {
        uint32_t i = 0; for (; i < 8; i++) if (name[i] != p1[i]) break;
        if (i == 8) return 1;
    }
    if (nl > 11) {
        uint32_t i = 0; for (; i < 11; i++) if (name[i] != p2[i]) break;
        if (i == 11) return 1;
    }
    return 0;
}

static int is_experiment(const char *name)
{
    static const char p2[] = "experiment.";
    uint32_t i;
    for (i = 0; p2[i]; i++) if (name[i] != p2[i]) return 0;
    return 1;
}

static int names_eq(const char *a, const char *b)
{
    for (uint32_t i = 0; i < FEATURE_NAME_CAP; i++) {
        if (a[i] != b[i]) return 0;
        if (a[i] == 0)    return 1;
    }
    return 0;
}

static uint32_t name_len(const char *s)
{
    uint32_t i = 0;
    while (i < FEATURE_NAME_CAP && s[i]) i++;
    return i;
}

static int find_index(const char *name)
{
    for (uint32_t i = 0; i < FEATURE_MAX; i++)
        if (s_features[i].used && names_eq(s_features[i].name, name))
            return (int)i;
    return -1;
}

/* Short name for audit logging: the part after the namespace prefix. */
static const char *short_name(const char *name)
{
    const char *dot = name;
    while (*dot && *dot != '.') dot++;
    return (*dot == '.') ? dot + 1 : name;
}

/* Parse a boolean token of vlen bytes. Returns 1 if recognized (sets *out),
 * 0 otherwise. */
static int parse_bool(const char *v, uint32_t vlen, int *out)
{
    struct { const char *s; int val; } tab[] = {
        {"on",1},{"1",1},{"true",1},{"yes",1},{"enabled",1},
        {"off",0},{"0",0},{"false",0},{"no",0},{"disabled",0},
    };
    for (uint32_t t = 0; t < sizeof(tab)/sizeof(tab[0]); t++) {
        const char *s = tab[t].s;
        uint32_t sl = 0; while (s[sl]) sl++;
        if (sl != vlen) continue;
        uint32_t i = 0; for (; i < vlen; i++) if (s[i] != v[i]) break;
        if (i == vlen) { *out = tab[t].val; return 1; }
    }
    return 0;
}

/* Scan the raw kernel command line for "<fullname>=value". The boot-argument
 * schema parser only stores known + unknown KEY names (not dynamic key=value),
 * so the feature subsystem owns this bounded scan. Returns 1 if found + parsed
 * (sets *out_enabled), 0 otherwise. */
static int cmdline_override(const char *fullname, int *out_enabled)
{
    const char *cl = g_boot_info.config.cmdline;
    uint32_t max = BOOT_CONF_CMDLINE_MAX;
    uint32_t nl = name_len(fullname);
    uint32_t i = 0;
    while (i < max && cl[i]) {
        while (i < max && cl[i] == ' ') i++;          /* skip separators */
        uint32_t ts = i;
        while (i < max && cl[i] && cl[i] != ' ') i++;  /* token [ts,i) */
        uint32_t tlen = i - ts;
        if (tlen > nl + 1 && cl[ts + nl] == '=') {
            uint32_t k = 0;
            for (; k < nl; k++) if (cl[ts + k] != fullname[k]) break;
            if (k == nl) {
                int v;
                if (parse_bool(cl + ts + nl + 1, tlen - nl - 1, &v)) {
                    *out_enabled = v;
                    return 1;
                }
            }
        }
    }
    return 0;
}

/* Stable cohort bucket [0,99] from sha256(machine_uuid || name). Returns 1 and
 * sets *bucket when a valid machine UUID exists; returns 0 when no stable ID is
 * available (caller falls back to the default -- never hash a sentinel UUID). */
static int cohort_bucket(const char *name, uint32_t *bucket)
{
    uint8_t uuid[16];
    if (!smbios_get_system_uuid(uuid))
        return 0;
    uint8_t buf[16 + FEATURE_NAME_CAP];
    uint32_t nl = name_len(name);
    for (uint32_t i = 0; i < 16; i++) buf[i] = uuid[i];
    for (uint32_t i = 0; i < nl; i++) buf[16 + i] = (uint8_t)name[i];
    uint8_t dg[SHA256_DIGEST_LEN];
    sha256(buf, 16 + nl, dg);
    uint32_t h = (uint32_t)dg[0] | ((uint32_t)dg[1] << 8) |
                 ((uint32_t)dg[2] << 16) | ((uint32_t)dg[3] << 24);
    *bucket = h % 100u;
    return 1;
}

/* A FEATURE_SECURITY flag may be DISABLED only when Secure Boot is known-off:
 * the state was readable AND inactive. Active or unreadable/unknown -> refuse
 * (fail closed). secure_boot_enabled is set only on a successful read, and the
 * UNREADABLE degraded-trust bit marks an unknown state. */
static int security_disable_allowed(void)
{
    return (g_boot_info.secure_boot_enabled == 0) &&
           !(g_boot_info.degraded_trust_flags &
             BOOT_DEGRADED_TRUST_SECURE_BOOT_UNREADABLE);
}

/* ---- public API --------------------------------------------------------- */

int kernel_feature_register(const char *name, int default_enabled,
                            uint16_t flags, uint32_t rollout_percent,
                            uint8_t owner)
{
    if (!name) return -1;
    uint32_t nl = name_len(name);
    if (nl == 0 || nl >= FEATURE_NAME_CAP) return -1;
    if (!name_in_namespace(name, nl))      return -1;
    if (rollout_percent > 100)             return -1;  /* checked BEFORE truncation */

    uint64_t irq;
    spin_lock_irqsave(&s_lock, &irq);
    if (find_index(name) >= 0) { spin_unlock_irqrestore(&s_lock, irq); return -1; }
    int slot = -1;
    for (uint32_t i = 0; i < FEATURE_MAX; i++)
        if (!s_features[i].used) { slot = (int)i; break; }
    if (slot < 0) { spin_unlock_irqrestore(&s_lock, irq); return -1; }

    feature_entry_t *f = &s_features[slot];
    for (uint32_t i = 0; i < FEATURE_NAME_CAP; i++) f->name[i] = 0;
    for (uint32_t i = 0; i < nl; i++) f->name[i] = name[i];
    f->default_enabled = (uint8_t)(default_enabled ? 1 : 0);
    f->flags = (uint8_t)flags;
    if (is_experiment(name)) f->flags |= FEATURE_EXPERIMENT;
    f->rollout_percent = (uint8_t)rollout_percent;   /* range-checked above */
    f->owner = owner;
    f->cur_state = 0; f->resolved = 0; f->source = 0;
    f->used = 1;
    s_count++;
    spin_unlock_irqrestore(&s_lock, irq);
    return 0;
}

int kernel_feature_enabled(const char *name)
{
    if (!name) return 0;

    uint64_t irq;
    spin_lock_irqsave(&s_lock, &irq);
    int idx = find_index(name);
    if (idx < 0) { spin_unlock_irqrestore(&s_lock, irq); return 0; }
    feature_entry_t *f = &s_features[idx];
    if (f->resolved) {
        int v = f->cur_state;
        spin_unlock_irqrestore(&s_lock, irq);
        return v;
    }

    int val; uint8_t src;
    int ov;
    if (f->flags & FEATURE_LOCKED) {
        /* Locked: no override or cohort provider is honored -- the registered
         * default is authoritative (still subject to the Secure Boot guard). */
        val = f->default_enabled; src = (uint8_t)FEATURE_SRC_DEFAULT;
    } else if (cmdline_override(f->name, &ov)) {
        val = ov; src = (uint8_t)FEATURE_SRC_CMDLINE;
    } else if (f->rollout_percent > 0) {
        uint32_t b;
        if (cohort_bucket(f->name, &b)) {
            val = (b < f->rollout_percent) ? 1 : 0; src = (uint8_t)FEATURE_SRC_COHORT;
        } else {
            val = f->default_enabled; src = (uint8_t)FEATURE_SRC_DEFAULT;
        }
    } else {
        val = f->default_enabled; src = (uint8_t)FEATURE_SRC_DEFAULT;
    }

    /* Secure Boot guard: refuse disabling a security feature unless Secure Boot
     * is known-off. Fail closed (keep enabled) otherwise. */
    int blocked = 0;
    if ((f->flags & FEATURE_SECURITY) && val == 0 && !security_disable_allowed()) {
        val = 1; src = (uint8_t)FEATURE_SRC_SB_GUARD; blocked = 1;
    }

    f->cur_state = (uint8_t)val; f->source = src; f->resolved = 1;
    /* Snapshot what we need to log outside the lock. */
    char shortbuf[FEATURE_NAME_CAP];
    const char *sn = short_name(f->name);
    uint32_t sl = 0; while (sn[sl] && sl < FEATURE_NAME_CAP - 1) { shortbuf[sl] = sn[sl]; sl++; }
    shortbuf[sl] = 0;
    spin_unlock_irqrestore(&s_lock, irq);

    if (blocked)
        klog(LOG_WARN, "CONF", "[CONF] secure feature override blocked: %s", shortbuf);
    else if (src == (uint8_t)FEATURE_SRC_CMDLINE)
        klog(LOG_INFO, "CONF", "[CONF] feature %s forced %s", shortbuf, val ? "on" : "off");
    return val;
}

uint32_t kernel_feature_count(void)
{
    uint64_t irq;
    spin_lock_irqsave(&s_lock, &irq);
    uint32_t c = s_count;
    spin_unlock_irqrestore(&s_lock, irq);
    return c;
}

uint32_t kernel_feature_dump(feature_snapshot_t *out, uint32_t max_rows)
{
    if (!out || max_rows == 0) return 0;
    uint32_t n = 0;
    uint64_t irq;
    spin_lock_irqsave(&s_lock, &irq);
    for (uint32_t i = 0; i < FEATURE_MAX && n < max_rows; i++) {
        if (!s_features[i].used) continue;
        feature_entry_t *f = &s_features[i];
        feature_snapshot_t *r = &out[n++];
        for (uint32_t j = 0; j < FEATURE_NAME_CAP; j++) r->name[j] = f->name[j];
        r->default_enabled = f->default_enabled;
        r->resolved_state = f->cur_state;
        r->resolved = f->resolved;
        r->flags = f->flags;
        r->rollout_percent = f->rollout_percent;
        r->owner = f->owner;
        r->source = f->source;
        r->_pad = 0;
    }
    spin_unlock_irqrestore(&s_lock, irq);
    return n;
}

void kernel_features_register_core(void)
{
    static int registered;
    if (registered) return;
    registered = 1;

    /* kpti: kernel page-table isolation. Security-critical -- cannot be turned
     * off under active/unknown Secure Boot. Defaults on. */
    kernel_feature_register("feature.kpti", 1, FEATURE_SECURITY, 0, 0);
    /* debug_menu: developer debug surface. Off by default. */
    kernel_feature_register("feature.debug_menu", 0, 0, 0, 0);

    klog(LOG_INFO, "CONF", "[CONF] feature registry: %u core features",
         kernel_feature_count());
}
