/* ============================================================================
 * ex_hashtable.c -- RTL_DYNAMIC_HASH_TABLE: resizable chained hash table (S7).
 *
 * Power-of-two bucket directory with caller-owned chained entries (the caller
 * embeds an RTL_DYNAMIC_HASH_TABLE_ENTRY in its own struct). The bucket
 * directory is the ONLY thing the table owns, and it is grown/shrunk through a
 * MANDATORY caller allocate/free pair -- there is no kmalloc fallback, so "no
 * hidden allocation" is literal. Resize runs at PASSIVE_LEVEL and FAILS CLOSED:
 * if the directory allocation fails the table is left exactly as it was (every
 * entry stays at its address, just in a denser table), so insert/remove never
 * fail for a valid entry. CALLER-SERIALIZED (not thread-safe).
 *
 * Arch-neutral: no inline asm, no arch headers.
 * ============================================================================ */

#include "kernel/ex.h"
#include "libc/string.h"   /* memset / memcpy */

typedef RTL_DYNAMIC_HASH_TABLE_ENTRY hentry_t;

#define HASH_MIN_BUCKETS      8u
#define HASH_DEFAULT_BUCKETS  16u
/* Cap so the directory byte size (buckets * sizeof(ptr) == buckets * 8) always
 * fits the uint32 allocate-size argument: 2^28 * 8 == 2 GiB < UINT32_MAX. The
 * table never grows past this (a 256M-bucket directory is already absurd). */
#define HASH_MAX_BUCKETS      (1u << 28)

/* splitmix64 finalizer: spreads sequential / low-entropy signatures across the
 * directory so power-of-two masking does not cluster. */
