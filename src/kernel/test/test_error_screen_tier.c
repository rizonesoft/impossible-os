/* test_error_screen_tier.c -- error-screen render-tier floor tests.
 *
 * The bootloader picks which error screen a display can carry with
 * bsod_tier_for() (include/boot/bsod_render_tier.h). That predicate decides
 * whether a user in front of a failing machine gets the full graphical BSOD,
 * the compact four-field screen, or no words at all -- and its floors are
 * MEASURED numbers, so the thing worth pinning is their exact boundaries.
 *
 * The smoke test proves the compact path renders end-to-end at 640x480, but a
 * QEMU GOP offers no 511-pixel-wide mode, so the boundary either side of each
 * floor is only reachable here.
 *
 * XREF: 01-boot-platform/TODO-03-bootloader-error-recovery.md "No On-Screen
 * Words Below 800x600 -- the QR Renders, the Text Does Not"
 *
 * PURE-HELPER TESTS ONLY. Per CLAUDE.md "Test Code Policy", these tests never
 * call live boot infrastructure: bsod_tier_for() is a static inline over five
 * scalars with no globals, no allocation and no UEFI types.
 */

#ifdef KERNEL_TESTS

#include "kernel/test/test.h"
#include "../../../include/boot/bsod_render_tier.h"
#include "kernel/types.h"

/* Common shape: a present framebuffer with square pixels and a sane stride. */
static bsod_tier_t tier_at(unsigned int w, unsigned int h)
{
    return bsod_tier_for(1, w, h, w, 0u);
}

/* ---- No framebuffer at all ---- */

static void test_absent_framebuffer_is_none(void)
{
    TEST_ASSERT_EQ(bsod_tier_for(0, 1920u, 1080u, 1920u, 0u), BSOD_TIER_NONE,
                   "no published framebuffer must never claim a render tier");
}

static void test_zero_dimensions_are_none(void)
{
    TEST_ASSERT_EQ(bsod_tier_for(1, 0u, 600u, 0u, 0u), BSOD_TIER_NONE,
                   "zero width is not a renderable screen");
    TEST_ASSERT_EQ(bsod_tier_for(1, 800u, 0u, 800u, 0u), BSOD_TIER_NONE,
                   "zero height is not a renderable screen");
}

/* ---- Pixel format ---- */

static void test_bitmask_format_is_none(void)
{
    /* BitMask carries no fixed channel order we can pack, so no text tier
     * accepts it however large the mode is. */
    TEST_ASSERT_EQ(bsod_tier_for(1, 1920u, 1080u, 1920u,
                                 BSOD_PIXEL_FORMAT_BITMASK),
                   BSOD_TIER_NONE,
                   "BitMask pixel format must not reach a text renderer");
}

static void test_both_packable_formats_reach_full(void)
{
    TEST_ASSERT_EQ(bsod_tier_for(1, 1024u, 768u, 1024u, 0u), BSOD_TIER_FULL,
                   "RGBX must reach the full tier");
    TEST_ASSERT_EQ(bsod_tier_for(1, 1024u, 768u, 1024u, 1u), BSOD_TIER_FULL,
                   "BGRX must reach the full tier");
}

/* ---- Stride ---- */

static void test_stride_below_width_is_none(void)
{
    /* A row stride shorter than the visible width makes every write past
     * pitch land on the next scanline, and the last row past the mapping. */
    TEST_ASSERT_EQ(bsod_tier_for(1, 1024u, 768u, 1023u, 0u), BSOD_TIER_NONE,
                   "stride below width must be refused, not rendered into");
}

static void test_stride_above_width_is_fine(void)
{
    /* Padded scanlines are normal on real firmware. */
    TEST_ASSERT_EQ(bsod_tier_for(1, 1024u, 768u, 2048u, 0u), BSOD_TIER_FULL,
                   "a padded stride is a valid mode, not a rejected one");
}

/* ---- FULL boundary: 800x600 ---- */

static void test_full_floor_exact(void)
{
    TEST_ASSERT_EQ(tier_at(BSOD_FULL_MIN_W, BSOD_FULL_MIN_H), BSOD_TIER_FULL,
                   "the full floor itself must be the full tier");
}

static void test_one_pixel_below_full_width_is_compact(void)
{
    TEST_ASSERT_EQ(tier_at(BSOD_FULL_MIN_W - 1u, BSOD_FULL_MIN_H),
                   BSOD_TIER_COMPACT,
                   "just under the full width must fall to compact, not none");
}

static void test_one_pixel_below_full_height_is_compact(void)
{
    TEST_ASSERT_EQ(tier_at(BSOD_FULL_MIN_W, BSOD_FULL_MIN_H - 1u),
                   BSOD_TIER_COMPACT,
                   "just under the full height must fall to compact, not none");
}

