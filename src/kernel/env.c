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

/* True if the "KEY" portion of entry "KEY=VALUE" equals `name` (namelen bytes),
 * case-insensitively (Windows env-name semantics). */
static int env_entry_key_eq(const char *entry, const char *name, uint32_t namelen)
{
    uint32_t i;
    for (i = 0; i < namelen; i++) {
        char e = entry[i];
        if (e == '\0' || e == '=')
            return 0;
        if (env_lc(e) != env_lc(name[i]))
            return 0;
    }
    return entry[namelen] == '=';
}

/* Classify a to-be-set variable name and return its length via `*out_len`.
 * Distinguishes ENV_ERR_TOOLONG (length > ENV_NAME_MAX) from ENV_ERR_INVAL
 * (NULL, empty, or contains '=' -- the separator; hidden "=X:" drive vars are
 * a later feature). Returns ENV_OK when the name is storable. */
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
    {
        uint32_t k;
        for (k = 0; k < n; k++)
            if (name[k] == '=')
                return ENV_ERR_INVAL;
    }
    *out_len = n;
    return ENV_OK;
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

/* Index of the entry matching `name` (case-insensitive), or -1. The caller MUST
 * hold t->environ_lock. */
static int env_find_index(struct task *t, const char *name, uint32_t namelen)
{
    uint32_t i;
    if (!t->environ)
        return -1;
    for (i = 0; i < t->environ_count; i++)
        if (t->environ[i] && env_entry_key_eq(t->environ[i], name, namelen))
            return (int)i;
    return -1;
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
    return t->environ[idx] + namelen + 1;   /* value portion, past "name=" */
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
        const char *val = t->environ[idx] + namelen + 1;
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

int env_set(struct task *t, const char *name, const char *value)
{
    uint32_t namelen, vallen, entlen;
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
    idx = env_find_index(t, name, namelen);
    if (idx >= 0) {
        char *old = t->environ[idx];
        uint32_t oldn = env_strlen(old) + 1;
        t->environ[idx] = newent;          /* swap pointer under lock */
        mutex_unlock(&t->environ_lock);
        env_str_free(old, oldn);           /* free old outside the lock */
        return ENV_OK;
    }
    if (t->environ_count >= ENV_MAX_ENTRIES) {
        mutex_unlock(&t->environ_lock);
        env_str_free(newent, entlen);
        return ENV_ERR_NOSPACE;
    }
    {
        uint32_t oldcnt = t->environ_count;
        uint32_t newcap = oldcnt + 2u;     /* +1 new entry, +1 NULL terminator */
        char **arr = (char **)krealloc(t->environ, newcap * sizeof(char *));
        if (!arr) {
            mutex_unlock(&t->environ_lock);
            env_str_free(newent, entlen);
            return ENV_ERR_NOMEM;
        }
        arr[oldcnt] = newent;
        arr[oldcnt + 1] = NULL;
        t->environ = arr;
        t->environ_count = oldcnt + 1;
    }
    mutex_unlock(&t->environ_lock);
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
