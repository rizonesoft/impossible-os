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
#include "libc/string.h"         /* memcpy (env_wrap_block body copy) */
#include "kernel/sched/task.h"
#include "kernel/sched/mutex.h"
#include "kernel/mm/heap.h"
#include "kernel/mm/pmm.h"
#include "kernel/smp.h"          /* smp_cpu_count() */
#include "kernel/boot_init.h"    /* kernel_subsystem_ready(), SUBSYS_REGISTRY */
#include "kernel/klog.h"         /* klog() */
#include "kernel/nt/nls_cp.h"    /* nls_cp_utf8_to_utf16 (env block build) */
#include "kernel/ob/teb.h"       /* TEB LastErrorValue (CommandLineToArgvW) */
#include "kernel/security/mic.h" /* SeGetTokenIntegrityLevel, SECURITY_MANDATORY_*_RID (s16 AT_SECURE) */
#include "registry.h"            /* RegOpenKeyEx / RegEnumValue / RegGet* + ERROR_* */

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
/* Frames needed for `n` bytes. The round-up is computed in 64-bit deliberately:
 * `n + (ENV_PAGE_SIZE - 1)` in 32-bit wraps to a tiny value (frames == 0) for n
 * within 4095 of UINT32_MAX, which would free nothing while the caller believes the
 * allocation is gone. No current caller comes near that, but this is the single
 * choke point for every env allocation and it is header-public, so it computes the
 * count that is always right rather than the one that happens to be. */
static uint64_t env_buf_frames(uint32_t n)
{
    return ((uint64_t)n + (ENV_PAGE_SIZE - 1u)) / ENV_PAGE_SIZE;
}

/* Self-describing allocation header (TODO-22 s22). Sits immediately below every
 * pointer env_buf_alloc hands out, so env_buf_free picks the deallocator from the
 * allocation rather than from a caller-supplied size that can silently disagree.
 * `total_bytes` is the full header+payload extent (what the size class was chosen
 * on); `payload_bytes` is what the caller asked for, kept solely to cross-check the
 * `n` the caller passes to free. */
struct env_buf_hdr {
    uint32_t magic;          /* ENV_BUF_MAGIC while live, 0 once freed */
    uint32_t total_bytes;    /* header + payload; selects kfree vs pmm_free_frame */
    uint32_t payload_bytes;  /* caller's requested n; cross-check only */
    uint32_t _reserved;      /* pad to ENV_BUF_HDR_BYTES */
};

/* The payload alignment every existing caller relies on (8-aligned pointer arrays
 * in cmdl_hdr, uint16_t slots in env_block_hdr) follows from the header being a
 * 16-byte multiple on top of kmalloc's >= 16-byte / PMM's page alignment. */
_Static_assert(sizeof(struct env_buf_hdr) == ENV_BUF_HDR_BYTES,
               "env_buf_hdr must be ENV_BUF_HDR_BYTES so the payload stays 16-aligned");
_Static_assert(ENV_BUF_HDR_BYTES % 16u == 0u,
               "env_buf header must be a 16-byte multiple to preserve payload alignment");
_Static_assert(ENV_BUF_PAYLOAD_MAX < ENV_STR_KMALLOC_MAX,
               "heap payload ceiling must leave room for the header inside the kmalloc cap");
/* The expansion budget charges each lookup namelen * ENV_LOOKUP_CMP_MAX as an upper
 * bound on env_find_index's binary search. Growing ENV_MAX_ENTRIES past 2^N without
 * raising the bound would silently under-charge and re-open the unbounded mutex hold. */
_Static_assert((1u << ENV_LOOKUP_CMP_MAX) >= (ENV_MAX_ENTRIES + 1u),
               "ENV_LOOKUP_CMP_MAX must bound the binary search over ENV_MAX_ENTRIES");
/* kmalloc is 4 KiB-only by repo doctrine, but heap.c's own KMALLOC_MAX is 256 MiB (an
 * overflow guard), so nothing downstream would catch a raise of this constant silently
 * violating the rule at env_buf_alloc's kmalloc branch. Pin it where it is depended on. */
_Static_assert(ENV_STR_KMALLOC_MAX <= 4096u,
               "env_buf's kmalloc size class must honour the repo-wide 4 KiB kmalloc ceiling");
/* The budget must clear the WORST LEGITIMATE input or it becomes a compat bug rather
 * than a DoS guard (see env.h's derivation): the densest packing of a max-size input is
 * ENV_VALUE_MAX/3 "%A%" triples, each charged ENV_LOOKUP_CMP_MAX*(ENV_NAME_MAX+1)+1.
 * Raising ENV_NAME_MAX or ENV_VALUE_MAX without raising the ceiling would silently start
 * refusing legal input. The UTF-16 peer pins its own floor at nt_rtlenv.c. */
_Static_assert((uint64_t)ENV_EXPAND_WORK_MAX >
                   ((uint64_t)(ENV_VALUE_MAX / 3u) *
                    ((uint64_t)ENV_LOOKUP_CMP_MAX * (ENV_NAME_MAX + 1u) + 1u)),
               "ENV_EXPAND_WORK_MAX must exceed the worst legitimate input's charge");

void *env_buf_alloc(uint32_t n)
{
    struct env_buf_hdr *hdr;
    uint32_t total;

    if (n == 0u)
        return NULL;
    /* Reject before the header push can wrap: a caller asking within
     * ENV_BUF_HDR_BYTES of UINT32_MAX would otherwise get a tiny allocation and
     * believe it owns 4 GiB. No current caller comes near this (ENV_VALUE_MAX is
     * 32 KiB), but this is the single choke point for every env allocation. */
    if (n > ENV_BUF_ALLOC_MAX)
        return NULL;
    total = n + ENV_BUF_HDR_BYTES;

    if (total <= ENV_STR_KMALLOC_MAX) {
        hdr = (struct env_buf_hdr *)kmalloc(total);
    } else {
        /* identity-mapped; 0 -> NULL */
        hdr = (struct env_buf_hdr *)pmm_alloc_contiguous(env_buf_frames(total));
    }
    if (!hdr)
        return NULL;

    hdr->magic = ENV_BUF_MAGIC;
    hdr->total_bytes = total;
    hdr->payload_bytes = n;
    hdr->_reserved = 0u;
    return (void *)(hdr + 1);
}

void env_buf_free(void *p, uint32_t n)
{
    struct env_buf_hdr *hdr;
    uint32_t total;

    if (!p)
        return;
    hdr = (struct env_buf_hdr *)p - 1;

    /* A bad magic means this pointer did not come from env_buf_alloc, or it was
     * already freed. Either way the extent below is unknowable, so refuse: leaking
     * a buffer is recoverable, handing a wrong extent to pmm_free_frame is not. */
    if (hdr->magic != ENV_BUF_MAGIC) {
        klog(LOG_ERROR, "env", "env_buf_free: bad magic %x (double free/foreign), leak %u",
             hdr->magic, n);
        return;
    }
    /* The magic alone is NOT sufficient. It sits at offset 0, FARTHEST from the
     * payload, so a payload underrun corrupts _reserved, payload_bytes and
     * total_bytes BEFORE it reaches the magic -- leaving a header that still looks
     * live while carrying an extent that would pick the wrong deallocator or free
     * arbitrary frames, exactly what this header exists to prevent. Validate the
     * whole invariant with overflow-safe arithmetic before trusting the extent. */
    if (hdr->_reserved != 0u ||
        hdr->payload_bytes == 0u ||
        hdr->payload_bytes > ENV_BUF_ALLOC_MAX ||
        hdr->total_bytes != hdr->payload_bytes + ENV_BUF_HDR_BYTES) {
        klog(LOG_ERROR, "env", "env_buf_free: header corrupt (t=%u p=%u r=%u), refused",
             hdr->total_bytes, hdr->payload_bytes, hdr->_reserved);
        return;
    }
    /* The header fields alone are NOT enough: they are adjacent and equally
     * corruptible, so an underrun that rewrites payload_bytes and total_bytes
     * COHERENTLY satisfies every check above and could hand heap memory to
     * pmm_free_frame. The caller's `n` is the one witness that underrun cannot
     * reach, so it is the independent cross-check -- free only when both agree,
     * and refuse otherwise. Leaking is recoverable; freeing a wrong extent is not. */
    if (n != hdr->payload_bytes) {
        klog(LOG_ERROR, "env", "env_buf_free: size mismatch (caller %u, alloc %u), refused",
             n, hdr->payload_bytes);
        return;
    }

    total = hdr->total_bytes;
    hdr->magic = 0u;             /* poison first: an immediate double free is a no-op */

    if (total <= ENV_STR_KMALLOC_MAX) {
        kfree(hdr);
    } else {
        uint64_t frames = env_buf_frames(total);
        uint64_t f;
        uintptr_t base = (uintptr_t)hdr;
        for (f = 0; f < frames; f++)
            pmm_free_frame(base + f * ENV_PAGE_SIZE);
    }
}

/* env.c's string-typed view of the shared allocator. Kept as a named adapter (not
 * a rename of every call site) because the "KEY=VALUE" strings this file allocates
 * are char*, and the cast belongs in one place. */
static char *env_str_alloc(uint32_t n)
{
    return (char *)env_buf_alloc(n);
}

/* Free a string allocated by env_str_alloc. `n` MUST be the same byte count
 * (strlen(str)+1) that was passed to env_str_alloc. */
