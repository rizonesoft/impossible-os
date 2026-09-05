/* test_error_screen_wrap.c -- line-break decision for bootloader error text.
 *
 * bsod_aa_wrap_next_break() (include/boot/bsod_aa_wrap.h) decides where each
 * line of an error screen ends for a given pixel budget. It is the subtlest
 * piece of the S23 compact renderer and the one that was wrong before it: the
 * previous shape accumulated a glyph advance and THEN tested the bound, so a
 * wrapped line could run one glyph past the rectangle its caller reserved --
 * invisible while the only caller wrapped inside a wide margin, and text drawn
 * into the recovery QR once S23 reserved a rectangle beside one.
 *
 * These tests run against the REAL 16px body atlas, so they pin the shipped
 * metrics rather than a synthetic table. The smoke legs prove the screen
 * renders; only this file can reach an oversized word, an exhausted budget, or
 * a single glyph wider than the whole box.
 *
 * XREF: 01-boot-platform/TODO-03-bootloader-error-recovery.md "No On-Screen
 * Words Below 800x600 -- the QR Renders, the Text Does Not"
 *
 * PURE-HELPER TESTS ONLY. Per CLAUDE.md "Test Code Policy": the break function
 * is a static inline over a const atlas, with no globals, no allocation, no
 * framebuffer and no UEFI types.
 */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "../../../include/boot/bsod_aa_wrap.h"
#include "../../../include/boot/bsod_render_tier.h"

/* The generated atlas declares `static const struct bsod_aa_glyph
 * bsod_aa_BODY[95]` plus its alpha data; the struct comes from the header
 * above, exactly as it does in the bootloader translation unit. */
typedef unsigned char UINT8;
#include "../../boot/uefi/bsod_font_body.inc"
#include "../../boot/uefi/bsod_font_sub.inc"

#include "kernel/types.h"

/* Width of the whole string at the shipped metrics, tracking included. */
static unsigned int body_width(const char *s)
{
    unsigned int w = 0;
    while (*s) { w += bsod_aa_advance(bsod_aa_BODY, *s); s++; }
    return w;
}

/* The renderer's own byte bound, taken from the header rather than restated:
 * bsod_aa_wrapped passes this exact constant and static-asserts its line
 * buffer against it, so there is one number and the tests bound the same
 * spans production does. */
#define WRAP_TEST_MAX_BYTES BSOD_AA_LINE_MAX_BYTES

static unsigned int brk(const char *s, unsigned int start, unsigned int max_px)
{
    return bsod_aa_wrap_next_break(s, start, bsod_aa_BODY, max_px,
                                   WRAP_TEST_MAX_BYTES);
}

static unsigned int brk_bytes(const char *s, unsigned int start,
                              unsigned int max_px, unsigned int max_bytes)
{
    return bsod_aa_wrap_next_break(s, start, bsod_aa_BODY, max_px, max_bytes);
}

/* ---- The bound is HARD ---- */

static void test_break_never_exceeds_budget(void)
{
    const char *t = "the firmware ran out of memory reading boot settings";
    unsigned int max_px = 120u;
    unsigned int start = 0u;
    unsigned int guard = 0u;

    /* Walk the whole string the way the renderer does and assert every line
     * measures within the budget. The old accumulate-then-test shape failed
     * this on the first line that ended mid-word. */
    while (t[start] && guard < 64u) {
        unsigned int end = brk(t, start, max_px);
        unsigned int w = 0u;
        unsigned int i;
        TEST_ASSERT_EQ(end > start, 1, "the break must consume at least one byte");
        for (i = start; i < end; i++) w += bsod_aa_advance(bsod_aa_BODY, t[i]);
        TEST_ASSERT_EQ(w <= max_px, 1, "a line must never exceed max_px");
        start = end;
        while (t[start] == ' ') start++;
        guard++;
    }
    TEST_ASSERT_EQ(t[start], 0, "the walk must consume the whole string");
}

static void test_exact_fit_takes_the_whole_string(void)
{
    const char *t = "Boot error";
    unsigned int w = body_width(t);
    TEST_ASSERT_EQ(brk(t, 0u, w), 10u,
                   "a budget equal to the string width takes all of it");
    TEST_ASSERT_EQ(brk(t, 0u, w - 1u) < 10u, 1,
                   "one pixel under, the last glyph must not be admitted");
}

/* ---- Word breaking ---- */

static void test_breaks_at_the_last_space(void)
{
    const char *t = "alpha beta gamma";
    unsigned int budget = body_width("alpha beta ") + 2u;
    unsigned int end = brk(t, 0u, budget);
    /* 5 = index of the space after "alpha"; 10 = the space after "beta". */
    TEST_ASSERT_EQ(end, 10u, "the break lands on the last space that fits");
}