/* ---- COMPACT boundary: 512x256 ---- */

static void test_compact_floor_exact(void)
{
    TEST_ASSERT_EQ(tier_at(BSOD_COMPACT_MIN_W, BSOD_COMPACT_MIN_H),
                   BSOD_TIER_COMPACT,
                   "the measured compact floor must render words");
}

static void test_one_pixel_below_compact_width_is_none(void)
{
    TEST_ASSERT_EQ(tier_at(BSOD_COMPACT_MIN_W - 1u, BSOD_COMPACT_MIN_H),
                   BSOD_TIER_NONE,
                   "below the compact width the text would be truncated, so "
                   "the tier must report none rather than a partial render");
}

static void test_one_pixel_below_compact_height_is_none(void)
{
    TEST_ASSERT_EQ(tier_at(BSOD_COMPACT_MIN_W, BSOD_COMPACT_MIN_H - 1u),
                   BSOD_TIER_NONE,
                   "below the compact height the field stack does not fit");
}

/* ---- LITERAL floors, not the constants ---- */

static void test_floors_are_the_literal_measured_values(void)
{
    /* Every boundary test above derives its inputs from BSOD_FULL_MIN_W and
     * friends, so raising a floor constant would move those tests with it and
     * they would all still pass. These pass LITERAL dimensions instead, so a
     * floor that moves changes a verdict here.
     *
     * Deliberately behaviour, not values: asserting BSOD_FULL_MIN_W == 800
     * would only confirm the number was typed, which the compiler already
     * knows. What is worth pinning is that an 800x600 display gets the full
     * screen and a 799-wide one does not. */
    TEST_ASSERT_EQ(tier_at(800u, 600u), BSOD_TIER_FULL, "800x600 is full");
    TEST_ASSERT_EQ(tier_at(799u, 600u), BSOD_TIER_COMPACT, "799x600 is compact");
    TEST_ASSERT_EQ(tier_at(512u, 256u), BSOD_TIER_COMPACT, "512x256 is compact");
    TEST_ASSERT_EQ(tier_at(511u, 256u), BSOD_TIER_NONE, "511x256 is none");
    /* The HEIGHT floors need their own literals. Every height-boundary test
     * above derives its input from the constant, so lowering BSOD_FULL_MIN_H
     * to 599 or BSOD_COMPACT_MIN_H to 255 moved those tests with it and left
     * the whole suite green -- admission below the measured height escaping
     * the coverage that claims to pin it. */
    TEST_ASSERT_EQ(tier_at(800u, 599u), BSOD_TIER_COMPACT, "800x599 is compact");
    TEST_ASSERT_EQ(tier_at(512u, 255u), BSOD_TIER_NONE, "512x255 is none");
}

/* ---- The modes this section exists for ---- */

static void test_common_low_modes_are_compact(void)
{
    /* 640x480 is the mode the section was filed over; 800x480 and 640x400 are
     * the other two real GOP modes that fail the full floor. */
    TEST_ASSERT_EQ(tier_at(640u, 480u), BSOD_TIER_COMPACT,
                   "640x480 must render words -- this is the reported gap");
    TEST_ASSERT_EQ(tier_at(800u, 480u), BSOD_TIER_COMPACT,
                   "800x480 clears the full width but not its height");
    TEST_ASSERT_EQ(tier_at(640u, 400u), BSOD_TIER_COMPACT,
                   "640x400 must still render words");
}

static void test_rejected_400x200_is_none(void)
{
    /* Measured and rejected during the section: at 400x200 the action line
     * wraps to a third row and would be truncated while the render still
     * reported success. Reporting none is the honest answer there. */
    TEST_ASSERT_EQ(tier_at(400u, 200u), BSOD_TIER_NONE,
                   "400x200 was measured as too small for the compact fields");
}

/* ---- QR recovery block: independent of the text floors ---- */

static void test_qr_extent_scale_ladder(void)
{
    /* 29 modules at scale 4 plus a 16px quiet zone either side. */
    TEST_ASSERT_EQ(bsod_qr_block_extent(1024u), 29u * 4u + 32u,
                   "default module scale is 4");
    TEST_ASSERT_EQ(bsod_qr_block_extent(1920u), 29u * 6u + 48u,
                   "scale steps to 6 at 1920 wide");
    TEST_ASSERT_EQ(bsod_qr_block_extent(2560u), 29u * 8u + 64u,
                   "scale steps to 8 at 2560 wide");
    TEST_ASSERT_EQ(bsod_qr_block_extent(1919u), 29u * 4u + 32u,
                   "one pixel below the 1920 step keeps scale 4");
    /* Both steps need their LOWER side pinned too. Asserting only the scale AT
     * 2560 leaves an earlier transition undetected: moving the step down to
     * 2048 raises the extent there from 222 to 296, which falsely rejects a QR
     * on a 2048x256 compact screen and removes a recovery route. */
    TEST_ASSERT_EQ(bsod_qr_block_extent(2559u), 29u * 6u + 48u,
                   "one pixel below the 2560 step keeps scale 6");
    TEST_ASSERT_EQ(bsod_qr_block_extent(2048u), 29u * 6u + 48u,
                   "2048 is scale 6, not 8 -- the step has not moved down");
    TEST_ASSERT_EQ(bsod_qr_block_fits(1, 2048u, 256u), 1,
                   "so the QR still fits a 2048x256 screen");
}

