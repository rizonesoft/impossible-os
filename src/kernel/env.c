/* ============================================================================
 * env.c -- Per-process environment variable storage & kernel API (TODO-22)
 *
 * Backing store: task->environ, a NULL-terminated array of "KEY=VALUE" UTF-8
 * strings, serialized by task->environ_lock (a mutex -- see env.h for why not a
 * spinlock). All mutation is allocate-before-swap so an out-of-memory failure
 * never destroys a live value. Value strings up to ENV_STR_KMALLOC_MAX use the
 * heap; larger ones are page-backed (CLAUDE.md: "kmalloc() for <= 4 KB only").
 *
 * This is arch-neutral kernel code (no gdt/idt/msr/cpuid includes).
 * ============================================================================ */

#include "kernel/env.h"
#include "kernel/sched/task.h"
#include "kernel/sched/mutex.h"
#include "kernel/mm/heap.h"
#include "kernel/mm/pmm.h"
#include "kernel/smp.h"          /* smp_cpu_count() */
#include "kernel/boot_init.h"    /* kernel_subsystem_ready(), SUBSYS_REGISTRY */
#include "kernel/klog.h"         /* klog() */
#include "kernel/nt/nls_cp.h"    /* nls_cp_utf8_to_utf16 (env block build) */
#include "registry.h"            /* RegOpenKeyEx / RegEnumValue / RegGet* */

#define ENV_PAGE_SIZE 4096u

/* --- Local freestanding string helpers (registry.c-style; no libc) -------- */

static uint32_t env_strlen(const char *s)
{
    uint32_t n = 0;
    while (s[n])
        n++;
    return n;
}

/* Bounded length: scan at most `maxlen` bytes. Returns the length if a NUL is
 * found within [0, maxlen), else `maxlen` (caller treats == maxlen as
 * "missing terminator / too long" and rejects before any allocation or pointer
 * arithmetic). Used on UNTRUSTED lengths (a caller-supplied name/value that may
 * lack a terminator) so a missing NUL near an unmapped page cannot drive an
 * unbounded kernel read. Stored entry strings are NUL-terminated by
 * construction and use the unbounded env_strlen. */
static uint32_t env_strnlen(const char *s, uint32_t maxlen)
{
    uint32_t n = 0;
    while (n < maxlen && s[n])
        n++;
    return n;
}

/* ASCII case fold. Environment names are case-insensitive; this folds A-Z only.
 * Locale-aware Unicode case folding for non-ASCII names is an NLS concern (see
 * the NLS/locale TODO), out of scope for this base storage layer. */
static char env_lc(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

/* Length of the "KEY" portion of a stored "KEY=VALUE" entry: the byte offset of
 * the first '=' (every stored entry has one). The value pointer is derived from
 * the STORED entry, never from a query's byte length -- the two can differ once a
 * case-fold equates names of unequal encoded length, which would make
 * "entry + querylen + 1" point past the '=' and read/overrun the value. */
static uint32_t env_entry_keylen(const char *entry)
{
    /* A hidden "=X:" drive variable's name BEGINS with '=' (entry "=C:=value");
     * that leading '=' is part of the name, so start the separator scan one byte
     * in. For an ordinary "KEY=VALUE" entry (no leading '='), this is the plain
     * first-'=' offset. */
    uint32_t i = (entry[0] == '=') ? 1u : 0u;
    while (entry[i] != '\0' && entry[i] != '=')
        i++;
    return i;
}

/* Borrowed pointer to the VALUE portion of a stored "KEY=VALUE" entry (past the
 * '='). If the entry has no '=' (never happens for a well-formed store) this
 * points at the terminating NUL, i.e. an empty value. */
static const char *env_entry_value(const char *entry)
{
    uint32_t k = env_entry_keylen(entry);
    return (entry[k] == '=') ? (entry + k + 1) : (entry + k);
}

/* Total order over environment-variable NAMES, case-insensitive with an
 * ASCII-only fold (A-Z <-> a-z); all other bytes compare ordinally as unsigned.
 * Returns <0 / 0 / >0. Shorter sorts before a longer name that shares its prefix
 * ("A" < "AA"). This is the SINGLE authority for both identity (== 0) and sort
 * order, so binary search is sound (equal keys are comparator-equal + adjacent).
 *
 * ASCII-ONLY BY DESIGN: the full-BMP NLS upcase table (nls_upcase_char, U+0100+)
 * is disk-backed, version-dependent, and ephemeral -- unsafe as the PERSISTENT
 * identity/order of a stored identifier, and its corpus is not yet shipped
 * (owned by TODO-13). A byte-wise ASCII fold is stable, compiled-in, and
 * byte-length-preserving (case-equal names keep identical UTF-8 length, which
 * keeps env_entry_value offsets valid), and never decodes code points (so a
 * malformed UTF-8 name from an exec envp cannot collapse into a different one).
 * Non-ASCII names therefore compare case-SENSITIVELY; full NLS-aware folding is
 * a deferred upgrade tracked in TODO-13. */
static int env_name_cmp(const char *a, uint32_t alen,
                        const char *b, uint32_t blen)
{
    uint32_t n = (alen < blen) ? alen : blen;
    uint32_t i;
    for (i = 0; i < n; i++) {
        unsigned char ca = (unsigned char)env_lc(a[i]);
        unsigned char cb = (unsigned char)env_lc(b[i]);
        if (ca != cb)
            return (ca < cb) ? -1 : 1;
    }
    if (alen == blen)
        return 0;
    return (alen < blen) ? -1 : 1;
}

/* Compare a stored "KEY=VALUE" entry's KEY against `name` (namelen bytes) via the
 * single name ordering. Returns <0 / 0 / >0; == 0 is the case-insensitive name
 * match that binary search and de-duplication rely on (derived from the same
 * order, so identity can never disagree with the sort). */
static int env_entry_name_cmp(const char *entry, const char *name,
                              uint32_t namelen)
{
    return env_name_cmp(entry, env_entry_keylen(entry), name, namelen);
}

/* Compare two stored "KEY=VALUE" entries by KEY via the single ordering. */
static int env_entry_cmp(const char *ea, const char *eb)
{
    return env_name_cmp(ea, env_entry_keylen(ea), eb, env_entry_keylen(eb));
}

/* STABLE bottom-up merge sort of a pointer array by entry key (needs an n-slot
 * scratch). Stable so that among equal keys the LAST input occurrence stays last
 * -- the adjacent-run de-duplication that follows can then keep the last-wins
 * winner in one linear pass. O(n log n) comparisons vs the O(n^2) an insertion
 * sort would spend on adversarial reverse-sorted, long-common-prefix envp. */
static void env_ptr_msort(char **arr, uint32_t n, char **scratch)
{
    uint32_t width;
    for (width = 1u; width < n; width *= 2u) {
        uint32_t i;
        for (i = 0; i < n; i += 2u * width) {
            uint32_t lo = i;
            uint32_t mid = (i + width < n) ? (i + width) : n;
            uint32_t hi = (i + 2u * width < n) ? (i + 2u * width) : n;
            uint32_t a = lo, b = mid, k = lo;
            while (a < mid && b < hi)
                scratch[k++] = (env_entry_cmp(arr[a], arr[b]) <= 0)
                             ? arr[a++] : arr[b++];   /* <=0 keeps left -> stable */
            while (a < mid) scratch[k++] = arr[a++];
            while (b < hi)  scratch[k++] = arr[b++];
        }
        for (i = 0; i < n; i++)
            arr[i] = scratch[i];
    }
}

/* True iff `name` (namelen bytes) is EXACTLY a hidden drive-variable name:
 * '=' + one ASCII letter + ':' (3 bytes, e.g. "=C:"). This is the ONLY name
 * form permitted to contain '='; every other '='-bearing name is invalid. */
static int env_name_is_drive_cwd(const char *name, uint32_t namelen)
{
    return namelen == 3u && name[0] == '=' &&
           ((name[1] >= 'A' && name[1] <= 'Z') ||
            (name[1] >= 'a' && name[1] <= 'z')) &&
           name[2] == ':';
}

/* Classify a to-be-set variable name and return its length via `*out_len`.
 * Distinguishes ENV_ERR_TOOLONG (length > ENV_NAME_MAX) from ENV_ERR_INVAL
 * (NULL, empty, or contains '=' -- the separator). The sole '='-containing name
 * accepted is the hidden "=X:" drive-cwd form (TODO-22 s12); any OTHER
 * leading-'=' or embedded-'=' name is ENV_ERR_INVAL. Returns ENV_OK when the
 * name is storable. */
static int env_name_classify(const char *name, uint32_t *out_len)
{
    uint32_t n;
    if (!name || !name[0])
        return ENV_ERR_INVAL;
    /* Bounded scan: reading ENV_NAME_MAX + 1 bytes is enough to prove a name is
     * over-limit -- a full scan of the first 257 bytes with no NUL means the
     * name is at least 257 chars (> ENV_NAME_MAX), so cap the read there and do
     * not touch a 258th byte on a missing-terminator name. */
    n = env_strnlen(name, ENV_NAME_MAX + 1u);
    if (n > ENV_NAME_MAX)
        return ENV_ERR_TOOLONG;
    if (name[0] == '=') {
        /* Only the exact "=X:" hidden drive-cwd shape is a legal '='-name. */
        if (env_name_is_drive_cwd(name, n)) {
            *out_len = n;
            return ENV_OK;
        }
        return ENV_ERR_INVAL;
    }
    {
        uint32_t k;
        for (k = 0; k < n; k++)
            if (name[k] == '=')
                return ENV_ERR_INVAL;
    }
    *out_len = n;
    return ENV_OK;
}

/* Key (name) span of a RAW untrusted "name=value" entry from an exec envp / a
 * CreateProcess block, honoring a leading '=' that belongs to a hidden "=X:"
 * drive variable. On a well-formed name sets *sep to the separator offset
 * (== key length) and returns 0; returns -1 when the name is empty, has no
 * separator within `elen`, or is a leading-'=' name that is not exactly "=X:".
 * Mirrors env_name_classify's '=' rule so env_adopt_block's per-entry filter
 * admits the same names env_set does. */
static int env_entry_key_span(const char *e, uint32_t elen, uint32_t *sep)
{
    uint32_t k = (e[0] == '=') ? 1u : 0u;
    while (k < elen && e[k] != '=')
        k++;
    if (k >= elen)
        return -1;                       /* no separator -> not "name=value" */
    if (e[0] == '=') {
        if (!env_name_is_drive_cwd(e, k)) /* leading '=' but not exactly "=X:" */
            return -1;
    } else if (k == 0) {
        return -1;                       /* empty key */
    }
    *sep = k;
    return 0;
}

/* --- String allocation: heap up to 4 KiB, page-backed PMM above ----------- */

/* Allocate `n` bytes (including NUL) for an env string. The allocator kind is a
 * pure function of `n`, so env_str_free recovers it from strlen()+1 with no
 * per-string bookkeeping. Returns NULL on failure. */
static char *env_str_alloc(uint32_t n)
{
    if (n <= ENV_STR_KMALLOC_MAX)
        return (char *)kmalloc(n);
    {
        uint64_t frames = (n + (ENV_PAGE_SIZE - 1)) / ENV_PAGE_SIZE;
        uintptr_t phys = pmm_alloc_contiguous(frames);   /* identity-mapped */
        return (char *)phys;                             /* 0 -> NULL */
    }
}

/* Free a string allocated by env_str_alloc. `n` MUST be the same byte count
 * (strlen(str)+1) that was passed to env_str_alloc. */
static void env_str_free(char *p, uint32_t n)
{
    if (!p)
        return;
    if (n <= ENV_STR_KMALLOC_MAX) {
        kfree(p);
    } else {
        uint64_t frames = (n + (ENV_PAGE_SIZE - 1)) / ENV_PAGE_SIZE;
        uint64_t f;
        uintptr_t base = (uintptr_t)p;
        for (f = 0; f < frames; f++)
            pmm_free_frame(base + f * ENV_PAGE_SIZE);
    }
}

/* Duplicate a NUL-terminated string via env_str_alloc. */
static char *env_strdup(const char *s)
{
    uint32_t n = env_strlen(s) + 1;
    char *p = env_str_alloc(n);
    uint32_t i;
    if (!p)
        return NULL;
    for (i = 0; i < n; i++)
        p[i] = s[i];
    return p;
}

/* Build a "name=value\0" string. Returns allocated buffer or NULL. */
static char *env_make_entry(const char *name, uint32_t namelen,
                            const char *value, uint32_t vallen)
{
    uint32_t n = namelen + 1u + vallen + 1u;   /* name + '=' + value + NUL */
    char *p = env_str_alloc(n);
    uint32_t i, j = 0;
    if (!p)
        return NULL;
    for (i = 0; i < namelen; i++)
        p[j++] = name[i];
    p[j++] = '=';
    for (i = 0; i < vallen; i++)
        p[j++] = value[i];
    p[j] = '\0';
    return p;
}

/* Binary search environ[] (kept sorted by env_name_cmp) for `name`. On a hit
 * returns the index; on a miss returns -1 and, when `pos` is non-NULL, sets *pos
 * to the insertion index that keeps the array sorted. environ[0..count) has no
 * NULL holes (every mutator keeps it dense), so the search needs no hole checks.
 * Caller MUST hold t->environ_lock. */
static int env_bsearch(struct task *t, const char *name, uint32_t namelen,
                       uint32_t *pos)
{
    uint32_t lo = 0, hi;
    if (!t->environ) {
        if (pos)
            *pos = 0;
        return -1;
    }
    hi = t->environ_count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        int c = env_entry_name_cmp(t->environ[mid], name, namelen);
        if (c == 0)
            return (int)mid;
        if (c < 0)            /* stored entry sorts before name -> go right */
            lo = mid + 1u;
        else
            hi = mid;
    }
    if (pos)
        *pos = lo;
    return -1;
}

