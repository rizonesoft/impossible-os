/* free_span.h -- where a buffer fits inside one free range without touching
 * the ranges that must stay clear.
 *
 * Shared by the loader (the kernel file's buffer) and the kernel's unit tests
 * (test_uefi_boot.c). Header-only because the two link separately, and it uses
 * no UEFI or kernel type because it compiles under both.
 */
#ifndef BOOT_FREE_SPAN_H
#define BOOT_FREE_SPAN_H

#define FREE_SPAN_PAGE 4096ULL

struct boot_span {
    unsigned long long base;
    unsigned long long bytes;
};

/* The end of a range, or the last address when it would wrap. */
static inline unsigned long long
boot_span_end(unsigned long long base, unsigned long long bytes)
{
    return bytes > ~0ULL - base ? ~0ULL : base + bytes;
}

/* The highest page-aligned start of `bytes` inside `room` that overlaps none
 * of the `n` ranges in `keep`; 0 when there is none. */
static inline unsigned long long
boot_span_pick(struct boot_span room, unsigned long long bytes,
               const struct boot_span *keep, unsigned n)
{
    unsigned long long end = boot_span_end(room.base, room.bytes);
    unsigned i = 0;

    if (bytes == 0 || end < bytes)
        return 0;
    unsigned long long at = (end - bytes) & ~(FREE_SPAN_PAGE - 1);
    while (i < n) {
        if (keep[i].bytes == 0 || at >= boot_span_end(keep[i].base, keep[i].bytes) ||
            at + bytes <= keep[i].base) {
            i++;
            continue;
        }
        if (keep[i].base < bytes)
            return 0;
        at = (keep[i].base - bytes) & ~(FREE_SPAN_PAGE - 1);
        i = 0;                          /* lower now: judge every range again */
    }
    return at < room.base ? 0 : at;
}

/* A range that is not free may lie over a free one (firmware maps do overlap). When
 * `taken` covers part of [at, at + bytes), end `room` below it and answer 1: the
 * caller picks again. */
static inline int
boot_span_clip(struct boot_span *room, unsigned long long at, unsigned long long bytes,
               struct boot_span taken)
{
    if (taken.bytes == 0 || taken.base >= at + bytes ||
        boot_span_end(taken.base, taken.bytes) <= at)
        return 0;
    room->bytes = taken.base > room->base ? taken.base - room->base : 0;
    return 1;
}

/* Let `run` take in `next` when it begins where `run` ends; answers whether it did. */
static inline int
boot_span_join(struct boot_span *run, struct boot_span next)
{
    if (next.bytes == 0 || next.base != boot_span_end(run->base, run->bytes) ||
        next.bytes > ~0ULL - next.base)
        return 0;
    run->bytes += next.bytes;
    return 1;
}

/* Whether `span` lies wholly inside [lo, hi). */
static inline int
boot_span_within(struct boot_span span, unsigned long long lo, unsigned long long hi)
{
    return span.base >= lo && span.bytes <= hi && span.base <= hi - span.bytes;
}

/* Whether `next` begins on a later page than `prev` ends on. */
static inline int
boot_span_follows(struct boot_span prev, struct boot_span next)
{
    return (next.base & ~(FREE_SPAN_PAGE - 1)) >= boot_span_end(prev.base, prev.bytes);
}

static inline int
boot_span_holds(struct boot_span span, unsigned long long at)
{
    return at >= span.base && at - span.base < span.bytes;
}

/* Takes `pages` from `base`, all or none; answers whether it did. */
typedef int (*boot_span_take)(void *ctx, unsigned long long base, unsigned long long pages);

/* Take all of [base, base + pages): whole, or on a refusal by halves, since firmware
 * refuses a range that crosses two of its free ones. 0 at the first page that cannot
 * be had. */
static inline int
boot_span_claim(unsigned long long base, unsigned long long pages, boot_span_take take, void *ctx)
{
    unsigned long long half = pages / 2;

    if (take(ctx, base, pages))
        return 1;
    return half != 0 && boot_span_claim(base, half, take, ctx) &&
           boot_span_claim(base + half * FREE_SPAN_PAGE, pages - half, take, ctx);
}

#endif /* BOOT_FREE_SPAN_H */