static void test_word_longer_than_the_budget_splits(void)
{
    /* No space to break at, so the word is split at the boundary instead of
     * overflowing the reserved rectangle.
     *
     * Every span is MEASURED, not just counted. Asserting only progress and
     * splitting let a breaker that admits one extra glyph on the no-space path
     * return a 51-pixel span against a 40-pixel budget with the whole suite
     * still green -- which is the hard-bound regression that draws into the
     * reserved QR rectangle. */
    const char *t = "IPOSRequiredSecVersion";
    const unsigned int budget = 40u;
    unsigned int start = 0u;
    unsigned int spans = 0u;

    while (t[start] && spans < 32u) {
        unsigned int end = brk(t, start, budget);
        unsigned int w = 0u;
        unsigned int i;
        TEST_ASSERT_EQ(end > start, 1, "an oversized word must still make progress");
        for (i = start; i < end; i++) w += bsod_aa_advance(bsod_aa_BODY, t[i]);
        TEST_ASSERT_EQ(w <= budget, 1,
                       "and every span of it must stay within the pixel budget");
        start = end;
        spans++;
    }
    TEST_ASSERT_EQ(t[start], 0, "the whole word must be consumed");
    TEST_ASSERT_EQ(spans > 1u, 1, "an oversized word must be split, not emitted whole");
}

/* ---- Progress guarantee ---- */

static void test_single_glyph_wider_than_budget_still_advances(void)
{
    /* Returning `start` here would spin the renderer's loop forever on a
     * pathological budget. One glyph is emitted and the blit clips it. */
    TEST_ASSERT_EQ(brk("W", 0u, 1u), 1u,
                   "a glyph wider than the whole budget must advance by one");
    TEST_ASSERT_EQ(brk("Wide", 0u, 0u), 1u,
                   "a zero budget must still advance by one");
}

/* ---- The BYTE bound: a wide screen with narrow glyphs ---- */

static void test_byte_bound_wraps_rather_than_dropping(void)
{
    /* The renderer copies each line into a fixed stack buffer. Bounding the
     * run in pixels alone let a wide screen produce a line longer than that
     * buffer, which the copy truncated while the caller advanced past every
     * discarded byte -- text lost with nothing reporting it. */
    const char *t = "aaaa bbbb cccc dddd eeee ffff gggg hhhh";
    unsigned int end = brk_bytes(t, 0u, 100000u, 10u);
    TEST_ASSERT_EQ(end <= 10u, 1, "the run must respect the byte bound");
    TEST_ASSERT_EQ(end > 0u, 1, "and must still advance");
}

static void test_byte_bound_loses_no_bytes(void)
{
    /* Walk the whole string under a tight byte bound and a pixel budget wide
     * enough to be irrelevant, and assert every byte is either emitted on a
     * line or is one of the spaces the caller skips between lines.
     *
     * The per-span bound is asserted too, not just the total. Summing spans
     * alone accepts an OVERSIZED span: raising every byte budget by one still
     * consumes every byte, but in production a 128-byte span exceeds the
     * 127-byte copy buffer and the renderer drops a byte while advancing past
     * it -- which is precisely the silent loss this test exists to prevent. */
    const char *t = "Reflash the firmware or boot recovery media to continue.";
    const unsigned int budget = 8u;
    unsigned int start = 0u;
    unsigned int consumed = 0u;
    unsigned int guard = 0u;

    while (t[start] && guard < 128u) {
        unsigned int end = brk_bytes(t, start, 100000u, budget);
        TEST_ASSERT_EQ(end > start, 1, "each line must consume bytes");
        TEST_ASSERT_EQ(end - start <= budget, 1,
                       "and no span may exceed the byte budget it was given");
        consumed += end - start;
        start = end;
        while (t[start] == ' ') { start++; consumed++; }
        guard++;
    }
    TEST_ASSERT_EQ(t[start], 0, "the walk must reach the end of the string");
    TEST_ASSERT_EQ(consumed, 56u,
                   "every byte must be either drawn or a skipped separator");
}

static void test_unbroken_string_past_the_copy_buffer(void)
{
    /* A single word longer than the renderer's 127-byte line buffer, with no
     * space anywhere to break at: the case the byte bound exists for. Every
     * span must fit the buffer and the spans must reconstruct the string
     * exactly, with nothing dropped between them. */
    static char t[300];
    unsigned int i;
    unsigned int start = 0u;
    unsigned int consumed = 0u;
    unsigned int spans = 0u;

    for (i = 0; i < sizeof(t) - 1u; i++) t[i] = (char)('a' + (i % 26u));
    t[sizeof(t) - 1u] = '\0';

    while (t[start] && spans < 64u) {
        unsigned int end = brk_bytes(t, start, 100000u, WRAP_TEST_MAX_BYTES);
        TEST_ASSERT_EQ(end > start, 1, "an unbroken word must still advance");
        TEST_ASSERT_EQ(end - start <= WRAP_TEST_MAX_BYTES, 1,
                       "no span may exceed the renderer's copy buffer");
        consumed += end - start;
        start = end;
        spans++;
    }
    TEST_ASSERT_EQ(t[start], 0, "the walk must consume the whole word");
    TEST_ASSERT_EQ(consumed, sizeof(t) - 1u,
                   "and every byte of it must land in exactly one span");
    TEST_ASSERT_EQ(spans >= 3u, 1,
                   "a 299-byte word needs at least three 127-byte spans");
}