static inline uint64_t hash_mix(uint64_t z)
{
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

static inline uint32_t hash_bucket(const RTL_DYNAMIC_HASH_TABLE *t, uint64_t sig)
{
    return (uint32_t)(hash_mix(sig) & (uint64_t)(t->BucketCount - 1u));
}

static uint32_t round_up_pow2(uint32_t x)
{
    uint32_t p = HASH_MIN_BUCKETS;
    if (x > HASH_MAX_BUCKETS)
        return HASH_MAX_BUCKETS;
    while (p < x) {
        uint32_t next = p << 1;
        if (next < p || next > HASH_MAX_BUCKETS)   /* overflow / cap guard */
            return HASH_MAX_BUCKETS;
        p = next;
    }
    return p;
}

/* Directory byte size; bounded by HASH_MAX_BUCKETS so it always fits uint32. */
static inline uint32_t dir_bytes(uint32_t buckets)
{
    return (uint32_t)((size_t)buckets * sizeof(RTL_DYNAMIC_HASH_TABLE_ENTRY *));
}

int RtlInitializeDynamicHashTable(RTL_DYNAMIC_HASH_TABLE *t,
                                  RTL_HASH_ALLOCATE_ROUTINE allocate,
                                  RTL_HASH_FREE_ROUTINE free, void *context,
                                  uint32_t initial_buckets)
{
    uint32_t buckets;
    hentry_t **dir;

    if (!t || !allocate || !free)
        return -1;

    buckets = round_up_pow2(initial_buckets ? initial_buckets : HASH_DEFAULT_BUCKETS);
    dir = (hentry_t **)allocate(t, dir_bytes(buckets));
    if (!dir)
        return -1;
    memset(dir, 0, (size_t)buckets * sizeof(hentry_t *));

    t->Directory = dir;
    t->BucketCount = buckets;
    t->NumEntries = 0;
    t->Allocate = allocate;
    t->Free = free;
    t->Context = context;
    return 0;
}

/* Rehash every entry into a freshly-allocated directory of `new_buckets` slots.
 * Returns true on success (old directory freed, table updated); false on
 * allocate failure (table completely unchanged). */
static bool hash_resize(RTL_DYNAMIC_HASH_TABLE *t, uint32_t new_buckets)
{
    hentry_t **newdir;
    uint32_t i, oldcount;

    if (new_buckets == t->BucketCount)
        return true;
    newdir = (hentry_t **)t->Allocate(t, dir_bytes(new_buckets));
    if (!newdir)
        return false;                /* fail closed: caller's table untouched */
    memset(newdir, 0, (size_t)new_buckets * sizeof(hentry_t *));

    oldcount = t->BucketCount;
    /* Swap the directory in first so hash_bucket() masks with the new size while
     * we rehash; entries move from the saved old directory. */
    {
        hentry_t **olddir = t->Directory;
        t->Directory = newdir;
        t->BucketCount = new_buckets;
        for (i = 0; i < oldcount; i++) {
            hentry_t *e = olddir[i];
            while (e) {
                hentry_t *next = e->Next;   /* save before relink */
                uint32_t b = hash_bucket(t, e->Signature);
                e->Next = newdir[b];
                newdir[b] = e;
                e = next;
            }
        }
        t->Free(t, olddir);
    }
    return true;
}

int RtlInsertEntryHashTable(RTL_DYNAMIC_HASH_TABLE *t,
                            RTL_DYNAMIC_HASH_TABLE_ENTRY *entry, uint64_t signature)
{
    uint32_t b;

    if (!t || !t->Directory || !entry)
        return -1;

    entry->Signature = signature;
    b = hash_bucket(t, signature);
    entry->Next = t->Directory[b];
    t->Directory[b] = entry;
    t->NumEntries++;

    /* Grow at load factor 1.0 (average chain length > 1). Best-effort: a failed
     * grow leaves the entry inserted in the current (denser) table. */
    if (t->NumEntries >= t->BucketCount && t->BucketCount < HASH_MAX_BUCKETS) {
        uint32_t want = t->BucketCount << 1;
        if (want > t->BucketCount && want <= HASH_MAX_BUCKETS)   /* no overflow / cap */
            (void)hash_resize(t, want);
    }
    return 0;
}

bool RtlRemoveEntryHashTable(RTL_DYNAMIC_HASH_TABLE *t,
                             RTL_DYNAMIC_HASH_TABLE_ENTRY *entry)
{
    uint32_t b;
    hentry_t *cur, *prev = (hentry_t *)0;

    if (!t || !t->Directory || !entry)
        return false;

    b = hash_bucket(t, entry->Signature);
    for (cur = t->Directory[b]; cur; prev = cur, cur = cur->Next) {
        if (cur != entry)
            continue;
        if (prev)
            prev->Next = cur->Next;
        else
            t->Directory[b] = cur->Next;
        cur->Next = (hentry_t *)0;
        t->NumEntries--;
        /* GROW-ONLY: remove never resizes. A shrink here would rehash and
         * reorder same-signature chains, which would invalidate an in-flight
         * RtlGetNextEntryHashTable cursor mid-walk (the documented remove-the-
         * just-returned-entry drain pattern would then skip entries). Trading
         * memory reclaim for a deterministic cursor-safe walk is the right call;
         * the directory is freed wholesale at RtlDeleteDynamicHashTable. */
        return true;
    }
    return false;   /* not in the table */
}

RTL_DYNAMIC_HASH_TABLE_ENTRY *RtlLookupEntryHashTable(RTL_DYNAMIC_HASH_TABLE *t,
                                                      uint64_t signature,
                                                      RTL_HASH_TABLE_CONTEXT *ctx)
{
    uint32_t b;
    hentry_t *e;

    if (!t || !t->Directory)
        return (hentry_t *)0;

    b = hash_bucket(t, signature);
    for (e = t->Directory[b]; e; e = e->Next) {
        if (e->Signature == signature) {
            if (ctx) {
                /* Cache the successor eagerly so removing THIS entry before the
                 * next call leaves the cursor valid (the successor is stable). */
                ctx->NextEntry = e->Next;
                ctx->Signature = signature;
            }
            return e;
        }
    }
    return (hentry_t *)0;
}

RTL_DYNAMIC_HASH_TABLE_ENTRY *RtlGetNextEntryHashTable(RTL_DYNAMIC_HASH_TABLE *t,
                                                       RTL_HASH_TABLE_CONTEXT *ctx)
{
    hentry_t *e;

    if (!t || !ctx)
        return (hentry_t *)0;

    for (e = ctx->NextEntry; e; e = e->Next) {
        if (e->Signature == ctx->Signature) {
            ctx->NextEntry = e->Next;   /* cache the next successor */
            return e;
        }
    }
    ctx->NextEntry = (hentry_t *)0;
    return (hentry_t *)0;
}

uint32_t RtlNumberOfEntriesHashTable(const RTL_DYNAMIC_HASH_TABLE *t)
{
    return t ? t->NumEntries : 0;
}

void RtlDeleteDynamicHashTable(RTL_DYNAMIC_HASH_TABLE *t)
{
    if (!t || !t->Directory)
        return;
    t->Free(t, t->Directory);
    t->Directory = (hentry_t **)0;
    t->BucketCount = 0;
    t->NumEntries = 0;
}