/* Index of the entry matching `name` (case-insensitive), or -1. The caller MUST
 * hold t->environ_lock. */
static int env_find_index(struct task *t, const char *name, uint32_t namelen)
{
    return env_bsearch(t, name, namelen, (uint32_t *)0);
}

/* --- Public API ----------------------------------------------------------- */

void env_lock(struct task *t)
{
    if (t)
        mutex_lock(&t->environ_lock);
}

void env_unlock(struct task *t)
{
    if (t)
        mutex_unlock(&t->environ_lock);
}

const char *env_peek_locked(struct task *t, const char *name)
{
    uint32_t namelen;
    int idx;
    if (!t)
        return NULL;
    if (env_name_classify(name, &namelen) != ENV_OK)   /* same validation as env_set */
        return NULL;
    idx = env_find_index(t, name, namelen);
    if (idx < 0)
        return NULL;
    return env_entry_value(t->environ[idx]);   /* value past the stored '=' */
}

int env_get_copy(struct task *t, const char *name, char *out, uint32_t out_size)
{
    uint32_t namelen;
    int idx;
    int ret;

    if (!t || !out || out_size == 0)
        return ENV_ERR_INVAL;
    out[0] = '\0';
    {
        int nrc = env_name_classify(name, &namelen);   /* INVAL vs TOOLONG, same as env_set */
        if (nrc != ENV_OK)
            return nrc;
    }

    mutex_lock(&t->environ_lock);
    idx = env_find_index(t, name, namelen);
    if (idx < 0) {
        mutex_unlock(&t->environ_lock);
        return ENV_ERR_NOTFOUND;
    }
    {
        const char *val = env_entry_value(t->environ[idx]);   /* past stored '=' */
        uint32_t vlen = env_strlen(val);
        uint32_t copy = (vlen < out_size) ? vlen : (out_size - 1);
        uint32_t i;
        for (i = 0; i < copy; i++)
            out[i] = val[i];
        out[copy] = '\0';
        ret = (int)vlen;   /* full length even when truncated */
    }
    mutex_unlock(&t->environ_lock);
    return ret;
}

/* Total byte size of the contiguous environment block ("name=value\0"... + one
 * trailing NUL) for the DoS-guard cap. O(1): read from the cached per-entry-bytes
 * sum that every mutator maintains under the lock, so even a near-1 MiB
 * environment does not pay a full-block scan on each env_set (which would stall
 * sibling readers under environ_lock). Caller MUST hold t->environ_lock. */
static uint32_t env_block_bytes_locked(struct task *t)
{
    return t->environ_bytes + 1u;          /* + the trailing block terminator NUL */
}

/* Log the ENV_BLOCK_WARN crossing (block grew from below to at/above the warn
 * threshold). Call AFTER dropping environ_lock -- never klog under the mutex. */
static void env_warn_block_crossing(struct task *t, uint32_t old_total,
                                    uint32_t new_total)
{
    if (old_total < ENV_BLOCK_WARN && new_total >= ENV_BLOCK_WARN)
        klog(LOG_WARN, "env",
             "environment block for pid %u exceeded %u KiB",
             (uint64_t)t->pid, (uint64_t)(ENV_BLOCK_WARN / 1024u));
}

int env_set(struct task *t, const char *name, const char *value)
{
    uint32_t namelen, vallen, entlen;
    uint32_t pos = 0;
    uint32_t old_total, new_total;
    char *newent;
    int idx;

    if (!t || !value)
        return ENV_ERR_INVAL;
    {
        int nrc = env_name_classify(name, &namelen);   /* INVAL vs TOOLONG */
        if (nrc != ENV_OK)
            return nrc;
    }
    vallen = env_strnlen(value, ENV_VALUE_MAX + 1u);   /* bounded: caps the untrusted read */
    if (vallen > ENV_VALUE_MAX)
        return ENV_ERR_TOOLONG;

    /* Build the replacement entry BEFORE touching the array or freeing the old
     * value: an allocation failure here leaves the prior state untouched. */
    entlen = namelen + 1u + vallen + 1u;
    newent = env_make_entry(name, namelen, value, vallen);
    if (!newent)
        return ENV_ERR_NOMEM;

    mutex_lock(&t->environ_lock);

    /* Block-size DoS guard: reject a set that would push the total block past
     * ENV_BLOCK_MAX (O(1) via the cached byte total). */
    old_total = env_block_bytes_locked(t);
    idx = env_bsearch(t, name, namelen, &pos);
    if (idx >= 0)
        new_total = old_total - (env_strlen(t->environ[idx]) + 1u) + entlen;
    else
        new_total = old_total + entlen;
    if (new_total > ENV_BLOCK_MAX) {
        mutex_unlock(&t->environ_lock);
        env_str_free(newent, entlen);
        return ENV_ERR_NOSPACE;
    }

    if (idx >= 0) {
        /* Replace in place: the name is unchanged, so the sorted position does
         * not move -- swap the pointer, no shift. */
        char *old = t->environ[idx];
        uint32_t oldn = env_strlen(old) + 1;
        t->environ[idx] = newent;
        t->environ_bytes += entlen - oldn;     /* cached total: -old +new */
        mutex_unlock(&t->environ_lock);
        env_str_free(old, oldn);           /* free old outside the lock */
    } else {
        if (t->environ_count >= ENV_MAX_ENTRIES) {
            mutex_unlock(&t->environ_lock);
            env_str_free(newent, entlen);
            return ENV_ERR_NOSPACE;
        }
        {
            uint32_t oldcnt = t->environ_count;
            uint32_t newcap = oldcnt + 2u; /* +1 new entry, +1 NULL terminator */
            char **arr = (char **)krealloc(t->environ, newcap * sizeof(char *));
            uint32_t i;
            if (!arr) {
                mutex_unlock(&t->environ_lock);
                env_str_free(newent, entlen);
                return ENV_ERR_NOMEM;
            }
            t->environ = arr;
            /* Open a slot at the sorted insertion index by shifting the tail up
             * one; keep the array sorted so binary search stays valid. */
            for (i = oldcnt; i > pos; i--)
                arr[i] = arr[i - 1];
            arr[pos] = newent;
            arr[oldcnt + 1] = NULL;        /* new terminator */
            t->environ_count = oldcnt + 1;
            t->environ_bytes += entlen;        /* cached total: +new entry */
        }
        mutex_unlock(&t->environ_lock);
    }

    env_warn_block_crossing(t, old_total, new_total);
    return ENV_OK;
}

