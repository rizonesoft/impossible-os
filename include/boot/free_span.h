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

/* The pages of spans[i] that no span before it touches. Two that share a page, as
 * segments may, hold it once. Empty when there are none. */
static inline struct boot_span
boot_span_own_pages(const struct boot_span *spans, unsigned i)
{
    unsigned long long lo = spans[i].base & ~(FREE_SPAN_PAGE - 1);
    unsigned long long hi = (boot_span_end(spans[i].base, spans[i].bytes) +
                             FREE_SPAN_PAGE - 1) & ~(FREE_SPAN_PAGE - 1);
    struct boot_span own;

    if (spans[i].bytes == 0)
        hi = lo;
    for (unsigned j = 0; j < i && lo < hi; ) {
        unsigned long long from = spans[j].base & ~(FREE_SPAN_PAGE - 1);
        unsigned long long to = (boot_span_end(spans[j].base, spans[j].bytes) +
                                 FREE_SPAN_PAGE - 1) & ~(FREE_SPAN_PAGE - 1);
        if (spans[j].bytes != 0 && from <= lo && to > lo)
            lo = to;
        else if (spans[j].bytes != 0 && from < hi && to >= hi)
            hi = from;
        else {
            j++;
            continue;
        }
        j = 0;                          /* smaller now: judge every span again */
    }
    own.base = lo;
    own.bytes = hi > lo ? hi - lo : 0;
    return own;
}

#endif /* BOOT_FREE_SPAN_H */
