/* Host-side regression for the boot-trend publisher's scan policy.
 *
 * WHY THIS IS A HOST TEST. The policy is a pure function, so a kernel-side
 * TEST_CAT_BOOT case is perfectly writable -- it just does not FIT. Measured
 * 2026-09-03: the kernel image had 15 bytes of .text headroom below the
 * 0x800000 user base after the change under test, and a test function plus
 * its assertion strings is orders of magnitude more than that. Testing here
 * costs the kernel image nothing, which is the same reason
 * tools/boot-entries-parser-tests/ and tools/boot-header-tests/ exist.
 *
 * WHAT IS UNDER TEST. boot_trend_publish_json() walks the existing
 * boots array and keeps at most BOOT_TREND_RING_DEPTH entries while
 * examining at most BOOT_TREND_MAX_SCAN. The first version of that loop
 * terminated when the ring filled. Because the ring fills after 15 existing
 * entries (kept starts at 1 for the current boot), the counter stopped at 15
 * and an array of thousands whose leading entries happened to be valid was
 * truncated and rewritten with NO oversize warning -- the exact input the
 * bound exists to report. The fix keeps counting past a full ring and only
 * skips emission, which is what BOOT_TREND_SCAN_COUNT_ONLY encodes.
 *
 * The last case is a CONTROL, and it is the point of the file: it replays the
 * OLD policy over the same input and asserts that it does NOT warn. Without
 * it the other cases would pass just as happily against an implementation
 * that never had the bug, and the file would be evidence of nothing. */
#include <stdio.h>
#include <stdint.h>

#include "kernel/boot_trend.h"

static int g_failures;

static void check(int ok, const char *what)
{
    printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) g_failures++;
}

static void check_eq(uint32_t got, uint32_t want, const char *what)
{
    if (got != want) {
        printf("  [FAIL] %s (got %u, want %u)\n", what, got, want);
        g_failures++;
    } else {
        printf("  [PASS] %s\n", what);
    }
}

struct scan_result { uint32_t seen; uint32_t emitted; int warned; };

/* Mirrors the shipped loop in src/kernel/main/boot_trend.c exactly, over a
 * synthetic validity array instead of a cJSON child list. `valid` is NULL for
 * an all-valid array. */
static struct scan_result run_scan(uint32_t n, const uint8_t *valid)
{
    struct scan_result r = { 0u, 0u, 0 };
    uint32_t kept = 1u;   /* the current boot is emitted before the loop */

    for (uint32_t i = 0; i < n; i++) {
        enum boot_trend_scan_action act = boot_trend_scan_action(++r.seen, kept);
        if (act == BOOT_TREND_SCAN_STOP) break;
        if (act == BOOT_TREND_SCAN_COUNT_ONLY) continue;
        if (valid && !valid[i]) continue;
        r.emitted++;
        kept++;
    }
    r.warned = (r.seen > BOOT_TREND_MAX_SCAN);
    return r;
}

/* The REJECTED policy: terminate when the ring is full. Used only by the
 * control case below. */
static struct scan_result run_scan_old(uint32_t n, const uint8_t *valid)
{
    struct scan_result r = { 0u, 0u, 0 };
    uint32_t kept = 1u;

    for (uint32_t i = 0; i < n; i++) {
        if (kept >= BOOT_TREND_RING_DEPTH) break;
        if (++r.seen > BOOT_TREND_MAX_SCAN) break;
        if (valid && !valid[i]) continue;
        r.emitted++;
        kept++;
    }
    r.warned = (r.seen > BOOT_TREND_MAX_SCAN);
    return r;
}

#define BIG 5000u
static uint8_t g_valid[BIG];