int env_unset(struct task *t, const char *name)
{
    uint32_t namelen;
    int idx;
    char *victim;
    uint32_t vn;

    if (!t)
        return ENV_ERR_INVAL;
    {
        int nrc = env_name_classify(name, &namelen);   /* INVAL vs TOOLONG, same as env_set */
        if (nrc != ENV_OK)
            return nrc;
    }

    mutex_lock(&t->environ_lock);
    idx = env_find_index(t, name, namelen);
    if (idx < 0) {
        mutex_unlock(&t->environ_lock);
        return ENV_ERR_NOTFOUND;
    }
    victim = t->environ[idx];
    vn = env_strlen(victim) + 1;
    {
        uint32_t i;
        for (i = (uint32_t)idx; i + 1 < t->environ_count; i++)
            t->environ[i] = t->environ[i + 1];
        t->environ[t->environ_count - 1] = NULL;   /* new terminator */
        t->environ_count--;
        t->environ_bytes -= vn;                    /* cached total: drop the victim */
    }
    mutex_unlock(&t->environ_lock);
    env_str_free(victim, vn);
    return ENV_OK;
}

int env_copy(struct task *dst, const struct task *src)
{
    struct task *s = (struct task *)src;   /* mutex_lock takes non-const; no logical mutation */
    char **arr;
    uint32_t count, i;

    if (!dst || !src)
        return ENV_ERR_INVAL;

    /* dst is an UNPUBLISHED child (not yet scheduled) -- its lock is not taken.
     * Snapshot src under src's lock so a sibling env_set cannot tear the copy. */
    dst->environ = NULL;
    dst->environ_count = 0;
    dst->environ_bytes = 0;

    mutex_lock(&s->environ_lock);
    count = s->environ_count;
    if (count == 0 || !s->environ) {
        mutex_unlock(&s->environ_lock);
        return ENV_OK;                     /* empty src -> empty dst */
    }
    arr = (char **)kmalloc((count + 1) * sizeof(char *));
    if (!arr) {
        mutex_unlock(&s->environ_lock);
        return ENV_ERR_NOMEM;
    }
    for (i = 0; i <= count; i++)
        arr[i] = NULL;                     /* pre-null (incl terminator) for clean unwind */
    for (i = 0; i < count; i++) {
        if (!s->environ[i])
            continue;
        arr[i] = env_strdup(s->environ[i]);
        if (!arr[i]) {
            uint32_t j;
            for (j = 0; j < i; j++)
                if (arr[j])
                    env_str_free(arr[j], env_strlen(arr[j]) + 1);
            kfree(arr);
            mutex_unlock(&s->environ_lock);
            return ENV_ERR_NOMEM;
        }
    }
    dst->environ = arr;
    dst->environ_count = count;
    dst->environ_bytes = s->environ_bytes;   /* exact deep copy -> same block bytes */
    mutex_unlock(&s->environ_lock);
    return ENV_OK;
}

void env_free(struct task *t)
{
    uint32_t i;
    int a;

    if (!t)
        return;

    /* Called only at the task_cleanup reap barrier: the task is TASK_DEAD and
     * no thread of it remains runnable, so no lock is taken (parallels
     * ob_handle_table_destroy). */
    if (t->environ) {
        for (i = 0; i < t->environ_count; i++)
            if (t->environ[i])
                env_str_free(t->environ[i], env_strlen(t->environ[i]) + 1);
        kfree(t->environ);
        t->environ = NULL;
    }
    t->environ_count = 0;
    t->environ_bytes = 0;

    /* argv strings are allocated via the same env string allocator by
     * task_set_argv (the argv-array feature); NULL/argc==0 until that lands. */
    if (t->argv) {
        for (a = 0; a < t->argc; a++)
            if (t->argv[a])
                env_str_free(t->argv[a], env_strlen(t->argv[a]) + 1);
        kfree(t->argv);
        t->argv = NULL;
    }
    t->argc = 0;
}

/* ===========================================================================
 * argv array + exec argument handoff
 *
 * task_set_argv / env_adopt_block replace the process-wide argv / environ under
 * t->environ_lock, mirroring env_copy's "build the whole new array before
 * freeing the old one, unwind on failure" discipline so a partial allocation
 * never publishes and a failure leaves the prior value intact. Argv strings use
 * the SAME env string allocator as environ, so env_free reclaims them with
 * env_str_free(str, env_strlen(str)+1). Callers pass KERNEL-side arrays only;
 * the SYS_EXEC path copies the untrusted user vectors into a kernel snapshot
 * first, so nothing here ever dereferences a raw user pointer.
 * =========================================================================== */

int task_set_argv(struct task *t, int argc, const char *const *argv)
{
    char **arr;
    char **old_argv;
    int old_argc;
    int i;

    if (!t)
        return ENV_ERR_INVAL;

    /* Empty argv -> clear to the "no argv" state (frame builder falls back to
     * argc=1/name). Free any prior argv under the lock. */
    if (argc <= 0 || !argv) {
        mutex_lock(&t->environ_lock);
        old_argv = t->argv;
        old_argc = t->argc;
        t->argv = NULL;
        t->argc = 0;
        mutex_unlock(&t->environ_lock);
        if (old_argv) {
            for (i = 0; i < old_argc; i++)
                if (old_argv[i])
                    env_str_free(old_argv[i], env_strlen(old_argv[i]) + 1);
            kfree(old_argv);
        }
        return ENV_OK;
    }

    if ((uint32_t)argc > ARG_ARGC_MAX)
        return ENV_ERR_NOSPACE;

    /* Build the new (argc+1)-entry array with strings duplicated via the env
     * allocator. Pre-null (incl terminator) so a mid-loop failure unwinds
     * cleanly. Allocation under the mutex is legal (env.h: mutex, not spinlock). */
    arr = (char **)kmalloc(((uint32_t)argc + 1u) * sizeof(char *));
    if (!arr)
        return ENV_ERR_NOMEM;
    for (i = 0; i <= argc; i++)
        arr[i] = NULL;
    for (i = 0; i < argc; i++) {
        const char *s = argv[i] ? argv[i] : "";
        arr[i] = env_strdup(s);
        if (!arr[i]) {
            int j;
            for (j = 0; j < i; j++)
                if (arr[j])
                    env_str_free(arr[j], env_strlen(arr[j]) + 1);
            kfree(arr);
            return ENV_ERR_NOMEM;
        }
    }

    mutex_lock(&t->environ_lock);
    old_argv = t->argv;
    old_argc = t->argc;
    t->argv = NULL;          /* publish empty briefly, then the full array */
    t->argc = argc;
    t->argv = arr;
    mutex_unlock(&t->environ_lock);

    /* Free the prior argv outside the lock (we hold the only reference). */
    if (old_argv) {
        for (i = 0; i < old_argc; i++)
            if (old_argv[i])
                env_str_free(old_argv[i], env_strlen(old_argv[i]) + 1);
        kfree(old_argv);
    }
    return ENV_OK;
}

int env_adopt_block(struct task *t, const char *const *entries, uint32_t count)
{
    char **arr;
    char **old_env;
    uint32_t old_count;
    uint32_t n = 0;          /* count of accepted (well-formed) entries */
    uint32_t new_bytes = 0;  /* SUM(strlen(entry)+1) of the published set */
    uint32_t old_total = 0;  /* prior block bytes (for the warn crossing) */
    uint32_t i;

    if (!t)
        return ENV_ERR_INVAL;

    /* Over-cap is a hard error, checked BEFORE the clear path so a malformed
     * (entries==NULL, count>cap) call is rejected uniformly rather than silently
     * clearing (SYS_EXEC already caps envc at this bound). */
    if (count > ENV_MAX_ENTRIES)
        return ENV_ERR_NOSPACE;

    /* Empty block (NULL entries or count 0) -> clear environ to empty. */
    if (!entries || count == 0) {
        mutex_lock(&t->environ_lock);
        old_env = t->environ;
        old_count = t->environ_count;
        t->environ = NULL;
        t->environ_count = 0;
        t->environ_bytes = 0;              /* clearing MUST reset the cached total */
        mutex_unlock(&t->environ_lock);
        if (old_env) {
            for (i = 0; i < old_count; i++)
                if (old_env[i])
                    env_str_free(old_env[i], env_strlen(old_env[i]) + 1);
            kfree(old_env);
        }
        return ENV_OK;
    }

    arr = (char **)kmalloc((count + 1u) * sizeof(char *));
    if (!arr)
        return ENV_ERR_NOMEM;
    for (i = 0; i <= count; i++)
        arr[i] = NULL;

    /* Deep-copy each well-formed "KEY=VALUE" entry (must contain '=', a non-empty
     * key, and fit ENV_NAME_MAX+ENV_VALUE_MAX). Malformed entries are skipped so
     * a bad envp cannot poison the whole exec. */
    for (i = 0; i < count; i++) {
        const char *e = entries[i];
        uint32_t elen, eq;
        if (!e)
            continue;
        elen = env_strnlen(e, ENV_NAME_MAX + 1u + ENV_VALUE_MAX + 1u);
        if (elen == 0 || elen > ENV_NAME_MAX + 1u + ENV_VALUE_MAX)
            continue;                       /* over-long or missing terminator */
        /* Split the name honoring a leading '=' hidden drive var; skip a name
         * that is empty, separator-less, or an illegal '='-name (only "=X:"). */
        if (env_entry_key_span(e, elen, &eq) != 0)
            continue;
        /* Enforce the name and value byte caps SEPARATELY, exactly as env_set
         * does: the combined-length check above still admits e.g. a 1-byte name
         * with a 32,769-byte value (total < NAME_MAX+1+VALUE_MAX), which env_set
         * would reject. Storing an over-ENV_VALUE_MAX value would let a reader
         * (NtQueryEnvironmentVariable) walk past its ENV_VALUE_MAX-sized buffer. */
        if (eq > ENV_NAME_MAX || (elen - eq - 1u) > ENV_VALUE_MAX)
            continue;                       /* name or value over its byte cap */
        arr[n] = env_strdup(e);
        if (!arr[n]) {
            uint32_t j;
            for (j = 0; j < n; j++)
                if (arr[j])
                    env_str_free(arr[j], env_strlen(arr[j]) + 1);
            kfree(arr);
            return ENV_ERR_NOMEM;
        }
        n++;
    }

    /* Sort the accepted entries by name (STABLE merge sort) so env_bsearch is
     * valid on the published array; a stable sort also lets the following linear
     * pass keep the LAST input occurrence of each name (last-wins, matching
     * env_set). This is O(n log n) + O(n) versus the O(n^2) an adversarial
     * 511-entry long-common-prefix envp would cost a pairwise-dedup + insertion
     * sort. */
    if (n > 1u) {
        char **scratch = (char **)kmalloc(n * sizeof(char *));
        uint32_t a;
        if (!scratch) {
            for (a = 0; a < n; a++)
                if (arr[a])
                    env_str_free(arr[a], env_strlen(arr[a]) + 1u);
            kfree(arr);
            return ENV_ERR_NOMEM;
        }
        env_ptr_msort(arr, n, scratch);
        kfree(scratch);
    }
    {
        /* Linear adjacent de-dup: within each equal-key run (now contiguous),
         * keep the last entry (last input occurrence, preserved by the stable
         * sort) and free the earlier ones. */
        uint32_t rd = 0, wr = 0, a;
        while (rd < n) {
            uint32_t run_end = rd + 1u;
            while (run_end < n && env_entry_cmp(arr[rd], arr[run_end]) == 0)
                run_end++;
            for (a = rd; a + 1u < run_end; a++)
                env_str_free(arr[a], env_strlen(arr[a]) + 1u);
            arr[wr++] = arr[run_end - 1u];
            rd = run_end;
        }
        for (a = wr; a <= count; a++)
            arr[a] = NULL;                  /* clear the freed tail + keep terminator */
        n = wr;
    }

    /* Enforce the per-process block-size DoS cap and compute the cached byte
     * total for the published environment. */
    for (i = 0; i < n; i++)
        new_bytes += env_strlen(arr[i]) + 1u;
    if (new_bytes + 1u > ENV_BLOCK_MAX) {
        for (i = 0; i < n; i++)
            if (arr[i])
                env_str_free(arr[i], env_strlen(arr[i]) + 1u);
        kfree(arr);
        return ENV_ERR_NOSPACE;
    }

    mutex_lock(&t->environ_lock);
    old_env = t->environ;
    old_count = t->environ_count;
    old_total = env_block_bytes_locked(t);   /* prior block bytes (old cached total) */
    t->environ = arr;
    t->environ_count = n;
    t->environ_bytes = new_bytes;
    mutex_unlock(&t->environ_lock);

    if (old_env) {
        for (i = 0; i < old_count; i++)
            if (old_env[i])
                env_str_free(old_env[i], env_strlen(old_env[i]) + 1);
        kfree(old_env);
    }
    env_warn_block_crossing(t, old_total, new_bytes + 1u);
    return ENV_OK;
}

