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

#endif /* BOOT_FREE_SPAN_H */
