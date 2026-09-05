/* bsod_render_tier.h -- which error-screen layout a framebuffer can carry.
 *
 * The bootloader has three on-screen error surfaces (the S18 graphical BSOD,
 * the S22 rejected-store notice, the anti-rollback halt screen) and every one
 * of them used to carry its own copy of a bare `width >= 800 && height >= 600`
 * test. Below that line they all fell through together, so a valid 640x480 GOP
 * mode -- which common firmware reports -- produced no on-screen WORDS at all.
 *
 * This header owns the floor. One predicate answers "what fits here", the
 * three surfaces route on its answer, and the numbers below are MEASURED
 * against the checked-in Selawik atlases rather than chosen. Full derivation
 * lives with bsod_render_tier() in src/boot/uefi/bootx64.c; the short version:
 *
 *   FULL is the S18 layout and it stays at 800x600. What is MEASURED here is
 *   why the modes below cannot carry it, not a lower floor: its last hint line
 *   bottoms at y=522 -- from real glyph placement, py + ascent + bearing_y +
 *   height, not py + ascent, which understated it by a descender and read 502
 *   on a first pass. A 480-row mode clips that line by 42 px, which is what
 *   excludes 640x480 and 800x480; width is not the constraint there.
 *
 *   No LOWER full floor is claimed, deliberately. Per-element bounds do not
 *   compose into a layout: at a hypothetical 640x522 the 572 px title, the
 *   hint column and the 194-row QR block each fit, while the wrapped detail
 *   text and the QR rectangle overlap. A real lower floor would mean checking
 *   every element pair, and it buys nothing -- the band below 800x600 is
 *   exactly what the compact tier renders properly.
 *
 *   COMPACT is the S23 four-field screen (heading / error code / cause /
 *   action), drawing from an 11-row pool of wrapped 16px body lines --
 *   2 heading + 1 code + 5 cause + 3 action nominal, with the cause taking
 *   whatever the other three leave. MEASURED at the 512x256 floor, where the
 *   reserved QR leaves a 312 px budget: the worst real content is the A/B GPT
 *   diagnosis, taking 6 of the 8 rows the cause is allotted there, for 9 rows
 *   and 212 px of the 256 available.
 *
 *   400x200 was measured and REJECTED. Under this allocation its 200 px budget
 *   wraps that diagnosis to 12 rows against a cap of 6, and the stack reaches
 *   252 px on a 200 px screen -- it overflows the display, not merely a field
 *   budget.
 *
 * The QR recovery route is deliberately NOT gated here. qr_render_error_url()
 * applies its own independent fit check and already paints at 640x480, so a
 * machine below every text floor keeps a scannable route to help. Folding the
 * QR into this predicate would have deleted a working recovery path while
 * every counter still read green.
 *
 * Pure C, freestanding, no UEFI types, no kernel/types.h -- same plain-type
 * pattern as boot_entries.h / boot_entry_kind.h, so the bootloader
 * (-ffreestanding x86_64-elf) and the kernel unit-test binary both link it and
 * the floors are pinned by a test rather than only by a boot.
 */

#ifndef BSOD_RENDER_TIER_H
#define BSOD_RENDER_TIER_H

_Static_assert(sizeof(unsigned int) == 4, "bsod_render_tier.h assumes 32-bit unsigned int");

#define BSOD_FULL_MIN_W     800u
#define BSOD_FULL_MIN_H     600u
#define BSOD_COMPACT_MIN_W  512u
#define BSOD_COMPACT_MIN_H  256u

/* Pixel-format encoding shared with boot_info: 0=RGBX, 1=BGRX, 2=BitMask.
 * BitMask carries no fixed channel order we can pack, so no text tier
 * accepts it -- the QR still renders there, because it writes palindromic
 * black/white pixels that land correctly whatever the order is. */
#define BSOD_PIXEL_FORMAT_BITMASK 2u

/* Recovery-QR geometry. The module count is the version-3 matrix the
 * bootloader encodes; the scale ladder was transcribed into three renderers
 * before this header existed, and a transcription that drifts does not fail a
 * build -- it silently draws a QR at a different size than the caller reserved
 * space for. bootx64.c static-asserts its own QR_SIZE against BSOD_QR_MODULES
 * so the two cannot separate. */