static void env_str_free(char *p, uint32_t n)
{
    env_buf_free(p, n);
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

/* --- s16: privilege-sensitive variable policy (AT_SECURE parallel) --------
 * A small blocklist of variable NAMES an elevated (High/System integrity)
 * process must never observe or export, mirroring glibc secure_getenv() /
 * AT_SECURE: LD_PRELOAD and LD_LIBRARY_PATH are DLL/loader-hijack vectors the
 * Linux compat layer honors for NORMAL processes but must ignore under
 * elevation; any name beginning "_IMPOSSIBLE_DEBUG_" is a debug knob that must
 * not be inheritable into a privileged process. Matching is case-INSENSITIVE
 * because the env store itself is (env_name_cmp lowercases via env_lc), so a
 * mixed-case spelling ("ld_preload") is the SAME stored entry and cannot bypass
 * the gate. env_get_copy/env_peek_locked hide these names, the block builders
 * omit them from a secure task's serialized block, and env_sanitize_for_elevation
 * physically strips them at an elevation transition. */
static int env_name_span_ci_eq(const char *name, uint32_t namelen, const char *lit)
{
    return env_name_cmp(name, namelen, lit, env_strlen(lit)) == 0;
}

static int env_name_span_ci_prefix(const char *name, uint32_t namelen, const char *lit)
{
    uint32_t llen = env_strlen(lit);
    return namelen >= llen && env_name_cmp(name, llen, lit, llen) == 0;
}

/* True iff `name` (namelen bytes, no NUL required) is on the elevation blocklist. */
int env_name_is_privilege_sensitive(const char *name, uint32_t namelen)
{
    if (!name || namelen == 0u)
        return 0;
    return env_name_span_ci_eq(name, namelen, "LD_PRELOAD")
        || env_name_span_ci_eq(name, namelen, "LD_LIBRARY_PATH")
        || env_name_span_ci_prefix(name, namelen, "_IMPOSSIBLE_DEBUG_");
}

/* Same test against a stored "name=value" entry (name is the key span). */
static int env_entry_is_privilege_sensitive(const char *entry)
{
    return env_name_is_privilege_sensitive(entry, env_entry_keylen(entry));
}

/* True iff task `t` runs at a privilege level where the blocklist applies --
 * i.e. its primary token's integrity level is above Medium (an elevated admin
 * or a service running as System). FAIL-CLOSED: a task with no assigned token
 * (no security context established yet) is treated as secure so an unassigned
 * process cannot be used to launder a blocklisted name. A malformed non-NULL
 * token FAILS CLOSED (treated as secure): SeTryGetTokenIntegrityLevel reports
 * validity separately, so a corrupt integrity SID no longer masquerades as a
 * benign Medium and cannot be used to launder a blocklisted name. A genuine
 * Medium token (valid IL) IS non-secure -- deliberate, so a normal process keeps
 * its legitimate LD_PRELOAD for the Linux compat layer.
 *
 * The primary token is assign-once at task creation (task.c) and atomically
 * exchanged to NULL only at reap (task.c task_cleanup, task already TASK_DEAD);
 * we ACQUIRE-load it to pair with that teardown store and avoid a torn pointer.
 * CALLER CONTRACT: `t` must be kept alive across the call (it is: every caller
 * passes task_current() or a caller-owned parent). A teardown-safe pin of the
 * primary token (so a future foreign-task caller cannot race reap-time free) is
 * the deferred kernel-wide per-token-reference protocol -> XREF TODO-15 s4; env
 * shares the exact contract every other t->token reader already relies on.
 * There is deliberately NO in-place Medium->High replacement path today. When
 * TODO-15 s9 adds one (UAC ProcessAccessToken swap), that transition MUST
 * publish the elevated token and restrict/sanitize the environment atomically
 * (a task-level elevating flag under environ_lock that rejects a blocklisted
 * env_set); until then the read gate below is sound because no concurrent
 * elevation of a running task can occur. */
int env_is_secure_context(struct task *t)
{
    void *tok;
    uint32_t il;
    if (!t)
        return 0;                    /* no task -> no boundary to enforce */
    tok = __atomic_load_n(&t->token, __ATOMIC_ACQUIRE);
    if (!tok)
        return 1;                    /* fail closed: no security context yet */
    if (!SeTryGetTokenIntegrityLevel((const ACCESS_TOKEN *)tok, &il))
        return 1;                    /* fail closed: malformed / absent IL SID */
    return il > SECURITY_MANDATORY_MEDIUM_RID;
}

/* True iff `t` is PROVEN elevated: a non-NULL task carrying a non-NULL token that
 * BOTH carries the authoritative IsElevated flag AND resolves to a valid integrity
 * level at or above High. This is the authorization inverse of
 * env_is_secure_context: for a WRITE/authorization decision (may this caller
 * create machine-wide state?) anything short of a fully consistent elevated token
 * must FAIL CLOSED to NOT-elevated (deny), whereas the READ blocklist gate above
 * fails closed to secure (restrict). Requiring BOTH signals rejects an
 * inconsistent token in either direction -- a valid High IL SID with IsElevated
 * clear (imported / corrupted / filtered-token-with-forged-SID), or an IsElevated
 * flag paired with a below-High integrity level. A NULL task, NULL token, or
 * malformed IL SID all return 0. Same ACQUIRE-load + caller-liveness contract as
 * env_is_secure_context. */
int env_is_proven_elevated(struct task *t)
{
    const ACCESS_TOKEN *tok;
    uint32_t il;
    if (!t)
        return 0;
    tok = (const ACCESS_TOKEN *)__atomic_load_n(&t->token, __ATOMIC_ACQUIRE);
    if (!tok)
        return 0;                    /* no token -> not proven elevated */
    if (!tok->IsElevated)
        return 0;                    /* token's own elevation flag must be set */
    if (!SeTryGetTokenIntegrityLevel(tok, &il))
        return 0;                    /* malformed / absent IL SID -> not proven */
    return il >= SECURITY_MANDATORY_HIGH_RID;
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
    /* AT_SECURE: an elevated process never observes a blocklisted name (s16). */
    if (env_is_secure_context(t) && env_name_is_privilege_sensitive(name, namelen))
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

    /* AT_SECURE: an elevated process reads a blocklisted name as absent (s16).
     * Checked before the lock -- env_is_secure_context reads only the (assign-once)
     * primary token, never t->environ. */
    if (env_is_secure_context(t) && env_name_is_privilege_sensitive(name, namelen))
        return ENV_ERR_NOTFOUND;

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

/* s16: physically strip every privilege-sensitive (blocklisted) variable from a
 * process's environment, for use at an elevation transition (the invocation
 * site -- UAC in-place token replacement and elevated-child env inheritance -- is
 * owned downstream; see TODO-22 s16 XREFs). Returns the number of variables
 * removed. Only the NAME is logged, never the value (which may hold a secret).
 *
 * ATOMICITY: all victims are detached in ONE locked compaction pass (two reads
 * of environ[] under a single mutex hold, no intervening unlock) so a concurrent
 * env_set cannot slip a blocklisted entry through a mid-scan window. Victim
 * pointers are retained in a kmalloc'd array (alloc under the sleeping mutex is
 * permitted, as env_build_block_utf16 documents; nv <= ENV_MAX_ENTRIES so the
 * array is <= 4 KiB) and the klog + env_str_free happen AFTER the unlock -- the
 * env module forbids klog (synchronous serial/framebuffer/disk I/O) under
 * environ_lock. On the rare victim-array OOM, victims are freed under the lock
 * (memory-safe) and a single count is logged instead of per-name lines. NOTE:
 * this establishes the postcondition at the point of the call; a full "no
 * blocklisted var may EVER exist while elevated" guarantee also needs env_set to
 * reject blocklisted names during the elevation transition, owned by TODO-15 s9
 * (the read gate above already masks any post-call residue while secure). */
int env_sanitize_for_elevation(struct task *t)
{
    char   **victims = NULL;
    uint32_t nv = 0, cap, rd, wr, i;
    int removed;

    if (!t)
        return 0;

    mutex_lock(&t->environ_lock);

    /* Pass 1: count blocklisted entries under the lock. */
    for (rd = 0; rd < t->environ_count; rd++) {
        const char *e = t->environ[rd];
        if (e && env_entry_is_privilege_sensitive(e))
            nv++;
    }
    if (nv == 0u) {
        mutex_unlock(&t->environ_lock);
        return 0;
    }

    /* Retain victims for post-unlock audit+free; NULL on OOM -> free under lock. */
    cap = nv;
    victims = (char **)kmalloc(cap * (uint32_t)sizeof(char *));

    /* Pass 2: single locked compaction. Keep non-victims (shift down), detach
     * victims (retain or free), fix the cached byte total and count. */
    nv = 0;
    wr = 0;
    for (rd = 0; rd < t->environ_count; rd++) {
        char *e = t->environ[rd];
        if (e && env_entry_is_privilege_sensitive(e)) {
            uint32_t vn = env_strlen(e) + 1u;
            t->environ_bytes -= vn;                 /* cached total: drop the victim */
            if (victims && nv < cap)
                victims[nv] = e;                    /* retain for post-unlock log+free */
            else
                env_str_free(e, vn);                /* OOM fallback: free under lock */
            nv++;
        } else {
            t->environ[wr++] = e;                   /* keep (compact down) */
        }
    }
    while (wr < t->environ_count)
        t->environ[wr++] = NULL;                    /* clear the freed tail */
    t->environ_count -= nv;
    removed = (int)nv;

    mutex_unlock(&t->environ_lock);

    /* Audit + free AFTER unlock. */
    if (victims) {
        for (i = 0; i < nv && i < cap; i++) {
            char *e = victims[i];
            char namebuf[ENV_NAME_MAX + 1];
            uint32_t keylen = env_entry_keylen(e);
            uint32_t k;
            if (keylen > ENV_NAME_MAX)
                keylen = ENV_NAME_MAX;              /* defensive; keys capped at set time */
            for (k = 0; k < keylen; k++)
                namebuf[k] = e[k];
            namebuf[keylen] = '\0';
            klog(LOG_WARN, "env", "sanitized privileged variable '%s' from elevated process",
                 namebuf);
            env_str_free(e, env_strlen(e) + 1u);
        }
        kfree(victims);
    } else {
        klog(LOG_WARN, "env",
             "sanitized %d privileged variable(s) from elevated process (name audit skipped: OOM)",
             removed);
    }

    return removed;
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
    /* AT_SECURE (s16): a secure SOURCE must not EXPORT a blocklisted name, so the
     * child never inherits it -- consistent with the read gate + block builders,
     * and defensive against a caller that forgets the elevated-child sanitize.
     * Compact the destination (write index `w` <= src index) and recompute the
     * cached byte total from the kept entries, since skipping breaks the exact
     * "same block bytes" equality. */
    {
        int secure = env_is_secure_context(s);
        uint32_t w = 0, kept_bytes = 0;
        for (i = 0; i < count; i++) {
            const char *e = s->environ[i];
            if (!e || (secure && env_entry_is_privilege_sensitive(e)))
                continue;
            arr[w] = env_strdup(e);
            if (!arr[w]) {
                uint32_t j;
                for (j = 0; j < w; j++)
                    if (arr[j])
                        env_str_free(arr[j], env_strlen(arr[j]) + 1);
                kfree(arr);
                mutex_unlock(&s->environ_lock);
                return ENV_ERR_NOMEM;
            }
            kept_bytes += env_strlen(e) + 1u;
            w++;
        }
        dst->environ = arr;
        dst->environ_count = w;
        dst->environ_bytes = kept_bytes;   /* recomputed: skipped entries excluded */
    }
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

/* Is `e` a storable "KEY=VALUE" entry? Must contain '=', carry a non-empty legal
 * name, and fit the name and value byte caps. SINGLE definition of well-formedness
 * for every path that publishes an entry: the lenient exec path skips what this
 * rejects, the strict adoption path refuses the whole block on it. One predicate is
 * what keeps "skipped by exec" and "refused for Rtl" the same set of inputs. */
static int env_entry_is_wellformed(const char *e)
{
    uint32_t elen, eq;

    if (!e)
        return 0;
    elen = env_strnlen(e, ENV_NAME_MAX + 1u + ENV_VALUE_MAX + 1u);
    if (elen == 0 || elen > ENV_NAME_MAX + 1u + ENV_VALUE_MAX)
        return 0;                       /* over-long or missing terminator */
    /* Split the name honoring a leading '=' hidden drive var; reject a name that is
     * empty, separator-less, or an illegal '='-name (only "=X:" is legal). */
    if (env_entry_key_span(e, elen, &eq) != 0)
        return 0;
    /* Enforce the name and value byte caps SEPARATELY, exactly as env_set does: the
     * combined-length check above still admits e.g. a 1-byte name with a 32,769-byte
     * value (total < NAME_MAX+1+VALUE_MAX), which env_set would reject. Storing an
     * over-ENV_VALUE_MAX value would let a reader (NtQueryEnvironmentVariable) walk
     * past its ENV_VALUE_MAX-sized buffer. */
    if (eq > ENV_NAME_MAX || (elen - eq - 1u) > ENV_VALUE_MAX)
        return 0;                       /* name or value over its byte cap */
    return 1;
}

/* Free an entry array built by env_prepare_entries, or an old store already detached
 * from its task: `n` string slots plus the array itself. */
static void env_free_entry_array(char **arr, uint32_t n)
{
    uint32_t i;

    if (!arr)
        return;
    for (i = 0; i < n; i++)
        if (arr[i])
            env_str_free(arr[i], env_strlen(arr[i]) + 1u);
    kfree(arr);
}

/* Build the publishable entry array from a kernel-side "KEY=VALUE" vector: deep-copy
 * every well-formed entry, sort by name, keep the last occurrence of each name, and
 * enforce the block-size cap. Touches NO task state and takes NO lock -- the array is
 * built completely before any caller acquires environ_lock, so a failure here cannot
 * disturb a live store. On ENV_OK the caller owns *out_arr (release with
 * env_free_entry_array); on failure nothing is allocated.
 *
 * POLICY: a malformed entry is SKIPPED, not rejected, so one bad envp entry cannot
 * poison a whole exec. That leniency is sound ONLY for the SYS_EXEC contract, where
 * the vector is assembled by the kernel and a skipped entry costs one variable. It is
 * NOT sound for an adoption path whose contract is all-or-nothing: silently dropping
 * entries from a caller-supplied block would replace a live environment with a
 * partial subset and report success. Such a caller MUST validate its block strictly
 * first (env_block_entries_strict), so no entry reaching here is ever malformed. */
static int env_prepare_entries(const char *const *entries, uint32_t count,
                               char ***out_arr, uint32_t *out_n,
                               uint32_t *out_bytes)
{
    char **arr;
    uint32_t n = 0;          /* count of accepted (well-formed) entries */
    uint32_t new_bytes = 0;  /* SUM(strlen(entry)+1) of the published set */
    uint32_t i;

    *out_arr = NULL;
    *out_n = 0;
    *out_bytes = 0;

    arr = (char **)kmalloc((count + 1u) * sizeof(char *));
    if (!arr)
        return ENV_ERR_NOMEM;
    for (i = 0; i <= count; i++)
        arr[i] = NULL;


    /* Deep-copy each well-formed "KEY=VALUE" entry. Malformed entries are skipped
     * so a bad envp cannot poison the whole exec (see the POLICY note above). */
    for (i = 0; i < count; i++) {
        const char *e = entries[i];
        if (!env_entry_is_wellformed(e))
            continue;
        arr[n] = env_strdup(e);
        if (!arr[n]) {
            env_free_entry_array(arr, n);
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
        if (!scratch) {
            env_free_entry_array(arr, n);
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
        env_free_entry_array(arr, n);
        return ENV_ERR_NOSPACE;
    }

    *out_arr = arr;
    *out_n = n;
    *out_bytes = new_bytes;
    return ENV_OK;
}

int env_adopt_block(struct task *t, const char *const *entries, uint32_t count)
{
    /* Exactly the NULL-PreviousEnvironment exchange: same validation, same sort/dedup,
     * same single-lock-span swap, old store freed here instead of handed back. Keeping
     * one primitive is what stops the two paths' entry policy from drifting apart. */
    return env_exchange_block(t, entries, count, NULL);
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

    /* Canonical Windows encode (inverse of CommandLineToArgvW for argv[1+];
     * argv[0] is encoded the same way here but Windows decodes the program name
     * specially, so an argv[0] with embedded quotes does not round-trip): inside a
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
 * CommandLineToArgvW command-line decode (TODO-22 s15)
 *
 * The inverse of argv_to_cmdline (above) for argv[1+] (argv[0] follows the
 * Windows program-name rule, not a general inverse). A single self-describing block
 * holds the whole result: a {magic,total_bytes} header, then the (argc+1)
 * NUL-terminated element-pointer array, then the argument strings. The returned
 * pointer is the pointer array so callers index it like argv[]; cmdline_free_argv
 * recovers the header (like env_create_block / env_destroy_block) since this
 * kernel has no LocalAlloc size bookkeeping.
 *
 * The parser body is macro-generated for both element widths (char and uint16_t)
 * so the UTF-8 core and the wide W form share ONE implementation with zero drift.
 * Because every SYNTAX character (space, tab, '"', '\\') is ASCII, the wide form
 * parses UTF-16 code units DIRECTLY and preserves every other WCHAR verbatim -- a
 * UTF-16<->UTF-8 transcode would mutate lone surrogates and mis-cap multibyte
 * scripts.
 * =========================================================================== */

#define CMDL_BLK_MAGIC 0x4C444D43u   /* 'CMDL' little-endian; header sentinel */

struct cmdl_hdr {
    uint32_t magic;        /* CMDL_BLK_MAGIC -- validated on free */
    uint32_t total_bytes;  /* whole-block size for env_str_free recovery */
};

/* Largest block either builder can produce: header + (argc+1) 8-byte pointer
 * slots + the argument bytes, worst case being the UTF-8 core (byte cap dominates
 * the WCHAR cap because argc <= units and sizeof(char)=1). cmdline_free_argv
 * rejects a header claiming more than this, so corrupted metadata cannot pick a
 * wrong free extent (mirrors env_destroy_block's wchar-cap guard). */
#define CMDL_BLK_MAX_BYTES \
    ((uint32_t)sizeof(struct cmdl_hdr) + (CMDL_ARGV_MAX_BYTES + 1u) * 8u + \
     CMDL_ARGV_MAX_BYTES)

/* An 8-byte header keeps the following pointer array 8-byte aligned: env_str_alloc
 * returns >= 16-byte-aligned storage on BOTH paths (kmalloc's own guarantee, and a
 * page-aligned PMM frame plus the 16-byte env_buf header -- s22), so hdr+1 is
 * 8-aligned and every char or uint16_t pointer slot lands naturally aligned. */
_Static_assert(sizeof(struct cmdl_hdr) == 8,
               "cmdl_hdr must be 8 bytes so the pointer array stays 8-aligned");

/* Free a whole cmdl block by its recovered total_bytes; poison the magic first so
 * an immediate double free is a no-op (best effort, not a lifetime guarantee). */
static void cmdl_free_block(struct cmdl_hdr *hdr)
{
    uint32_t tb = hdr->total_bytes;
    hdr->magic = 0u;
    env_str_free((char *)hdr, tb);
}

/* Emit one element to `strbuf` (or just count when strbuf==NULL), honoring the
 * per-width unit cap. Used only inside the generated parser bodies, where `pos`,
 * `cap`, `strbuf` and `out_units` are locals; on overflow it records the count so
 * far and returns -1 from the enclosing parser (both passes agree on the cap). */
#define CMDL_PUT(c) do {                                   \
        unsigned _cv = (unsigned)(c);   /* evaluate side effects even when */  \
        if (pos >= cap) { if (out_units) *out_units = pos; return -1; }        \
        if (strbuf) strbuf[pos] = _cv;  /* sizing (strbuf==NULL) skips store */ \
        pos++;                                             \
    } while (0)

/* Generate a width-specific parser. cmd is the command line; arg0!=NULL forces
 * argv[0] to that string (the empty-cmdline module-path case) and stops. strbuf
 * and argv are NULL for the sizing pass and non-NULL for the fill pass; on the
 * fill pass argv[i] receives &strbuf[start-of-arg-i]. Returns argc, or -1 if the
 * decoded strings would exceed the cap. Windows parse rules (see the header). */
/* Record the start of argument `argc`, bounded by argv_cap so a divergent fill
 * pass can never write past the pointer array (used only in the fill pass, where
 * argv != NULL; the sizing pass passes argv == NULL and this is a no-op). */
#define CMDL_ARG_START() do {                              \
        if (argv) {                                        \
            if ((uint32_t)argc >= argv_cap) return -1;     \
            argv[argc] = &strbuf[pos];                     \
        }                                                  \
    } while (0)

#define CMDL_GEN_PARSER(SUF, CH)                                               \
static int cmdl_parse_##SUF(const CH *cmd, const CH *arg0, CH *strbuf,          \
                            CH **argv, uint32_t argv_cap, uint32_t cap,         \
                            uint32_t *out_units)                                \
{                                                                              \
    uint32_t pos = 0;                                                          \
    int argc = 0;                                                             \
    int in_q;                                                                \
    const CH *p = cmd;                                                        \
    if (arg0) {                                                               \
        const CH *s = arg0;                                                   \
        CMDL_ARG_START();                                                    \
        while (*s) CMDL_PUT(*s++);                                            \
        CMDL_PUT((CH)0);                                                      \
        argc++;                                                              \
        if (out_units) *out_units = pos;                                     \
        return argc;                                                          \
    }                                                                         \
    /* argv[0]: quote-delimited if it opens with '"', else whitespace, and     \
     * backslashes are literal inside it (no escape processing). */            \
    CMDL_ARG_START();                                                        \
    if (*p == (CH)'"') {                                                      \
        p++;                                                                 \
        while (*p && *p != (CH)'"') CMDL_PUT(*p++);                          \
        if (*p == (CH)'"') p++;                                              \
    } else {                                                                 \
        while (*p && *p != (CH)' ' && *p != (CH)'\t') CMDL_PUT(*p++);        \
    }                                                                        \
    CMDL_PUT((CH)0);                                                          \
    argc++;                                                                  \
    /* argv[1..]: backslash/quote state machine. */                           \
    for (;;) {                                                                \
        while (*p == (CH)' ' || *p == (CH)'\t') p++;                         \
        if (*p == 0) break;                                                  \
        CMDL_ARG_START();                                                    \
        in_q = 0;                                                            \
        for (;;) {                                                            \
            uint32_t nbs = 0, k;                                             \
            while (*p == (CH)'\\') { p++; nbs++; }                           \
            if (*p == (CH)'"') {                                             \
                for (k = 0; k < nbs / 2u; k++) CMDL_PUT((CH)'\\');           \
                if (nbs & 1u) {                                              \
                    CMDL_PUT((CH)'"'); p++;      /* 2n+1: literal quote */   \
                } else if (in_q && p[1] == (CH)'"') {                        \
                    /* "" inside quotes: one literal quote, then CLOSE the    \
                     * quoted region (Windows modulo-3 consecutive-quote      \
                     * rule) so the next whitespace delimits the argument. */ \
                    CMDL_PUT((CH)'"'); p += 2; in_q = 0;                     \
                } else {                                                     \
                    in_q = !in_q; p++;           /* 2n: toggle in-quotes */  \
                }                                                            \
            } else if (*p == 0) {                                            \
                for (k = 0; k < nbs; k++) CMDL_PUT((CH)'\\');                \
                break;                                                       \
            } else if (!in_q && (*p == (CH)' ' || *p == (CH)'\t')) {         \
                for (k = 0; k < nbs; k++) CMDL_PUT((CH)'\\');                \
                break;                                                       \
            } else {                                                         \
                for (k = 0; k < nbs; k++) CMDL_PUT((CH)'\\');                \
                CMDL_PUT(*p++);                                              \
            }                                                                \
        }                                                                    \
        CMDL_PUT((CH)0);                                                      \
        argc++;                                                              \
    }                                                                        \
    if (out_units) *out_units = pos;                                        \
    return argc;                                                             \
}

CMDL_GEN_PARSER(u8,  char)
CMDL_GEN_PARSER(u16, uint16_t)

/* Build a width-specific argv block. First bound the RAW input length (require a
 * terminator within MAXCAP) and SNAPSHOT the command line into immutable kernel
 * memory, so both passes read identical bytes -- a concurrent mutation of the
 * caller's buffer can no longer make pass 2 diverge from pass 1 and overrun the
 * result block. Then size (pass 1), allocate one self-describing block, fill
 * (pass 2, bounded by BOTH the exact pass-1 unit count AND the argc+1 pointer-slot
 * count), and cross-check pass 2 reproduced pass 1 exactly. */
#define CMDL_GEN_BUILD(SUF, CH, MAXCAP)                                        \
static CH **cmdl_build_##SUF(const CH *cmd_in, const CH *arg0, int *out_argc)   \
{                                                                             \
    int argc, argc2;                                                         \
    uint32_t units = 0, units2 = 0, ptr_bytes, clen = 0, snap_bytes, i;      \
    uint64_t total64;                                                        \
    uint32_t total;                                                          \
    struct cmdl_hdr *hdr;                                                    \
    char *base;                                                             \
    CH **argv;                                                              \
    CH *strbuf, *snap;                                                      \
    while (clen < (MAXCAP) && cmd_in[clen] != 0)                             \
        clen++;                                                             \
    if (cmd_in[clen] != 0)                                                   \
        return (CH **)0;                    /* raw source exceeds the cap */ \
    snap_bytes = (clen + 1u) * (uint32_t)sizeof(CH);                        \
    snap = (CH *)env_str_alloc(snap_bytes);                                 \
    if (!snap)                                                               \
        return (CH **)0;                                                     \
    for (i = 0; i < clen; i++)                                               \
        snap[i] = cmd_in[i];                                                \
    snap[clen] = 0;    /* force the terminator INDEPENDENTLY of the mutable  \
                        * source: a concurrent write flipping cmd_in[clen]    \
                        * after the length scan cannot un-terminate the snap */\
    argc = cmdl_parse_##SUF(snap, arg0, (CH *)0, (CH **)0, 0u, (MAXCAP), &units); \
    if (argc < 0) {                                                          \
        env_str_free((char *)snap, snap_bytes);                            \
        return (CH **)0;                    /* over the unit cap */          \
    }                                                                       \
    ptr_bytes = ((uint32_t)argc + 1u) * (uint32_t)sizeof(CH *);             \
    total64 = (uint64_t)sizeof(struct cmdl_hdr) + (uint64_t)ptr_bytes       \
            + (uint64_t)units * (uint64_t)sizeof(CH);                       \
    if (total64 > 0xFFFFFF00ull) {                                          \
        env_str_free((char *)snap, snap_bytes);                            \
        return (CH **)0;                    /* keep total under UINT32 */    \
    }                                                                       \
    total = (uint32_t)total64;                                              \
    hdr = (struct cmdl_hdr *)env_str_alloc(total);                          \
    if (!hdr) {                                                              \
        env_str_free((char *)snap, snap_bytes);                            \
        return (CH **)0;                                                     \
    }                                                                       \
    hdr->magic = CMDL_BLK_MAGIC;                                            \
    hdr->total_bytes = total;                                               \
    base = (char *)(hdr + 1);                                               \
    argv = (CH **)base;                                                     \
    strbuf = (CH *)(base + ptr_bytes);                                      \
    argc2 = cmdl_parse_##SUF(snap, arg0, strbuf, argv,                       \
                             (uint32_t)argc + 1u, units, &units2);           \
    env_str_free((char *)snap, snap_bytes);                                 \
    if (argc2 != argc || units2 != units) {                                 \
        cmdl_free_block(hdr);                                              \
        return (CH **)0;                                                     \
    }                                                                       \
    argv[argc] = (CH *)0;                   /* NULL-terminate the vector */  \
    if (out_argc) *out_argc = argc;                                        \
    return argv;                                                            \
}

CMDL_GEN_BUILD(u8,  char,     CMDL_ARGV_MAX_BYTES)
CMDL_GEN_BUILD(u16, uint16_t, CMDL_ARGV_MAX_WCHARS)

char **cmdline_to_argv(struct task *caller, const char *cmdline, int *out_argc)
{
    const char *arg0;

    if (out_argc)
        *out_argc = 0;
    if (!cmdline)
        return (char **)0;
    /* Empty command line -> a single argument that is the caller's module
     * identity (Win32 returns the executable path). The full kernel-owned
     * ImagePathName is deferred with SearchPathW (see env_searchpath.h); the
     * stable caller->name is the identity available today and is returned to the
     * caller itself, not used as any privileged path. */
    arg0 = (cmdline[0] == '\0')
             ? ((caller && caller->name) ? caller->name : "")
             : (const char *)0;
    return cmdl_build_u8(cmdline, arg0, out_argc);
}

/* Set the executing thread's TEB LastErrorValue when a live TEB exists (a fixture
 * test task has none -> no-op). Mirrors env_searchpath.c's propagation. */
static void cmdl_set_last_error(uint32_t code)
{
    struct thread *thr = thread_current();
    if (thr && thr->teb)
        ((TEB *)thr->teb)->LastErrorValue = code;
}

uint16_t **CommandLineToArgvW(struct task *caller, const uint16_t *lpCmdLine,
                              int *pNumArgs)
{
    uint16_t **argv;

    /* Win32: pNumArgs is REQUIRED. A NULL count pointer is a caller error --
     * return NULL + ERROR_INVALID_PARAMETER rather than allocating a vector the
     * caller cannot size. */
    if (!pNumArgs) {
        cmdl_set_last_error(ERROR_INVALID_PARAMETER);
        return (uint16_t **)0;
    }
    *pNumArgs = 0;
    if (!lpCmdLine)
        return (uint16_t **)0;   /* Win32: NULL command line -> NULL result */

    if (lpCmdLine[0] == 0) {
        /* Empty -> argv[0] = caller module identity widened to UTF-16. Size the
         * conversion first; a name that does not fit (or a conversion error)
         * returns NULL, never a silently-empty argv[0] (which the UTF-8 core
         * never produces). A 512-WCHAR buffer covers any realistic module name;
         * the true kernel-owned image path is deferred (see env_searchpath.h). */
        uint16_t namew[512];
        int nl = 0;
        static const uint16_t empty_w[1] = { 0 };
        if (caller && caller->name) {
            uint32_t nb = env_strlen(caller->name);
            int need = nls_cp_utf8_to_utf16((const uint8_t *)caller->name, nb,
                                            (uint16_t *)0, 0, NLS_CP_REPLACE);
            if (need < 0 ||
                (uint32_t)need >= sizeof(namew) / sizeof(namew[0])) {
                cmdl_set_last_error(ERROR_INVALID_PARAMETER);
                return (uint16_t **)0;   /* name too long / not convertible */
            }
            nl = nls_cp_utf8_to_utf16((const uint8_t *)caller->name, nb, namew,
                                      sizeof(namew) / sizeof(namew[0]) - 1u,
                                      NLS_CP_REPLACE);
            if (nl < 0) {
                cmdl_set_last_error(ERROR_INVALID_PARAMETER);
                return (uint16_t **)0;
            }
        }
        namew[nl] = 0;
        argv = cmdl_build_u16(empty_w, namew, pNumArgs);
    } else {
        argv = cmdl_build_u16(lpCmdLine, (const uint16_t *)0, pNumArgs);
    }

    if (!argv)
        cmdl_set_last_error(ERROR_OUTOFMEMORY);
    return argv;
}

void cmdline_free_argv(void *argv)
{
    struct cmdl_hdr *hdr;

    if (!argv)
        return;
    /* CONTRACT: `argv` MUST be a value returned by cmdline_to_argv /
     * CommandLineToArgvW, freed exactly once. The checks below best-effort reject
     * an obviously-bad header; they are not a foreign-pointer validator. */
    hdr = (struct cmdl_hdr *)argv - 1;
    if (hdr->magic != CMDL_BLK_MAGIC ||
        hdr->total_bytes < sizeof(struct cmdl_hdr) ||
        hdr->total_bytes > CMDL_BLK_MAX_BYTES)
        return;   /* corrupted / impossible size -> refuse to pick a free extent */
    cmdl_free_block(hdr);
}

#undef CMDL_PUT
#undef CMDL_ARG_START
#undef CMDL_GEN_PARSER
#undef CMDL_GEN_BUILD
#undef CMDL_BLK_MAGIC
#undef CMDL_BLK_MAX_BYTES

/* ===========================================================================
 * %VAR% expansion (single-pass; cmd.exe / Win32 ExpandEnvironmentStrings)
 *
 * env_expand walks `input` once and substitutes each `%NAME%` with its value.
 * Substitution is single-pass by design: an expanded value that itself contains
 * `%OTHER%` is NOT recursively re-expanded, so there is no depth limit and no
 * infinite-loop risk (delayed `!VAR!` re-expansion is a separate cmd.exe mode).
 * =========================================================================== */

/* Append one byte. Truncation is NOT this function's job any more (s22): every
 * caller now gates on capacity (out + 1 < max_len) BEFORE charging the work budget
 * and sets `trunc` itself, because charging first let a budget expiring on exactly
 * the filling byte convert a TRUNCATION into an over-budget refusal. With capacity
 * owned by the loops, the emitter just emits. */
static void env_exp_put(char *out, uint32_t *pos, char c)
{
    out[(*pos)++] = c;
}

int env_expand(struct task *t, const char *input, char *output, uint32_t max_len)
{
    return env_expand_budget(t, input, output, max_len, ENV_EXPAND_WORK_MAX);
}

int env_expand_budget(struct task *t, const char *input, char *output,
                      uint32_t max_len, uint64_t work_max)
{
    uint32_t in = 0, out = 0;
    int trunc = 0;
    uint64_t work = 0;   /* bytes examined; capped at work_max (s22) */

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
     * input (kernel-resident, NUL-terminated by contract) then compare ranges.
     *
     * The measure is BUDGETED: a plain env_strlen would walk an arbitrarily long
     * input for free, which is the exact uncapped scan this budget exists to close.
     * Charge every byte, and if the extent cannot be established within budget,
     * refuse WITHOUT touching output -- an alias past the scanned prefix cannot be
     * ruled out, so the same caution the overlap rejection takes applies. */
    {
        uint32_t inlen = 0;
        uintptr_t i0, i1, o0, o1;
        while (input[inlen]) {
            if (++work > work_max) {
                klog(LOG_WARN, "env", "env_expand: over budget %u at measure (pid %u)",
                     (unsigned)work_max, (unsigned)t->pid);
                return ENV_EXPAND_OVER_BUDGET;   /* both buffers untouched */
            }
            inlen++;
        }
        i0 = (uintptr_t)input; i1 = i0 + inlen + 1u;
        o0 = (uintptr_t)output; o1 = o0 + max_len;
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
        /* Charge before the work, so a refusal lands BEFORE the cycles are burnt.
         * EVERY unbounded loop below charges too: the closing-'%' scan, the
         * no-closing-'%' remainder copy, and the value copy each walk
         * caller-controlled or environment-controlled lengths, so budgeting only
         * this outer step would leave three uncapped paths. */
        /* Output is full: every remaining byte would be discarded by env_exp_put,
         * so scanning on changes neither `output` nor the returned max_len sentinel
         * -- it only holds environ_lock longer. Stopping here bounds the real work
         * by the OUTPUT capacity rather than the input length, which is the bound
         * that actually matters (the budget is the backstop above it). Tested
         * BEFORE the charge: an already-final result must never be turned into an
         * over-budget refusal by an iteration that could not have changed it. */
        if (trunc)
            break;
        if (c != '%') {
            /* Capacity BEFORE the charge: a verbatim byte always emits, so once
             * output is final this byte is a truncation, and charging first would
             * let a budget expiring on exactly this byte report a refusal instead.
             * (Deliberately NOT hoisted above the '%' branch: a `%EMPTY%` emits
             * nothing, so a full output does not imply truncation there.) */
            if (out + 1u >= max_len) {
                trunc = 1;
                break;
            }
            if (++work > work_max)
                goto over_budget;
            env_exp_put(output, &out, c);   /* verbatim */
            in++;
            continue;
        }
        if (++work > work_max)
            goto over_budget;
        /* Win32/ntdll ExpandEnvironmentStrings semantics: `%%` is NOT an escape.
         * An empty name (`%%`) is an unresolved variable, so both percent signs
         * are preserved verbatim by the empty-name -> literal branch below.
         * cmd.exe's `%%` -> `%` batch escape is a distinct shell mode (owned by
         * the pseudo-variable / delayed-expansion section), not this primitive. */
        /* Scan for the closing '%'. */
        {
            uint32_t j = in + 1;
            while (input[j] && input[j] != '%') {
                if (++work > work_max)
                    goto over_budget;
                j++;
            }
            if (input[j] != '%') {
                /* No closing '%': copy the remainder verbatim and stop. Capacity
                 * gates the loop for the same reason as the copies above. */
                while (input[in] && out + 1u < max_len) {
                    if (++work > work_max)
                        goto over_budget;
                    env_exp_put(output, &out, input[in]);
                    in++;
                }
                if (input[in])
                    trunc = 1;                  /* remainder did not fit */
                break;
            }
            {
                uint32_t namelen = j - (in + 1);
                const char *val = NULL;
                if (namelen >= 1 && namelen <= ENV_NAME_MAX) {
                    char nb[ENV_NAME_MAX + 1];
                    uint32_t k;
                    /* Charge the LOOKUP before doing it. env_peek_locked runs a
                     * binary search whose comparisons are invisible to the
                     * per-input-byte charge; left uncharged, the budget would not
                     * bound the mutex hold time at all -- the entire point of it.
                     * Each of the <= ENV_LOOKUP_CMP_MAX comparisons costs the
                     * STORED key scan (env_entry_keylen walks to the '=', up to
                     * ENV_NAME_MAX) PLUS the compare itself (up to namelen). The
                     * stored-key term is what makes this a true upper bound: a
                     * ONE-byte miss against 256-byte keys really inspects ~257
                     * bytes per comparison, so charging namelen alone under-counts
                     * by ~250x. The trailing namelen is the name copy below. */
                    {
                        uint64_t cost = (uint64_t)ENV_LOOKUP_CMP_MAX *
                                            ((uint64_t)ENV_NAME_MAX + namelen) + namelen;
                        if (work + cost > work_max)
                            goto over_budget;
                        work += cost;
                    }
                    for (k = 0; k < namelen; k++)
                        nb[k] = input[in + 1 + k];
                    nb[namelen] = '\0';
                    val = env_peek_locked(t, nb);            /* borrowed, under lock */
                }
                /* Both copies below stop the instant `output` fills. Without that,
                 * a long value or literal keeps charging AFTER the result is
                 * already final, so a budget that runs out mid-substitution would
                 * report ENV_EXPAND_OVER_BUDGET and erase a result that is properly
                 * a TRUNCATION -- turning a max_len return into -1 and discarding
                 * bytes the caller is entitled to. Truncation wins over the budget:
                 * once out is full, no remaining work can change the answer. */
                if (val) {
                    uint32_t v = 0;
                    while (val[v] && out + 1u < max_len) {
                        if (++work > work_max)
                            goto over_budget;
                        env_exp_put(output, &out, val[v]);
                        v++;
                    }
                    if (val[v])
                        trunc = 1;              /* value did not fit */
                } else {
                    /* Unknown or over-long name: copy the literal `%NAME%`. */
                    uint32_t p;
                    for (p = in; p <= j && out + 1u < max_len; p++) {
                        if (++work > work_max)
                            goto over_budget;
                        env_exp_put(output, &out, input[p]);
                    }
                    if (p <= j)
                        trunc = 1;              /* literal did not fit */
                }
                in = j + 1;
            }
        }
    }
    env_unlock(t);

    output[out] = '\0';
    return trunc ? (int)max_len : (int)out;

    /* Single refusal exit for every budget check above. Placed past the normal
     * return so the success path cannot fall into it. The empty output is
     * deliberate: a caller that ignores the return value gets nothing rather than
     * a silently truncated expansion it would treat as the real result. */
over_budget:
    env_unlock(t);
    output[0] = '\0';
    klog(LOG_WARN, "env", "env_expand: over work budget %u (pid %u)",
         (unsigned)work_max, (unsigned)t->pid);
    return ENV_EXPAND_OVER_BUDGET;
}

/* ===========================================================================
 * UTF-16 environment-block builder (for RtlExpandEnvironmentStrings_U's
 * NULL-Environment "calling process's own block" path)
 * =========================================================================== */

/* Core of env_build_block_utf16: REQUIRES t->environ_lock to be HELD by the caller
 * and never touches it, so a caller already inside an environ_lock span can encode
 * the exact store it is about to replace. env_build_block_utf16 is the locking
 * wrapper and the only other caller.
 *
 * WHY THE SPLIT: an encoder that takes the lock itself cannot serve a transactional
 * exchange. The old-store snapshot would have to be built AFTER the swap released the
 * lock, so an allocation failure there would leave the new environment published with
 * no snapshot to return -- unwindable only by a second swap racing every other
 * writer. Encoding inside the swap's own lock span keeps "capture the exact old store"
 * and "publish the new one" a single serialized step, and lets a failed snapshot abort
 * before anything is published. */
static int env_build_block_utf16_locked(struct task *t, uint16_t **out_block,
                                        uint32_t *out_wchars, uint32_t max_wchars,
                                        int apply_secure_filter)
{
    uint32_t total, i, w, nentries;
    char *raw;
    uint16_t *blk;
    int secure;

    if (!t || !out_block || !out_wchars)
        return ENV_ERR_INVAL;
    *out_block = NULL;
    *out_wchars = 0;

    /* Size, allocate, and convert all inside the caller's lock span so a sibling
     * env_set/env_unset cannot change the block between the sizing and conversion
     * passes (both feed nls_cp_utf8_to_utf16 the same bytes). Allocation under a
     * MUTEX is permitted (env is a sleeping lock, not a spinlock). NOTE: for a
     * block above ENV_STR_KMALLOC_MAX, env_str_alloc routes to
     * pmm_alloc_contiguous, whose bitmap/accounting is NOT yet SMP-locked (a
     * pre-existing kernel-wide gap; kmalloc's s_heap_lock covers the small-block
     * path). Tracked for a real physical-allocator lock. */

    /* AT_SECURE: an elevated task's serialized block omits blocklisted names, so
     * a child receiving this block (CreateEnvironmentBlock / Rtl expansion) cannot
     * recover LD_PRELOAD et al. Constant for the whole build; both passes agree.
     * The filter is CALLER-SELECTED because it must NOT apply to a same-process
     * PreviousEnvironment restore token: that snapshot goes back to the very task
     * that already held those vars, so filtering it exposes nothing new and would
     * silently drop physically-present entries when the token is restored. */
    secure = apply_secure_filter ? env_is_secure_context(t) : 0;

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
            if (!e || (secure && env_entry_is_privilege_sensitive(e)))
                continue;
            need = nls_cp_utf8_to_utf16((const uint8_t *)e, env_strlen(e),
                                        (uint16_t *)0, 0u, NLS_CP_REPLACE);
            if (need < 0)
                return ENV_ERR_INVAL;             /* only on a bad mode/NULL -- guarded */
            total += (uint32_t)need + 1u;         /* entry WCHARs + its NUL */
            nentries++;
            /* Bail the instant the block exceeds the cap rather than sizing every
             * remaining entry under the lock (a pathological environ could be
             * tens of MiB); sibling env ops should not wait on a doomed build. */
            if (total > max_wchars)
                return ENV_ERR_NOSPACE;
        }
    }
    /* A non-empty block already ends "...\0\0" (last entry's NUL + the trailing
     * NUL). An empty environment would otherwise be a single NUL, violating the
     * NT double-NUL block contract -- emit two NULs so it is "\0\0". */
    if (nentries == 0u)
        total = 2u;
    if (total > max_wchars)
        return ENV_ERR_NOSPACE;

    raw = env_str_alloc(total * 2u);              /* wchars -> bytes */
    if (!raw)
        return ENV_ERR_NOMEM;
    blk = (uint16_t *)raw;

    w = 0;
    if (t->environ) {
        for (i = 0; i < t->environ_count; i++) {
            const char *e = t->environ[i];
            int got;
            if (!e || (secure && env_entry_is_privilege_sensitive(e)))
                continue;
            got = nls_cp_utf8_to_utf16((const uint8_t *)e, env_strlen(e),
                                       &blk[w], total - w, NLS_CP_REPLACE);
            if (got < 0) {                        /* sizing guaranteed room; defensive */
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

    *out_block = blk;
    *out_wchars = w;                              /* == total */
    return ENV_OK;
}

int env_build_block_utf16(struct task *t, uint16_t **out_block,
                          uint32_t *out_wchars, uint32_t max_wchars)
{
    int rc;

    /* `t` is dereferenced by the lock itself, so it is checked before the acquire;
     * the core re-checks the out params for its other caller. */
    if (!t)
        return ENV_ERR_INVAL;

    mutex_lock(&t->environ_lock);
    rc = env_build_block_utf16_locked(t, out_block, out_wchars, max_wchars,
                                      1 /* child/inheritance block: apply AT_SECURE filter */);
    mutex_unlock(&t->environ_lock);
    return rc;
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
    int secure;

    if (!t || !out_len)
        return ENV_ERR_INVAL;
    *out_len = 0;

    mutex_lock(&t->environ_lock);

    /* AT_SECURE: omit blocklisted names from an elevated task's block (s16). */
    secure = env_is_secure_context(t);

    if (is_unicode) {
        uint32_t wchars = 1u;              /* trailing block terminator wchar */
        uint32_t nent = 0;
        uint32_t need, cap, w;
        uint16_t *blk;
        if (t->environ) {
            for (i = 0; i < t->environ_count; i++) {
                const char *e = t->environ[i];
                int nn;
                if (!e || (secure && env_entry_is_privilege_sensitive(e)))
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
                if (!e || (secure && env_entry_is_privilege_sensitive(e)))
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
                if (!e || (secure && env_entry_is_privilege_sensitive(e)))
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
                if (!e || (secure && env_entry_is_privilege_sensitive(e)))
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
 * userenv.dll CreateEnvironmentBlock / DestroyEnvironmentBlock (TODO-22 s13)
 *
 * A created block is SELF-DESCRIBING: env_str_alloc gives back a size-class the
 * free path derives purely from the byte count, but that byte count is not stored
 * inline, and the Win32 DestroyEnvironmentBlock signature carries only the block
 * pointer (LocalAlloc tracks its own size on real Windows; this kernel has no
 * LocalAlloc). So a fixed-size header holding the wchar count precedes the body;
 * env_destroy_block recovers the total byte count from it. The body pointer we
 * hand back is the header + 8 bytes, so it stays 2-byte aligned for the UTF-16
 * block regardless of the header size.
 * =========================================================================== */

#define ENV_BLK_MAGIC 0x424E5645u   /* 'EVNB' little-endian; header sentinel */

struct env_block_hdr {
    uint32_t magic;    /* ENV_BLK_MAGIC -- validated on destroy */
    uint32_t wchars;   /* body length in WCHARs (env_str_free size recovery) */
};

/* Same 8-byte/alignment contract cmdl_hdr asserts, and for the same reason: the body
 * handed out is `hdr + 1`, so an 8-byte header keeps every uint16_t in it naturally
 * aligned on top of env_str_alloc's >= 16-byte-aligned payload (s22 puts the env_buf
 * header underneath, which preserves that). This assert was missing while its
 * cmdl_hdr sibling had one -- an asymmetry, not a deliberate exemption. */
_Static_assert(sizeof(struct env_block_hdr) == 8,
               "env_block_hdr must be 8 bytes so the WCHAR body stays 2-aligned");

/* Copy `wchars` WCHARs of `src` into a fresh self-describing allocation and hand
 * back the BODY pointer. SINGLE constructor for the env_destroy_block format: every
 * entry that produces a block goes through here, so the header layout, the magic,
 * and the body offset cannot drift between them. Returns ENV_OK or ENV_ERR_NOMEM.
 * `wchars` is bounded by ENV_CREATE_BLOCK_MAX_WCHARS by every caller, so
 * total_bytes cannot overflow uint32_t. */
static int env_wrap_block(const uint16_t *src, uint32_t wchars, void **out_block)
{
    uint32_t total_bytes = (uint32_t)sizeof(struct env_block_hdr) + wchars * 2u;
    struct env_block_hdr *hdr = (struct env_block_hdr *)env_str_alloc(total_bytes);
    uint16_t *body;

    if (!hdr)
        return ENV_ERR_NOMEM;
    hdr->magic = ENV_BLK_MAGIC;
    hdr->wchars = wchars;
    body = (uint16_t *)(hdr + 1);
    /* Scalar libc memcpy, not memcpy_fast: this runs in arbitrary kernel context and
     * memcpy_fast's SIMD path would need FPU-state protection. A block reaches
     * ENV_CREATE_BLOCK_MAX_WCHARS (128 KiB), where a per-WCHAR loop is pure waste. */
    memcpy(body, src, (uint64_t)wchars * sizeof(*body));
    *out_block = (void *)body;
    return ENV_OK;
}

/* Snapshot the CURRENT store into a wrapped block in the same form env_create_block
 * hands out, so the result is freeable by env_destroy_block. REQUIRES environ_lock
 * HELD: the snapshot must be the exact store the caller is about to replace. */
static int env_snapshot_wrapped_locked(struct task *t, void **out_block)
{
    uint16_t *blk;
    uint32_t wchars;
    int rc;

    /* apply_secure_filter = 0: an EXACT snapshot. This is a same-process restore token
     * (env_exchange_block's out_old / RtlSetCurrentEnvironment's PreviousEnvironment),
     * not a child-inheritance block, so the AT_SECURE filter must NOT strip entries the
     * old store physically held -- else restoring the token permanently loses them. */
    rc = env_build_block_utf16_locked(t, &blk, &wchars,
                                      ENV_CREATE_BLOCK_MAX_WCHARS, 0);
    if (rc != ENV_OK)
        return rc;
    rc = env_wrap_block(blk, wchars, out_block);
    env_free_block_utf16(blk, wchars);
    return rc;
}

int env_exchange_block(struct task *t, const char *const *entries,
                       uint32_t count, void **out_old)
{
    char **arr = NULL;       /* prepared new store (NULL == clear to empty) */
    char **old_env;
    uint32_t old_count;
    uint32_t n = 0;
    uint32_t new_bytes = 0;
    uint32_t old_total = 0;
    int rc;

    if (out_old)
        *out_old = NULL;
    if (!t)
        return ENV_ERR_INVAL;

    /* Over-cap is a hard error, checked BEFORE the clear path so a malformed
     * (entries==NULL, count>cap) call is rejected uniformly rather than silently
     * clearing (SYS_EXEC already caps envc at this bound). */
    if (count > ENV_MAX_ENTRIES)
        return ENV_ERR_NOSPACE;

    /* Build the whole new store first. A NULL vector or a zero count clears the
     * environment, which needs no preparation: arr stays NULL. */
    if (entries && count > 0u) {
        rc = env_prepare_entries(entries, count, &arr, &n, &new_bytes);
        if (rc != ENV_OK)
            return rc;
    }

    mutex_lock(&t->environ_lock);

    /* The old-store snapshot is built INSIDE the span that publishes the swap, and
     * BEFORE it: the block handed back is then provably the store this call replaced
     * and not one a racing writer installed in between. Failing here aborts the whole
     * exchange with the prior environment still live and published. */
    if (out_old) {
        rc = env_snapshot_wrapped_locked(t, out_old);
        if (rc != ENV_OK) {
            mutex_unlock(&t->environ_lock);
            env_free_entry_array(arr, n);
            return rc;
        }
    }

    old_env = t->environ;
    old_count = t->environ_count;
    old_total = env_block_bytes_locked(t);   /* prior block bytes (old cached total) */
    t->environ = arr;                        /* NULL on the clear path */
    t->environ_count = n;
    t->environ_bytes = new_bytes;            /* clearing MUST reset the cached total */
    mutex_unlock(&t->environ_lock);

    /* The old array is unreachable now, so it is freed outside the lock. */
    env_free_entry_array(old_env, old_count);
    env_warn_block_crossing(t, old_total, new_bytes + 1u);
    return ENV_OK;
}

int env_replace_from_block_utf16(struct task *t, const uint16_t *body,
                                 uint32_t wchars, void **out_old)
{
    const char **list;
    char **owned;                          /* temp UTF-8 entry copies to free */
    uint32_t n = 0, i;
    uint32_t cum = 1u;                     /* cumulative block bytes (+ terminator) */
    uint32_t p = 0;
    int rc = ENV_OK;

    if (out_old)
        *out_old = NULL;
    if (!t || !body)
        return ENV_ERR_INVAL;

    /* STRICT gate -- everything below is validated BEFORE a single entry is published,
     * because this path's contract is all-or-nothing: it either installs the caller's
     * block entire or leaves the live environment exactly as it was. A lenient scan
     * (skip what does not parse, adopt the rest) would report success after replacing
     * the environment with a silently truncated subset.
     *
     * The terminator check is what makes the decode loop below safe: a block whose
     * last two WCHARs are NUL cannot have a final entry that runs to the extent, so
     * the scan can never mistake "ran out of buffer" for "reached the block end". */
    if (wchars < 2u)
        return ENV_ERR_INVAL;              /* below the two-WCHAR "\0\0" empty form */
    if (wchars > ENV_CREATE_BLOCK_MAX_WCHARS)
        return ENV_ERR_NOSPACE;
    if (body[wchars - 1u] != 0u || body[wchars - 2u] != 0u)
        return ENV_ERR_INVAL;              /* no double-NUL terminator within the count */

    /* list[] + owned[] hold one extra slot for the NULL terminator; (511+1)*8 ==
     * 4096 bytes, at the kmalloc ceiling. */
    list  = (const char **)kmalloc((ENV_MAX_ENTRIES + 1u) * sizeof(char *));
    owned = (char **)kmalloc((ENV_MAX_ENTRIES + 1u) * sizeof(char *));
    if (!list || !owned) {
        if (list)  kfree(list);
        if (owned) kfree(owned);
        return ENV_ERR_NOMEM;
    }

    while (p < wchars) {
        uint32_t q = p;
        int need;
        char *u8;

        while (q < wchars && body[q] != 0)
            q++;
        if (q == p)
            break;                         /* empty entry -> verified block end */
        if (n >= ENV_MAX_ENTRIES) {
            rc = ENV_ERR_NOSPACE;          /* too many entries -> refuse the block */
            goto done;
        }
        /* STRICT decode, not REPLACE: an all-or-nothing decode+copy must REFUSE a
         * malformed UTF-16 unit (unpaired surrogate), never silently rewrite it to
         * U+FFFD -- that would publish an environment whose bytes differ from the
         * counted block the caller handed in. A bad unit returns need < 0 here. */
        need = nls_cp_utf16_to_utf8(&body[p], q - p, (uint8_t *)0, 0u,
                                    NLS_CP_STRICT);
        if (need < 0 || (uint32_t)need > ENV_PARSE_ENTRY_MAX) {
            rc = ENV_ERR_INVAL;            /* malformed/over-cap entry -> refuse, never skip */
            goto done;
        }
        if (cum + (uint32_t)need + 1u > ENV_BLOCK_MAX) {
            rc = ENV_ERR_NOSPACE;          /* cumulative block cap -> refuse the block */
            goto done;
        }
        u8 = env_str_alloc((uint32_t)need + 1u);
        if (!u8) {
            rc = ENV_ERR_NOMEM;
            goto done;
        }
        /* Re-decode under STRICT must reproduce the measured size exactly; a mismatch
         * means the unit stopped being decodable between the two passes -- refuse
         * rather than adopt an altered entry. u8 is not yet tracked in owned[], so
         * free it directly on this path. */
        if (nls_cp_utf16_to_utf8(&body[p], q - p, (uint8_t *)u8, (uint32_t)need,
                                 NLS_CP_STRICT) != need) {
            env_str_free(u8, (uint32_t)need + 1u);
            rc = ENV_ERR_INVAL;
            goto done;
        }
        u8[need] = '\0';
        owned[n] = u8;
        list[n] = u8;
        n++;
        cum += (uint32_t)need + 1u;
        if (!env_entry_is_wellformed(u8)) {
            rc = ENV_ERR_INVAL;            /* malformed entry -> refuse, never skip */
            goto done;
        }
        p = q + 1u;
    }

    /* All-or-nothing terminator discipline: the empty entry that ended the scan MUST be
     * the block's FINAL terminator. The tail check above only proves the last two WCHARs
     * are NUL, so a block with an interior double-NUL and trailing data (A=1\0\0B=2\0\0)
     * would otherwise pass the gate yet adopt only the prefix and report success -- silent
     * data loss under an all-or-nothing ABI. The 2-WCHAR "\0\0" empty block is the sole
     * valid n==0 form; for a non-empty block the scan breaks with p pointing AT the final
     * terminator WCHAR (index wchars-1). Anything else is trailing data -> refuse. */
    if (n == 0u) {
        if (wchars != 2u) {
            rc = ENV_ERR_INVAL;
            goto done;
        }
    } else if (p != wchars - 1u) {
        rc = ENV_ERR_INVAL;
        goto done;
    }

    /* Every entry validated: the exchange below cannot silently drop any of them. */
    list[n] = NULL;
    rc = env_exchange_block(t, list, n, out_old);

done:
    for (i = 0; i < n; i++)
        if (owned[i])
            env_str_free(owned[i], env_strlen(owned[i]) + 1u);
    kfree(owned);
    kfree(list);
    return rc;
}

int env_block_extent(const void *block, uint32_t *out_wchars)
{
    const struct env_block_hdr *hdr;

    if (!out_wchars)
        return ENV_ERR_INVAL;
    *out_wchars = 0;
    if (!block)
        return ENV_ERR_INVAL;

    /* CONTRACT (identical to env_destroy_block's, and for the same reason): `block`
     * MUST be a live pointer returned by env_create_block / env_create_empty_block.
     * Reading the predecessor header of an arbitrary pointer is a caller error, not a
     * supported input -- this is a BEST-EFFORT reject of an obviously-malformed header,
     * NOT a foreign-pointer validator. It cannot be one: the check must dereference
     * block-1 to run at all, so a wild pointer faults before any verdict, and an
     * unrelated allocation whose predecessor bytes happen to match passes. Callers get
     * their safety from the kernel-resident input contract, not from this function. */
    hdr = (const struct env_block_hdr *)block - 1;
    if (hdr->magic != ENV_BLK_MAGIC || hdr->wchars > ENV_CREATE_BLOCK_MAX_WCHARS)
        return ENV_ERR_INVAL;
    /* A wrapped block always carries at least the two-WCHAR "\0\0" empty form. */
    if (hdr->wchars < 2u)
        return ENV_ERR_INVAL;
    *out_wchars = hdr->wchars;
    return ENV_OK;
}

int env_create_empty_block(void **out_block)
{
    /* The empty block is the two-WCHAR "\0\0" form -- the same shape
     * env_build_block_utf16 forces for an empty environment, so consumers see one
     * empty-block representation no matter which entry produced it. */
    static const uint16_t empty[2] = { 0u, 0u };

    if (!out_block)
        return ENV_ERR_INVAL;
    *out_block = NULL;
    return env_wrap_block(empty, 2u, out_block);
}

int env_create_block(struct task *caller, const void *htoken, int inherit,
                     void **out_block)
{
    uint16_t *blk;
    uint32_t wchars;
    int rc;

    if (!out_block)
        return ENV_ERR_INVAL;
    *out_block = NULL;
    if (!caller)
        return ENV_ERR_INVAL;

    /* Deferred Win32 branches -- refused explicitly, never fabricated (see the
     * env.h contract). A per-user token needs a SID->hive map + LoadUserProfile;
     * a no-inherit fresh block needs an SMP-safe runtime Registry snapshot. */
    if (htoken != NULL)
        return ENV_ERR_UNSUPPORTED;
    if (!inherit)
        return ENV_ERR_UNSUPPORTED;

    /* Supported path: snapshot the caller's current environment (already the
     * assembled system+user set) into a sorted UTF-16 double-NUL block. The
     * environ snapshot serializes on caller->environ_lock and walks no Registry;
     * a block over ENV_STR_KMALLOC_MAX shares the pre-existing unlocked-PMM
     * exposure env_build_block_utf16 already carries (owner: the PMM bitmap SMP-
     * locking work in 03-memory-concurrency/TODO-03), not a new hazard. Cap at
     * ENV_CREATE_BLOCK_MAX_WCHARS so the result is consumable by the Rtl expansion
     * path; an over-large environ returns ENV_ERR_NOSPACE. */
    rc = env_build_block_utf16(caller, &blk, &wchars, ENV_CREATE_BLOCK_MAX_WCHARS);
    if (rc != ENV_OK)
        return rc;

    /* Wrap the builder's block in the shared self-describing allocation, then drop
     * the builder's copy -- the wrap owns the returned body. */
    rc = env_wrap_block(blk, wchars, out_block);
    env_free_block_utf16(blk, wchars);
    return rc;
}

void env_destroy_block(void *block)
{
    struct env_block_hdr *hdr;
    uint32_t total_bytes;

    if (!block)
        return;
    /* CONTRACT: `block` MUST be a pointer returned by env_create_block or
     * env_create_empty_block (or NULL) -- both build through env_wrap_block, so both
     * carry this header and free identically here --
     * called EXACTLY once, exactly as Win32 DestroyEnvironmentBlock requires a
     * CreateEnvironmentBlock pointer -- reading the predecessor header of an
     * arbitrary pointer is a caller error, not a supported input. The checks below
     * are a BEST-EFFORT reject of an obviously-malformed header, NOT a foreign-
     * pointer validator and NOT a double-free guarantee:
     *   - magic mismatch  -> not our header (or already poisoned): no-op.
     *   - wchars over the create-time cap -> corrupted header: no-op rather than
     *     compute a wrapped/oversized free extent (wchars <= ENV_CREATE_BLOCK_MAX_
     *     WCHARS keeps total_bytes well under UINT32_MAX). */
    hdr = (struct env_block_hdr *)block - 1;
    if (hdr->magic != ENV_BLK_MAGIC || hdr->wchars > ENV_CREATE_BLOCK_MAX_WCHARS)
        return;
    total_bytes = (uint32_t)sizeof(struct env_block_hdr) + hdr->wchars * 2u;
    /* Poison the header before freeing: a best-effort net so an IMMEDIATE double-
     * free (before this memory is handed back out) is a no-op rather than a
     * wrong-size re-free. Once the allocator reuses the memory this net is gone --
     * the single-free contract above is the real guarantee. */
    hdr->magic = 0u;
    env_str_free((char *)hdr, total_bytes);
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
#define ENV_DEF_SYSTEMROOT   ENV_SYSTEMROOT_DIR   /* single source of truth: include/kernel/env.h */
#define ENV_DEF_TEMP         "C:\\Temp"
#define ENV_DEF_PROC_ARCH    "AMD64"
#define ENV_DEF_OS           "Impossible_OS"
/* Composed from the immutable install-root constants (env.h) so a future root
 * move never leaves the live PATH pointing at an obsolete System32 tree. */
#define ENV_DEF_PATH_BASE    ENV_SYSTEMROOT_DIR "\\Bin;" ENV_SYSTEM32_DIR ";C:\\Programs"
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
            /* An over-budget refusal leaves `out` empty; recording that as this
             * variable's expansion would silently erase a value the caller still
             * holds. Report it and leave results[i] NULL (the caller's absent
             * marker) rather than publishing an empty expansion. */
            if (env_expand(t, raw, out, ENV_VALUE_MAX + 1u) == ENV_EXPAND_OVER_BUDGET) {
                if (*err == ENV_OK)
                    *err = ENV_ERR_TOOLONG;
            } else {
                results[i] = env_strdup(out);         /* keep the expanded result */
                if (!results[i] && *err == ENV_OK)
                    *err = ENV_ERR_NOMEM;
            }
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
