/* ============================================================================
 * gallery.c -- Control Gallery dialog
 *
 * Showcase window displaying all implemented controls for testing:
 *   - Buttons (normal, hover, disabled states)
 *   - Labels (title, body, colored text)
 *   - TextBox (editable text input)
 *   - ScrollBar (vertical and horizontal)
 *
 * Sections are arranged in card-style groups with headers.
 * ============================================================================ */

#include "desktop/gallery.h"
#include "desktop/controls.h"
#include "desktop/wm.h"
#include "font_mgr.h"
#include "gfx.h"
#include "kernel/klog.h"

/* ---- Gallery state ---- */

static int gallery_handle = -1;   /* WM window handle */
static uint8_t gallery_created;   /* 1 if controls have been created */

/* Control IDs */
static int btn_normal;
static int btn_accent;
static int btn_disabled;

static int lbl_body;
static int tb_input;

static int sb_vert;
static int sb_horiz;

static int lbl_status;

/* ---- Card drawing helpers ---- */

#define CARD_BG      0xFF202020   /* same as client bg -- border-only cards */
#define CARD_BORDER  0xFF454545   /* subtle border                */
#define CARD_HEADER  0xFF60CDFF   /* accent blue section header   */
#define CARD_PAD     12
#define GAL_BG       0xFF202020   /* opaque client area background */

/* Draw a section header label using TrueType */
static void draw_section_header(int handle, int32_t x, int32_t y,
                                const char *text)
{
    uint32_t *fb = wm_get_framebuffer(handle);
    uint32_t cw = wm_get_client_width(handle);
    uint32_t ch = wm_get_client_height(handle);
    gfx_surface_t ws;
    ttf_font_t *f;

    if (!fb || !cw || !ch) return;
    f = ttf_get(FONT_UI_BOLD, 14);
    if (!f) return;

    gfx_surface_init(&ws, fb, cw, ch, cw);
    ttf_draw_string(&ws, f, x, y, text, CARD_HEADER);
}

/* Draw a card background (rounded rect with border) */
static void draw_card(int handle, uint32_t x, uint32_t y,
                      uint32_t w, uint32_t h)
{
    uint32_t *fb = wm_get_framebuffer(handle);
    uint32_t cw = wm_get_client_width(handle);
    uint32_t ch = wm_get_client_height(handle);
    gfx_surface_t ws;

    if (!fb || !cw || !ch) return;

    gfx_surface_init(&ws, fb, cw, ch, cw);
    gfx_fill_rounded_rect(&ws, (int32_t)x, (int32_t)y, w, h, 4, CARD_BG);
    /* gfx_draw_rounded_rect is broken (fills entire rect with border color),
     * so use gfx_draw_rect for a clean 1px outline instead. */
    gfx_draw_rect(&ws, (int32_t)x, (int32_t)y, w, h, 1, CARD_BORDER);
}

/* ---- Button callbacks ---- */

static void on_btn_normal(int ctrl_id, int wh)
{
    (void)ctrl_id;
    ctrl_set_text(wh, lbl_status, "Normal button clicked!");
    wm_mark_dirty();
}

static void on_btn_accent(int ctrl_id, int wh)
{
    (void)ctrl_id;
    ctrl_set_text(wh, lbl_status, "Accent button clicked!");
    wm_mark_dirty();
}

/* ---- Gallery lifecycle ---- */