#define BSOD_QR_MODULES       29u
#define BSOD_QR_BLOCK_MARGIN  12u

/* Pixel extent of the QR block (matrix plus quiet zone) at the module scale
 * this framebuffer width selects. */
static inline unsigned int bsod_qr_block_extent(unsigned int width)
{
    unsigned int mod = 4u;
    if (width >= 1920u) mod = 6u;
    if (width >= 2560u) mod = 8u;
    return BSOD_QR_MODULES * mod + (mod * 4u) * 2u;
}

/* Whether the QR block fits at all. Deliberately INDEPENDENT of the text tier
 * floors above: a machine too small for any words still keeps a scannable
 * recovery route, and every consumer applies this predicate rather than
 * assuming a text tier implies it. */
static inline int bsod_qr_block_fits(int fb_present,
                                     unsigned int width,
                                     unsigned int height)
{
    unsigned int t;
    if (!fb_present) return 0;
    t = bsod_qr_block_extent(width);
    return width  >= t + BSOD_QR_BLOCK_MARGIN &&
           height >= t + BSOD_QR_BLOCK_MARGIN;
}

typedef enum {
    BSOD_TIER_NONE = 0,   /* no usable framebuffer -- ConOut / serial only */
    BSOD_TIER_COMPACT,    /* words fit, the S18 layout does not */
    BSOD_TIER_FULL,       /* the full S18 graphical BSOD fits */
} bsod_tier_t;

/* fb_present: non-zero when a framebuffer address was actually published.
 * pitch is the row stride in PIXELS, not bytes. */
static inline bsod_tier_t bsod_tier_for(int fb_present,
                                        unsigned int width,
                                        unsigned int height,
                                        unsigned int pitch,
                                        unsigned int pixel_format)
{
    if (!fb_present) return BSOD_TIER_NONE;
    if (pixel_format == BSOD_PIXEL_FORMAT_BITMASK) return BSOD_TIER_NONE;
    if (width == 0u || height == 0u) return BSOD_TIER_NONE;
    /* Stride below the visible width means every row write past pitch spills
     * into the next scanline, and the last row past the end of the mapping.
     * The GOP mode selector already rejects such modes; this is the second
     * layer, because a firmware-default mode never passes through it. */
    if (pitch < width) return BSOD_TIER_NONE;

    if (width >= BSOD_FULL_MIN_W && height >= BSOD_FULL_MIN_H)
        return BSOD_TIER_FULL;
    if (width >= BSOD_COMPACT_MIN_W && height >= BSOD_COMPACT_MIN_H)
        return BSOD_TIER_COMPACT;
    return BSOD_TIER_NONE;
}

static inline const char *bsod_tier_name(bsod_tier_t t)
{
    switch (t) {
        case BSOD_TIER_FULL:    return "full";
        case BSOD_TIER_COMPACT: return "compact";
        case BSOD_TIER_NONE:    return "none";
    }
    return "none";
}

/* ---- Compact-screen geometry -------------------------------------------
 *
 * The compact renderer reserves the QR's rectangle when the two would share
 * rows, and takes the full width when they would not. That decision lived
 * inside the renderer against its framebuffer globals, so no test could reach
 * it: at 640x480 the QR starts well below the text band and the early return
 * is taken, and only a short screen such as the 512x256 floor exercises the
 * reservation at all. Returning the full width unconditionally would have kept
 * every test green while letting text run into the QR. */

#define BSOD_COMPACT_HEAD_LINES    2u
#define BSOD_COMPACT_CODE_LINES    1u
/* The cause is the caller's own diagnosis and is the field worth spending
 * rows on, so its budget is the NOMINAL share and the renderer hands it
 * whatever the other three fields do not use. MEASURED against every
 * boot_fatal detail string in the tree at the 512x256 floor's 312-pixel
 * budget: the longest needs 7 rows, which the dynamic cap supplies when the
 * heading, code and action take their usual 4 between them. A fixed 3 dropped
 * the tail of real messages -- including the words "Reflash or boot recovery
 * media" from the A/B GPT failure -- while the fill erased the ConOut text
 * that had carried them. */
