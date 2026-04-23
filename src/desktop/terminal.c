/* ============================================================================
 * terminal.c -- Graphical Command Prompt window
 *
 * Kernel-side terminal that renders cmd.exe text output into a WM window and
 * provides a keyboard input ring buffer for sys_read consumption.
 *
 * The text buffer is a grid of (TERM_ROWS × TERM_COLS) characters.  Writing
 * past the last column wraps, writing past the last row scrolls up.  The
 * terminal renders via font_get_glyph() + wm_put_pixel() into the WM
 * window's client-area framebuffer.
 * ============================================================================ */

#include "desktop/terminal.h"
#include "desktop/wm.h"
#include "desktop/font.h"
#include "kernel/klog.h"

/* ---- Internal state ---- */

/* Text buffer */
static char     term_cells[TERM_ROWS][TERM_COLS];
static int      cursor_row;
static int      cursor_col;

/* Input ring buffer (keyboard → sys_read) */
static char     input_ring[TERM_INPUT_BUF];
static int      ring_head;   /* next write position */
static int      ring_tail;   /* next read position */

/* Window handle */
static int      term_handle = -1;
static int      term_dirty;  /* 1 = needs re-render */

/* ---- Colors ---- */
#define TERM_BG   0xFF0C0C0C   /* Win11 Command Prompt black    */
#define TERM_FG   0xFFCCCCCC   /* light gray text */
#define TERM_PROMPT_FG 0xFF60CDFF  /* Win11 accent blue prompt */

/* ---- Padding (inner margin like Windows Command Prompt) ---- */
#define TERM_PAD_X  8   /* left/right padding in pixels */
#define TERM_PAD_Y  4   /* top/bottom padding in pixels */

/* ---- Pixel dimensions ---- */
#define TERM_PX_WIDTH  (TERM_COLS * FONT_WIDTH  + TERM_PAD_X * 2)
#define TERM_PX_HEIGHT (TERM_ROWS * FONT_HEIGHT + TERM_PAD_Y * 2)

/* ---- Helpers ---- */

static void term_memset(void *dst, int val, uint64_t n)
{
    uint8_t *d = (uint8_t *)dst;
    uint64_t i;
    for (i = 0; i < n; i++)
        d[i] = (uint8_t)val;
}

static void term_memcpy(void *dst, const void *src, uint64_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    uint64_t i;
    for (i = 0; i < n; i++)
        d[i] = s[i];
}

/* Scroll the text buffer up by one row */
static void scroll_up(void)
{
    int r;
    for (r = 0; r < TERM_ROWS - 1; r++)
        term_memcpy(term_cells[r], term_cells[r + 1], TERM_COLS);
    term_memset(term_cells[TERM_ROWS - 1], ' ', TERM_COLS);
}

/* ---- Lifecycle ---- */

int terminal_open(void)
{
    int r, c;

    if (term_handle >= 0)
        return 0;  /* already open */

    term_handle = wm_create_window("Command Prompt",
                                   50, 30,
                                   TERM_PX_WIDTH, TERM_PX_HEIGHT,
                                   WM_DEFAULT_FLAGS);
    if (term_handle < 0) {
        klog(LOG_ERROR, "TERM", "Failed to create window");
        return -1;
    }

    /* Clear text buffer */
    for (r = 0; r < TERM_ROWS; r++)
        for (c = 0; c < TERM_COLS; c++)
            term_cells[r][c] = ' ';

    cursor_row = 0;
    cursor_col = 0;
    ring_head = 0;
    ring_tail = 0;
    term_dirty = 1;

    /* Fill window background */
    wm_fill_rect(term_handle, 0, 0, TERM_PX_WIDTH, TERM_PX_HEIGHT, TERM_BG);

    wm_raise_window(term_handle);
    wm_focus_window(term_handle);

    klog(LOG_INFO, "TERM", "Command Prompt window opened (%ux%u chars)",
           (uint64_t)TERM_COLS, (uint64_t)TERM_ROWS);
    return 0;
}

void terminal_close(void)
{
    if (term_handle >= 0) {
        wm_destroy_window(term_handle);
        term_handle = -1;
    }
}

int terminal_is_open(void)
{
    return (term_handle >= 0) ? 1 : 0;
}

int terminal_get_handle(void)
{
    return term_handle;
}

/* ---- Output ---- */

void terminal_putchar(char c)
{
    if (term_handle < 0)
        return;

    switch (c) {
    case '\n':
        cursor_col = 0;
        cursor_row++;
        break;

    case '\r':
        cursor_col = 0;
        break;

    case '\b':
        if (cursor_col > 0) {
            cursor_col--;
            term_cells[cursor_row][cursor_col] = ' ';
        }
        break;

    case '\t':
        /* Tab to next 4-char boundary */
        cursor_col = (cursor_col + 4) & ~3;
        if (cursor_col >= TERM_COLS) {
            cursor_col = 0;
            cursor_row++;
        }
        break;

    default:
        if (c >= 32 && c <= 126) {
            if (cursor_col >= TERM_COLS) {
                cursor_col = 0;
                cursor_row++;
            }
            term_cells[cursor_row][cursor_col] = c;
            cursor_col++;
        }
        break;
    }

    /* Scroll if needed */
    while (cursor_row >= TERM_ROWS) {
        scroll_up();
        cursor_row = TERM_ROWS - 1;
    }

    term_dirty = 1;
}