/* Does an argument need quoting? Windows quotes an arg that is empty or contains
 * a space, tab, or double-quote. */
static int argv_arg_needs_quote(const char *a)
{
    uint32_t i;
    if (!a || !a[0])
        return 1;                           /* empty arg -> "" */
    for (i = 0; a[i]; i++)
        if (a[i] == ' ' || a[i] == '\t' || a[i] == '"')
            return 1;
    return 0;
}

uint32_t argv_to_cmdline(int argc, const char *const *argv, char *out, uint32_t max)
{
    uint32_t w = 0;                         /* bytes written (excl NUL) */
    int i;
    int trunc = 0;

    /* Append one byte, reserving the final slot for NUL; sets trunc on overflow. */
    #define CMDL_PUT(ch) do { \
            if (w + 1u < max) out[w++] = (char)(ch); else trunc = 1; \
        } while (0)

    if (max == 0)
        return 0;
    out[0] = '\0';
    if (argc <= 0 || !argv) {
        return 0;
    }

    /* Canonical Windows encode (exact inverse of CommandLineToArgvW): inside a
     * quoted arg a backslash run is doubled before the closing '"' and before an
     * interior '"' (with one extra to escape the quote); elsewhere it is
     * verbatim. An unquoted arg (no space/tab/quote by construction) emits its
     * backslashes verbatim. */
    for (i = 0; i < argc && !trunc; i++) {
        const char *a = argv[i] ? argv[i] : "";
        int quote = argv_arg_needs_quote(a);
        uint32_t j = 0;

        if (i > 0)
            CMDL_PUT(' ');
        if (quote)
            CMDL_PUT('"');

        for (;;) {
            uint32_t nbs = 0;
            uint32_t k;
            while (a[j] == '\\') { j++; nbs++; }
            if (a[j] == '\0') {
                uint32_t reps = quote ? nbs * 2u : nbs;
                for (k = 0; k < reps; k++) CMDL_PUT('\\');
                break;
            } else if (a[j] == '"') {
                for (k = 0; k < nbs * 2u + 1u; k++) CMDL_PUT('\\');
                CMDL_PUT('"');
                j++;
            } else {
                for (k = 0; k < nbs; k++) CMDL_PUT('\\');
                CMDL_PUT(a[j]);
                j++;
            }
        }
        if (quote)
            CMDL_PUT('"');
    }

    #undef CMDL_PUT
    out[w] = '\0';
    return trunc ? max : w;
}

uint32_t argv_frame_bytes(int argc, const char *const *argv)
{
    uint32_t str_qwords = 0;
    uint32_t qwords;
    int i;

    if (argc <= 0 || !argv)
        return 0;

    /* Match the task_exec frame builder EXACTLY (an undercount here permits a
     * user-stack overwrite): every string is qword-rounded on the stack, the
     * string area is padded to an even qword count, and one more qword pads the
     * pointer block when argc is even -- both keep &argc 16-byte aligned. */
    for (i = 0; i < argc; i++) {
        const char *s = argv[i] ? argv[i] : "";
        str_qwords += (env_strlen(s) + 1u + 7u) / 8u;   /* qword-rounded string */
    }
    qwords = str_qwords;
    if (str_qwords & 1u)
        qwords += 1u;                       /* string-area parity pad */
    if (((uint32_t)argc & 1u) == 0u)
        qwords += 1u;                       /* argc parity pad (argc even) */
    qwords += (uint32_t)argc + 1u;          /* argv pointer array (argc + NULL) */
    qwords += 1u;                           /* argc slot */
    return qwords * 8u;
}

/* ===========================================================================
 * %VAR% expansion (single-pass; cmd.exe / Win32 ExpandEnvironmentStrings)
 *
 * env_expand walks `input` once and substitutes each `%NAME%` with its value.
 * Substitution is single-pass by design: an expanded value that itself contains
 * `%OTHER%` is NOT recursively re-expanded, so there is no depth limit and no
 * infinite-loop risk (delayed `!VAR!` re-expansion is a separate cmd.exe mode).
 * =========================================================================== */

/* Append one byte to `out`, reserving the final slot for the NUL terminator.
 * Sets *trunc when the byte does not fit (out stays NUL-terminatable). */
static void env_exp_put(char *out, uint32_t max_len, uint32_t *pos, int *trunc,
                        char c)
{
    if (*pos + 1u < max_len)
        out[(*pos)++] = c;
    else
        *trunc = 1;
}

int env_expand(struct task *t, const char *input, char *output, uint32_t max_len)
{
    uint32_t in = 0, out = 0;
    int trunc = 0;

    if (!output || max_len == 0)
        return 0;
    if (!t || !input) {
        output[0] = '\0';
        return 0;
    }
    /* `input` and `output` must not overlap (Win32 ExpandEnvironmentStrings has
     * the same contract). Reject an alias and return 0 WITHOUT touching either
     * buffer -- writing output[0] here would corrupt the source (exact alias
     * erases input[0]; a partial overlap inserts a NUL into the source). This is
     * the one exception to the "output always NUL-terminated" guarantee. Measure
     * input (kernel-resident, NUL-terminated by contract) then compare ranges. */
    {
        uint32_t inlen = env_strlen(input);
        uintptr_t i0 = (uintptr_t)input, i1 = i0 + inlen + 1u;
        uintptr_t o0 = (uintptr_t)output, o1 = o0 + max_len;
        if (i0 < o1 && o0 < i1)
            return 0;                            /* overlap: leave both buffers intact */
    }
    output[0] = '\0';

    /* One lock for the whole walk: env_peek_locked returns a borrowed pointer
     * valid only under the lock, and we copy its bytes into `output` before any
     * unlock. No allocation happens under the lock (append is a plain byte
     * copy), so holding the env mutex across the walk is safe and gives a
     * coherent snapshot. Caller contract: must NOT already hold environ_lock. */
    env_lock(t);
    while (input[in]) {
        char c = input[in];
        if (c != '%') {
            env_exp_put(output, max_len, &out, &trunc, c);   /* verbatim */
            in++;
            continue;
        }
        /* Win32/ntdll ExpandEnvironmentStrings semantics: `%%` is NOT an escape.
         * An empty name (`%%`) is an unresolved variable, so both percent signs
         * are preserved verbatim by the empty-name -> literal branch below.
         * cmd.exe's `%%` -> `%` batch escape is a distinct shell mode (owned by
         * the pseudo-variable / delayed-expansion section), not this primitive. */
        /* Scan for the closing '%'. */
        {
            uint32_t j = in + 1;
            while (input[j] && input[j] != '%')
                j++;
            if (input[j] != '%') {
                /* No closing '%': copy the remainder verbatim and stop. */
                while (input[in]) {
                    env_exp_put(output, max_len, &out, &trunc, input[in]);
                    in++;
                }
                break;
            }
            {
                uint32_t namelen = j - (in + 1);
                const char *val = NULL;
                if (namelen >= 1 && namelen <= ENV_NAME_MAX) {
                    char nb[ENV_NAME_MAX + 1];
                    uint32_t k;
                    for (k = 0; k < namelen; k++)
                        nb[k] = input[in + 1 + k];
                    nb[namelen] = '\0';
                    val = env_peek_locked(t, nb);            /* borrowed, under lock */
                }
                if (val) {
                    uint32_t v = 0;
                    while (val[v]) {
                        env_exp_put(output, max_len, &out, &trunc, val[v]);
                        v++;
                    }
                } else {
                    /* Unknown or over-long name: copy the literal `%NAME%`. */
                    uint32_t p;
                    for (p = in; p <= j; p++)
                        env_exp_put(output, max_len, &out, &trunc, input[p]);
                }
                in = j + 1;
            }
        }
    }
    env_unlock(t);

    output[out] = '\0';
    return trunc ? (int)max_len : (int)out;
}

