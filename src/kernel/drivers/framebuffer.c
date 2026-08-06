/* ============================================================================
 * framebuffer.c -- Framebuffer graphics driver
 *
 * Renders text and graphics on the GOP framebuffer provided by GRUB via
 * Multiboot2.  Uses an embedded 8x16 bitmap font (basic ASCII, 32-126).
 *
 * Features:
 *   - Double buffering (back buffer allocated via PMM)
 *   - Drawing primitives: fill_rect, draw_rect, draw_line, circles
 *   - Block copy (blit) for compositing
 * ============================================================================ */

#include "kernel/drivers/framebuffer.h"
#include "kernel/boot_info.h"
#include "kernel/mm/heap.h"
#include "kernel/mm/pmm.h"
#include "kernel/mm/vmm.h"
#include "kernel/klog.h"
#include "kernel/printk.h"
#include "gfx_simd.h"

/* --- Embedded 8x16 bitmap font (ASCII 32–126) ---
 * Each character is 8 pixels wide × 16 pixels tall = 16 bytes per glyph.
 * Font data: basic VGA-style bitmap font. */

#define FONT_WIDTH  8
#define FONT_HEIGHT 16
#define FONT_FIRST  32
#define FONT_LAST   126
#define FONT_GLYPHS (FONT_LAST - FONT_FIRST + 1)

/* Minimal 8x16 bitmap font for ASCII 32-126 (VGA-style).
 * Non-static so boot_splash.c can access it for status text rendering. */