static void test_zero_byte_bound_still_advances(void)
{
    TEST_ASSERT_EQ(brk_bytes("abc", 0u, 100000u, 0u), 1u,
                   "a zero byte bound must not stall the caller");
}

/* ---- Fixed-position lines are CLIPPED, not wrapped ---- */

static void test_full_layout_subtitle_clips_at_800(void)
{
    /* The S18 renderer draws its error-code subtitle unwrapped at a fixed x,
     * so a long title is CLIPPED at the screen edge with nothing reporting
     * it. This is the exact case that made the full tier's fit report a
     * claim rather than a measurement: at the 800x600 floor the A/B GPT
     * title runs off the right edge.
     *
     * This calls bsod_aa_would_clip -- the SAME function the renderer calls,
     * not a reimplementation of it. An earlier version of this test computed
     * the widths itself, which meant stubbing the renderer's predicate to
     * "never clips" left it green: it was asserting a copy of the decision
     * rather than the decision. */
    const char *prefix = "Code 0x0000000D: ";
    const char *title  = "A/B select: primary/backup GPT metadata range mismatch";
    unsigned int x = 80u + bsod_aa_text_width(bsod_aa_SUB, prefix)
                         + BSOD_AA_TRACKING;

    TEST_ASSERT_EQ(bsod_aa_would_clip(x, 800u, bsod_aa_SUB, title), 1,
                   "this title genuinely overruns an 800-wide screen");
    TEST_ASSERT_EQ(bsod_aa_would_clip(x, 1024u, bsod_aa_SUB, title), 0,
                   "and genuinely fits a 1024-wide one, so the bound is real");
}

static void test_would_clip_boundary_and_degenerates(void)
{
    const char *t = "Boot error";
    unsigned int w = bsod_aa_text_width(bsod_aa_BODY, t);

    /* Exactly enough room is not a clip; one pixel less is. */
    TEST_ASSERT_EQ(bsod_aa_would_clip(0u, w, bsod_aa_BODY, t), 0,
                   "a string that exactly fits must not report a clip");
    TEST_ASSERT_EQ(bsod_aa_would_clip(0u, w - 1u, bsod_aa_BODY, t), 1,
                   "one pixel short must report a clip");
    TEST_ASSERT_EQ(bsod_aa_would_clip(1u, w, bsod_aa_BODY, t), 1,
                   "and shifting it right by one must too");
    TEST_ASSERT_EQ(bsod_aa_would_clip(800u, 800u, bsod_aa_BODY, t), 1,
                   "an x at or past the right edge is always a clip");
    TEST_ASSERT_EQ(bsod_aa_would_clip(0u, 100u, bsod_aa_BODY, ""), 0,
                   "an empty string cannot be clipped");
    TEST_ASSERT_EQ(bsod_aa_would_clip(0u, 100u, bsod_aa_BODY,
                                      (const char *)0), 0,
                   "a NULL string cannot be clipped");
}

/* ---- The FULL layout's measured vertical bound ---- */

/* Lowest pixel row a string touches when drawn at py: the blit places each
 * glyph at py + ascent + bearing_y and fills `height` rows from there. Using
 * py + ascent alone -- which is what a first measurement of this layout did --
 * ignores descenders and understates the line. */
static unsigned int aa_bottom(const struct bsod_aa_glyph *glyphs,
                              unsigned int ascent, unsigned int py,
                              const char *str)
{
    unsigned int b = py;
    while (*str) {
        unsigned char ch = (unsigned char)*str;
        const struct bsod_aa_glyph *g;
        if (ch < 0x20u || ch > 0x7Eu) ch = (unsigned char)'?';
        g = &glyphs[ch - 0x20u];
        if (g->height) {
            unsigned int bot = py + ascent + (unsigned int)((int)g->bearing_y)
                             + g->height;
            if (bot > b) b = bot;
        }
        str++;
    }
    return b;
}

static void test_full_layout_last_hint_bottom_is_522(void)
{
    /* What this pins, precisely: the shipped hint string, at the y the S18
     * renderer draws it, in the shipped atlas, descends to row 522. It does
     * NOT validate the layout -- the y and the string are literals inside
     * bsod_render_graphical, which a kernel test cannot reach, so this is a
     * copy of that position and would not notice the renderer moving it.
     *
     * It is worth having anyway because the number it guards was WRONG: it was
     * recorded as 502 from py + ascent, which ignores descenders, and the real
     * bound is what explains why a 480-row mode cannot carry the layout. A
     * font swap now fails here instead of silently changing that reasoning. */
    unsigned int bottom = aa_bottom(bsod_aa_BODY, BSOD_AA_BODY_ASCENT, 486u,
                                    "- Press any key to restart");
    TEST_ASSERT_EQ(bottom, 522u, "the last hint line bottoms at y=522");
    TEST_ASSERT_EQ(bottom > 512u, 1,
                   "so a 512-row screen would clip it -- the old figure was wrong");
    TEST_ASSERT_EQ(bottom <= 600u, 1,
                   "and the shipped 800x600 floor clears it");
}

/* ---- The production truncation DECISION, not a restatement of it ---- */

