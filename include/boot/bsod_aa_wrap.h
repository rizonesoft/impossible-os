/* bsod_aa_wrap.h -- glyph metrics and the pure line-break decision for the
 * bootloader's antialiased error-screen text.
 *
 * The DRAWING stays in src/boot/uefi/bootx64.c, where it belongs: it writes
 * the GOP framebuffer through file-scope globals. What lives here is the part
 * with no side effects and the part that was wrong -- deciding where a line
 * ends for a given pixel budget.
 *
 * That decision used to accumulate a glyph advance and THEN test the bound, so
 * every wrapped line could run one glyph past the box its caller reserved. The
 * low-level blit clips at the SCREEN edge rather than at max_px, so nothing
 * downstream caught it; it was invisible while the only caller wrapped inside
 * a wide margin, and it became text drawn into the recovery QR the moment S23
 * reserved a rectangle beside one. Pulling the decision out here is what lets
 * a kernel unit test pin it at the boundaries a QEMU GOP mode cannot reach.
 *
 * Pure C, freestanding, no UEFI types, no kernel/types.h -- the same plain-type
 * pattern as boot_entries.h, so the bootloader and the kernel test binary both
 * compile it.
 */

#ifndef BSOD_AA_WRAP_H
#define BSOD_AA_WRAP_H

/* One glyph's metrics in an atlas. Layout is fixed by the generated
 * bsod_font_*.inc files, which are arrays of this struct. */
struct bsod_aa_glyph {
    unsigned char  width;
    unsigned char  height;
    signed char    bearing_x;
    signed char    bearing_y;
    unsigned char  advance;
    unsigned char  _pad;
    unsigned short data_offset;
};

/* Extra letter spacing added to each glyph advance. Selawik's own metrics are
 * tight for text read at a distance from a failing machine. */
#define BSOD_AA_TRACKING 1u

/* Longest line the renderer can copy before blitting it. The renderer holds a
 * fixed stack buffer of exactly this many bytes plus a terminator, and the
 * wrap must be bounded by it or an overlong line is truncated on copy while
 * the caller advances past the discarded bytes.
 *
 * It lives HERE, as one number, rather than being derived from sizeof(buf) at
 * the call site. A derived bound is a second place to change: passing
 * sizeof(buf) instead of sizeof(buf) - 1 would reintroduce exactly that
 * one-byte loss, and no kernel test could see it, because the tests call the
 * break function directly and never observe what the renderer passed. One
 * constant plus the renderer's _Static_assert removes the second place rather
 * than trying to detect a change to it. */
#define BSOD_AA_LINE_MAX_BYTES 127u

/* Advance of one byte in this atlas, including tracking. Anything outside
 * printable ASCII renders as '?', which is also what the blit does. */
static inline unsigned int bsod_aa_advance(const struct bsod_aa_glyph *glyphs,
                                           char c)
{
    unsigned char ch = (unsigned char)c;
    if (ch < 0x20u || ch > 0x7Eu) ch = (unsigned char)'?';
    return (unsigned int)glyphs[ch - 0x20u].advance + BSOD_AA_TRACKING;
}

/* Index one past the last byte of the line beginning at `start`.
 *
 * max_px is a HARD bound: the returned run never exceeds it, except in the one
 * case where a SINGLE glyph is wider than the whole budget. There the function
 * returns start+1 rather than start, because returning start would leave the
 * caller's loop unable to advance -- an infinite loop on a pathological
 * width. That one glyph is emitted and clipped at the screen edge.
 *
 * max_bytes is the SECOND hard bound, and it exists because the renderer
 * copies each line into a fixed stack buffer before blitting it. Bounding the
 * run in pixels alone let a wide screen with narrow glyphs produce a line
 * longer than that buffer, which the copy then truncated while the caller
 * advanced past every discarded byte -- text lost with nothing reporting it.
 * Bounding here instead means the overflow WRAPS to the next line and is
 * counted, so it can never be silently dropped.
 *
 * A word longer than either budget has no space to break at, so it is split at
 * the boundary and continues on the next line instead of overflowing. */
