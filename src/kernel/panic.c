/* ============================================================================
 * panic.c — Styled kernel panic screen
 *
 * Renders a Windows-style blue screen with:
 *   - Sad face emoticon
 *   - Exception name and stop code
 *   - Faulting address and RIP
 *   - Source file + line
 *   - Full register dump
 *   - Stack trace via RBP chain walk
 *   - Crash dump to C:\Impossible\System\crashdump.log
 *   - Auto-restart countdown (configurable via Registry)
 *
 * The blue screen uses direct framebuffer rendering (no compositor)
 * to ensure it works even when the graphics stack is broken.
 * ============================================================================ */

#include "kernel/panic.h"
#include "kernel/idt.h"
#include "kernel/drivers/framebuffer.h"
#include "kernel/klog.h"
#include "kernel/printk.h"
#include "kernel/version.h"
#include "kernel/fs/vfs.h"
#include "kernel/drivers/pit.h"
#include "kernel/boot_splash.h"
#include "registry.h"
#include "bsod_icon.h"

/* --- Constants --- */

/* Impossible OS blue (same hue as Windows BSOD but richer) */
#define PANIC_BG_COLOR    0x00003380
#define PANIC_FG_COLOR    0x00FFFFFF
#define PANIC_DIM_COLOR   0x00A0B0D0
#define PANIC_ACCENT      0x004488FF

/* Default auto-restart seconds (0 = disabled) */
#define DEFAULT_RESTART_SECS  30

/* Maximum stack trace depth */
#define MAX_STACK_DEPTH   16

/* Exception names (duplicated here so we don't depend on idt.c internals) */
static const char *panic_exception_names[32] = {
    "DIVISION_BY_ZERO",              /* 0 */
    "DEBUG_EXCEPTION",               /* 1 */
    "NMI_INTERRUPT",                 /* 2 */
    "BREAKPOINT",                    /* 3 */
    "OVERFLOW",                      /* 4 */
    "BOUND_RANGE_EXCEEDED",          /* 5 */
    "INVALID_OPCODE",                /* 6 */
    "DEVICE_NOT_AVAILABLE",          /* 7 */
    "DOUBLE_FAULT",                  /* 8 */
    "COPROCESSOR_SEGMENT_OVERRUN",   /* 9 */
    "INVALID_TSS",                   /* 10 */
    "SEGMENT_NOT_PRESENT",           /* 11 */
    "STACK_SEGMENT_FAULT",           /* 12 */
    "GENERAL_PROTECTION_FAULT",      /* 13 */
    "PAGE_FAULT",                    /* 14 */
    "RESERVED",                      /* 15 */
    "X87_FP_EXCEPTION",              /* 16 */
    "ALIGNMENT_CHECK",               /* 17 */
    "MACHINE_CHECK",                 /* 18 */
    "SIMD_FP_EXCEPTION",             /* 19 */
    "VIRTUALIZATION_EXCEPTION",      /* 20 */
    "CONTROL_PROTECTION",            /* 21 */
    "RESERVED", "RESERVED", "RESERVED", "RESERVED",
    "RESERVED", "RESERVED",
    "HYPERVISOR_INJECTION",          /* 28 */
    "VMM_COMMUNICATION",             /* 29 */
    "SECURITY_EXCEPTION",            /* 30 */
    "RESERVED",                      /* 31 */
};

/* --- Helpers --- */

static inline uint64_t read_cr2(void)
{
    uint64_t val;
    __asm__ volatile ("mov %%cr2, %0" : "=r"(val));
    return val;
}

static inline uint64_t read_cr3(void)
{
    uint64_t val;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(val));
    return val;
}

/* Simple integer to decimal string (for countdown display) */
static void itoa_simple(uint32_t val, char *buf)
{
    char tmp[12];
    int i = 0;
    int j;

    if (val == 0) {
        buf[0] = '0';
        buf[1] = '\0';
        return;
    }
    while (val > 0) {
        tmp[i++] = '0' + (char)(val % 10);
        val /= 10;
    }
    for (j = 0; j < i; j++)
        buf[j] = tmp[i - 1 - j];
    buf[i] = '\0';
}

/* Draw the embedded BSOD icon (alpha-blended white over blue background) */
static void draw_bsod_icon(uint32_t x, uint32_t y)
{
    uint32_t ix, iy;
    for (iy = 0; iy < BSOD_ICON_H; iy++) {
        for (ix = 0; ix < BSOD_ICON_W; ix++) {
            uint32_t px = bsod_icon_pixels[iy * BSOD_ICON_W + ix];
            uint32_t a = (px >> 24) & 0xFF;
            if (a == 0) continue;  /* fully transparent */
            if (a == 255) {
                fb_put_pixel(x + ix, y + iy, 0x00FFFFFF);
            } else {
                /* Alpha blend: white * a + bg * (255-a) */
                uint32_t bg_r = (PANIC_BG_COLOR >> 16) & 0xFF;
                uint32_t bg_g = (PANIC_BG_COLOR >> 8) & 0xFF;
                uint32_t bg_b = PANIC_BG_COLOR & 0xFF;
                uint32_t r = (0xFF * a + bg_r * (255 - a)) / 255;
                uint32_t g = (0xFF * a + bg_g * (255 - a)) / 255;
                uint32_t b = (0xFF * a + bg_b * (255 - a)) / 255;
                fb_put_pixel(x + ix, y + iy, (r << 16) | (g << 8) | b);
            }
        }
    }
}