static void test_wrap_plan_reports_needed_beyond_the_cap(void)
{
    /* bsod_render_compact reports fit=truncated when a field NEEDS more lines
     * than its budget draws. bsod_aa_wrap_plan is that same walk with the
     * drawing removed, so this asserts the production decision. Stopping the
     * walk at max_lines -- the bug this guards -- would make needed equal
     * drawn and hide every omitted line. */
    const char *t = "Boot a newer signed kernel, or clear "
                    "IPOSRequiredSecVersion, then power-cycle.";
    unsigned int drawn = 0;
    unsigned int needed = bsod_aa_wrap_plan(t, bsod_aa_BODY, 120u,
                                            WRAP_TEST_MAX_BYTES, 2u, &drawn);

    TEST_ASSERT_EQ(drawn, 2u, "the cap must bound what is drawn");
    TEST_ASSERT_EQ(needed > drawn, 1,
                   "and needed must exceed it, which is what fit=truncated means");
}

static void test_wrap_plan_zero_exact_and_overflow_caps(void)
{
    /* The RETURN value is what production reports truncation from, so every
     * case captures it. Discarding it left two mutations green: returning zero
     * needed lines under a zero cap (which hides omitted content), and
     * returning needed+1 when needed equals the cap (which reports truncation
     * that did not happen). */
    const char *t = "alpha beta gamma delta";
    unsigned int drawn = 0;
    unsigned int needed = bsod_aa_wrap_plan(t, bsod_aa_BODY, 60u,
                                            WRAP_TEST_MAX_BYTES, 99u, &drawn);

    TEST_ASSERT_EQ(needed, drawn, "an ample cap draws everything it needs");
    TEST_ASSERT_EQ(needed > 1u, 1, "and this text genuinely needs several lines");

    /* Zero cap: nothing drawn, everything still COUNTED. */
    {
        unsigned int n0 = bsod_aa_wrap_plan(t, bsod_aa_BODY, 60u,
                                            WRAP_TEST_MAX_BYTES, 0u, &drawn);
        TEST_ASSERT_EQ(drawn, 0u, "a zero cap draws nothing");
        TEST_ASSERT_EQ(n0, needed,
                       "but the requirement is unchanged by the cap");
    }
    /* Exact cap: needed == drawn, so no truncation is reported. */
    {
        unsigned int ne = bsod_aa_wrap_plan(t, bsod_aa_BODY, 60u,
                                            WRAP_TEST_MAX_BYTES, needed, &drawn);
        TEST_ASSERT_EQ(ne, needed, "an exact cap does not change the requirement");
        TEST_ASSERT_EQ(drawn, needed, "and draws all of it, so fit is not truncated");
    }
}

static void test_wrap_plan_counts_spans_past_the_byte_bound(void)
{
    /* The 127-byte line buffer used to discard the tail of a long line while
     * the caller advanced past it, so needed never rose. Planning under a
     * tight byte bound must count every span. */
    const char *t = "Reflash the firmware or boot recovery media to continue.";
    unsigned int drawn = 0;
    unsigned int needed = bsod_aa_wrap_plan(t, bsod_aa_BODY, 100000u, 8u,
                                            99u, &drawn);
    TEST_ASSERT_EQ(needed >= 7u, 1,
                   "a 56-byte string under an 8-byte bound needs >= 7 spans");
    TEST_ASSERT_EQ(needed, drawn, "and an ample cap draws all of them");
}

/* ---- The compact field allocation ---- */

static void test_cause_cap_absorbs_the_slack(void)
{
    /* The cause gets every row the other three fields leave. Restoring the
     * fixed three-row share it replaced left every other assertion green, so
     * this is where that regression is caught. */
    TEST_ASSERT_EQ(bsod_compact_cause_cap(1u, 1u, 1u),
                   BSOD_COMPACT_MAX_LINES - 3u,
                   "one row each leaves the rest for the cause");
    TEST_ASSERT_EQ(bsod_compact_cause_cap(2u, 1u, 3u),
                   BSOD_COMPACT_MAX_LINES - 6u,
                   "and the cause shrinks as the others grow");
    TEST_ASSERT_EQ(bsod_compact_cause_cap(1u, 1u, 1u) > 3u, 1,
                   "the usual case must give the cause MORE than the fixed "
                   "three rows that truncated real diagnoses");
}

static void test_cause_cap_never_underflows_or_reaches_zero(void)
{
    /* used >= MAX_LINES must not wrap the unsigned subtraction, and the cause
     * must never be allotted zero rows -- a screen with no diagnosis at all is
     * worse than a truncated one. */
    TEST_ASSERT_EQ(bsod_compact_cause_cap(BSOD_COMPACT_MAX_LINES, 0u, 0u), 1u,
                   "an exactly-consumed budget still leaves one row");
    TEST_ASSERT_EQ(bsod_compact_cause_cap(99u, 99u, 99u), 1u,
                   "and an over-consumed one cannot underflow");
}