static inline unsigned int bsod_aa_wrap_next_break(const char *text,
                                                   unsigned int start,
                                                   const struct bsod_aa_glyph *glyphs,
                                                   unsigned int max_px,
                                                   unsigned int max_bytes)
{
    unsigned int end = start;
    unsigned int last_space = start;
    int have_space = 0;
    unsigned int cur_w = 0;

    if (!text || !glyphs) return start;
    if (max_bytes == 0u) max_bytes = 1u;   /* progress guarantee */

    while (text[end] && (end - start) < max_bytes) {
        unsigned int adv = bsod_aa_advance(glyphs, text[end]);
        if (cur_w + adv > max_px) break;
        cur_w += adv;
        if (text[end] == ' ') { last_space = end; have_space = 1; }
        end++;
    }
    if (text[end] != '\0' && have_space && last_space > start)
        end = last_space;
    if (end == start && text[start] != '\0')
        end = start + 1;
    return end;
}

/* Pixel width of a string: tracking sits BETWEEN glyphs, not after the last
 * one, which is the same rule the blit advances by. */
static inline unsigned int bsod_aa_text_width(const struct bsod_aa_glyph *glyphs,
                                              const char *str)
{
    unsigned int w = 0;
    unsigned int n = 0;
    if (!glyphs || !str) return 0;
    while (*str) { w += bsod_aa_advance(glyphs, *str); n++; str++; }
    return n ? w - BSOD_AA_TRACKING : 0u;
}

/* Would a string drawn at x be cut off by the right edge of a fb_width screen?
 *
 * This SIMULATES the blit's own stop condition rather than approximating it,
 * because production asks this question and a test asks the same one. An
 * earlier version of this check lived in the renderer while the test computed
 * its own widths, so stubbing the renderer's predicate to "never clips" left
 * every test green -- the test was measuring a copy of the decision instead of
 * the decision. */
static inline int bsod_aa_would_clip(unsigned int x, unsigned int fb_width,
                                     const struct bsod_aa_glyph *glyphs,
                                     const char *str)
{
    unsigned int cx = x;
    if (!glyphs || !str || !*str) return 0;
    if (x >= fb_width) return 1;
    while (*str) {
        unsigned char ch = (unsigned char)*str;
        unsigned int adv;
        if (ch < 0x20u || ch > 0x7Eu) ch = (unsigned char)'?';
        adv = (unsigned int)glyphs[ch - 0x20u].advance;
        if (cx + adv > fb_width) return 1;   /* the blit stops here */
        cx += adv + BSOD_AA_TRACKING;
        str++;
    }
    return 0;
}

/* How many lines `text` needs at this width, and how many of them would
 * actually be DRAWN under a max_lines cap. Production wraps by calling
 * bsod_aa_wrap_next_break in a loop and reports needed > cap as truncation;
 * this is that same loop with the drawing removed, so a test can assert the
 * planning decision rather than a reimplementation of it. */
static inline unsigned int bsod_aa_wrap_plan(const char *text,
                                             const struct bsod_aa_glyph *glyphs,
                                             unsigned int max_px,
                                             unsigned int max_bytes,
                                             unsigned int max_lines,
                                             unsigned int *out_drawn)
{
    unsigned int start = 0;
    unsigned int needed = 0;
    unsigned int drawn = 0;

    if (out_drawn) *out_drawn = 0;
    if (!text || !glyphs) return 0;
    while (text[start]) {
        unsigned int end = bsod_aa_wrap_next_break(text, start, glyphs,
                                                   max_px, max_bytes);
        if (end == start) break;            /* cannot happen; belt and braces */
        needed++;
        if (drawn < max_lines) drawn++;
        start = end;
        while (text[start] == ' ') start++;
    }
    if (out_drawn) *out_drawn = drawn;
    return needed;
}

#endif /* BSOD_AA_WRAP_H */