/* ===========================================================================
 * UTF-16 environment-block builder (for RtlExpandEnvironmentStrings_U's
 * NULL-Environment "calling process's own block" path)
 * =========================================================================== */

int env_build_block_utf16(struct task *t, uint16_t **out_block,
                          uint32_t *out_wchars, uint32_t max_wchars)
{
    uint32_t total, i, w, nentries;
    char *raw;
    uint16_t *blk;

    if (!t || !out_block || !out_wchars)
        return ENV_ERR_INVAL;
    *out_block = NULL;
    *out_wchars = 0;

    /* Snapshot under the env mutex: size, allocate, and convert all while holding
     * the lock so a sibling env_set/env_unset cannot change the block between the
     * sizing and conversion passes (both feed nls_cp_utf8_to_utf16 the same
     * bytes). Allocation under a MUTEX is permitted (env is a sleeping lock, not
     * a spinlock). NOTE: for a block above ENV_STR_KMALLOC_MAX, env_str_alloc
     * routes to pmm_alloc_contiguous, whose bitmap/accounting is NOT yet
     * SMP-locked (a pre-existing kernel-wide gap; kmalloc's s_heap_lock covers
     * the small-block path). Tracked for a real physical-allocator lock. */
    mutex_lock(&t->environ_lock);

    /* Sizing pass: UTF-8 entries convert to a variable number of WCHARs (a
     * multibyte UTF-8 char is one BMP WCHAR or a surrogate pair, never one WCHAR
     * per byte), so measure the real UTF-16 length via nls_cp_utf8_to_utf16 with
     * a NULL destination. NLS_CP_REPLACE keeps a malformed stored value building
     * (U+FFFD) instead of failing the whole block. */
    nentries = 0u;
    total = 1u;                                   /* trailing block terminator NUL */
    if (t->environ) {
        for (i = 0; i < t->environ_count; i++) {
            const char *e = t->environ[i];
            int need;
            if (!e)
                continue;
            need = nls_cp_utf8_to_utf16((const uint8_t *)e, env_strlen(e),
                                        (uint16_t *)0, 0u, NLS_CP_REPLACE);
            if (need < 0) {
                mutex_unlock(&t->environ_lock);
                return ENV_ERR_INVAL;             /* only on a bad mode/NULL -- guarded */
            }
            total += (uint32_t)need + 1u;         /* entry WCHARs + its NUL */
            nentries++;
            /* Bail the instant the block exceeds the cap rather than sizing every
             * remaining entry under the lock (a pathological environ could be
             * tens of MiB); sibling env ops should not wait on a doomed build. */
            if (total > max_wchars) {
                mutex_unlock(&t->environ_lock);
                return ENV_ERR_NOSPACE;
            }
        }
    }
    /* A non-empty block already ends "...\0\0" (last entry's NUL + the trailing
     * NUL). An empty environment would otherwise be a single NUL, violating the
     * NT double-NUL block contract -- emit two NULs so it is "\0\0". */
    if (nentries == 0u)
        total = 2u;
    if (total > max_wchars) {
        mutex_unlock(&t->environ_lock);
        return ENV_ERR_NOSPACE;
    }

    raw = env_str_alloc(total * 2u);              /* wchars -> bytes */
    if (!raw) {
        mutex_unlock(&t->environ_lock);
        return ENV_ERR_NOMEM;
    }
    blk = (uint16_t *)raw;

    w = 0;
    if (t->environ) {
        for (i = 0; i < t->environ_count; i++) {
            const char *e = t->environ[i];
            int got;
            if (!e)
                continue;
            got = nls_cp_utf8_to_utf16((const uint8_t *)e, env_strlen(e),
                                       &blk[w], total - w, NLS_CP_REPLACE);
            if (got < 0) {                        /* sizing guaranteed room; defensive */
                mutex_unlock(&t->environ_lock);
                env_str_free(raw, total * 2u);
                return ENV_ERR_INVAL;
            }
            w += (uint32_t)got;
            blk[w++] = 0;                         /* terminate this entry */
        }
    }
    blk[w++] = 0;                                 /* empty entry: block terminator */
    if (nentries == 0u)
        blk[w++] = 0;                             /* second NUL for the empty block */

    mutex_unlock(&t->environ_lock);

    *out_block = blk;
    *out_wchars = w;                              /* == total */
    return ENV_OK;
}

void env_free_block_utf16(uint16_t *block, uint32_t wchars)
{
    if (block)
        env_str_free((char *)block, wchars * 2u);
}

/* ===========================================================================
 * Contiguous CreateProcess environment blocks (caller-buffer build + parse)
 *
 * env_build_block() serializes the SORTED environ into a caller-supplied buffer
 * (ANSI = raw UTF-8 bytes, or UTF-16); env_parse_block() decodes such a block
 * back into the environ via the atomic env_adopt_block() path (validate + sort +
 * dedup + block cap). These are the CreateProcess lpEnvironment surface; the
 * CREATE_UNICODE_ENVIRONMENT flag decision lives in the process-creation caller,
 * not here.
 * =========================================================================== */

int env_build_block(struct task *t, void *out_buf, uint32_t max_len,
                    int is_unicode, uint32_t *out_len)
{
    uint32_t i;

    if (!t || !out_len)
        return ENV_ERR_INVAL;
    *out_len = 0;

    mutex_lock(&t->environ_lock);

    if (is_unicode) {
        uint32_t wchars = 1u;              /* trailing block terminator wchar */
        uint32_t nent = 0;
        uint32_t need, cap, w;
        uint16_t *blk;
        if (t->environ) {
            for (i = 0; i < t->environ_count; i++) {
                const char *e = t->environ[i];
                int nn;
                if (!e)
                    continue;
                nn = nls_cp_utf8_to_utf16((const uint8_t *)e, env_strlen(e),
                                          (uint16_t *)0, 0u, NLS_CP_REPLACE);
                if (nn < 0) {
                    mutex_unlock(&t->environ_lock);
                    return ENV_ERR_INVAL;
                }
                wchars += (uint32_t)nn + 1u;   /* entry WCHARs + its NUL */
                nent++;
            }
        }
        if (nent == 0u)
            wchars = 2u;                   /* empty block is "\0\0" */
        need = wchars * 2u;                /* bytes */
        *out_len = need;
        if (!out_buf || need > max_len) {
            mutex_unlock(&t->environ_lock);
            return ENV_ERR_NOSPACE;
        }
        blk = (uint16_t *)out_buf;
        cap = max_len / 2u;
        w = 0;
        if (t->environ) {
            for (i = 0; i < t->environ_count; i++) {
                const char *e = t->environ[i];
                int got;
                if (!e)
                    continue;
                got = nls_cp_utf8_to_utf16((const uint8_t *)e, env_strlen(e),
                                           &blk[w], cap - w, NLS_CP_REPLACE);
                if (got < 0) {             /* sizing guaranteed room; defensive */
                    mutex_unlock(&t->environ_lock);
                    return ENV_ERR_INVAL;
                }
                w += (uint32_t)got;
                blk[w++] = 0;              /* terminate this entry */
            }
        }
        blk[w++] = 0;                      /* block terminator */
        if (nent == 0u)
            blk[w++] = 0;                  /* second NUL for the empty block */
    } else {
        uint32_t bytes = 1u;               /* trailing block terminator byte */
        uint32_t nent = 0;
        uint32_t p;
        char *blk;
        if (t->environ) {
            for (i = 0; i < t->environ_count; i++) {
                const char *e = t->environ[i];
                if (!e)
                    continue;
                bytes += env_strlen(e) + 1u;   /* "name=value" + its NUL */
                nent++;
            }
        }
        if (nent == 0u)
            bytes = 2u;                    /* empty block is "\0\0" */
        *out_len = bytes;
        if (!out_buf || bytes > max_len) {
            mutex_unlock(&t->environ_lock);
            return ENV_ERR_NOSPACE;
        }
        blk = (char *)out_buf;
        p = 0;
        if (t->environ) {
            for (i = 0; i < t->environ_count; i++) {
                const char *e = t->environ[i];
                uint32_t el, j;
                if (!e)
                    continue;
                el = env_strlen(e);
                for (j = 0; j < el; j++)
                    blk[p++] = e[j];
                blk[p++] = '\0';           /* terminate this entry */
            }
        }
        blk[p++] = '\0';                   /* block terminator */
        if (nent == 0u)
            blk[p++] = '\0';               /* second NUL for the empty block */
    }

    mutex_unlock(&t->environ_lock);
    return ENV_OK;
}

/* The largest a single "name=value" entry can be and still satisfy the byte
 * caps; parse skips anything larger BEFORE allocating it. */
#define ENV_PARSE_ENTRY_MAX  (ENV_NAME_MAX + 1u + ENV_VALUE_MAX)