static void test_real_diagnosis_fits_at_the_compact_floor(void)
{
    /* The message an earlier round found truncated, at the ACTUAL floor. This
     * composes the SHIPPED geometry: the text rectangle beside the QR at
     * 480x256, the planner, and the cause allocation. Reverting any one of
     * them fails here.
     *
     * It ran at the OLD 512-wide floor until that floor moved to 480, which
     * meant it exercised a budget the floor no longer has -- admission at 480
     * was proved
     * by the tier tests while nothing composed the real diagnosis against the
     * 280 px budget that admission actually gets. */
    const char *cause =
        "The primary and backup GPT are not both valid-and-agreeing on "
        "the A/B metadata partition (a corrupt copy or a primary/backup "
        "disagreement) -- cannot trust slot selection. Reflash or boot "
        "recovery media.";
    const unsigned int LH = BSOD_AA_BODY_LINE_H;
    unsigned int right = bsod_compact_text_right(480u, 256u, 1, LH);
    unsigned int budget = right - 2u * BSOD_COMPACT_MARGIN;

    /* Pin the budget itself: if the reservation changes, the row counts below
     * would move with it and prove nothing. */
    TEST_ASSERT_EQ(budget, 280u, "the 480x256 floor leaves a 280px text budget");
    unsigned int head_d = 0, code_d = 0, action_d = 0, cap, needed, drawn = 0;

    (void)bsod_aa_wrap_plan("Impossible OS could not start", bsod_aa_BODY,
                            budget, WRAP_TEST_MAX_BYTES,
                            BSOD_COMPACT_HEAD_LINES, &head_d);
    (void)bsod_aa_wrap_plan("Error code: 0x0000000D", bsod_aa_BODY, budget,
                            WRAP_TEST_MAX_BYTES,
                            BSOD_COMPACT_CODE_LINES, &code_d);
    (void)bsod_aa_wrap_plan("Scan the QR code below for help.", bsod_aa_BODY,
                            budget, WRAP_TEST_MAX_BYTES,
                            BSOD_COMPACT_ACTION_LINES, &action_d);
    cap = bsod_compact_cause_cap(head_d, code_d, action_d);
    needed = bsod_aa_wrap_plan(cause, bsod_aa_BODY, budget,
                               WRAP_TEST_MAX_BYTES, cap, &drawn);

    TEST_ASSERT_EQ(needed, 7u,
                   "the diagnosis takes exactly 7 rows at the floor's budget");
    TEST_ASSERT_EQ(needed <= cap, 1,
                   "and the dynamic cap must render all of it, not truncate");
    TEST_ASSERT_EQ(drawn, needed, "every row it needs must be drawn");
    /* The whole stack must still fit the 256-row screen. */
    TEST_ASSERT_EQ(2u * BSOD_COMPACT_MARGIN +
                   (head_d + code_d + action_d + drawn) * LH <= 256u, 1,
                   "and the resulting stack must fit the screen it was sized for");

    /* And at the panel size the floor was lowered to admit. */
    {
        unsigned int r2 = bsod_compact_text_right(480u, 272u, 1, LH);
        unsigned int d2 = 0;
        unsigned int n2 = bsod_aa_wrap_plan(cause, bsod_aa_BODY,
                                            r2 - 2u * BSOD_COMPACT_MARGIN,
                                            WRAP_TEST_MAX_BYTES, cap, &d2);
        TEST_ASSERT_EQ(r2 - 2u * BSOD_COMPACT_MARGIN, 280u,
                       "a 480x272 panel gets the same 280px budget");
        TEST_ASSERT_EQ(n2, 7u, "and the same 7 rows");
        TEST_ASSERT_EQ(d2, n2, "all of which are drawn");
    }
}

static void test_wrap_plan_exact_counts_across_the_byte_bound(void)
{
    /* EXACT counts, not lower bounds. A planner that skips one byte per line
     * -- start = end + 1 instead of start = end -- undercounts, and a >= 7
     * assertion accepts that happily. With a 128-byte unbroken word against a
     * 127-byte limit the answer is exactly two spans; the mutation says one,
     * and production then draws 127 bytes under a cap of 1 and reports no
     * truncation at all. */
    static char w[129];
    unsigned int i, drawn = 0, needed;

    for (i = 0; i < 128u; i++) w[i] = 'a';
    w[128] = '\0';

    needed = bsod_aa_wrap_plan(w, bsod_aa_BODY, 1000000u, 127u, 99u, &drawn);
    TEST_ASSERT_EQ(needed, 2u, "128 bytes under a 127-byte bound needs 2 spans");
    TEST_ASSERT_EQ(drawn, 2u, "and an ample cap draws both");

    (void)bsod_aa_wrap_plan(w, bsod_aa_BODY, 1000000u, 127u, 0u, &drawn);
    TEST_ASSERT_EQ(drawn, 0u, "a zero cap draws neither");
    needed = bsod_aa_wrap_plan(w, bsod_aa_BODY, 1000000u, 127u, 1u, &drawn);
    TEST_ASSERT_EQ(drawn, 1u, "a cap of one draws one");
    TEST_ASSERT_EQ(needed, 2u, "while the requirement stays two, so fit is truncated");

    /* Exactly at the bound is one span, not two. */
    w[127] = '\0';
    needed = bsod_aa_wrap_plan(w, bsod_aa_BODY, 1000000u, 127u, 99u, &drawn);
    TEST_ASSERT_EQ(needed, 1u, "127 bytes under a 127-byte bound is a single span");
}