/* --- Crash dump to file --- */

static void write_crash_dump(struct interrupt_frame *frame,
                             const char *description, const char *file,
                             uint32_t line)
{
    char buf[2048];
    int pos = 0;
    uint64_t cr2_val;
    uint64_t cr3_val;
    uint64_t rbp;
    uint32_t depth;
    char num[12];

    if (!vfs_is_mounted('C'))
        return;

    /* Build crash dump text manually (no snprintf in freestanding) */
    /* Header */
    {
        const char *hdr = "=== IMPOSSIBLE OS CRASH DUMP ===\n";
        const char *s = hdr;
        while (*s && pos < 2040) buf[pos++] = *s++;
    }

    /* Description */
    {
        const char *s;
        const char *lbl = "Description: ";
        s = lbl;
        while (*s && pos < 2040) buf[pos++] = *s++;
        s = description;
        while (*s && pos < 2040) buf[pos++] = *s++;
        buf[pos++] = '\n';
    }

    /* File:line */
    if (file) {
        const char *lbl = "Source: ";
        const char *s = lbl;
        while (*s && pos < 2040) buf[pos++] = *s++;
        s = file;
        while (*s && pos < 2040) buf[pos++] = *s++;
        buf[pos++] = ':';
        itoa_simple(line, num);
        s = num;
        while (*s && pos < 2040) buf[pos++] = *s++;
        buf[pos++] = '\n';
    }

    /* Registers from frame */
    if (frame) {
        const char *rlbl = "\nRegisters:\n";
        const char *s = rlbl;
        while (*s && pos < 2040) buf[pos++] = *s++;

        /* We'll just note that registers are available in the frame */
        {
            const char *note = "  (See serial output for full register dump)\n";
            s = note;
            while (*s && pos < 2040) buf[pos++] = *s++;
        }
    }

    /* Stack trace */
    cr2_val = read_cr2();
    cr3_val = read_cr3();
    (void)cr2_val;
    (void)cr3_val;

    {
        const char *stlbl = "\nStack Trace:\n";
        const char *s = stlbl;
        while (*s && pos < 2040) buf[pos++] = *s++;
    }

    rbp = frame ? frame->rbp : 0;
    if (!rbp) {
        __asm__ volatile ("mov %%rbp, %0" : "=r"(rbp));
    }

    for (depth = 0; depth < MAX_STACK_DEPTH && rbp != 0; depth++) {
        uint64_t *frame_ptr = (uint64_t *)rbp;
        uint64_t ret_addr;

        /* Safety check: ensure RBP points to a reasonable address */
        if (rbp < 0x100000 || rbp > 0x200000)
            break;

        ret_addr = frame_ptr[1];  /* return address is at [rbp+8] */
        if (ret_addr == 0)
            break;

        {
            const char *fr = "  #";
            const char *s = fr;
            while (*s && pos < 2040) buf[pos++] = *s++;
            itoa_simple(depth, num);
            s = num;
            while (*s && pos < 2040) buf[pos++] = *s++;
            {
                const char *at = " at 0x";
                s = at;
                while (*s && pos < 2040) buf[pos++] = *s++;
            }
            /* Hex format for return address - simplified */
            {
                uint64_t v = ret_addr;
                char hex[17];
                int hi;
                for (hi = 15; hi >= 0; hi--) {
                    uint32_t nib = (uint32_t)(v & 0xF);
                    hex[hi] = "0123456789ABCDEF"[nib];
                    v >>= 4;
                }
                hex[16] = '\0';
                s = hex;
                while (*s && pos < 2040) buf[pos++] = *s++;
            }
            buf[pos++] = '\n';
        }

        rbp = frame_ptr[0];  /* previous RBP */
    }

    buf[pos] = '\0';

    /* Ensure the directory exists and write the crash dump */
    {
        struct vfs_node *sys_dir = vfs_open("C:\\Impossible\\System", VFS_O_READ);
        if (sys_dir) {
            vfs_close(sys_dir);
        }
    }

    vfs_create("C:\\Impossible\\System\\crashdump.log", 1);
    {
        struct vfs_node *dump_file = vfs_open(
            "C:\\Impossible\\System\\crashdump.log", VFS_O_WRITE);
        if (dump_file) {
            vfs_write(dump_file, 0, (uint32_t)pos, (const uint8_t *)buf);
            vfs_close(dump_file);
        }
    }
}