static void test_qr_fits_at_its_own_boundary(void)
{
    /* 148px block + a 12px margin = 160. This boundary is the recovery route
     * for machines below every text floor, so it must not move with them. */
    unsigned int need = bsod_qr_block_extent(160u) + BSOD_QR_BLOCK_MARGIN;
    TEST_ASSERT_EQ(need, 160u, "the QR block needs exactly 160px each way");
    TEST_ASSERT_EQ(bsod_qr_block_fits(1, 160u, 160u), 1,
                   "the QR must fit at exactly its own extent plus margin");
    TEST_ASSERT_EQ(bsod_qr_block_fits(1, 159u, 160u), 0,
                   "one pixel under the width the QR must not claim a fit");
    TEST_ASSERT_EQ(bsod_qr_block_fits(1, 160u, 159u), 0,
                   "one pixel under the height the QR must not claim a fit");
}

static void test_qr_survives_below_every_text_floor(void)
{
    /* The point of keeping the QR out of the tier predicate: a mode with no
     * words still has a scannable route to help. */
    TEST_ASSERT_EQ(tier_at(320u, 200u), BSOD_TIER_NONE,
                   "320x200 carries no text tier");
    TEST_ASSERT_EQ(bsod_qr_block_fits(1, 320u, 200u), 1,
                   "yet the recovery QR still fits there and must be drawn");
    TEST_ASSERT_EQ(bsod_qr_block_fits(1, 640u, 480u), 1,
                   "and at the mode this section was filed over");
}

static void test_compact_tier_does_not_imply_a_qr(void)
{
    /* A wide, short mode clears the compact floor while the QR block does
     * not fit: at 2560 wide the module scale steps to 8, so the block needs
     * 296 + 12 = 308 rows against 256. The renderer must not tell the user to
     * scan a QR that will not be there -- and it has already erased whatever
     * the console said. */
    TEST_ASSERT_EQ(tier_at(2560u, 256u), BSOD_TIER_COMPACT,
                   "2560x256 is a valid compact mode");
    TEST_ASSERT_EQ(bsod_qr_block_fits(1, 2560u, 256u), 0,
                   "yet no QR fits on it, so the two must be asked separately");
    TEST_ASSERT_EQ(bsod_qr_block_extent(2560u) + BSOD_QR_BLOCK_MARGIN, 308u,
                   "the QR block needs 308 rows at that module scale");
}

static int action_is(const char *got, const char *want)
{
    while (*want && *got && *want == *got) { want++; got++; }
    return *want == '\0' && *got == '\0';
}

static void test_compact_action_follows_the_qr_not_the_tier(void)
{
    /* The branch itself, not just the geometry behind it. Returning the QR
     * instruction unconditionally -- which is what shipped before -- leaves
     * every geometry assertion green while telling a 2560x256 user to scan
     * something that is not on the screen. */
    const char *url = "https://impossibleos.co/err/000d";

    TEST_ASSERT_EQ(action_is(bsod_compact_action(1, url),
                             "Scan the QR code below for help."), 1,
                   "with a QR on screen, point at the QR");
    TEST_ASSERT_EQ(action_is(bsod_compact_action(0, url), url), 1,
                   "without one, give the user the URL to type");
    TEST_ASSERT_EQ(action_is(bsod_compact_action(0, ""),
                             "See the serial log for the full diagnosis."), 1,
                   "and with no URL either, say where the diagnosis is");
    TEST_ASSERT_EQ(action_is(bsod_compact_action(0, (const char *)0),
                             "See the serial log for the full diagnosis."), 1,
                   "a NULL URL buffer must not be dereferenced");
    /* Bound to the real geometry: the mode that motivated the branch. */
    TEST_ASSERT_EQ(action_is(bsod_compact_action(
                       bsod_qr_block_fits(1, 2560u, 256u), url), url), 1,
                   "at 2560x256 the user gets the URL, not a QR instruction");
    TEST_ASSERT_EQ(action_is(bsod_compact_action(
                       bsod_qr_block_fits(1, 640u, 480u), url),
                       "Scan the QR code below for help."), 1,
                   "and at 640x480 they get the QR instruction");
}

