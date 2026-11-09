/* boot_menu_layout.h -- geometry of the bootloader's GOP boot menu.
 *
 * Shared by src/boot/uefi/bootx64.c (which draws the menu) and the kernel
 * boot tests (which check every band against the real font atlases), so the
 * layout the tests prove is the layout that ships.
 *
 * The menu repaints one band at a time (boot_menu_repaint_plan in
 * boot_policy.h): a row band on a highlight move, the footer band on a
 * countdown tick. A band repaint clears only its own rectangle, so each line's
 * ink must lie wholly inside its band or a repaint erases a neighbour's text.
 *
 * Freestanding: no UEFI types, no kernel headers.
 */
#ifndef BOOT_MENU_LAYOUT_H
#define BOOT_MENU_LAYOUT_H

#include "bsod_aa_wrap.h"

/* Boot menu cap. UEFI section 13.5 allows far more entries; 16 visible rows
 * matches systemd-boot. */
#define BOOT_MENU_MAX_VISIBLE 16u

#define BOOT_MENU_COL_X      80u
#define BOOT_MENU_TITLE_Y    60u    /* title band: [TITLE_Y, LIST_Y0) */
#define BOOT_MENU_LIST_Y0    130u   /* row i band: [LIST_Y0 + i*ROW_H, +ROW_H) */
#define BOOT_MENU_ROW_H      32u
#define BOOT_MENU_LIST_END   (BOOT_MENU_LIST_Y0 + BOOT_MENU_ROW_H * BOOT_MENU_MAX_VISIBLE)
#define BOOT_MENU_FOOTER_GAP 30u
#define BOOT_MENU_FOOTER_Y   (BOOT_MENU_LIST_END + BOOT_MENU_FOOTER_GAP)
#define BOOT_MENU_FOOTER_H   BOOT_MENU_ROW_H
#define BOOT_MENU_HI_X       (BOOT_MENU_COL_X - 8u)   /* highlight bar */
#define BOOT_MENU_HI_W       808u
#define BOOT_MENU_TEXT_PAD   8u     /* text stops this far inside the bar's right end */

/* How much of the layout fits a framebuffer of a given height. The list shows
 * `rows` entries (the rest scroll), and the footer sits right below them, so
 * an 800x600 or 640x480 mode keeps its countdown on screen. rows == 0: not
 * even one row and the footer fit, and the caller uses the text fallback. */
typedef struct {
    unsigned int rows;
    unsigned int footer_y;
} boot_menu_fit_t;

static inline boot_menu_fit_t boot_menu_fit(unsigned int fb_height)
{
    boot_menu_fit_t f = { 0u, 0u };
    unsigned int fixed = BOOT_MENU_LIST_Y0 + BOOT_MENU_FOOTER_GAP + BOOT_MENU_FOOTER_H;
    if (fb_height <= fixed)
        return f;
    unsigned int rows = (fb_height - fixed) / BOOT_MENU_ROW_H;
    if (rows > BOOT_MENU_MAX_VISIBLE)
        rows = BOOT_MENU_MAX_VISIBLE;
    if (rows == 0u)
        return f;
    f.rows = rows;
    f.footer_y = BOOT_MENU_LIST_Y0 + rows * BOOT_MENU_ROW_H + BOOT_MENU_FOOTER_GAP;
    return f;
}

/* The one-off editor's field: which character to draw first so the cursor
 * stays inside a field `budget` pixels wide. Starts from the previous window
 * `start` so the text does not jump on every key: it moves left only when the
 * cursor went left of it, and right only as far as keeping the text up to the
 * cursor (plus `caret` pixels) inside the field needs. Characters are
 * measured with bsod_aa_advance, as the blit draws them. */
static inline unsigned int boot_menu_edit_view_start(const struct bsod_aa_glyph *glyphs, const char *text,
                                                     unsigned int len, unsigned int cursor,
                                                     unsigned int start, unsigned int budget,
                                                     unsigned int caret)
{
    if (!glyphs || !text)
        return 0u;
    if (cursor > len)
        cursor = len;
    if (start > cursor)
        start = cursor;
    for (;;) {
        unsigned int w = caret;
        for (unsigned int i = start; i < cursor; i++)
            w += bsod_aa_advance(glyphs, text[i]);
        if (w <= budget || start >= cursor)
            return start;
        start++;
    }
}

/* Copy `src` into `out` (capacity `cap`, including the NUL) so its drawn width
 * is at most `budget` pixels: whole when it fits, otherwise the longest prefix
 * that still fits with "..." after it, and empty when not even that does.
 * Returns 1 when the text was cut. */
static inline int boot_menu_fit_text(const struct bsod_aa_glyph *glyphs, const char *src,
                                     unsigned int budget, char *out, unsigned int cap)
{
    if (!out || cap == 0u)
        return 0;
    out[0] = '\0';
    if (!glyphs || !src)
        return 0;
    unsigned int n = 0;
    while (src[n] && n + 1u < cap) { out[n] = src[n]; n++; }
    out[n] = '\0';
    int cut = src[n] != '\0';
    if (!cut && bsod_aa_text_width(glyphs, out) <= budget)
        return 0;
    /* Shorten until the prefix plus the ellipsis fits. */
    while (n > 0u) {
        n--;
        if (n + 4u > cap)
            continue;
        out[n] = '.'; out[n + 1u] = '.'; out[n + 2u] = '.'; out[n + 3u] = '\0';
        if (bsod_aa_text_width(glyphs, out) <= budget)
            return 1;
    }
    out[0] = '\0';
    return 1;
}

/* The rows of ink a glyph set can cover, relative to the y its renderer adds
 * the ascent to. bsod_aa_char places a glyph's ink at
 * py + ascent + bearing_y .. + height, and the shipped atlases measure
 * bearing_y from the line top, so [*top, *bottom) is the ink box of a line
 * drawn at py with ascent 0. */
static inline void bsod_aa_ink_rows(const struct bsod_aa_glyph *glyphs,
                                    int *top, int *bottom)
{
    int t = 0, b = 0, seen = 0;
    for (unsigned int i = 0; i < 95u; i++) {
        const struct bsod_aa_glyph *g = &glyphs[i];
        if (g->width == 0u || g->height == 0u)
            continue;
        int gt = (int)g->bearing_y;
        int gb = gt + (int)g->height;
        if (!seen || gt < t) t = gt;
        if (!seen || gb > b) b = gb;
        seen = 1;
    }
    *top = t;
    *bottom = b;
}

/* The py to pass bsod_aa_string (with `ascent`) so a line's ink sits centred
 * in the band [band_y, band_y + band_h). Returns 0 when the ink cannot fit in
 * the band or the origin would fall above row 0; callers treat 0 as "do not
 * draw" and the tests pin that the shipped layout never hits it. */
static inline unsigned int boot_menu_text_py(const struct bsod_aa_glyph *glyphs,
                                             unsigned int ascent,
                                             unsigned int band_y, unsigned int band_h)
{
    int top, bottom;
    bsod_aa_ink_rows(glyphs, &top, &bottom);
    int ink_h = bottom - top;
    if (ink_h <= 0 || (unsigned int)ink_h > band_h)
        return 0u;
    int py = (int)band_y + ((int)band_h - ink_h) / 2 - top - (int)ascent;
    return py > 0 ? (unsigned int)py : 0u;
}

#endif /* BOOT_MENU_LAYOUT_H */