void gallery_open(void)
{
    int h;

    /* If already open, do nothing */
    if (gallery_handle >= 0 && gallery_is_open())
        return;

    /* Degraded controls subsystem (TODO-33 s13, window-pool OOM): every
     * ctrl_create_*() call below would return -1, and this function used
     * to ignore that and unconditionally report success. Refuse up front
     * instead of opening a blank, non-functional dialog under a "Control
     * Gallery opened" log line. */
    if (!ctrl_ready()) {
        klog(LOG_ERROR, "GALLERY", "controls degraded -- gallery not opened");
        return;
    }

    h = wm_create_window("Control Gallery", 50, 40, 420, 360,
                          WM_DIALOG_FLAGS);
    if (h < 0) {
        klog(LOG_ERROR, "GALLERY", "Failed to create window");
        return;
    }

    gallery_handle = h;
    gallery_created = 0;

    /* Clear client area */
    {
        uint32_t cw = wm_get_client_width(h);
        uint32_t ch = wm_get_client_height(h);
        wm_fill_rect(h, 0, 0, cw, ch, GAL_BG);
    }

    /* -------- Create controls -------- */

    /* == Section 1: Buttons (y=8) == */
    btn_normal  = ctrl_create_button(h, CARD_PAD + 4, 32, 90, 28,
                                     "Normal", on_btn_normal);
    btn_accent  = ctrl_create_button(h, CARD_PAD + 100, 32, 90, 28,
                                     "Accent", on_btn_accent);
    btn_disabled = ctrl_create_button(h, CARD_PAD + 196, 32, 90, 28,
                                      "Disabled", (ctrl_click_fn)0);

    /* Disable the third button to show disabled state */
    ctrl_set_enabled(h, btn_disabled, 0);

    /* == Section 2: Text Input (y=76) == */
    lbl_body = ctrl_create_label(h, CARD_PAD + 4, 98, 280, 16,
                                 "Type in the box below:", 0xFFC0C0C0);
    tb_input = ctrl_create_textbox(h, CARD_PAD + 4, 120, 280, 26);

    /* == Section 3: ScrollBars (y=162) == */
    sb_horiz = ctrl_create_scrollbar(h, CARD_PAD + 4, 184, 240, 18,
                                     CTRL_SCROLLBAR_HORIZ, 100, 30);
    sb_vert  = ctrl_create_scrollbar(h, CARD_PAD + 260, 184, 18, 100,
                                     CTRL_SCROLLBAR_VERT, 200, 50);

    /* == Status label at bottom == */
    lbl_status = ctrl_create_label(h, CARD_PAD, 310, 400, 16,
                                   "Click a button or interact with controls",
                                   0xFF9B9B9B);

    gallery_created = 1;
    wm_mark_dirty();

    klog(LOG_INFO, "GALLERY", "Control Gallery opened");
}

void gallery_render(void)
{
    uint32_t cw, ch;

    if (gallery_handle < 0 || !gallery_created)
        return;

    /* Check if window is still alive */
    if (!wm_get_framebuffer(gallery_handle)) {
        gallery_handle = -1;
        gallery_created = 0;
        return;
    }

    cw = wm_get_client_width(gallery_handle);
    ch = wm_get_client_height(gallery_handle);
    (void)ch;

    /* Clear the background */
    wm_fill_rect(gallery_handle, 0, 0, cw, ch, GAL_BG);

    /* Draw section cards */
    draw_card(gallery_handle, 4, 4, cw - 8, 64);       /* Buttons */
    draw_card(gallery_handle, 4, 72, cw - 8, 82);      /* Text */
    draw_card(gallery_handle, 4, 158, cw - 8, 138);    /* ScrollBars */

    /* Draw section headers (TrueType) */
    draw_section_header(gallery_handle, CARD_PAD, 10, "Buttons");
    draw_section_header(gallery_handle, CARD_PAD, 76, "Text Input");
    draw_section_header(gallery_handle, CARD_PAD, 162, "ScrollBars");

    /* Draw a separator line above status */
    wm_fill_rect(gallery_handle, CARD_PAD, 302, cw - CARD_PAD * 2, 1,
                 CARD_BORDER);

    /* Draw all controls */
    ctrl_draw_all(gallery_handle);
}

int gallery_handle_mouse(int32_t cx, int32_t cy, uint8_t buttons)
{
    if (gallery_handle < 0 || !gallery_created)
        return 0;

    return ctrl_handle_mouse(gallery_handle, cx, cy, buttons);
}

int gallery_handle_key(char key)
{
    if (gallery_handle < 0 || !gallery_created)
        return 0;

    return ctrl_handle_key(gallery_handle, key);
}

int gallery_is_open(void)
{
    if (gallery_handle < 0)
        return 0;

    /* Check if window still exists */
    if (!wm_get_framebuffer(gallery_handle)) {
        gallery_handle = -1;
        gallery_created = 0;
        return 0;
    }

    return 1;
}