static void test_wrap_plan_exact_counts_across_the_pixel_bound(void)
{
    /* The same exactness on the PIXEL side: a word whose width is just over
     * two budgets must plan as exactly three spans, so an off-by-one in the
     * break cannot hide inside a lower bound. */
    const char *w = "aaaaaa";
    unsigned int one = bsod_aa_advance(bsod_aa_BODY, 'a');
    unsigned int drawn = 0;
    unsigned int needed = bsod_aa_wrap_plan(w, bsod_aa_BODY, 2u * one - 1u,
                                            WRAP_TEST_MAX_BYTES, 99u, &drawn);
    /* Two glyphs never fit, so each span holds exactly one: six spans. */
    TEST_ASSERT_EQ(needed, 6u, "a one-glyph-per-line budget needs one span per glyph");
    TEST_ASSERT_EQ(drawn, 6u, "and an ample cap draws all of them");
}

static void test_wrap_plan_exact_counts_on_spaced_text(void)
{
    /* The exact-count tests above use unbroken words, so a planner that
     * mishandles the SPACE path -- the one the renderer takes for every real
     * message -- slips past them. Spaced text needs its own exact expectation.
     *
     * Six three-letter words at a budget that admits exactly one word per line
     * is six spans, and the separator-skipping must consume each space rather
     * than emitting a line for it. */
    const char *t = "aaa bbb ccc ddd eee fff";
    unsigned int one = bsod_aa_advance(bsod_aa_BODY, 'a');
    unsigned int drawn = 0;
    unsigned int needed = bsod_aa_wrap_plan(t, bsod_aa_BODY, 4u * one,
                                            WRAP_TEST_MAX_BYTES, 99u, &drawn);

    TEST_ASSERT_EQ(needed, 6u, "one word per line is exactly six spans");
    TEST_ASSERT_EQ(drawn, 6u, "and an ample cap draws all six");

    /* Two words per line halves it, exactly. */
    needed = bsod_aa_wrap_plan(t, bsod_aa_BODY, 8u * one,
                               WRAP_TEST_MAX_BYTES, 99u, &drawn);
    TEST_ASSERT_EQ(needed, 3u, "two words per line is exactly three spans");
    TEST_ASSERT_EQ(drawn, 3u, "and all three are drawn");

    /* Trailing and repeated separators must not manufacture empty lines. */
    needed = bsod_aa_wrap_plan("aaa   bbb  ", bsod_aa_BODY, 4u * one,
                               WRAP_TEST_MAX_BYTES, 99u, &drawn);
    TEST_ASSERT_EQ(needed, 2u, "runs of spaces and a trailing space add no lines");
}

static void test_wrap_plan_separator_path_exact_on_real_text(void)
{
    /* The spaced-text cases above use uniform words, whose budgets tolerate a
     * separator loop that eats the next word's first byte. Real prose does
     * not: at a 372px budget the A/B GPT diagnosis needs exactly six rows, and
     * the byte-eating mutation reports five -- so production under a five-row
     * cap would omit the last line and report no truncation at all. */
    const char *gpt =
        "The primary and backup GPT are not both valid-and-agreeing on "
        "the A/B metadata partition (a corrupt copy or a primary/backup "
        "disagreement) -- cannot trust slot selection. Reflash or boot "
        "recovery media.";
    unsigned int drawn = 0;
    unsigned int needed = bsod_aa_wrap_plan(gpt, bsod_aa_BODY, 372u,
                                            WRAP_TEST_MAX_BYTES, 5u, &drawn);

    TEST_ASSERT_EQ(needed, 6u, "this diagnosis needs exactly six rows at 372px");
    TEST_ASSERT_EQ(drawn, 5u, "a five-row cap draws five, so fit is truncated");
}

/* ---- Compact text rectangle vs the QR block ---- */

static void test_compact_text_right_reserves_only_when_they_overlap(void)
{
    const unsigned int LH = BSOD_AA_BODY_LINE_H;   /* the renderer's own */
    unsigned int qr = bsod_qr_block_extent(512u);
    unsigned int reserve = qr + BSOD_QR_BLOCK_MARGIN + BSOD_COMPACT_QR_GAP;

    /* 480x256 is the compact floor and the case the smoke legs cannot reach:
     * the QR starts above the text band, so the text must stop short of it.
     * Returning the full width here would let text run into the QR while
     * every tier and QR-fit test stayed green. */
    TEST_ASSERT_EQ(bsod_compact_text_right(512u, 256u, 1, LH), 512u - reserve,
                   "on a short screen the text stops left of the QR");
    /* 640x480: the QR sits well below the band, so the full width is free. */
    TEST_ASSERT_EQ(bsod_compact_text_right(640u, 480u, 1, LH), 640u,
                   "on a tall screen there is nothing to avoid");
    /* A caller that paints no QR is never narrowed. */
    TEST_ASSERT_EQ(bsod_compact_text_right(512u, 256u, 0, LH), 512u,
                   "reserve_qr=0 keeps the full width");
}