static void test_qr_requires_a_framebuffer(void)
{
    TEST_ASSERT_EQ(bsod_qr_block_fits(0, 1920u, 1080u), 0,
                   "no framebuffer means no QR, whatever the reported size");
}

/* ---- Names ---- */

static int tier_name_is(bsod_tier_t t, const char *want)
{
    const char *got = bsod_tier_name(t);
    while (*want && *got && *want == *got) { want++; got++; }
    return *want == '\0' && *got == '\0';
}

static void test_tier_names(void)
{
    /* The names are the smoke test's only view of which layout ran, so a
     * rename is a silent break of a shell assertion rather than a compile
     * error. Compared WHOLE, including the terminator: matching only the first
     * character accepted "failure", "corrupt" and "nothing" in their place. */
    TEST_ASSERT_EQ(tier_name_is(BSOD_TIER_FULL, "full"), 1, "full name");
    TEST_ASSERT_EQ(tier_name_is(BSOD_TIER_COMPACT, "compact"), 1, "compact name");
    TEST_ASSERT_EQ(tier_name_is(BSOD_TIER_NONE, "none"), 1, "none name");
    TEST_ASSERT_EQ(tier_name_is(BSOD_TIER_FULL, "fullx"), 0,
                   "and the comparison itself must reject a longer name");
}

void test_register_error_screen_tier(void)
{
    test_suite_register_cat("error-screen tier: absent framebuffer is none",
        test_absent_framebuffer_is_none, TEST_CAT_BOOT);
    test_suite_register_cat("error-screen tier: zero dimensions are none",
        test_zero_dimensions_are_none, TEST_CAT_BOOT);
    test_suite_register_cat("error-screen tier: BitMask format is none",
        test_bitmask_format_is_none, TEST_CAT_BOOT);
    test_suite_register_cat("error-screen tier: RGBX and BGRX reach full",
        test_both_packable_formats_reach_full, TEST_CAT_BOOT);
    test_suite_register_cat("error-screen tier: stride below width is none",
        test_stride_below_width_is_none, TEST_CAT_BOOT);
    test_suite_register_cat("error-screen tier: padded stride is accepted",
        test_stride_above_width_is_fine, TEST_CAT_BOOT);
    test_suite_register_cat("error-screen tier: full floor exact",
        test_full_floor_exact, TEST_CAT_BOOT);
    test_suite_register_cat("error-screen tier: one px under full width is compact",
        test_one_pixel_below_full_width_is_compact, TEST_CAT_BOOT);
    test_suite_register_cat("error-screen tier: one px under full height is compact",
        test_one_pixel_below_full_height_is_compact, TEST_CAT_BOOT);
    test_suite_register_cat("error-screen tier: compact floor exact",
        test_compact_floor_exact, TEST_CAT_BOOT);
    test_suite_register_cat("error-screen tier: one px under compact width is none",
        test_one_pixel_below_compact_width_is_none, TEST_CAT_BOOT);
    test_suite_register_cat("error-screen tier: one px under compact height is none",
        test_one_pixel_below_compact_height_is_none, TEST_CAT_BOOT);
    test_suite_register_cat("error-screen tier: floors are the literal measured values",
        test_floors_are_the_literal_measured_values, TEST_CAT_BOOT);
    test_suite_register_cat("error-screen tier: 640x480 / 800x480 / 640x400 are compact",
        test_common_low_modes_are_compact, TEST_CAT_BOOT);
    test_suite_register_cat("error-screen tier: measured-rejected 400x200 is none",
        test_rejected_400x200_is_none, TEST_CAT_BOOT);
    test_suite_register_cat("error-screen tier: channel names are stable",
        test_tier_names, TEST_CAT_BOOT);
    test_suite_register_cat("error-screen QR: module scale ladder",
        test_qr_extent_scale_ladder, TEST_CAT_BOOT);
    test_suite_register_cat("error-screen QR: fit boundary at 160px",
        test_qr_fits_at_its_own_boundary, TEST_CAT_BOOT);
    test_suite_register_cat("error-screen QR: survives below every text floor",
        test_qr_survives_below_every_text_floor, TEST_CAT_BOOT);
    test_suite_register_cat("error-screen QR: compact tier does not imply a QR",
        test_compact_tier_does_not_imply_a_qr, TEST_CAT_BOOT);
    test_suite_register_cat("error-screen action: follows the QR, not the tier",
        test_compact_action_follows_the_qr_not_the_tier, TEST_CAT_BOOT);
    test_suite_register_cat("error-screen QR: needs a framebuffer",
        test_qr_requires_a_framebuffer, TEST_CAT_BOOT);
}

#endif /* KERNEL_TESTS */