void terminal_puts(const char *s, int len)
{
    int i;
    for (i = 0; i < len; i++) {
        if (s[i] == '\0')
            break;
        terminal_putchar(s[i]);
    }
}

/* ---- Input ring buffer ---- */

void terminal_key_input(char c)
{
    int next = (ring_head + 1) % TERM_INPUT_BUF;
    if (next == ring_tail)
        return;  /* buffer full, drop */
    input_ring[ring_head] = c;
    ring_head = next;
}

char terminal_trygetchar(void)
{
    char c;
    if (ring_tail == ring_head)
        return 0;  /* empty */
    c = input_ring[ring_tail];
    ring_tail = (ring_tail + 1) % TERM_INPUT_BUF;
    return c;
}

/* ---- Introspection (desktop UI test framework hook) ------------------ */

int terminal_get_buffer(char *dest, int dest_capacity)
{
    int need = TERM_ROWS * TERM_COLS;
    int r;

    if (!dest || dest_capacity < need)
        return -1;

    if (term_handle < 0)
        return 0;   /* terminal not open; honest zero-byte snapshot */

    /* term_cells is row-major; copy row-by-row into the flat dest. The
     * terminal driver lives in desktop-land where the compositor owns
     * mutation, not interrupt context, so a plain copy is safe at test
     * time (tests run in Phase 3 before the desktop compositor thread
     * starts). */
    for (r = 0; r < TERM_ROWS; r++)
        term_memcpy(dest + r * TERM_COLS, term_cells[r], TERM_COLS);

    return need;
}

int terminal_buffer_contains(const char *needle)
{
    int need_total = TERM_ROWS * TERM_COLS;
    int nlen = 0;
    int i, j;

    if (!needle || term_handle < 0)
        return 0;

    while (needle[nlen] != '\0')
        nlen++;

    if (nlen == 0 || nlen > need_total)
        return 0;

    /* Walk the flat grid looking for a contiguous match. Row boundaries
     * are invisible to the search -- a needle that straddles them is
     * still found, matching how the rendered terminal reads to a human
     * (rows wrap, there are no null-terminators between them). */
    for (i = 0; i + nlen <= need_total; i++) {
        int match = 1;
        for (j = 0; j < nlen; j++) {
            int row = (i + j) / TERM_COLS;
            int col = (i + j) % TERM_COLS;
            if (term_cells[row][col] != needle[j]) {
                match = 0;
                break;
            }
        }
        if (match)
            return 1;
    }
    return 0;
}

#ifdef KERNEL_TESTS
/* Test-only harness: initialize internal state without calling
 * wm_create_window. Bypasses the WM dependency so kernel tests (which
 * run in Phase 3 before the compositor starts) can exercise the
 * introspection API's success paths against a real term_cells grid.
 * Do NOT call terminal_render under this mode -- there is no backing
 * WM window. See include/desktop/terminal.h for the full contract. */
void terminal_test_force_open(void)
{
    int r, c;

    /* Sentinel handle: anything >= 0 makes terminal_is_open() return 1. */
    term_handle = 0x7FFFFFFE;

    for (r = 0; r < TERM_ROWS; r++)
        for (c = 0; c < TERM_COLS; c++)
            term_cells[r][c] = ' ';

    cursor_row = 0;
    cursor_col = 0;
    ring_head = 0;
    ring_tail = 0;
    term_dirty = 0;
}

void terminal_test_force_close(void)
{
    term_handle = -1;
}
#endif

/* ---- Rendering ---- */

void terminal_render(void)
{
    int r, c;
    uint32_t px, py;

    if (term_handle < 0 || !term_dirty)
        return;

    term_dirty = 0;

    for (r = 0; r < TERM_ROWS; r++) {
        for (c = 0; c < TERM_COLS; c++) {
            char ch = term_cells[r][c];
            const uint8_t *glyph = font_get_glyph(ch);
            uint32_t x0 = (uint32_t)c * FONT_WIDTH  + TERM_PAD_X;
            uint32_t y0 = (uint32_t)r * FONT_HEIGHT + TERM_PAD_Y;

            for (py = 0; py < FONT_HEIGHT; py++) {
                uint8_t row = glyph[py];
                for (px = 0; px < FONT_WIDTH; px++) {
                    uint32_t clr = (row & (0x80 >> px)) ? TERM_FG : TERM_BG;
                    wm_put_pixel(term_handle, x0 + px, y0 + py, clr);
                }
            }
        }
    }

    /* Draw cursor (inverted block at current position) */
    if (cursor_col < TERM_COLS && cursor_row < TERM_ROWS) {
        uint32_t cx = (uint32_t)cursor_col * FONT_WIDTH  + TERM_PAD_X;
        uint32_t cy = (uint32_t)cursor_row * FONT_HEIGHT + TERM_PAD_Y;
        for (py = 0; py < FONT_HEIGHT; py++) {
            for (px = 0; px < FONT_WIDTH; px++) {
                wm_put_pixel(term_handle, cx + px, cy + py, TERM_FG);
            }
        }
    }

    wm_mark_dirty();
}