#define BSOD_COMPACT_CAUSE_LINES   5u
#define BSOD_COMPACT_ACTION_LINES  3u
#define BSOD_COMPACT_MAX_LINES  (BSOD_COMPACT_HEAD_LINES +  \
                                 BSOD_COMPACT_CODE_LINES +  \
                                 BSOD_COMPACT_CAUSE_LINES + \
                                 BSOD_COMPACT_ACTION_LINES)
#define BSOD_COMPACT_MARGIN     16u
/* Horizontal clearance between the compact text rectangle and the QR block. */
#define BSOD_COMPACT_QR_GAP      8u

/* Worst-case height of the text stack. Worst-case rather than this message's
 * height on purpose: the reservation must be decided before wrapping, and a
 * content-dependent boundary would make the layout depend on its own text.
 * body_line_h comes from the atlas, which this header deliberately knows
 * nothing about. */
static inline unsigned int bsod_compact_band_height(unsigned int body_line_h)
{
    return BSOD_COMPACT_MARGIN + BSOD_COMPACT_MAX_LINES * body_line_h +
           BSOD_COMPACT_MARGIN;
}

/* Right edge available to compact text on a width x height screen. */
static inline unsigned int bsod_compact_text_right(unsigned int width,
                                                   unsigned int height,
                                                   int reserve_qr,
                                                   unsigned int body_line_h)
{
    unsigned int qr_total = bsod_qr_block_extent(width);
    unsigned int band_h = bsod_compact_band_height(body_line_h);
    unsigned int reserve = qr_total + BSOD_QR_BLOCK_MARGIN + BSOD_COMPACT_QR_GAP;

    /* A caller that paints no QR must not be narrowed for one. */
    if (!reserve_qr) return width;
    /* No QR fits on this screen -- nothing to avoid. */
    if (!bsod_qr_block_fits(1, width, height)) return width;
    /* The QR sits entirely below the text stack. */
    if (height - qr_total - BSOD_QR_BLOCK_MARGIN >= band_h) return width;
    /* Overlapping rows: keep the text left of the QR. */
    if (width <= reserve) return width;
    return width - reserve;
}

/* Rows the CAUSE field may use, given what the other three fields took.
 *
 * The cause carries the caller's own diagnosis and real fatal detail runs to
 * about 200 characters, so it gets every row the others leave rather than a
 * fixed share. A fixed three rows cut the tail off the longest messages at the
 * 512x256 floor -- including the words "Reflash or boot recovery media" -- and
 * the full-screen fill had already erased the console text that carried them.
 *
 * It lives here rather than inline in the renderer for the reason the rest of
 * this header exists: an inline allocator is a decision no test can reach, and
 * restoring the fixed cap left every assertion green. */
static inline unsigned int bsod_compact_cause_cap(unsigned int head_drawn,
                                                  unsigned int code_drawn,
                                                  unsigned int action_drawn)
{
    unsigned int used = head_drawn + code_drawn + action_drawn;
    if (used >= BSOD_COMPACT_MAX_LINES) return 1u;   /* always show something */
    return BSOD_COMPACT_MAX_LINES - used;
}

/* Which recovery action the compact screen tells the user to take.
 *
 * A compact tier does NOT imply a QR -- a valid 2560x256 mode clears the
 * compact floor while the QR block needs 308 rows -- so the instruction is
 * chosen from whether the QR will actually be there, not from the tier. The
 * full-screen fill has already erased whatever the console said, so an
 * instruction pointing at an absent QR leaves the user with nothing.
 *
 * `url` is the caller's recovery-URL buffer, or NULL when it could not be
 * built. The returned pointer is either a string literal or `url` itself, so
 * the caller's buffer must outlive the render. */
static inline const char *bsod_compact_action(int qr_fits, const char *url)
{
    if (qr_fits) return "Scan the QR code below for help.";
    if (url && *url) return url;
    return "See the serial log for the full diagnosis.";
}

#endif /* BSOD_RENDER_TIER_H */
