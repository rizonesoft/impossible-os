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

/* Takes `pages` from `base`, all or none; answers whether it did. */
typedef int (*boot_span_take)(void *ctx, unsigned long long base, unsigned long long pages);

/* Take every page of [base, base + pages) that can be had, asking for each once at
 * most: the whole range, or on a refusal its halves (firmware refuses a range that
 * crosses two of its free ones). 0 when a page of `must` could not be. */
static inline int
boot_span_claim(unsigned long long base, unsigned long long pages, boot_span_take take,
                void *ctx, const struct boot_span *must, unsigned n)
{
    unsigned long long half = pages / 2;

    if (pages == 0 || take(ctx, base, pages))
        return 1;
    if (half != 0)
        return boot_span_claim(base, half, take, ctx, must, n) &&
               boot_span_claim(base + half * FREE_SPAN_PAGE, pages - half, take, ctx, must, n);
    for (unsigned i = 0; i < n; i++)
        if (must[i].bytes != 0 && must[i].base < base + FREE_SPAN_PAGE &&
            boot_span_end(must[i].base, must[i].bytes) > base)
            return 0;
    return 1;
}

#endif /* BOOT_FREE_SPAN_H */