/* --- Main panic screen --- */

void panic_screen(struct interrupt_frame *frame, uint64_t error_code,
                  const char *description, const char *file, uint32_t line)
{
    uint32_t screen_w;
    uint32_t screen_h;
    uint64_t cr2_val;
    uint64_t cr3_val;
    uint64_t rbp;
    uint32_t depth;
    int32_t restart_secs = 0;
    uint32_t reg_restart;

    /* Disable interrupts to prevent further exceptions */
    __asm__ volatile ("cli");

    /* Stop boot animation dots if still running */
    if (boot_splash_active())
        boot_splash_finish();

    /* Unlock the compositor so we can draw directly */
    fb_unlock_compositor();

    screen_w = fb_get_width();
    screen_h = fb_get_height();

    /* === Draw the blue background === */
    fb_fill_rect(0, 0, screen_w, screen_h, PANIC_BG_COLOR);
    fb_swap();

    /* Read CR2/CR3 before we lose context */
    cr2_val = read_cr2();
    cr3_val = read_cr3();

    /* === Draw BSOD icon === */
    draw_bsod_icon(80, 60);
    fb_swap();

    /* Text position is determined by printk cursor */

    /* Move cursor down to below the icon
     * (each char row is ~16px, icon is 128px + 60px offset + gap = ~14 rows) */
    {
        uint32_t i;
        fb_set_color(PANIC_FG_COLOR, PANIC_BG_COLOR);
        for (i = 0; i < 14; i++)
            printk("\n");
    }

    /* Title */
    printk("    Your Impossible OS ran into a problem and needs to restart.\n");
    printk("    We're just collecting some error info, and then we'll restart for you.\n\n");

    /* Stop code */
    fb_set_color(PANIC_DIM_COLOR, PANIC_BG_COLOR);
    if (frame && frame->int_no < 32) {
        printk("    Stop code:  %s\n", (uint64_t)(uintptr_t)panic_exception_names[frame->int_no]);
    } else {
        printk("    Stop code:  KERNEL_PANIC\n");
    }

    /* Description */
    fb_set_color(PANIC_FG_COLOR, PANIC_BG_COLOR);
    printk("    Description: %s\n", (uint64_t)(uintptr_t)description);

    /* Source location */
    if (file) {
        fb_set_color(PANIC_DIM_COLOR, PANIC_BG_COLOR);
        printk("    Source:  %s:%u\n", (uint64_t)(uintptr_t)file, (uint64_t)line);
    }

    printk("\n");

    /* Error code and addresses */
    fb_set_color(PANIC_FG_COLOR, PANIC_BG_COLOR);
    if (frame) {
        printk("    Error code: %p\n", error_code);
        printk("    RIP:        %p\n", frame->rip);
        printk("    CR2:        %p\n", cr2_val);
    }

    printk("\n");

    /* === Register dump === */
    if (frame) {
        fb_set_color(PANIC_DIM_COLOR, PANIC_BG_COLOR);
        printk("    --- Register Dump ---\n");
        fb_set_color(PANIC_FG_COLOR, PANIC_BG_COLOR);
        printk("    RAX=%p  RBX=%p  RCX=%p\n", frame->rax, frame->rbx, frame->rcx);
        printk("    RDX=%p  RSI=%p  RDI=%p\n", frame->rdx, frame->rsi, frame->rdi);
        printk("    R8 =%p  R9 =%p  R10=%p\n", frame->r8, frame->r9, frame->r10);
        printk("    R11=%p  R12=%p  R13=%p\n", frame->r11, frame->r12, frame->r13);
        printk("    R14=%p  R15=%p  RBP=%p\n", frame->r14, frame->r15, frame->rbp);
        printk("    RSP=%p  RFLAGS=%p\n", frame->rsp, frame->rflags);
        printk("    CR2=%p  CR3=%p\n", cr2_val, cr3_val);
        printk("    CS=%x  SS=%x\n", frame->cs, frame->ss);
    }

    printk("\n");

    /* === Stack Trace === */
    fb_set_color(PANIC_DIM_COLOR, PANIC_BG_COLOR);
    printk("    --- Stack Trace ---\n");
    fb_set_color(PANIC_FG_COLOR, PANIC_BG_COLOR);

    /* Walk the RBP chain */
    rbp = frame ? frame->rbp : 0;
    if (!rbp) {
        __asm__ volatile ("mov %%rbp, %0" : "=r"(rbp));
    }

    for (depth = 0; depth < MAX_STACK_DEPTH && rbp != 0; depth++) {
        uint64_t *frame_ptr = (uint64_t *)rbp;
        uint64_t ret_addr;

        /* Safety: RBP should be in kernel memory range */
        if (rbp < 0x100000 || rbp > 0x200000)
            break;

        ret_addr = frame_ptr[1];
        if (ret_addr == 0)
            break;

        printk("    #%u  %p\n", (uint64_t)depth, ret_addr);
        rbp = frame_ptr[0];
    }

    if (depth == 0) {
        printk("    (no stack frames available)\n");
    }

    /* === Version footer === */
    printk("\n");
    fb_set_color(PANIC_DIM_COLOR, PANIC_BG_COLOR);
    printk("    Impossible OS v%s (build %u, %s@%s)\n",
           (uint64_t)(uintptr_t)version_short(),
           (uint64_t)version_build_number(),
           (uint64_t)(uintptr_t)version_branch(),
           (uint64_t)(uintptr_t)version_git_hash());

    /* === Write crash dump to disk === */
    write_crash_dump(frame, description, file, line);
    {
        fb_set_color(PANIC_DIM_COLOR, PANIC_BG_COLOR);
        if (vfs_is_mounted('C'))
            printk("\n    Crash dump saved to C:\\Impossible\\System\\crashdump.log\n");
    }

    /* Swap to show everything */
    fb_swap();

    /* === Auto-restart countdown === */
    /* Check Registry for auto-restart preference */
    {
        HKEY hRecovery = (HKEY)0;
        long rc = RegOpenKeyEx(HKEY_LOCAL_MACHINE, "SYSTEM\\Recovery",
                               0, KEY_READ, &hRecovery);
        if (rc == ERROR_SUCCESS) {
            if (RegGetDword(hRecovery, "AutoRestart", &reg_restart)
                == ERROR_SUCCESS) {
                restart_secs = (int32_t)reg_restart;
            } else {
                restart_secs = DEFAULT_RESTART_SECS;
                RegSetDword(hRecovery, "AutoRestart",
                            (uint32_t)DEFAULT_RESTART_SECS);
            }
            RegCloseKey(hRecovery);
        } else {
            /* Create the key with defaults */
            uint32_t disp;
            rc = RegCreateKeyEx(HKEY_LOCAL_MACHINE, "SYSTEM\\Recovery",
                                0, (const char *)0, 0, KEY_ALL_ACCESS,
                                (void *)0, &hRecovery, &disp);
            if (rc == ERROR_SUCCESS) {
                RegSetDword(hRecovery, "AutoRestart",
                            (uint32_t)DEFAULT_RESTART_SECS);
                restart_secs = DEFAULT_RESTART_SECS;
                RegCloseKey(hRecovery);
            }
        }
    }

    if (restart_secs > 0) {
        /* Countdown loop using PIT ticks */
        int32_t secs_left = restart_secs;
        uint64_t last_tick = uptime();

        /* Re-enable interrupts just for the PIT timer */
        __asm__ volatile ("sti");

        while (secs_left > 0) {
            uint64_t now = uptime();
            if (now != last_tick) {
                last_tick = now;
                secs_left--;

                /* Redraw countdown line at bottom */
                fb_set_color(PANIC_ACCENT, PANIC_BG_COLOR);
                /* Clear the countdown line area */
                fb_fill_rect(0, screen_h - 40, screen_w, 40, PANIC_BG_COLOR);

                /* Draw a progress bar */
                {
                    uint32_t bar_w = (screen_w - 160) *
                        (uint32_t)(restart_secs - secs_left) / (uint32_t)restart_secs;
                    fb_fill_rect(80, screen_h - 30, bar_w, 8, PANIC_ACCENT);
                    fb_fill_rect(80, screen_h - 30,
                                 screen_w - 160, 8, PANIC_DIM_COLOR);
                    fb_fill_rect(80, screen_h - 30, bar_w, 8, PANIC_ACCENT);
                }

                fb_swap();
            }

            /* Busy-wait (HLT until next interrupt) */
            __asm__ volatile ("hlt");
        }

        /* Restart via ACPI reset or triple fault */
        {
            /* Try ACPI reset first (port 0xCF9) */
            __asm__ volatile ("outb %0, %1" : : "a"((uint8_t)0x06), "Nd"((uint16_t)0xCF9));
            /* If that didn't work, triple fault by loading invalid IDT */
            {
                struct { uint16_t limit; uint64_t base; } __attribute__((packed)) null_idt = {0, 0};
                __asm__ volatile ("lidt %0" : : "m"(null_idt));
                __asm__ volatile ("int $0");  /* triple fault → reboot */
            }
        }
    }

    /* No auto-restart — halt permanently */
    fb_set_color(PANIC_DIM_COLOR, PANIC_BG_COLOR);
    printk("\n    System halted. Press reset to restart.\n");
    fb_swap();

    for (;;)
        __asm__ volatile ("cli; hlt");
}