static void test_compact_text_right_transition_is_at_the_band_edge(void)
{
    const unsigned int LH = BSOD_AA_BODY_LINE_H;

    /* LITERAL expectations, derived once by hand from the shipped constants:
     * the band is 16 + 11*20 + 16 = 252, the QR block is 148 with a 12px
     * margin, so the reservation stops at height 252+148+12 = 412, and below
     * it the text stops at 512 - (148+12+8) = 344.
     *
     * Computing these from bsod_compact_band_height would make the assertion
     * move with the helper it is checking: understating the band to
     * (MAX_LINES - CAUSE_LINES) rows passed every such test while a 512x400
     * screen silently got the full 512 px and let its text run into the QR. */
    TEST_ASSERT_EQ(bsod_compact_text_right(512u, 412u, 1, LH), 512u,
                   "at 412 rows the QR clears the band and the full width is free");
    TEST_ASSERT_EQ(bsod_compact_text_right(512u, 411u, 1, LH), 344u,
                   "at 411 the rectangle is reserved and the text stops at 344");
    TEST_ASSERT_EQ(bsod_compact_text_right(512u, 400u, 1, LH), 344u,
                   "and a 400-row screen is well inside the reserving range");
    /* The helper is still asserted, against the same hand-derived number. */
    TEST_ASSERT_EQ(bsod_compact_band_height(LH), 252u,
                   "the worst-case band is 252 px");
}

static void test_compact_text_right_at_larger_qr_scales(void)
{
    /* Every case above is a 512-wide screen, which is the DEFAULT QR scale.
     * That let the reservation consume a constant extent instead of the
     * width-dependent one: substituting bsod_qr_block_extent(512) for
     * bsod_qr_block_extent(width) passed every assertion while letting text
     * run inside the larger QR block on wide displays.
     *
     * Literals, hand-derived from the shipped ladder: at 1920 the scale steps
     * to 6, so the block is 29*6 + 48 = 222 and the reserve 222+12+8 = 242,
     * leaving 1678. At 2560 the scale steps to 8, giving 29*8 + 64 = 296, a
     * reserve of 316, and 2244. */
    const unsigned int LH = BSOD_AA_BODY_LINE_H;

    TEST_ASSERT_EQ(bsod_compact_text_right(1920u, 256u, 1, LH), 1678u,
                   "a 1920-wide screen reserves the scale-6 block");
    TEST_ASSERT_EQ(bsod_compact_text_right(2560u, 400u, 1, LH), 2244u,
                   "a 2560-wide screen reserves the scale-8 block");

    /* Their vertical transitions sit at band + block + margin: 252+222+12=486
     * and 252+296+12=560. */
    TEST_ASSERT_EQ(bsod_compact_text_right(1920u, 486u, 1, LH), 1920u,
                   "at 486 rows the scale-6 block clears the band");
    TEST_ASSERT_EQ(bsod_compact_text_right(1920u, 485u, 1, LH), 1678u,
                   "one row below it is reserved");
    TEST_ASSERT_EQ(bsod_compact_text_right(2560u, 560u, 1, LH), 2560u,
                   "at 560 rows the scale-8 block clears the band");
    TEST_ASSERT_EQ(bsod_compact_text_right(2560u, 559u, 1, LH), 2244u,
                   "one row below it is reserved");
}

static void test_compact_text_right_when_no_qr_fits(void)
{
    /* Below the QR's own floor there is no QR to avoid, so the text keeps the
     * whole width even though reserve_qr was asked for. */
    TEST_ASSERT_EQ(bsod_qr_block_fits(1, 150u, 150u), 0, "no QR at 150x150");
    TEST_ASSERT_EQ(bsod_compact_text_right(150u, 150u, 1, BSOD_AA_BODY_LINE_H), 150u,
                   "with no QR on screen the text is not narrowed");
}

/* ---- Degenerate inputs ---- */

static void test_empty_and_null_inputs(void)
{
    TEST_ASSERT_EQ(brk("", 0u, 100u), 0u, "an empty string breaks at 0");
    TEST_ASSERT_EQ(bsod_aa_wrap_next_break((const char *)0, 0u,
                                           bsod_aa_BODY, 100u,
                                           WRAP_TEST_MAX_BYTES), 0u,
                   "a NULL string must not be walked");
    TEST_ASSERT_EQ(bsod_aa_wrap_next_break("abc", 0u,
                                           (const struct bsod_aa_glyph *)0, 100u,
                                           WRAP_TEST_MAX_BYTES), 0u,
                   "a NULL atlas must not be indexed");
}

static void test_start_offset_is_honoured(void)
{
    const char *t = "alpha beta";
    /* Starting mid-string must measure from there, not from byte 0. */
    TEST_ASSERT_EQ(brk(t, 6u, body_width("beta")), 10u,
                   "the walk starts at the given offset");
}

static void test_non_printable_bytes_measure_as_question_mark(void)
{
    unsigned int q = bsod_aa_advance(bsod_aa_BODY, '?');
    TEST_ASSERT_EQ(bsod_aa_advance(bsod_aa_BODY, (char)0x01), q,
                   "a control byte measures as the '?' it renders as");
    TEST_ASSERT_EQ(bsod_aa_advance(bsod_aa_BODY, (char)0xE9), q,
                   "a high byte measures as the '?' it renders as");
}