int env_parse_block(struct task *t, const void *block, uint32_t len,
                    int is_unicode)
{
    const char **list;
    char **owned;                          /* temp UTF-8 entry copies to free */
    uint32_t n = 0, i;
    uint32_t cum = 1u;                     /* cumulative block bytes (+ terminator) */
    int overflow = 0;
    int rc = ENV_OK;

    if (!t)
        return ENV_ERR_INVAL;
    if (!block || len == 0u)
        return env_adopt_block(t, (const char *const *)0, 0u);   /* clear */

    /* Bound the raw work BEFORE any scan/alloc/convert: a block whose raw size
     * cannot fit ENV_BLOCK_MAX cannot yield a valid environment. Each unit (a
     * byte for ANSI, a wchar for UTF-16) decodes to >= 1 UTF-8 byte, so the unit
     * count is a lower bound on the decoded byte count -- reject a huge caller
     * block outright instead of scanning/converting it. */
    if ((is_unicode ? (len / 2u) : len) > ENV_BLOCK_MAX)
        return ENV_ERR_NOSPACE;

    /* list[] + owned[] hold one extra slot for the NULL terminator; (511+1)*8 ==
     * 4096 bytes, at the kmalloc ceiling. */
    list  = (const char **)kmalloc((ENV_MAX_ENTRIES + 1u) * sizeof(char *));
    owned = (char **)kmalloc((ENV_MAX_ENTRIES + 1u) * sizeof(char *));
    if (!list || !owned) {
        if (list)  kfree(list);
        if (owned) kfree(owned);
        return ENV_ERR_NOMEM;
    }

    if (is_unicode) {
        const uint16_t *w = (const uint16_t *)block;
        uint32_t wlen = len / 2u;
        uint32_t p = 0;
        while (p < wlen) {
            uint32_t q = p;
            int need;
            char *u8;
            while (q < wlen && w[q] != 0)
                q++;
            if (q == p)
                break;                     /* empty entry -> block end */
            if (n >= ENV_MAX_ENTRIES) {
                overflow = 1;
                break;
            }
            if (q - p > ENV_PARSE_ENTRY_MAX) {   /* over-max entry -> skip, no work */
                if (q >= wlen)
                    break;
                p = q + 1u;
                continue;
            }
            need = nls_cp_utf16_to_utf8(&w[p], q - p, (uint8_t *)0, 0u,
                                        NLS_CP_REPLACE);
            if (need < 0 || (uint32_t)need > ENV_PARSE_ENTRY_MAX) {
                if (q >= wlen)             /* bad or over-cap entry -> skip */
                    break;
                p = q + 1u;
                continue;
            }
            if (cum + (uint32_t)need + 1u > ENV_BLOCK_MAX) {
                overflow = 1;             /* cumulative block cap -> reject block */
                break;
            }
            u8 = env_str_alloc((uint32_t)need + 1u);
            if (!u8) {
                rc = ENV_ERR_NOMEM;
                goto done;
            }
            nls_cp_utf16_to_utf8(&w[p], q - p, (uint8_t *)u8, (uint32_t)need,
                                 NLS_CP_REPLACE);
            u8[need] = '\0';
            owned[n] = u8;
            list[n] = u8;
            n++;
            cum += (uint32_t)need + 1u;
            if (q >= wlen)
                break;
            p = q + 1u;
        }
    } else {
        const char *b = (const char *)block;
        uint32_t p = 0;
        while (p < len) {
            uint32_t q = p, el, j;
            char *u8;
            while (q < len && b[q] != '\0')
                q++;
            if (q == p)
                break;                     /* empty entry -> block end */
            if (n >= ENV_MAX_ENTRIES) {
                overflow = 1;
                break;
            }
            el = q - p;
            if (el > ENV_PARSE_ENTRY_MAX) {      /* over-max entry -> skip, no alloc */
                if (q >= len)
                    break;
                p = q + 1u;
                continue;
            }
            if (cum + el + 1u > ENV_BLOCK_MAX) {
                overflow = 1;             /* cumulative block cap -> reject block */
                break;
            }
            u8 = env_str_alloc(el + 1u);
            if (!u8) {
                rc = ENV_ERR_NOMEM;
                goto done;
            }
            for (j = 0; j < el; j++)
                u8[j] = b[p + j];
            u8[el] = '\0';
            owned[n] = u8;
            list[n] = u8;
            n++;
            cum += el + 1u;
            if (q >= len)
                break;
            p = q + 1u;
        }
    }

    if (overflow) {
        rc = ENV_ERR_NOSPACE;
    } else {
        list[n] = NULL;
        rc = env_adopt_block(t, list, n);  /* deep-copies; validates + sorts */
    }

done:
    for (i = 0; i < n; i++)
        if (owned[i])
            env_str_free(owned[i], env_strlen(owned[i]) + 1u);
    kfree(owned);
    kfree(list);
    return rc;
}

/* ===========================================================================
 * Hidden drive-letter current-directory variables (=C:, =D:) -- TODO-22 s12
 *
 * Windows records the current directory of each drive letter in a hidden env
 * variable whose name begins with '=' ("=C:", "=D:", ...). These live in the
 * SAME sorted environ[] as ordinary variables: '=' (0x3D) sorts before any
 * letter so they land at the front of a built block, env_copy carries them along
 * once child inheritance is wired (TODO-12 s7), and env_build_block emits them
 * first -- no separate storage. env_set /
 * env_get_copy already accept the "=X:" name (env_name_classify), so these are
 * thin drive-letter -> "=X:" adapters.
 * =========================================================================== */

/* Uppercase an ASCII drive letter; returns 0 for a non-letter. */
static char env_drive_upper(char drive)
{
    if (drive >= 'a' && drive <= 'z')
        return (char)(drive - 32);
    if (drive >= 'A' && drive <= 'Z')
        return drive;
    return '\0';
}

int env_set_drive_cwd(struct task *t, char drive, const char *path)
{
    char name[4];                          /* "=X:" + NUL */
    char up = env_drive_upper(drive);

    if (!t || !path || up == '\0')
        return ENV_ERR_INVAL;
    name[0] = '=';
    name[1] = up;
    name[2] = ':';
    name[3] = '\0';
    return env_set(t, name, path);         /* serializes on t->environ_lock */
}

int env_get_drive_cwd(struct task *t, char drive, char *out, uint32_t out_size)
{
    char name[4];                          /* "=X:" + NUL */
    char up = env_drive_upper(drive);
    int rc;

    if (!t || !out || out_size < 4u || up == '\0')   /* room for "X:\" + NUL */
        return ENV_ERR_INVAL;
    name[0] = '=';
    name[1] = up;
    name[2] = ':';
    name[3] = '\0';
    rc = env_get_copy(t, name, out, out_size);
    if (rc == ENV_ERR_NOTFOUND) {
        /* No remembered directory for this drive -> its root "X:\". out_size >= 4
         * is guaranteed above, so this never truncates. */
        out[0] = up;
        out[1] = ':';
        out[2] = '\\';
        out[3] = '\0';
        return 3;                          /* value length, excluding NUL */
    }
    if (rc < 0)
        return rc;
    /* A PRESENT "=X:" value MUST be a canonical absolute path ON drive X
     * ("X:\..."). env_set / env_adopt_block / env_parse_block validate the NAME
     * (only "=X:") but NOT the value, so a crafted "=D:=C:\Victim" (or a
     * non-absolute "=D:=foo") can be stored. Consuming such a value as the
     * resolution base would let vfs_resolve_path take the drive from the VALUE and
     * silently retarget "D:relative" to another volume -- reaching destructive
     * callers (NtDeleteFile). Reject a foreign-drive or non-absolute value
     * (fail-closed via ENV_ERR_INVAL) rather than resolve against it. A truncated
     * value that starts "X:\" still passes here and is caught by the caller's
     * rc >= out_size guard. */
    if (env_drive_upper(out[0]) != up || out[1] != ':' ||
        (out[2] != '\\' && out[2] != '/'))
        return ENV_ERR_INVAL;
    return rc;
}

/* ===========================================================================
 * System default environment (system-default-variables feature)
 *
 * env_init_defaults() seeds a task with the machine's default variables in
 * three precedence layers, applied via env_set so the LAST write wins:
 *   1. synthesised base defaults (computed / hardcoded);
 *   2. the machine-wide Registry Environment key (overrides synth);
 *   3. the per-user Registry Environment key (overrides system), with PATH
 *      concatenated to the base rather than replaced.
 * Registry reads are best-effort: a missing/failed key leaves the synth base in
 * place, so this is safe even if registry_populate_defaults() partially failed.
 * env_init_kernel_task() applies this (or a hardcoded fallback) to PID 0 at boot.
 * Boot-context only (BSP, single-threaded when seeding PID 0): the Registry has
 * no SMP lock yet, so a concurrent AP mutation would race -- a pre-existing repo-
 * wide exposure owned by the registry SMP-locking work, not introduced here.
 * =========================================================================== */

/* Registry sources for the two Environment layers + the computed-var lookups. */
#define ENV_REG_SYSTEM_ENV \
    "SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment"
#define ENV_REG_USER_ENV     "Environment"                        /* under HKCU */
#define ENV_REG_COMPUTERNAME "SYSTEM\\ComputerName\\ActiveComputerName"
#define ENV_REG_CPU          "HARDWARE\\CPU"

/* Synth default values (freestanding string literals; see the TODO OS table). */
#define ENV_DEF_COMPUTERNAME "IMPOSSIBLE-PC"
#define ENV_DEF_USERNAME     "Default"
#define ENV_DEF_SYSTEMDRIVE  "C:"
#define ENV_DEF_SYSTEMROOT   "C:\\Impossible"
#define ENV_DEF_TEMP         "C:\\Temp"
#define ENV_DEF_PROC_ARCH    "AMD64"
#define ENV_DEF_OS           "Impossible_OS"
#define ENV_DEF_PATH_BASE    "C:\\Impossible\\Bin;C:\\Impossible\\System32;C:\\Programs"
/* PATHEXT: only .EXE is seeded because exec.c (src/kernel/exec.c exec_init)
 * registers loaders for PE32+/ELF/EIF binaries only -- there is no .CMD/.BAT
 * batch interpreter, so advertising those extensions would let a future PATHEXT
 * lookup select a script it cannot run over a valid .EXE. Windows' fuller
 * default (.COM;.EXE;.BAT;.CMD;...) is deferred until a batch processor exists.
 * The consumer that iterates PATHEXT is the (deferred) user-mode shell
 * PATH-lookup path (shell_find_command), not the kernel env layer. */
#define ENV_DEF_PATHEXT      ".EXE"

/* Case-insensitive compare of two NUL-terminated names (env_lc folds ASCII). */
static int env_name_ci_eq(const char *a, const char *b)
{
    uint32_t i = 0;
    for (; a[i] && b[i]; i++)
        if (env_lc(a[i]) != env_lc(b[i]))
            return 0;
    return a[i] == b[i];   /* equal length and all chars matched */
}