int main(void)
{
    printf("boot-trend scan policy\n");

    /* --- the decision table itself, at every boundary that matters ------- */
    check(boot_trend_scan_action(1u, 1u) == BOOT_TREND_SCAN_CONSIDER,
          "first entry with an empty ring is considered");
    check(boot_trend_scan_action(BOOT_TREND_MAX_SCAN, 1u) == BOOT_TREND_SCAN_CONSIDER,
          "the 64th entry is still considered");
    check(boot_trend_scan_action(BOOT_TREND_MAX_SCAN + 1u, 1u) == BOOT_TREND_SCAN_STOP,
          "the 65th entry stops the walk");
    check(boot_trend_scan_action(BOOT_TREND_RING_DEPTH, BOOT_TREND_RING_DEPTH - 1u)
              == BOOT_TREND_SCAN_CONSIDER,
          "a ring one short of full still considers");
    /* THE regression: a full ring must COUNT, never STOP. */
    check(boot_trend_scan_action(BOOT_TREND_RING_DEPTH, BOOT_TREND_RING_DEPTH)
              == BOOT_TREND_SCAN_COUNT_ONLY,
          "a full ring counts rather than stopping");
    check(boot_trend_scan_action(BOOT_TREND_MAX_SCAN, BOOT_TREND_RING_DEPTH)
              == BOOT_TREND_SCAN_COUNT_ONLY,
          "a full ring still counts at the scan bound");
    check(boot_trend_scan_action(BOOT_TREND_MAX_SCAN + 1u, BOOT_TREND_RING_DEPTH)
              == BOOT_TREND_SCAN_STOP,
          "the scan bound outranks a full ring");

    /* --- whole-array behaviour, all entries valid ------------------------ */
    const uint32_t sizes[] = { 0u, 1u, 15u, 16u, 17u, 64u, 65u, BIG };
    const uint32_t want_seen[] = { 0u, 1u, 15u, 16u, 17u, 64u, 65u, 65u };
    const uint32_t want_emit[] = { 0u, 1u, 15u, 15u, 15u, 15u, 15u, 15u };
    const int      want_warn[] = { 0,  0,  0,   0,   0,   0,   1,   1   };

    for (uint32_t k = 0; k < sizeof(sizes) / sizeof(sizes[0]); k++) {
        struct scan_result r = run_scan(sizes[k], (const uint8_t *)0);
        char label[96];
        snprintf(label, sizeof(label), "all-valid n=%u scanned", sizes[k]);
        check_eq(r.seen, want_seen[k], label);
        snprintf(label, sizeof(label), "all-valid n=%u emitted", sizes[k]);
        check_eq(r.emitted, want_emit[k], label);
        snprintf(label, sizeof(label), "all-valid n=%u warned", sizes[k]);
        check_eq((uint32_t)r.warned, (uint32_t)want_warn[k], label);
    }

    /* --- all entries invalid: nothing kept, bound still reported --------- */
    for (uint32_t i = 0; i < BIG; i++) g_valid[i] = 0u;
    {
        struct scan_result r = run_scan(BIG, g_valid);
        check_eq(r.emitted, 0u, "all-invalid n=5000 emits nothing");
        check_eq(r.seen, BOOT_TREND_MAX_SCAN + 1u, "all-invalid n=5000 stops at the bound");
        check(r.warned, "all-invalid n=5000 warns");
    }

    /* --- valid prefix then invalid tail: the shape that hid the bug ------ */
    for (uint32_t i = 0; i < BIG; i++) g_valid[i] = (i < 15u) ? 1u : 0u;
    {
        struct scan_result r = run_scan(BIG, g_valid);
        check_eq(r.emitted, 15u, "15-valid prefix fills the ring");
        check_eq(r.seen, BOOT_TREND_MAX_SCAN + 1u, "15-valid prefix keeps scanning to the bound");
        check(r.warned, "15-valid prefix over 5000 entries warns");
    }

    /* --- CONTROL: the rejected policy must NOT warn on that input -------- */
    {
        struct scan_result r = run_scan_old(BIG, g_valid);
        check_eq(r.emitted, 15u, "CONTROL: old policy emits the same 15 entries");
        check(!r.warned,
              "CONTROL: old policy silently truncates 5000 entries without warning");
        check(r.seen < BOOT_TREND_MAX_SCAN,
              "CONTROL: old policy stops counting at the full ring");
    }

    printf("%s: %d failure(s)\n", g_failures ? "FAIL" : "PASS", g_failures);
    return g_failures ? 1 : 0;
}