const uint8_t kernel_font_data[FONT_GLYPHS][FONT_HEIGHT] = {
    /* 32 ' ' */ {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    /* 33 '!' */ {0x00,0x00,0x18,0x3C,0x3C,0x3C,0x18,0x18,0x18,0x00,0x18,0x18,0x00,0x00,0x00,0x00},
    /* 34 '"' */ {0x00,0x66,0x66,0x66,0x24,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    /* 35 '#' */ {0x00,0x00,0x00,0x6C,0x6C,0xFE,0x6C,0x6C,0x6C,0xFE,0x6C,0x6C,0x00,0x00,0x00,0x00},
    /* 36 '$' */ {0x18,0x18,0x7C,0xC6,0xC2,0xC0,0x7C,0x06,0x06,0x86,0xC6,0x7C,0x18,0x18,0x00,0x00},
    /* 37 '%' */ {0x00,0x00,0x00,0x00,0xC2,0xC6,0x0C,0x18,0x30,0x60,0xC6,0x86,0x00,0x00,0x00,0x00},
    /* 38 '&' */ {0x00,0x00,0x38,0x6C,0x6C,0x38,0x76,0xDC,0xCC,0xCC,0xCC,0x76,0x00,0x00,0x00,0x00},
    /* 39 ''' */ {0x00,0x30,0x30,0x30,0x60,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    /* 40 '(' */ {0x00,0x00,0x0C,0x18,0x30,0x30,0x30,0x30,0x30,0x30,0x18,0x0C,0x00,0x00,0x00,0x00},
    /* 41 ')' */ {0x00,0x00,0x30,0x18,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x18,0x30,0x00,0x00,0x00,0x00},
    /* 42 '*' */ {0x00,0x00,0x00,0x00,0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00,0x00,0x00,0x00,0x00},
    /* 43 '+' */ {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x7E,0x18,0x18,0x00,0x00,0x00,0x00,0x00,0x00},
    /* 44 ',' */ {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x18,0x30,0x00,0x00,0x00},
    /* 45 '-' */ {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFE,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    /* 46 '.' */ {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x00,0x00},
    /* 47 '/' */ {0x00,0x00,0x00,0x00,0x02,0x06,0x0C,0x18,0x30,0x60,0xC0,0x80,0x00,0x00,0x00,0x00},
    /* 48 '0' */ {0x00,0x00,0x3C,0x66,0xC3,0xC3,0xDB,0xDB,0xC3,0xC3,0x66,0x3C,0x00,0x00,0x00,0x00},
    /* 49 '1' */ {0x00,0x00,0x18,0x38,0x78,0x18,0x18,0x18,0x18,0x18,0x18,0x7E,0x00,0x00,0x00,0x00},
    /* 50 '2' */ {0x00,0x00,0x7C,0xC6,0x06,0x0C,0x18,0x30,0x60,0xC0,0xC6,0xFE,0x00,0x00,0x00,0x00},
    /* 51 '3' */ {0x00,0x00,0x7C,0xC6,0x06,0x06,0x3C,0x06,0x06,0x06,0xC6,0x7C,0x00,0x00,0x00,0x00},
    /* 52 '4' */ {0x00,0x00,0x0C,0x1C,0x3C,0x6C,0xCC,0xFE,0x0C,0x0C,0x0C,0x1E,0x00,0x00,0x00,0x00},
    /* 53 '5' */ {0x00,0x00,0xFE,0xC0,0xC0,0xC0,0xFC,0x06,0x06,0x06,0xC6,0x7C,0x00,0x00,0x00,0x00},
    /* 54 '6' */ {0x00,0x00,0x38,0x60,0xC0,0xC0,0xFC,0xC6,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00},
    /* 55 '7' */ {0x00,0x00,0xFE,0xC6,0x06,0x06,0x0C,0x18,0x30,0x30,0x30,0x30,0x00,0x00,0x00,0x00},
    /* 56 '8' */ {0x00,0x00,0x7C,0xC6,0xC6,0xC6,0x7C,0xC6,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00},
    /* 57 '9' */ {0x00,0x00,0x7C,0xC6,0xC6,0xC6,0x7E,0x06,0x06,0x06,0x0C,0x78,0x00,0x00,0x00,0x00},
    /* 58 ':' */ {0x00,0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x00,0x00,0x00},
    /* 59 ';' */ {0x00,0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x00,0x18,0x18,0x30,0x00,0x00,0x00,0x00},
    /* 60 '<' */ {0x00,0x00,0x00,0x06,0x0C,0x18,0x30,0x60,0x30,0x18,0x0C,0x06,0x00,0x00,0x00,0x00},
    /* 61 '=' */ {0x00,0x00,0x00,0x00,0x00,0x7E,0x00,0x00,0x7E,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    /* 62 '>' */ {0x00,0x00,0x00,0x60,0x30,0x18,0x0C,0x06,0x0C,0x18,0x30,0x60,0x00,0x00,0x00,0x00},
    /* 63 '?' */ {0x00,0x00,0x7C,0xC6,0xC6,0x0C,0x18,0x18,0x18,0x00,0x18,0x18,0x00,0x00,0x00,0x00},
    /* 64 '@' */ {0x00,0x00,0x00,0x7C,0xC6,0xC6,0xDE,0xDE,0xDE,0xDC,0xC0,0x7C,0x00,0x00,0x00,0x00},
    /* 65 'A' */ {0x00,0x00,0x10,0x38,0x6C,0xC6,0xC6,0xFE,0xC6,0xC6,0xC6,0xC6,0x00,0x00,0x00,0x00},
    /* 66 'B' */ {0x00,0x00,0xFC,0x66,0x66,0x66,0x7C,0x66,0x66,0x66,0x66,0xFC,0x00,0x00,0x00,0x00},
    /* 67 'C' */ {0x00,0x00,0x3C,0x66,0xC2,0xC0,0xC0,0xC0,0xC0,0xC2,0x66,0x3C,0x00,0x00,0x00,0x00},
    /* 68 'D' */ {0x00,0x00,0xF8,0x6C,0x66,0x66,0x66,0x66,0x66,0x66,0x6C,0xF8,0x00,0x00,0x00,0x00},
    /* 69 'E' */ {0x00,0x00,0xFE,0x66,0x62,0x68,0x78,0x68,0x60,0x62,0x66,0xFE,0x00,0x00,0x00,0x00},
    /* 70 'F' */ {0x00,0x00,0xFE,0x66,0x62,0x68,0x78,0x68,0x60,0x60,0x60,0xF0,0x00,0x00,0x00,0x00},
    /* 71 'G' */ {0x00,0x00,0x3C,0x66,0xC2,0xC0,0xC0,0xDE,0xC6,0xC6,0x66,0x3A,0x00,0x00,0x00,0x00},
    /* 72 'H' */ {0x00,0x00,0xC6,0xC6,0xC6,0xC6,0xFE,0xC6,0xC6,0xC6,0xC6,0xC6,0x00,0x00,0x00,0x00},
    /* 73 'I' */ {0x00,0x00,0x3C,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00,0x00},
    /* 74 'J' */ {0x00,0x00,0x1E,0x0C,0x0C,0x0C,0x0C,0x0C,0xCC,0xCC,0xCC,0x78,0x00,0x00,0x00,0x00},
    /* 75 'K' */ {0x00,0x00,0xE6,0x66,0x66,0x6C,0x78,0x78,0x6C,0x66,0x66,0xE6,0x00,0x00,0x00,0x00},
    /* 76 'L' */ {0x00,0x00,0xF0,0x60,0x60,0x60,0x60,0x60,0x60,0x62,0x66,0xFE,0x00,0x00,0x00,0x00},
    /* 77 'M' */ {0x00,0x00,0xC6,0xEE,0xFE,0xFE,0xD6,0xC6,0xC6,0xC6,0xC6,0xC6,0x00,0x00,0x00,0x00},
    /* 78 'N' */ {0x00,0x00,0xC6,0xE6,0xF6,0xFE,0xDE,0xCE,0xC6,0xC6,0xC6,0xC6,0x00,0x00,0x00,0x00},
    /* 79 'O' */ {0x00,0x00,0x7C,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00},
    /* 80 'P' */ {0x00,0x00,0xFC,0x66,0x66,0x66,0x7C,0x60,0x60,0x60,0x60,0xF0,0x00,0x00,0x00,0x00},
    /* 81 'Q' */ {0x00,0x00,0x7C,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xD6,0xDE,0x7C,0x0C,0x0E,0x00,0x00},
    /* 82 'R' */ {0x00,0x00,0xFC,0x66,0x66,0x66,0x7C,0x6C,0x66,0x66,0x66,0xE6,0x00,0x00,0x00,0x00},
    /* 83 'S' */ {0x00,0x00,0x7C,0xC6,0xC6,0x60,0x38,0x0C,0x06,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00},
    /* 84 'T' */ {0x00,0x00,0xFF,0xDB,0x99,0x18,0x18,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00,0x00},
    /* 85 'U' */ {0x00,0x00,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00},
    /* 86 'V' */ {0x00,0x00,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x6C,0x38,0x10,0x00,0x00,0x00,0x00},
    /* 87 'W' */ {0x00,0x00,0xC6,0xC6,0xC6,0xC6,0xD6,0xD6,0xD6,0xFE,0xEE,0x6C,0x00,0x00,0x00,0x00},
    /* 88 'X' */ {0x00,0x00,0xC6,0xC6,0x6C,0x7C,0x38,0x38,0x7C,0x6C,0xC6,0xC6,0x00,0x00,0x00,0x00},
    /* 89 'Y' */ {0x00,0x00,0xCC,0xCC,0xCC,0xCC,0x78,0x30,0x30,0x30,0x30,0x78,0x00,0x00,0x00,0x00},
    /* 90 'Z' */ {0x00,0x00,0xFE,0xC6,0x86,0x0C,0x18,0x30,0x60,0xC2,0xC6,0xFE,0x00,0x00,0x00,0x00},
    /* 91 '[' */ {0x00,0x00,0x3C,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x3C,0x00,0x00,0x00,0x00},
    /* 92 '\' */ {0x00,0x00,0x00,0x80,0xC0,0xE0,0x70,0x38,0x1C,0x0E,0x06,0x02,0x00,0x00,0x00,0x00},
    /* 93 ']' */ {0x00,0x00,0x3C,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x3C,0x00,0x00,0x00,0x00},
    /* 94 '^' */ {0x10,0x38,0x6C,0xC6,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    /* 95 '_' */ {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF,0x00,0x00},
    /* 96 '`' */ {0x00,0x30,0x18,0x0C,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    /* 97 'a' */ {0x00,0x00,0x00,0x00,0x00,0x78,0x0C,0x7C,0xCC,0xCC,0xCC,0x76,0x00,0x00,0x00,0x00},
    /* 98 'b' */ {0x00,0x00,0xE0,0x60,0x60,0x78,0x6C,0x66,0x66,0x66,0x66,0x7C,0x00,0x00,0x00,0x00},
    /* 99 'c' */ {0x00,0x00,0x00,0x00,0x00,0x7C,0xC6,0xC0,0xC0,0xC0,0xC6,0x7C,0x00,0x00,0x00,0x00},
    /*100 'd' */ {0x00,0x00,0x1C,0x0C,0x0C,0x3C,0x6C,0xCC,0xCC,0xCC,0xCC,0x76,0x00,0x00,0x00,0x00},
    /*101 'e' */ {0x00,0x00,0x00,0x00,0x00,0x7C,0xC6,0xFE,0xC0,0xC0,0xC6,0x7C,0x00,0x00,0x00,0x00},
    /*102 'f' */ {0x00,0x00,0x1C,0x36,0x32,0x30,0x78,0x30,0x30,0x30,0x30,0x78,0x00,0x00,0x00,0x00},
    /*103 'g' */ {0x00,0x00,0x00,0x00,0x00,0x76,0xCC,0xCC,0xCC,0xCC,0xCC,0x7C,0x0C,0xCC,0x78,0x00},
    /*104 'h' */ {0x00,0x00,0xE0,0x60,0x60,0x6C,0x76,0x66,0x66,0x66,0x66,0xE6,0x00,0x00,0x00,0x00},
    /*105 'i' */ {0x00,0x00,0x18,0x18,0x00,0x38,0x18,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00,0x00},
    /*106 'j' */ {0x00,0x00,0x06,0x06,0x00,0x0E,0x06,0x06,0x06,0x06,0x06,0x06,0x66,0x66,0x3C,0x00},
    /*107 'k' */ {0x00,0x00,0xE0,0x60,0x60,0x66,0x6C,0x78,0x78,0x6C,0x66,0xE6,0x00,0x00,0x00,0x00},
    /*108 'l' */ {0x00,0x00,0x38,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00,0x00},
    /*109 'm' */ {0x00,0x00,0x00,0x00,0x00,0xEC,0xFE,0xD6,0xD6,0xD6,0xD6,0xC6,0x00,0x00,0x00,0x00},
    /*110 'n' */ {0x00,0x00,0x00,0x00,0x00,0xDC,0x66,0x66,0x66,0x66,0x66,0x66,0x00,0x00,0x00,0x00},
    /*111 'o' */ {0x00,0x00,0x00,0x00,0x00,0x7C,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00},
    /*112 'p' */ {0x00,0x00,0x00,0x00,0x00,0xDC,0x66,0x66,0x66,0x66,0x66,0x7C,0x60,0x60,0xF0,0x00},
    /*113 'q' */ {0x00,0x00,0x00,0x00,0x00,0x76,0xCC,0xCC,0xCC,0xCC,0xCC,0x7C,0x0C,0x0C,0x1E,0x00},
    /*114 'r' */ {0x00,0x00,0x00,0x00,0x00,0xDC,0x76,0x66,0x60,0x60,0x60,0xF0,0x00,0x00,0x00,0x00},
    /*115 's' */ {0x00,0x00,0x00,0x00,0x00,0x7C,0xC6,0x60,0x38,0x0C,0xC6,0x7C,0x00,0x00,0x00,0x00},
    /*116 't' */ {0x00,0x00,0x10,0x30,0x30,0xFC,0x30,0x30,0x30,0x30,0x36,0x1C,0x00,0x00,0x00,0x00},
    /*117 'u' */ {0x00,0x00,0x00,0x00,0x00,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0x76,0x00,0x00,0x00,0x00},
    /*118 'v' */ {0x00,0x00,0x00,0x00,0x00,0xCC,0xCC,0xCC,0xCC,0xCC,0x78,0x30,0x00,0x00,0x00,0x00},
    /*119 'w' */ {0x00,0x00,0x00,0x00,0x00,0xC6,0xC6,0xD6,0xD6,0xD6,0xFE,0x6C,0x00,0x00,0x00,0x00},
    /*120 'x' */ {0x00,0x00,0x00,0x00,0x00,0xC6,0x6C,0x38,0x38,0x38,0x6C,0xC6,0x00,0x00,0x00,0x00},
    /*121 'y' */ {0x00,0x00,0x00,0x00,0x00,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x7E,0x06,0x0C,0xF8,0x00},
    /*122 'z' */ {0x00,0x00,0x00,0x00,0x00,0xFE,0xCC,0x18,0x30,0x60,0xC6,0xFE,0x00,0x00,0x00,0x00},
    /*123 '{' */ {0x00,0x00,0x0E,0x18,0x18,0x18,0x70,0x18,0x18,0x18,0x18,0x0E,0x00,0x00,0x00,0x00},
    /*124 '|' */ {0x00,0x00,0x18,0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x18,0x18,0x00,0x00,0x00,0x00},
    /*125 '}' */ {0x00,0x00,0x70,0x18,0x18,0x18,0x0E,0x18,0x18,0x18,0x18,0x70,0x00,0x00,0x00,0x00},
    /*126 '~' */ {0x00,0x00,0x76,0xDC,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
};

/* ---- Console / framebuffer state ---- */

static uint32_t *hw_addr;       /* hardware framebuffer base (write-only) */
static uint32_t *back_buf;      /* back buffer (all drawing goes here) */
static uint32_t fb_pitch;       /* bytes per scanline (hardware) */
static uint32_t fb_width;       /* pixels */
static uint32_t fb_height;      /* pixels */
static uint32_t fb_bpp;         /* bits per pixel */
static uint32_t fb_stride;      /* pixels per scanline in back buffer */

static uint32_t cursor_x;       /* text column (in characters) */
static uint32_t cursor_y;       /* text row (in characters) */
static uint32_t max_cols;       /* max text columns */
static uint32_t max_rows;       /* max text rows */

static uint32_t fg_color;       /* foreground color */
static uint32_t bg_color;       /* background color */

static uint8_t compositor_locked; /* when set, fb_putchar/draw_char are no-ops */

static uint8_t  fb_ready;       /* 1 if framebuffer is initialized */

/* VBE page flip state -- two pages in VRAM for tear-free rendering */
static uint8_t  page_flip_ok;   /* 1 if VBE page flipping is available */
static uint8_t  page_current;   /* which page (0 or 1) is currently displayed */

/* ---- Helper: absolute value ---- */

static inline int32_t iabs(int32_t v) { return v < 0 ? -v : v; }

/* ---- Fast memory helpers (no libc) ---- */

static void mem_set32(uint32_t *dst, uint32_t val, uint32_t count)
{
    if (simd_avx512_ok) {
        fb_fill_avx512(dst, val, count);
        return;
    }
    if (simd_avx2_ok) {
        fb_fill_avx(dst, val, count);
        return;
    }
    {
        uint32_t i;
        for (i = 0; i < count; i++)
            dst[i] = val;
    }
}

static void mem_cpy32(uint32_t *dst, const uint32_t *src, uint32_t count)
{
    if (simd_avx512_ok) {
        fb_blit_avx512(dst, src, count);
        return;
    }
    if (simd_avx2_ok) {
        fb_blit_avx(dst, src, count);
        return;
    }
    {
        uint32_t i;
        for (i = 0; i < count; i++)
            dst[i] = src[i];
    }
}

/* ============================================================================
 * Lifecycle
 * ============================================================================ */

void fb_init(void)
{
    if (!g_boot_info.fb_available) {
        fb_ready = 0;
        return;
    }

    fb_pitch  = g_boot_info.fb.pitch;
    fb_width  = g_boot_info.fb.width;
    fb_height = g_boot_info.fb.height;
    fb_bpp    = g_boot_info.fb.bpp;
    fb_stride = fb_width;   /* back buffer is tightly packed */

    /* --- Guardrail: pixel format must be RGBX or BGRX (32-bit) ---
     * The rendering code assumes 4 bytes/pixel throughout. BitMask and other
     * exotic formats are rejected by the bootloader, but verify here too. */
    if (g_boot_info.fb.pixel_format != GOP_PIXEL_BGRX &&
        g_boot_info.fb.pixel_format != GOP_PIXEL_RGBX) {
        klog(LOG_FATAL, "gfx", "Unsupported pixel format %u (expected RGBX=0 or BGRX=1)",
             (uint64_t)g_boot_info.fb.pixel_format);
        fb_ready = 0;
        return;
    }
    if (fb_bpp != 32) {
        klog(LOG_FATAL, "gfx", "Unsupported bpp %u (expected 32)", (uint64_t)fb_bpp);
        fb_ready = 0;
        return;
    }

    /* --- Guardrail: pitch must be aligned to pixel size and >= width ---
     * PixelsPerScanLine may include padding beyond visible width. If pitch
     * is not a multiple of bpp/8, stride math produces wrong values. */
    if ((fb_pitch % (fb_bpp / 8)) != 0) {
        klog(LOG_FATAL, "gfx", "Pitch %u not aligned to %u-byte pixels",
             (uint64_t)fb_pitch, (uint64_t)(fb_bpp / 8));
        fb_ready = 0;
        return;
    }
    if (fb_pitch < fb_width * (fb_bpp / 8)) {
        klog(LOG_FATAL, "gfx", "Pitch %u too small for %u pixels @ %u bpp",
             (uint64_t)fb_pitch, (uint64_t)fb_width, (uint64_t)fb_bpp);
        fb_ready = 0;
        return;
    }

    /* Remap framebuffer VRAM as Write-Combining for fast sequential writes.
     * Map 2x height for VBE page-flip second page (Bochs VGA). */
    {
        uint64_t fb_phys = g_boot_info.fb.addr;
        uint32_t fb_size = fb_pitch * fb_height * 2;
        /* Align physical base down to page boundary (should already be aligned) */
        uint64_t fb_phys_aligned = fb_phys & ~0xFFFULL;
        uint32_t fb_offset = (uint32_t)(fb_phys - fb_phys_aligned);
        void *wc = vmm_map_mmio_wc(fb_phys_aligned, fb_size + fb_offset);
        if (wc) {
            hw_addr = (uint32_t *)((uintptr_t)wc + fb_offset);
            klog(LOG_INFO, "gfx", "Framebuffer WC-mapped: phys %p -> VA %p (%u KiB)",
                 fb_phys, (uint64_t)(uintptr_t)hw_addr, (uint64_t)(fb_size / 1024));
        } else {
            /* Guardrail: WC mapping is required for bare-metal performance.
             * Falling back to identity map silently would hide a VMM bug. */
            klog(LOG_ERROR, "gfx", "Framebuffer WC remap failed -- falling back to identity map");
            hw_addr = (uint32_t *)(uintptr_t)fb_phys;
        }
    }

    max_cols  = fb_width / FONT_WIDTH;
    max_rows  = fb_height / FONT_HEIGHT;

    cursor_x  = 0;
    cursor_y  = 0;

    fg_color  = FB_COLOR_FG_DEFAULT;
    bg_color  = FB_COLOR_BG_DEFAULT;

    /* Allocate the back buffer via PMM (contiguous physical frames).
     * The kernel heap is only 2 MiB -- far too small for a 1280×720×4 = 3.6 MiB
     * back buffer.  PMM has 2041 MiB available and supports large contiguous
     * allocations.  Identity mapping means phys addr == virt addr. */
    {
        uint32_t bb_bytes = fb_width * fb_height * sizeof(uint32_t);
        uint32_t bb_pages = (bb_bytes + 4095) / 4096;
        uintptr_t bb_base = pmm_alloc_contiguous(bb_pages);

        if (bb_base) {
            back_buf = (uint32_t *)bb_base;
            klog(LOG_INFO, "gfx", "Framebuffer back buffer: %u KiB (%u pages) at %p",
                   (uint64_t)(bb_bytes / 1024), (uint64_t)bb_pages, bb_base);
        } else {
            /* Fallback: draw directly to HW framebuffer (no double buffering).
             * This WILL flicker -- the host display reads VRAM while we draw. */
            printk("[WARN] Back buffer alloc failed (%u KiB) -- no double buffering!\n",
                   (uint64_t)(bb_bytes / 1024));
            back_buf = hw_addr;
            fb_stride = fb_pitch / (fb_bpp / 8);
        }
    }

    fb_ready = 1;

    /* Try to enable VBE page flipping for tear-free rendering.
     * Bochs VGA supports VBE_DISPI_INDEX_Y_OFFSET for instant display
     * offset switching.  We set virtual height = 2× physical height,
     * creating two pages.  We copy back_buf to the INVISIBLE page,
     * then flip to show it.  This eliminates tearing because the
     * host display thread never reads a partially-written page. */
    if (back_buf != hw_addr) {
        uint16_t vbe_id;

        /* Probe: read VBE DISPI ID */
        __asm__ volatile ("outw %0, %1" : : "a"((uint16_t)0x00),
                          "Nd"((uint16_t)0x01CE));
        __asm__ volatile ("inw %1, %0" : "=a"(vbe_id) :
                          "Nd"((uint16_t)0x01CF));

        if ((vbe_id & 0xFFF0) == 0xB0C0) {
            /* Valid Bochs VGA -- set virtual height to 2× for page flipping */
            __asm__ volatile ("outw %0, %1" : : "a"((uint16_t)0x07),
                              "Nd"((uint16_t)0x01CE));
            __asm__ volatile ("outw %0, %1" : : "a"((uint16_t)(fb_height * 2)),
                              "Nd"((uint16_t)0x01CF));

            page_flip_ok = 1;
            page_current = 0;
            klog(LOG_INFO, "gfx", "VBE page flip enabled (Bochs VGA 0x%x, 2x%u virt height)",
                   (uint64_t)vbe_id, (uint64_t)fb_height);

            /* Zero BOTH VRAM pages immediately after setting virtual height.
             *
             * WHY: when QEMU processes the VBE virtual_height register write, it
             * resizes its internal display scan buffer.  The second VRAM page
             * (rows fb_height..fb_height*2-1) is uninitialized.  On the very
             * next QEMU display-refresh tick (~16ms), the Bochs VGA emulator
             * may scan or display part of that raw memory, producing the brief
             * multi-colour "static noise" flash visible in QEMU just before the
             * boot splash.
             *
             * Zeroing the second page here (directly through hw_addr) happens
             * within the same KVM exit cycle as the VBE write, so QEMU sees
             * clean VRAM on the first refresh after the resize. Using rep stosq
             * for speed (~0.5ms for 3.6 MiB at memory bandwidth). */
            {
                uint32_t hw_stride_px = fb_pitch / (fb_bpp / 8);
                uint64_t *p1 = (uint64_t *)(hw_addr + (uint64_t)fb_height * hw_stride_px);
                uint64_t qwords = ((uint64_t)fb_height * hw_stride_px * sizeof(uint32_t)) / 8;
                __asm__ volatile (
                    "xorq %%rax, %%rax\n\t"
                    "rep stosq"
                    : "+D"(p1), "+c"(qwords)
                    :
                    : "rax", "memory"
                );
            }
        }
    }

    fb_clear();
}

/* ============================================================================
 * Double buffering -- VBE page flip + direct copy fallback
 *
 * All drawing goes to back_buf (in PMM memory, not VRAM).  fb_swap() copies
 * the finished frame to VRAM.  With VBE page flipping enabled:
 *   1. Copy back_buf → INVISIBLE VRAM page (host display can't see this)
 *   2. Atomic flip via VBE_DISPI_INDEX_Y_OFFSET (single outw instruction)
 *   3. Host display reads the fully-written page on next refresh = no tearing
 *
 * Fallback (no Bochs VGA): direct rep movsq copy to displayed VRAM.
 * Minor tearing possible if host refresh catches the copy mid-row.
 * ============================================================================ */

void fb_swap(void)
{
    uint64_t *src;
    uint64_t *dst;
    uint64_t count;
    uint32_t hw_stride;
    uint32_t target_y;

    if (!fb_ready)
        return;

    if (back_buf == hw_addr)
        return;

    hw_stride = fb_pitch / (fb_bpp / 8);

    __asm__ volatile ("sfence" ::: "memory");

    /* Save/restore interrupt state instead of cli/sti -- fb_swap may be
     * called from ISR context (indirectly via alive_blink_tick).
     * Bare cli/sti re-enables interrupts inside the ISR, causing
     * recursive timer interrupts and stack overflow on bare metal. */
    {
        uint64_t rflags;
        __asm__ volatile ("pushfq; pop %0; cli" : "=r"(rflags) ::"memory");

        if (page_flip_ok) {
            /* Copy back_buf to the INVISIBLE VRAM page, then flip */
            target_y = (page_current == 0) ? fb_height : 0;
            dst = (uint64_t *)(hw_addr + target_y * hw_stride);
            src = (uint64_t *)back_buf;

            if (fb_stride == hw_stride) {
                count = (uint64_t)(fb_stride * fb_height) / 2;
                __asm__ volatile (
                    "rep movsq"
                    : "+S"(src), "+D"(dst), "+c"(count) : : "memory"
                );
            } else {
                uint32_t y;
                for (y = 0; y < fb_height; y++) {
                    src = (uint64_t *)(back_buf + y * fb_stride);
                    dst = (uint64_t *)(hw_addr + (target_y + y) * hw_stride);
                    count = (uint64_t)fb_width / 2;
                    __asm__ volatile (
                        "rep movsq"
                        : "+S"(src), "+D"(dst), "+c"(count) : : "memory"
                    );
                }
            }

            __asm__ volatile ("push %0; popfq" ::"r"(rflags) : "memory");

            /* Atomic flip: switch display to the page we just wrote */
            __asm__ volatile ("outw %0, %1" : : "a"((uint16_t)0x09),
                              "Nd"((uint16_t)0x01CE));
            __asm__ volatile ("outw %0, %1" : : "a"((uint16_t)target_y),
                              "Nd"((uint16_t)0x01CF));

            page_current ^= 1;
            return;
        }

        /* Fallback: direct copy to displayed buffer */
        if (fb_stride == hw_stride) {
            src = (uint64_t *)back_buf;
            dst = (uint64_t *)hw_addr;
            count = (uint64_t)(fb_stride * fb_height) / 2;
            __asm__ volatile (
                "rep movsq"
                : "+S"(src), "+D"(dst), "+c"(count) : : "memory"
            );
        } else {
            uint32_t y;
            for (y = 0; y < fb_height; y++) {
                src = (uint64_t *)(back_buf + y * fb_stride);
                dst = (uint64_t *)(hw_addr  + y * hw_stride);
                count = (uint64_t)fb_width / 2;
                __asm__ volatile (
                    "rep movsq"
                    : "+S"(src), "+D"(dst), "+c"(count) : : "memory"
                );
            }
        }

        __asm__ volatile ("push %0; popfq" ::"r"(rflags) : "memory");
    }
}

void fb_swap_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    uint32_t hw_stride;
    uint32_t row;
    uint32_t x1, y1;
    uint32_t vram_y_offset;

    if (!fb_ready || back_buf == hw_addr)
        return;

    hw_stride = fb_pitch / (fb_bpp / 8);

    /* Clamp to screen bounds */
    if (x >= fb_width || y >= fb_height)
        return;
    x1 = (x + w > fb_width)  ? fb_width  : x + w;
    y1 = (y + h > fb_height) ? fb_height : y + h;
    w = x1 - x;
    h = y1 - y;

    if (w == 0 || h == 0)
        return;

    /* When VBE page flipping is active, fb_swap() alternates between
     * page 0 (y_offset=0) and page 1 (y_offset=fb_height).  We must
     * write to the CURRENTLY DISPLAYED page so the update is visible. */
    vram_y_offset = 0;
    if (page_flip_ok)
        vram_y_offset = (page_current == 0) ? 0 : fb_height;

    __asm__ volatile ("sfence" ::: "memory");

    /* Save interrupt state -- fb_swap_rect may be called from ISR context
     * (e.g., alive_blink_tick in timer handler).  Using cli/sti would
     * re-enable interrupts inside the ISR, causing recursive timer
     * interrupts and stack overflow on bare metal. */
    {
        uint64_t rflags;
        __asm__ volatile ("pushfq; pop %0; cli" : "=r"(rflags) ::"memory");

        /* Copy each row of the dirty rectangle using 64-bit moves */
        for (row = y; row < y1; row++) {
            uint64_t *src = (uint64_t *)(back_buf + row * fb_stride + x);
            uint64_t *dst = (uint64_t *)(hw_addr  + (vram_y_offset + row) * hw_stride + x);
            uint64_t count = (uint64_t)w / 2;  /* DWORD pairs → QWORDs */
            if (count > 0) {
                __asm__ volatile (
                    "rep movsq"
                    : "+S"(src), "+D"(dst), "+c"(count)
                    :
                    : "memory"
                );
            }
            /* Handle odd trailing pixel */
            if (w & 1) {
                hw_addr[(vram_y_offset + row) * hw_stride + x + w - 1] =
                    back_buf[row * fb_stride + x + w - 1];
            }
        }

        __asm__ volatile ("push %0; popfq" ::"r"(rflags) : "memory");
    }
}

void fb_blit(uint32_t dst_x, uint32_t dst_y,
             const uint32_t *src, uint32_t w, uint32_t h, uint32_t src_pitch)
{
    uint32_t row;
    uint32_t copy_w, copy_h;

    if (!fb_ready || !src)
        return;

    /* Reject out-of-range origin before unsigned subtraction */
    if (dst_x >= fb_width || dst_y >= fb_height)
        return;

    /* Overflow-safe clamp: compute remaining space without addition */
    {
        uint32_t max_w = fb_width  - dst_x;
        uint32_t max_h = fb_height - dst_y;
        copy_w = (w > max_w) ? max_w : w;
        copy_h = (h > max_h) ? max_h : h;
    }

    for (row = 0; row < copy_h; row++) {
        mem_cpy32(back_buf + (dst_y + row) * fb_stride + dst_x,
                  src + row * src_pitch,
                  copy_w);
    }
}

/* ============================================================================
 * Pixel / drawing primitives
 * ============================================================================ */

void fb_put_pixel(uint32_t x, uint32_t y, uint32_t color)
{
    if (x >= fb_width || y >= fb_height)
        return;

    back_buf[y * fb_stride + x] = color;
}

uint32_t fb_read_pixel(uint32_t x, uint32_t y)
{
    if (x >= fb_width || y >= fb_height)
        return 0;

    return back_buf[y * fb_stride + x];
}

void fb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                  uint32_t color)
{
    uint32_t row;
    uint32_t x1, y1;

    if (!fb_ready)
        return;

    /* Clamp */
    x1 = (x + w > fb_width)  ? fb_width  : x + w;
    y1 = (y + h > fb_height) ? fb_height : y + h;

    for (row = y; row < y1; row++) {
        mem_set32(back_buf + row * fb_stride + x, color, x1 - x);
    }
}

void fb_draw_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                  uint32_t color)
{
    if (!fb_ready || w == 0 || h == 0)
        return;

    /* Top edge */
    fb_fill_rect(x, y, w, 1, color);
    /* Bottom edge */
    fb_fill_rect(x, y + h - 1, w, 1, color);
    /* Left edge */
    fb_fill_rect(x, y, 1, h, color);
    /* Right edge */
    fb_fill_rect(x + w - 1, y, 1, h, color);
}

void fb_draw_line(int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                  uint32_t color)
{
    int32_t dx, dy, sx, sy, err, e2;

    if (!fb_ready)
        return;

    dx = iabs(x1 - x0);
    dy = -iabs(y1 - y0);
    sx = x0 < x1 ? 1 : -1;
    sy = y0 < y1 ? 1 : -1;
    err = dx + dy;

    for (;;) {
        if (x0 >= 0 && (uint32_t)x0 < fb_width &&
            y0 >= 0 && (uint32_t)y0 < fb_height) {
            back_buf[(uint32_t)y0 * fb_stride + (uint32_t)x0] = color;
        }

        if (x0 == x1 && y0 == y1)
            break;

        e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

/* Midpoint circle -- plots 8 symmetric points per step */
void fb_draw_circle(int32_t cx, int32_t cy, int32_t r, uint32_t color)
{
    int32_t x, y, d;

    if (!fb_ready || r <= 0)
        return;

    x = 0;
    y = r;
    d = 1 - r;

    while (x <= y) {
        /* 8-way symmetry */
        fb_put_pixel((uint32_t)(cx + x), (uint32_t)(cy + y), color);
        fb_put_pixel((uint32_t)(cx - x), (uint32_t)(cy + y), color);
        fb_put_pixel((uint32_t)(cx + x), (uint32_t)(cy - y), color);
        fb_put_pixel((uint32_t)(cx - x), (uint32_t)(cy - y), color);
        fb_put_pixel((uint32_t)(cx + y), (uint32_t)(cy + x), color);
        fb_put_pixel((uint32_t)(cx - y), (uint32_t)(cy + x), color);
        fb_put_pixel((uint32_t)(cx + y), (uint32_t)(cy - x), color);
        fb_put_pixel((uint32_t)(cx - y), (uint32_t)(cy - x), color);

        if (d < 0) {
            d += 2 * x + 3;
        } else {
            d += 2 * (x - y) + 5;
            y--;
        }
        x++;
    }
}

/* Filled circle -- horizontal spans via midpoint algorithm */
void fb_fill_circle(int32_t cx, int32_t cy, int32_t r, uint32_t color)
{
    int32_t x, y, d;

    if (!fb_ready || r <= 0)
        return;

    x = 0;
    y = r;
    d = 1 - r;

    while (x <= y) {
        /* Draw horizontal spans for each pair of symmetric points */
        fb_draw_line(cx - x, cy + y, cx + x, cy + y, color);
        fb_draw_line(cx - x, cy - y, cx + x, cy - y, color);
        fb_draw_line(cx - y, cy + x, cx + y, cy + x, color);
        fb_draw_line(cx - y, cy - x, cx + y, cy - x, color);

        if (d < 0) {
            d += 2 * x + 3;
        } else {
            d += 2 * (x - y) + 5;
            y--;
        }
        x++;
    }
}

/* ============================================================================
 * Text rendering (writes to back buffer, auto-swaps on newline)
 * ============================================================================ */

static void fb_draw_char(uint32_t cx, uint32_t cy, char c)
{
    uint32_t glyph_index;
    uint32_t px, py;
    const uint8_t *glyph;

    if (compositor_locked)
        return;

    if (c < FONT_FIRST || c > FONT_LAST)
        glyph_index = 0; /* space for unprintable */
    else
        glyph_index = (uint32_t)(c - FONT_FIRST);

    glyph = kernel_font_data[glyph_index];

    for (py = 0; py < FONT_HEIGHT; py++) {
        uint8_t row = glyph[py];
        uint32_t screen_y = cy * FONT_HEIGHT + py;
        uint32_t base_x   = cx * FONT_WIDTH;

        if (screen_y >= fb_height)
            break;

        for (px = 0; px < FONT_WIDTH; px++) {
            uint32_t screen_x = base_x + px;
            if (screen_x >= fb_width)
                break;
            uint32_t clr = (row & (0x80 >> px)) ? fg_color : bg_color;
            back_buf[screen_y * fb_stride + screen_x] = clr;
        }
    }
}

void fb_clear(void)
{
    if (!fb_ready)
        return;

    mem_set32(back_buf, bg_color, fb_stride * fb_height);

    cursor_x = 0;
    cursor_y = 0;
}

void fb_scroll(void)
{
    uint32_t row_pixels;
    uint32_t text_area;

    if (!fb_ready)
        return;

    row_pixels = fb_stride * FONT_HEIGHT;
    text_area  = fb_stride * (max_rows - 1) * FONT_HEIGHT;

    /* Shift all rows up by one text line */
    mem_cpy32(back_buf, back_buf + row_pixels, text_area);
    /* Clear the last row */
    mem_set32(back_buf + text_area, bg_color, row_pixels);
}

void fb_putchar(char c)
{
    if (!fb_ready || compositor_locked)
        return;

    switch (c) {
    case '\n':
        cursor_x = 0;
        cursor_y++;
        /* During boot, flush back buffer to VRAM so text is visible.
         * One bulk rep-movsq per line is fast; per-pixel MMIO is not. */
        if (!compositor_locked && back_buf != hw_addr)
            fb_swap();
        break;
    case '\r':
        cursor_x = 0;
        break;
    case '\t':
        cursor_x = (cursor_x + 4) & ~3u;
        break;
    case '\b':
        if (cursor_x > 0) {
            cursor_x--;
            fb_draw_char(cursor_x, cursor_y, ' ');
        }
        break;
    default:
        fb_draw_char(cursor_x, cursor_y, c);
        cursor_x++;
        break;
    }

    /* Line wrap */
    if (cursor_x >= max_cols) {
        cursor_x = 0;
        cursor_y++;
    }

    /* Scroll if needed */
    if (cursor_y >= max_rows) {
        fb_scroll();
        cursor_y = max_rows - 1;
    }
}

void fb_write(const char *str)
{
    if (!fb_ready)
        return;

    while (*str) {
        fb_putchar(*str++);
    }
}

void fb_set_color(uint32_t fg, uint32_t bg)
{
    fg_color = fg;
    bg_color = bg;
}

/* ============================================================================
 * Queries
 * ============================================================================ */

uint32_t fb_get_width(void)  { return fb_width; }
uint32_t fb_get_height(void) { return fb_height; }
uint32_t *fb_get_backbuffer(void) { return back_buf; }
uint32_t fb_get_stride(void) { return fb_stride; }

uint64_t fb_snapshot_size(void)
{
    if (!fb_ready || fb_width == 0 || fb_height == 0)
        return 0;

    /* Overflow guard: uint32_t * uint32_t * 4 can wrap uint64_t only on
     * absurd geometries (>= 2^31 pixels per axis). Refuse those: no GOP
     * mode in practice exceeds 16K x 16K, and silently returning a
     * wrapped size would let callers under-allocate. */
    uint64_t pixels = (uint64_t)fb_width * (uint64_t)fb_height;
    if (pixels > (0xFFFFFFFFFFFFFFFFULL / 4u))
        return 0;
    return pixels * 4u;
}

int fb_snapshot(void *dest_buf, uint32_t *width, uint32_t *height)
{
    if (!dest_buf || !width || !height)
        return -1;
    if (!fb_ready || !back_buf || fb_width == 0 || fb_height == 0)
        return -1;

    /* Reject geometries where a single row would not fit in the
     * mem_cpy32 count type (uint32_t). A >= 2^32-pixel row is pathological
     * but the check costs nothing and keeps the copy counts honest. */
    if ((uint64_t)fb_width > 0xFFFFFFFFu)
        return -1;

    /* Caller-visible layout is tightly packed 32-bit pixels. Fast path:
     * when the back buffer is contiguous (stride == width) AND the total
     * pixel count fits in uint32_t (the mem_cpy32 count type), issue a
     * single bulk copy so the SIMD memcpy dispatch fires once instead of
     * per-row. Fallback path: row-by-row copy for stride-padded buffers
     * or pathological >= 2^32-pixel totals. */
    uint32_t *dst = (uint32_t *)dest_buf;
    uint32_t w   = fb_width;
    uint32_t h   = fb_height;

    uint64_t total_px = (uint64_t)w * (uint64_t)h;
    if (fb_stride == w && total_px <= 0xFFFFFFFFu) {
        mem_cpy32(dst, back_buf, (uint32_t)total_px);
    } else {
        for (uint32_t y = 0; y < h; y++)
            mem_cpy32(dst + (uint64_t)y * w,
                      back_buf + (uint64_t)y * fb_stride,
                      w);
    }

    *width  = w;
    *height = h;
    return 0;
}

/* ---- Multi-output stubs (single-output today) ----------------------- */

uint32_t fb_get_output_count(void)
{  /* INTENTIONAL-STUB: single-output hardcoded until virtio-gpu multi-output
   lands -- every shipping display path (GOP, Bochs VGA) exposes exactly one
   scanout today, so 1 is the correct value, not a placeholder awaiting
   completion of THIS function specifically */
    /* When the virtio-gpu multi-output driver lands, return the negotiated
     * `max_outputs` here -- callers already iterate [0, count) so the
     * surface change is a one-line swap in this function plus scanout
     * routing in the driver. See the multi-monitor matrix row of the
     * test-isolation prerequisites. */
    return 1;
}

int fb_snapshot_monitor(uint32_t index, void *dest_buf,
                        uint32_t *width, uint32_t *height)
{
    if (index >= fb_get_output_count())
        return -2;
    /* Index 0 with single-output hardware is exactly fb_snapshot. */
    return fb_snapshot(dest_buf, width, height);
}

void fb_lock_compositor(void)
{
    compositor_locked = 1;
}

void fb_unlock_compositor(void)
{
    compositor_locked = 0;
}