/* Format an unsigned decimal into out[] (always NUL-terminated). */
static void env_u32_to_str(uint32_t v, char *out, uint32_t out_size)
{
    char tmp[11];              /* 2^32-1 == 4294967295 -> 10 digits */
    uint32_t n = 0, i;
    if (out_size == 0)
        return;
    do {
        tmp[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    } while (v && n < sizeof(tmp));
    for (i = 0; i < n && i < out_size - 1u; i++)
        out[i] = tmp[n - 1u - i];
    out[i] = '\0';
}

/* Append src to dst[] at *pos, bounded by dst_size (keeps dst NUL-terminated). */
static void env_str_append(char *dst, uint32_t dst_size, uint32_t *pos,
                           const char *src)
{
    uint32_t p = *pos;
    while (*src && p + 1u < dst_size)
        dst[p++] = *src++;
    dst[p] = '\0';
    *pos = p;
}

/* Read a single REG_SZ/REG_EXPAND_SZ value into out[] (always NUL-terminated).
 * Returns 1 on a non-empty string result, 0 if absent / wrong type / empty /
 * larger than out[] (RegQueryValueEx returns ERROR_MORE_DATA -> treated absent).
 * Used only for the SMALL single-value computed-var reads (ComputerName). */
static int env_reg_read_sz(HKEY hkey, const char *valname,
                           char *out, uint32_t out_size)
{
    uint32_t type = 0;
    uint32_t size = out_size;
    if (out_size == 0)
        return 0;
    out[0] = '\0';
    if (RegQueryValueEx(hkey, valname, (uint32_t *)0, &type,
                        (uint8_t *)out, &size) != ERROR_SUCCESS)
        return 0;
    if (type != REG_SZ && type != REG_EXPAND_SZ)
        return 0;
    /* Terminate at the RETURNED byte count, not the buffer end: a REG_SZ value
     * stored without a trailing NUL must not leave env_strlen scanning the
     * uninitialized tail of `out` (kernel-memory disclosure). On ERROR_SUCCESS
     * RegQueryValueEx guarantees size <= out_size; clamp the exact-fit edge. */
    if (size >= out_size)
        size = out_size - 1u;
    out[size] = '\0';
    return out[0] ? 1 : 0;
}

/* Record the first negative env_set result into *err (best-effort accumulator). */
static void env_seed(struct task *t, const char *name, const char *value,
                     int *err)
{
    int rc = env_set(t, name, value);
    if (rc != ENV_OK && *err == ENV_OK)
        *err = rc;
}

/* Layer 1: synthesised base defaults. Computed vars (COMPUTERNAME, CPU count)
 * read the Registry with a hardcoded fallback; the rest are fixed literals. */
static void env_synth_base(struct task *t, int *err)
{
    char cn[ENV_NAME_MAX];       /* ComputerName (short) */
    char path[512];              /* derived C:\Users\... paths */
    char nproc[12];
    uint32_t ncpu = 0;
    uint32_t p;

    /* COMPUTERNAME: ActiveComputerName\ComputerName, else the hardcoded default. */
    cn[0] = '\0';
    {
        HKEY hk;
        if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, ENV_REG_COMPUTERNAME, 0,
                         KEY_READ, &hk) == ERROR_SUCCESS) {
            env_reg_read_sz(hk, "ComputerName", cn, sizeof(cn));
            RegCloseKey(hk);
        }
    }
    if (!cn[0]) {
        p = 0;
        env_str_append(cn, sizeof(cn), &p, ENV_DEF_COMPUTERNAME);
    }
    env_seed(t, "COMPUTERNAME", cn, err);

    /* USERNAME: the token UserSid -> account-name lookup is owned by the SRM
     * account-name work; until then every process runs as the default account. */
    env_seed(t, "USERNAME", ENV_DEF_USERNAME, err);

    /* USERPROFILE / APPDATA / LOCALAPPDATA derived from USERNAME. Windows stores
     * these with NO trailing separator ("C:\Users\Default", not "...\Default\"),
     * so consumers that do %USERPROFILE%\file get one backslash, not two. */
    p = 0;
    env_str_append(path, sizeof(path), &p, "C:\\Users\\");
    env_str_append(path, sizeof(path), &p, ENV_DEF_USERNAME);
    env_seed(t, "USERPROFILE", path, err);
    p = 0;
    env_str_append(path, sizeof(path), &p, "C:\\Users\\");
    env_str_append(path, sizeof(path), &p, ENV_DEF_USERNAME);
    env_str_append(path, sizeof(path), &p, "\\AppData\\Roaming");
    env_seed(t, "APPDATA", path, err);
    p = 0;
    env_str_append(path, sizeof(path), &p, "C:\\Users\\");
    env_str_append(path, sizeof(path), &p, ENV_DEF_USERNAME);
    env_str_append(path, sizeof(path), &p, "\\AppData\\Local");
    env_seed(t, "LOCALAPPDATA", path, err);

    /* Fixed literals. */
    env_seed(t, "TEMP", ENV_DEF_TEMP, err);
    env_seed(t, "TMP", ENV_DEF_TEMP, err);
    env_seed(t, "PROCESSOR_ARCHITECTURE", ENV_DEF_PROC_ARCH, err);
    env_seed(t, "OS", ENV_DEF_OS, err);
    env_seed(t, "WINDIR", ENV_DEF_SYSTEMROOT, err);
    env_seed(t, "SYSTEMROOT", ENV_DEF_SYSTEMROOT, err);
    env_seed(t, "SYSTEMDRIVE", ENV_DEF_SYSTEMDRIVE, err);
    env_seed(t, "PATH", ENV_DEF_PATH_BASE, err);
    env_seed(t, "PATHEXT", ENV_DEF_PATHEXT, err);

    /* NUMBER_OF_PROCESSORS: HARDWARE\CPU\Count (DWORD), else the live CPU count. */
    {
        HKEY hk;
        if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, ENV_REG_CPU, 0,
                         KEY_READ, &hk) == ERROR_SUCCESS) {
            RegGetDword(hk, "Count", &ncpu);
            RegCloseKey(hk);
        }
    }
    if (ncpu == 0)
        ncpu = smp_cpu_count();
    if (ncpu == 0)
        ncpu = 1;
    env_u32_to_str(ncpu, nproc, sizeof(nproc));
    env_seed(t, "NUMBER_OF_PROCESSORS", nproc, err);
}

/* PATH overlay from a user key: append user PATH to the existing base with ';'.
 * env_set enforces ENV_VALUE_MAX; an over-long join is rejected there, keeping
 * the base PATH intact. */
static void env_path_append(struct task *t, const char *user_path, int *err)
{
    char probe[1];
    int plen;
    uint32_t base_len, ulen, need, p, i;
    char *joined;

    ulen = env_strlen(user_path);
    if (ulen == 0)
        return;                                   /* empty user PATH -> keep base */

    plen = env_get_copy(t, "PATH", probe, sizeof(probe));   /* returns full len */
    base_len = (plen > 0) ? (uint32_t)plen : 0u;

    need = base_len + 1u + ulen + 1u;             /* base + ';' + user + NUL */
    joined = env_str_alloc(need);
    if (!joined) {
        if (*err == ENV_OK)
            *err = ENV_ERR_NOMEM;
        return;
    }
    if (base_len)
        env_get_copy(t, "PATH", joined, base_len + 1u);       /* fills [0..base_len] */
    else
        joined[0] = '\0';
    p = base_len;
    if (base_len)
        joined[p++] = ';';
    for (i = 0; i < ulen; i++)
        joined[p++] = user_path[i];
    joined[p] = '\0';

    {
        int rc = env_set(t, "PATH", joined);
        if (rc != ENV_OK && *err == ENV_OK)
            *err = rc;
    }
    env_str_free(joined, need);
}

/* Deferred-expansion collector: the NAMES of REG_EXPAND_SZ values seen while
 * overlaying, so their %VAR% references are expanded AFTER every layer is applied
 * -- never inline during RegEnumValue (whose linked-list enumeration order would
 * make "BIN=%BASE%\\bin" capture a stale or missing BASE purely from insertion
 * history, and make a retry produce a different result). Expanding once at the
 * end against the fully-assembled environment is deterministic and independent of
 * enumeration order. */
struct env_expand_list {
    char   **names;    /* env_strdup'd REG_EXPAND_SZ names */
    uint32_t count;
    uint32_t cap;
    int      err;      /* first failure (NOMEM) sticks */
};

/* Index of `name` in the list (case-insensitive), or -1. */
static int env_expand_list_find(struct env_expand_list *l, const char *name)
{
    uint32_t i;
    for (i = 0; i < l->count; i++)
        if (l->names[i] && env_name_ci_eq(l->names[i], name))
            return (int)i;
    return -1;
}

/* Mark a REG_EXPAND_SZ name for the later expansion pass. Idempotent: a name is
 * recorded once (dedup), so a duplicate registry record cannot expand it twice. */
static void env_expand_list_add(struct env_expand_list *l, const char *name)
{
    if (env_expand_list_find(l, name) >= 0)
        return;                               /* already marked */
    if (l->count >= l->cap) {
        uint32_t newcap = l->cap ? (l->cap * 2u) : 8u;
        char **grown = (char **)krealloc(l->names, newcap * sizeof(char *));
        if (!grown) {
            if (l->err == ENV_OK)
                l->err = ENV_ERR_NOMEM;
            return;
        }
        l->names = grown;
        l->cap = newcap;
    }
    l->names[l->count] = env_strdup(name);
    if (!l->names[l->count]) {
        if (l->err == ENV_OK)
            l->err = ENV_ERR_NOMEM;
        return;
    }
    l->count++;
}

/* Unmark a name: a later layer overwrote it with a REG_SZ (non-expandable)
 * winner, so its final value must NOT be %-expanded. No-op if not marked. This is
 * the type-provenance guard -- without it an HKLM REG_EXPAND_SZ value that HKCU
 * later replaces with a plain REG_SZ would still be wrongly expanded. */
static void env_expand_list_remove(struct env_expand_list *l, const char *name)
{
    int idx = env_expand_list_find(l, name);
    uint32_t i;
    if (idx < 0)
        return;
    env_str_free(l->names[idx], env_strlen(l->names[idx]) + 1u);
    for (i = (uint32_t)idx; i + 1u < l->count; i++)
        l->names[i] = l->names[i + 1u];
    l->count--;
}

static void env_expand_list_free(struct env_expand_list *l)
{
    uint32_t i;
    for (i = 0; i < l->count; i++)
        if (l->names[i])
            env_str_free(l->names[i], env_strlen(l->names[i]) + 1u);
    if (l->names)
        kfree(l->names);
    l->names = NULL;
    l->count = 0;
    l->cap = 0;
}