void test_register_error_screen_wrap(void)
{
    test_suite_register_cat("error-screen wrap: no line exceeds max_px",
        test_break_never_exceeds_budget, TEST_CAT_BOOT);
    test_suite_register_cat("error-screen wrap: exact-fit boundary",
        test_exact_fit_takes_the_whole_string, TEST_CAT_BOOT);
    test_suite_register_cat("error-screen wrap: breaks at the last space",
        test_breaks_at_the_last_space, TEST_CAT_BOOT);
    test_suite_register_cat("error-screen wrap: oversized word splits",
        test_word_longer_than_the_budget_splits, TEST_CAT_BOOT);
    test_suite_register_cat("error-screen wrap: single wide glyph advances",
        test_single_glyph_wider_than_budget_still_advances, TEST_CAT_BOOT);
    test_suite_register_cat("error-screen wrap: byte bound wraps, never drops",
        test_byte_bound_wraps_rather_than_dropping, TEST_CAT_BOOT);
    test_suite_register_cat("error-screen wrap: unbroken string past the copy buffer",
        test_unbroken_string_past_the_copy_buffer, TEST_CAT_BOOT);
    test_suite_register_cat("error-screen wrap: byte-bounded walk loses no bytes",
        test_byte_bound_loses_no_bytes, TEST_CAT_BOOT);
    test_suite_register_cat("error-screen wrap: zero byte bound still advances",
        test_zero_byte_bound_still_advances, TEST_CAT_BOOT);
    test_suite_register_cat("error-screen wrap: S18 subtitle overruns 800px",
        test_full_layout_subtitle_clips_at_800, TEST_CAT_BOOT);
    test_suite_register_cat("error-screen wrap: would-clip boundary + degenerates",
        test_would_clip_boundary_and_degenerates, TEST_CAT_BOOT);
    test_suite_register_cat("error-screen wrap: plan reports needed past the cap",
        test_wrap_plan_reports_needed_beyond_the_cap, TEST_CAT_BOOT);
    test_suite_register_cat("error-screen wrap: plan at zero/exact/ample caps",
        test_wrap_plan_zero_exact_and_overflow_caps, TEST_CAT_BOOT);
    test_suite_register_cat("error-screen wrap: plan counts spans past byte bound",
        test_wrap_plan_counts_spans_past_the_byte_bound, TEST_CAT_BOOT);
    test_suite_register_cat("error-screen layout: last hint bottoms at y=522",
        test_full_layout_last_hint_bottom_is_522, TEST_CAT_BOOT);
    test_suite_register_cat("error-screen wrap: exact plan on real prose",
        test_wrap_plan_separator_path_exact_on_real_text, TEST_CAT_BOOT);
    test_suite_register_cat("error-screen wrap: exact plan on spaced text",
        test_wrap_plan_exact_counts_on_spaced_text, TEST_CAT_BOOT);
    test_suite_register_cat("error-screen wrap: exact plan across the byte bound",
        test_wrap_plan_exact_counts_across_the_byte_bound, TEST_CAT_BOOT);
    test_suite_register_cat("error-screen wrap: exact plan across the pixel bound",
        test_wrap_plan_exact_counts_across_the_pixel_bound, TEST_CAT_BOOT);
    test_suite_register_cat("error-screen compact: cause cap absorbs the slack",
        test_cause_cap_absorbs_the_slack, TEST_CAT_BOOT);
    test_suite_register_cat("error-screen compact: cause cap never underflows",
        test_cause_cap_never_underflows_or_reaches_zero, TEST_CAT_BOOT);
    test_suite_register_cat("error-screen compact: real diagnosis fits at the 480x256 floor",
        test_real_diagnosis_fits_at_the_compact_floor, TEST_CAT_BOOT);
    test_suite_register_cat("error-screen compact: text right reserves only on overlap",
        test_compact_text_right_reserves_only_when_they_overlap, TEST_CAT_BOOT);
    test_suite_register_cat("error-screen compact: reservation at larger QR scales",
        test_compact_text_right_at_larger_qr_scales, TEST_CAT_BOOT);
    test_suite_register_cat("error-screen compact: reservation transition boundary",
        test_compact_text_right_transition_is_at_the_band_edge, TEST_CAT_BOOT);
    test_suite_register_cat("error-screen compact: no QR on screen, no reservation",
        test_compact_text_right_when_no_qr_fits, TEST_CAT_BOOT);
    test_suite_register_cat("error-screen wrap: empty and NULL inputs",
        test_empty_and_null_inputs, TEST_CAT_BOOT);
    test_suite_register_cat("error-screen wrap: start offset honoured",
        test_start_offset_is_honoured, TEST_CAT_BOOT);
    test_suite_register_cat("error-screen wrap: non-printables measure as '?'",
        test_non_printable_bytes_measure_as_question_mark, TEST_CAT_BOOT);
}

#endif /* KERNEL_TESTS */
