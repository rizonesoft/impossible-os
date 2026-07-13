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
    uint32_t i;

    if (!t)
        return ENV_ERR_INVAL;

    /* Empty block -> clear environ to empty. */
    if (!entries || count == 0) {
        mutex_lock(&t->environ_lock);
        old_env = t->environ;
        old_count = t->environ_count;
        t->environ = NULL;
        t->environ_count = 0;
        mutex_unlock(&t->environ_lock);
        if (old_env) {
            for (i = 0; i < old_count; i++)
                if (old_env[i])
                    env_str_free(old_env[i], env_strlen(old_env[i]) + 1);
            kfree(old_env);
        }
        return ENV_OK;
    }

    if (count > ENV_MAX_ENTRIES)
        count = ENV_MAX_ENTRIES;   /* honor the process env array cap */

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
        for (eq = 0; eq < elen && e[eq] != '='; eq++)
            ;
        if (eq == 0 || eq >= elen)
            continue;                       /* empty key or no '=' */
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

    mutex_lock(&t->environ_lock);
    old_env = t->environ;
    old_count = t->environ_count;
    t->environ = NULL;
    t->environ_count = n;
    t->environ = arr;
    mutex_unlock(&t->environ_lock);

    if (old_env) {
        for (i = 0; i < old_count; i++)
            if (old_env[i])
                env_str_free(old_env[i], env_strlen(old_env[i]) + 1);
        kfree(old_env);
    }
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

/* Layers 2/3: overlay every REG_SZ/REG_EXPAND_SZ value under root\subkey via
 * env_set. When path_append is set, a "PATH" value is appended to the base
 * instead of replacing it. Buffers are heap/PMM-sized from the key metadata
 * (never a 32 KiB stack buffer). A missing key is a silent no-op. */
static void env_overlay_key(struct task *t, HKEY root, const char *subkey,
                            int path_append, int *err)
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
        } else {
            int rc2 = env_set(t, namebuf, valbuf);
            if (rc2 != ENV_OK && *err == ENV_OK)
                *err = rc2;
        }
    }

    env_str_free(namebuf, namecap);
    env_str_free(valbuf, valcap);
    RegCloseKey(hk);
}

int env_init_defaults(struct task *t)
{
    int err = ENV_OK;
    if (!t)
        return ENV_ERR_INVAL;
    env_synth_base(t, &err);                                            /* layer 1 */
    env_overlay_key(t, HKEY_LOCAL_MACHINE, ENV_REG_SYSTEM_ENV, 0, &err); /* layer 2 */
    env_overlay_key(t, HKEY_CURRENT_USER, ENV_REG_USER_ENV, 1, &err);    /* layer 3 */
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