/* Layers 2/3: overlay every REG_SZ/REG_EXPAND_SZ value under root\subkey via
 * env_set. When path_append is set, a "PATH" value is appended to the base
 * instead of replacing it. REG_EXPAND_SZ values are stored RAW and their name
 * recorded in `exlist` for the deterministic post-overlay expansion pass.
 * Buffers are heap/PMM-sized from the key metadata (never a 32 KiB stack buffer).
 * A missing key is a silent no-op. */
static void env_overlay_key(struct task *t, HKEY root, const char *subkey,
                            int path_append, int *err,
                            struct env_expand_list *exlist)
{
    HKEY hk;
    uint32_t nvals = 0, max_name = 0, max_val = 0;
    uint32_t namecap, valcap, idx;
    char *namebuf, *valbuf;

    if (RegOpenKeyEx(root, subkey, 0, KEY_READ, &hk) != ERROR_SUCCESS)
        return;                                   /* key absent -> nothing to overlay */

    if (RegQueryInfoKey(hk, (char *)0, (uint32_t *)0, (uint32_t *)0,
                        (uint32_t *)0, (uint32_t *)0, (uint32_t *)0,
                        &nvals, &max_name, &max_val,
                        (uint32_t *)0, (uint64_t *)0) != ERROR_SUCCESS ||
        nvals == 0) {
        RegCloseKey(hk);
        return;
    }

    /* RegEnumValue name/data lengths exclude the NUL; add room and cap at the
     * env limits (a longer value cannot be stored anyway and is skipped). */
    namecap = max_name + 2u;
    if (namecap > ENV_NAME_MAX + 1u)  namecap = ENV_NAME_MAX + 1u;
    if (namecap < 2u)                 namecap = 2u;
    valcap = max_val + 2u;
    if (valcap > ENV_VALUE_MAX + 1u)  valcap = ENV_VALUE_MAX + 1u;
    if (valcap < 2u)                  valcap = 2u;

    namebuf = env_str_alloc(namecap);
    valbuf = env_str_alloc(valcap);
    if (!namebuf || !valbuf) {
        if (namebuf) env_str_free(namebuf, namecap);
        if (valbuf)  env_str_free(valbuf, valcap);
        RegCloseKey(hk);
        if (*err == ENV_OK)
            *err = ENV_ERR_NOMEM;
        return;
    }

    for (idx = 0; ; idx++) {
        uint32_t nlen = namecap, vlen = valcap, type = 0;
        long rc = RegEnumValue(hk, idx, namebuf, &nlen, (uint32_t *)0, &type,
                               (uint8_t *)valbuf, &vlen);
        if (rc == ERROR_NO_MORE_ITEMS)
            break;
        if (rc == ERROR_MORE_DATA)
            continue;                             /* value over cap -> skip, keep going */
        if (rc != ERROR_SUCCESS)
            break;
        if (type != REG_SZ && type != REG_EXPAND_SZ)
            continue;
        /* Terminate at the RETURNED length, never at buffer capacity: env_str_alloc
         * buffers are uninitialized, and a REG_SZ value stored without a trailing
         * NUL would otherwise make env_set scan (and persist) the allocator bytes
         * between the value end and a capacity-terminator -- a kernel-memory
         * disclosure into the environment. */
        if (nlen >= namecap) nlen = namecap - 1u;
        if (vlen >= valcap)  vlen = valcap - 1u;
        namebuf[nlen] = '\0';
        valbuf[vlen] = '\0';
        if (!namebuf[0])
            continue;                             /* skip the unnamed default value */
        if (path_append && env_name_ci_eq(namebuf, "PATH")) {
            env_path_append(t, valbuf, err);
            /* PATH is a JOIN, not a replace: a REG_EXPAND_SZ layer marks it for
             * expansion; a REG_SZ append must NOT unmark it, because the base
             * half may still hold %refs from an earlier REG_EXPAND_SZ layer. */
            if (type == REG_EXPAND_SZ)
                env_expand_list_add(exlist, namebuf);
        } else {
            int rc2 = env_set(t, namebuf, valbuf);
            if (rc2 != ENV_OK && *err == ENV_OK)
                *err = rc2;
            /* Track the WINNING value's type (last write wins), but ONLY when the
             * value was actually STORED (rc2 == ENV_OK). Recording a name whose
             * env_set hit the entry/block cap would let the collector -- and the
             * results[] array it later sizes -- grow past the stored-var count.
             * Mark for the deferred expansion pass on REG_EXPAND_SZ; unmark on a
             * REG_SZ that overwrote a previously-marked name. */
            if (rc2 == ENV_OK) {
                if (type == REG_EXPAND_SZ)
                    env_expand_list_add(exlist, namebuf);
                else
                    env_expand_list_remove(exlist, namebuf);
            }
        }
    }

    env_str_free(namebuf, namecap);
    env_str_free(valbuf, valcap);
    RegCloseKey(hk);
}

/* Deterministic post-overlay expansion pass for the REG_EXPAND_SZ winners.
 *
 * TWO PHASES so the result is independent of Registry enumeration order and of
 * the order names appear in `l`:
 *   1. COMPUTE: for every marked name, expand its raw %VAR% value against the
 *      environment while it still holds ONLY raw values (nothing is written back
 *      yet), buffering each result. Because no expansion has been published, a
 *      value referencing %OTHER% resolves %OTHER% to its RAW form -- one pass,
 *      exactly matching Win32 REG_EXPAND_SZ (a reference to another expandable
 *      value is NOT recursively resolved). Unknown references stay verbatim;
 *      single-pass means cycles cannot hang.
 *   2. APPLY: write every computed result back. Only names whose WINNING value
 *      was REG_EXPAND_SZ are in `l` (a later REG_SZ layer unmarked them), so a
 *      REG_SZ winner that happens to contain '%' is never expanded.
 * The array of buffered results is (count+0) pointers; count <= number of stored
 * vars <= ENV_MAX_ENTRIES, so it fits one <=4 KiB kmalloc. */
static void env_expand_reg_values(struct task *t, struct env_expand_list *l,
                                  int *err)
{
    uint32_t i;
    char probe[1];
    char **results;

    if (l->count == 0u)
        return;
    results = (char **)kmalloc(l->count * sizeof(char *));
    if (!results) {
        if (*err == ENV_OK)
            *err = ENV_ERR_NOMEM;
        return;
    }

    /* Phase 1: expand each raw value against the still-raw environment. */
    for (i = 0; i < l->count; i++) {
        const char *name = l->names[i];
        int rawlen;
        char *raw, *out;
        results[i] = NULL;
        if (!name)
            continue;
        rawlen = env_get_copy(t, name, probe, sizeof(probe));   /* full length */
        if (rawlen <= 0)
            continue;                                 /* absent/empty -> nothing to do */
        raw = env_str_alloc((uint32_t)rawlen + 1u);
        out = env_str_alloc(ENV_VALUE_MAX + 1u);      /* expansion bounded by the cap */
        if (!raw || !out) {
            if (raw) env_str_free(raw, (uint32_t)rawlen + 1u);
            if (out) env_str_free(out, ENV_VALUE_MAX + 1u);
            if (*err == ENV_OK)
                *err = ENV_ERR_NOMEM;
            continue;
        }
        if (env_get_copy(t, name, raw, (uint32_t)rawlen + 1u) > 0) {
            env_expand(t, raw, out, ENV_VALUE_MAX + 1u);
            results[i] = env_strdup(out);             /* keep the expanded result */
            if (!results[i] && *err == ENV_OK)
                *err = ENV_ERR_NOMEM;
        }
        env_str_free(raw, (uint32_t)rawlen + 1u);
        env_str_free(out, ENV_VALUE_MAX + 1u);
    }

    /* Phase 2: publish every computed result (now the environment mutates). */
    for (i = 0; i < l->count; i++) {
        if (results[i]) {
            int rc = env_set(t, l->names[i], results[i]);
            if (rc != ENV_OK && *err == ENV_OK)
                *err = rc;
            env_str_free(results[i], env_strlen(results[i]) + 1u);
        }
    }
    kfree(results);
}

int env_init_defaults(struct task *t)
{
    int err = ENV_OK;
    struct env_expand_list ex = { (char **)0, 0u, 0u, ENV_OK };
    if (!t)
        return ENV_ERR_INVAL;
    env_synth_base(t, &err);                                                  /* layer 1 */
    env_overlay_key(t, HKEY_LOCAL_MACHINE, ENV_REG_SYSTEM_ENV, 0, &err, &ex); /* layer 2 */
    env_overlay_key(t, HKEY_CURRENT_USER, ENV_REG_USER_ENV, 1, &err, &ex);    /* layer 3 */
    if (ex.err != ENV_OK && err == ENV_OK)
        err = ex.err;
    env_expand_reg_values(t, &ex, &err);   /* layer 4: deterministic %VAR% expand */
    env_expand_list_free(&ex);
    return err;
}

int env_init_kernel_task(void)
{
    struct task *sys = task_get_by_pid(0);

    if (!sys)
        return ENV_OK;                            /* PID 0 not created yet -> no-op */

    if (kernel_subsystem_ready(SUBSYS_REGISTRY)) {
        int rc = env_init_defaults(sys);
        if (rc != ENV_OK)
            klog(LOG_WARN, "env",
                 "env_init_kernel_task: defaults incomplete (rc=%d)",
                 (uint64_t)(int64_t)rc);
        else
            klog(LOG_INFO, "env",
                 "System environment initialized for PID 0 (%u vars)",
                 (uint64_t)sys->environ_count);
        return rc;
    }

    /* Registry unavailable (population failed): minimal hardcoded fallback so a
     * shell still has PATH/SYSTEMROOT/TEMP. `static const` -> no lock needed. */
    {
        static const struct { const char *name, *value; } bootstrap_env[] = {
            { "PATH",       "C:\\Impossible\\Bin" },
            { "SYSTEMROOT", ENV_DEF_SYSTEMROOT },
            { "TEMP",       ENV_DEF_TEMP },
        };
        int err = ENV_OK;
        uint32_t i;
        for (i = 0; i < sizeof(bootstrap_env) / sizeof(bootstrap_env[0]); i++)
            env_seed(sys, bootstrap_env[i].name, bootstrap_env[i].value, &err);
        klog(LOG_WARN, "env",
             "Registry unavailable; PID 0 seeded with bootstrap env fallback");
        return err;
    }
}
