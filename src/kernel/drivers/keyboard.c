/* ============================================================================
 * keyboard.c -- PS/2 Keyboard driver
 *
 * Handles IRQ 1 (interrupt vector 33 after PIC remap).
 * Reads scan codes from port 0x60, translates to ASCII using a
 * US QWERTY lookup table, and pushes characters into a circular buffer.
 *
 * Supports: Shift (left/right), Caps Lock, Ctrl, Alt modifier keys.
 * ============================================================================ */

#include "kernel/drivers/keyboard.h"
#include "kernel/irq.h"
#include "kernel/drivers/pic.h"
#include "kernel/drivers/ioapic.h"
#include "kernel/acpi.h"
#include "kernel/boot_init.h"
#include "kernel/klog.h"
#include "kernel/ipc/signal.h"
#include "kernel/sched/spinlock.h"
#include "desktop/terminal.h"
#include "desktop/wm.h"

/* --- Port I/O --- */
#define KB_DATA_PORT  0x60
#define KB_STATUS_PORT 0x64

static inline uint8_t inb(uint16_t port)
{
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

/* --- Circular input buffer --- */
#define KB_BUFFER_SIZE 256

static char kb_buffer[KB_BUFFER_SIZE];
static volatile uint32_t kb_head = 0;  /* write position (IRQ side) */
static volatile uint32_t kb_tail = 0;  /* read position  (thread side) */

/* Spinlock protecting the ring buffer.
 * Both IRQ-side push and thread-side pop use irqsave/irqrestore so the
 * lock is safe regardless of which context the caller runs in. */
static spinlock_t kb_lock = SPINLOCK_INIT;

/* Set 1 once keyboard_init reaches the success path (i8042 present); 0 on the
 * early-exit (no-i8042 / ACPI-reduced) paths. For the input diag summary. */
static volatile uint8_t s_kbd_present;

static void kb_buffer_push(char c)
{
    /* Called from IRQ context -- use irqsave (IRQs are already off here,
     * so irqrestore will NOT blindly re-enable them). */
    uint64_t flags;
    spin_lock_irqsave(&kb_lock, &flags);
    uint32_t next = (kb_head + 1) % KB_BUFFER_SIZE;
    if (next != kb_tail) {
        kb_buffer[kb_head] = c;
        kb_head = next;
    }
    spin_unlock_irqrestore(&kb_lock, flags);
}

/* --- Modifier key state --- */
static uint8_t shift_held = 0;
static uint8_t ctrl_held  = 0;
static uint8_t alt_held   = 0;
static uint8_t capslock_on = 0;
static uint8_t e0_prefix  = 0;   /* set when 0xE0 prefix byte received */

/* --- Scan code set 1 → ASCII lookup tables (US QWERTY) --- */

/* Normal (unshifted) ASCII for scan codes 0x00-0x58 */
static const char scancode_normal[0x59] = {
    0,    27,  '1', '2', '3', '4', '5', '6',   /* 00-07 */
    '7', '8', '9', '0', '-', '=', '\b', '\t',  /* 08-0F */
    'q', 'w', 'e', 'r', 't', 'y', 'u', 'i',   /* 10-17 */
    'o', 'p', '[', ']', '\n', 0,   'a', 's',   /* 18-1F (1D=LCtrl) */
    'd', 'f', 'g', 'h', 'j', 'k', 'l', ';',   /* 20-27 */
    '\'', '`', 0,   '\\', 'z', 'x', 'c', 'v', /* 28-2F (2A=LShift) */
    'b', 'n', 'm', ',', '.', '/', 0,   '*',    /* 30-37 (36=RShift) */
    0,   ' ', 0,   0,   0,   0,   0,   0,      /* 38-3F (38=LAlt, 3A=CapsLock) */
    0,   0,   0,   0,   0,   0,   0,   '7',    /* 40-47 (F1-F10, NumLock, ScrollLock, KP7) */
    '8', '9', '-', '4', '5', '6', '+', '1',    /* 48-4F */
    '2', '3', '0', '.',  0,   0,   0,   0,     /* 50-57 */
    0,                                           /* 58 */
};

/* Shifted ASCII for scan codes 0x00-0x58 */
static const char scancode_shifted[0x59] = {
    0,    27,  '!', '@', '#', '$', '%', '^',   /* 00-07 */
    '&', '*', '(', ')', '_', '+', '\b', '\t',  /* 08-0F */
    'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I',   /* 10-17 */
    'O', 'P', '{', '}', '\n', 0,   'A', 'S',   /* 18-1F */
    'D', 'F', 'G', 'H', 'J', 'K', 'L', ':',   /* 20-27 */
    '"', '~', 0,   '|', 'Z', 'X', 'C', 'V',   /* 28-2F */
    'B', 'N', 'M', '<', '>', '?', 0,   '*',    /* 30-37 */
    0,   ' ', 0,   0,   0,   0,   0,   0,      /* 38-3F */
    0,   0,   0,   0,   0,   0,   0,   '7',    /* 40-47 */
    '8', '9', '-', '4', '5', '6', '+', '1',    /* 48-4F */
    '2', '3', '0', '.',  0,   0,   0,   0,     /* 50-57 */
    0,                                           /* 58 */
};

/* --- USB HID boot-protocol usage -> ASCII (US QWERTY) ---
 * Indexed by HID usage ID 0x00-0x3F (the printable boot-protocol range).
 * Independent of the PS/2 scancode tables and the shift_held latch -- the HID
 * report carries its own modifier byte, so USB keys never mutate the shared
 * PS/2 modifier state. */
#define HID_USAGE_MAX 0x40

static const char hid_normal[HID_USAGE_MAX] = {
    0, 0, 0, 0, 'a', 'b', 'c', 'd',            /* 00-07 (01-03 = rollover/error) */
    'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l',    /* 08-0F */
    'm', 'n', 'o', 'p', 'q', 'r', 's', 't',    /* 10-17 */
    'u', 'v', 'w', 'x', 'y', 'z', '1', '2',    /* 18-1F */
    '3', '4', '5', '6', '7', '8', '9', '0',    /* 20-27 */
    '\n', 27, '\b', '\t', ' ', '-', '=', '[',  /* 28-2F (28=Enter 29=Esc 2A=BS 2B=Tab 2C=Sp) */
    ']', '\\', 0, ';', '\'', '`', ',', '.',    /* 30-37 (32=non-US# skip) */
    '/', 0, 0, 0, 0, 0, 0, 0,                  /* 38-3F (39=CapsLock, 3A+=F-keys) */
};

static const char hid_shifted[HID_USAGE_MAX] = {
    0, 0, 0, 0, 'A', 'B', 'C', 'D',
    'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L',
    'M', 'N', 'O', 'P', 'Q', 'R', 'S', 'T',
    'U', 'V', 'W', 'X', 'Y', 'Z', '!', '@',
    '#', '$', '%', '^', '&', '*', '(', ')',
    '\n', 27, '\b', '\t', ' ', '_', '+', '{',
    '}', '|', 0, ':', '"', '~', '<', '>',
    '?', 0, 0, 0, 0, 0, 0, 0,
};

/* HID boot-protocol modifier byte bits */
#define HID_MOD_LCTRL   0x01
#define HID_MOD_LSHIFT  0x02
#define HID_MOD_RCTRL   0x10
#define HID_MOD_RSHIFT  0x20

/* Scan code constants for modifier keys */
#define SC_LSHIFT_PRESS   0x2A
#define SC_LSHIFT_RELEASE 0xAA
#define SC_RSHIFT_PRESS   0x36
#define SC_RSHIFT_RELEASE 0xB6
#define SC_LCTRL_PRESS    0x1D
#define SC_LCTRL_RELEASE  0x9D
#define SC_LALT_PRESS     0x38
#define SC_LALT_RELEASE   0xB8
#define SC_CAPSLOCK       0x3A
#define SC_F4             0x3E    /* Alt+F4 closes the focused window */

/* --- IRQ 1 handler (via irq_register API) --- */
static void keyboard_irq_callback(uint8_t vector, void *ctx)
{
    uint8_t scancode;
    char c;

    (void)vector;
    (void)ctx;

    /* Read the scan code from the keyboard data port */
    scancode = inb(KB_DATA_PORT);

    /* Ctrl+ScrollLock crash trigger (TODO-16 S1) */
    {
        extern void bugcheck_keyboard_check(uint8_t sc, int ctrl);
        bugcheck_keyboard_check(scancode, ctrl_held);
    }

    /* Handle 0xE0 prefix (extended scan codes for arrow keys etc.) */
    if (scancode == 0xE0) {
        e0_prefix = 1;
        return;
    }

    if (e0_prefix) {
        e0_prefix = 0;
        /* Only handle key presses (bit 7 clear) */
        if (!(scancode & 0x80)) {
            switch (scancode) {
            case 0x48:  /* Up arrow */
                if (terminal_is_open())
                    terminal_key_input((char)KEY_UP);
                else
                    kb_buffer_push((char)KEY_UP);
                break;
            case 0x50:  /* Down arrow */
                if (terminal_is_open())
                    terminal_key_input((char)KEY_DOWN);
                else
                    kb_buffer_push((char)KEY_DOWN);
                break;
            case 0x4B:  /* Left arrow */
                if (terminal_is_open())
                    terminal_key_input((char)KEY_LEFT);
                else
                    kb_buffer_push((char)KEY_LEFT);
                break;
            case 0x4D:  /* Right arrow */
                if (terminal_is_open())
                    terminal_key_input((char)KEY_RIGHT);
                else
                    kb_buffer_push((char)KEY_RIGHT);
                break;
            default: break;
            }
        }
        return;
    }

    /* Handle modifier key presses and releases */
    switch (scancode) {
    case SC_LSHIFT_PRESS:
    case SC_RSHIFT_PRESS:
        shift_held = 1;
        return;
    case SC_LSHIFT_RELEASE:
    case SC_RSHIFT_RELEASE:
        shift_held = 0;
        return;
    case SC_LCTRL_PRESS:
        ctrl_held = 1;
        return;
    case SC_LCTRL_RELEASE:
        ctrl_held = 0;
        return;
    case SC_LALT_PRESS:
        alt_held = 1;
        return;
    case SC_LALT_RELEASE:
        alt_held = 0;
        return;
    case SC_CAPSLOCK:
        capslock_on = !capslock_on;
        return;
    default:
        break;
    }

    /* Alt+F4: close the focused window. Trap before the release/range
     * filter so a one-shot Alt+F4 press fires exactly once (the press
     * goes through; the matching 0xBE release falls through and is
     * swallowed by the release check below). F4's scancode_normal[]
     * entry is 0, so allowing it past the filter would produce no char;
     * handling it here keeps the key out of the terminal input ring
     * too. */
    if (alt_held && scancode == SC_F4) {
        wm_close_focused_window();
        return;
    }

    /* Ignore key releases (bit 7 set) */
    if (scancode & 0x80)
        return;

    /* Ignore out-of-range scan codes */
    if (scancode >= 0x59)
        return;

    /* Look up the character */
    if (shift_held)
        c = scancode_shifted[scancode];
    else
        c = scancode_normal[scancode];

    /* Apply Caps Lock (only affects a-z / A-Z) */
    if (capslock_on && c >= 'a' && c <= 'z')
        c -= 32;   /* to uppercase */
    else if (capslock_on && c >= 'A' && c <= 'Z')
        c += 32;   /* Shift+CapsLock = lowercase */

    /* Handle Ctrl+letter (produce control codes 1-26) */
    if (ctrl_held && c >= 'a' && c <= 'z')
        c = (char)(c - 'a' + 1);
    else if (ctrl_held && c >= 'A' && c <= 'Z')
        c = (char)(c - 'A' + 1);

    /* Ctrl+C (char code 3) → dispatch SIGINT to foreground task */
    if (c == 3) {
        signal_ctrl_c();
        return;
    }

    /* Push printable/control characters into the appropriate buffer */
    if (c != 0) {
        if (terminal_is_open())
            terminal_key_input(c);
        else
            kb_buffer_push(c);
    }

    /* EOI handled by irq_dispatch_wrapper */
}

void keyboard_init(void)
{
    POST16(0xD502);

    /* Gate: never touch 0x60/0x64 unless an i8042 is actually present.
     * - Hardware-reduced ACPI is the reliable "no legacy fixed hardware"
     *   signal -- hard-skip without probing.
     * - Otherwise the FADT IAPC_BOOT_ARCH.8042 bit is unreliable (QEMU WHPX
     *   reports 0 for a working emulated i8042), so the authoritative test is
     *   a direct port-0x64 probe: 0xFF = floating bus = no controller. The
     *   probe runs unconditionally (not only when acpi_has_8042() is false),
     *   so a no/short-FADT box with no i8042 is still skipped before any I/O. */
    if (acpi_hw_reduced()) {
        klog(LOG_INFO, "input", "PS/2 keyboard: skipped (ACPI hardware-reduced -- no i8042)");
        return;
    }
    {
        uint8_t probe = inb(KB_STATUS_PORT);
        if (probe == 0xFF) {
            klog(LOG_INFO, "input", "PS/2 keyboard: skipped (no i8042 -- port 0x64 reads 0xFF)");
            return;
        }
        if (!acpi_has_8042())
            klog(LOG_INFO, "input", "PS/2 keyboard: FADT says no i8042 but port probe OK (0x%x)",
                 (uint32_t)probe);
    }

    /* Flush any pending data in the keyboard buffer.
     * Timeout prevents infinite loop on platforms without an i8042
     * controller where port 0x64 returns 0xFF. */
    {
        uint32_t timeout = 1024;
        while ((inb(KB_STATUS_PORT) & 0x01) && --timeout)
            inb(KB_DATA_PORT);
    }

    /* Register keyboard via GSI-based routing (IOAPIC handles vector allocation) */
    if (ioapic_available()) {
        irq_request_gsi(ioapic_isa_to_gsi(IRQ_KEYBOARD),
                         keyboard_irq_callback, (void *)0, "ps2_kbd");
    } else {
        irq_register(33, keyboard_irq_callback, (void *)0, "ps2_kbd");
        pic_unmask_irq(IRQ_KEYBOARD);
    }

    s_kbd_present = 1;   /* reached only on the success path (not early-exits) */
    klog(LOG_INFO, "input", "PS/2 keyboard initialized (US QWERTY, IRQ %u)",
         ioapic_available() ? (uint32_t)ioapic_isa_to_gsi(IRQ_KEYBOARD)
                            : (uint32_t)IRQ_KEYBOARD);
}

int keyboard_is_present(void)
{
    return s_kbd_present ? 1 : 0;
}

char keyboard_getchar(void)
{
    char c;
    uint64_t flags;

    /* Block until a character is available */
    for (;;) {
        spin_lock_irqsave(&kb_lock, &flags);
        if (kb_head != kb_tail) break;
        spin_unlock_irqrestore(&kb_lock, flags);
        __asm__ volatile("hlt");   /* sleep until next interrupt */
    }
    c = kb_buffer[kb_tail];
    kb_tail = (kb_tail + 1) % KB_BUFFER_SIZE;
    spin_unlock_irqrestore(&kb_lock, flags);
    return c;
}

char keyboard_trygetchar(void)
{
    char c;
    uint64_t flags;

    spin_lock_irqsave(&kb_lock, &flags);
    if (kb_head == kb_tail) {
        spin_unlock_irqrestore(&kb_lock, flags);
        return 0;
    }
    c = kb_buffer[kb_tail];
    kb_tail = (kb_tail + 1) % KB_BUFFER_SIZE;
    spin_unlock_irqrestore(&kb_lock, flags);
    return c;
}

void keyboard_inject_scancode(uint8_t scancode)
{
    char c;

    /* Handle 0xE0 prefix */
    if (scancode == 0xE0) {
        e0_prefix = 1;
        return;
    }

    if (e0_prefix) {
        e0_prefix = 0;
        if (!(scancode & 0x80)) {
            switch (scancode) {
            case 0x48: kb_buffer_push((char)KEY_UP); break;
            case 0x50: kb_buffer_push((char)KEY_DOWN); break;
            case 0x4B: kb_buffer_push((char)KEY_LEFT); break;
            case 0x4D: kb_buffer_push((char)KEY_RIGHT); break;
            default: break;
            }
        }
        return;
    }

    /* Modifier keys */
    switch (scancode) {
    case SC_LSHIFT_PRESS: case SC_RSHIFT_PRESS:   shift_held = 1;  return;
    case SC_LSHIFT_RELEASE: case SC_RSHIFT_RELEASE: shift_held = 0; return;
    case SC_LCTRL_PRESS:   ctrl_held = 1;   return;
    case SC_LCTRL_RELEASE: ctrl_held = 0;   return;
    case SC_LALT_PRESS:    alt_held = 1;    return;
    case SC_LALT_RELEASE:  alt_held = 0;    return;
    case SC_CAPSLOCK:      capslock_on = !capslock_on; return;
    default: break;
    }

    /* Alt+F4 closes the focused window. Mirrors the IRQ path above so
     * the inject harness (test framework section 4) exercises the same
     * code path as the real i8042 IRQ. */
    if (alt_held && scancode == SC_F4) {
        wm_close_focused_window();
        return;
    }

    if (scancode & 0x80) return;  /* release */
    if (scancode >= 0x59) return; /* out of range */

    c = shift_held ? scancode_shifted[scancode] : scancode_normal[scancode];

    if (capslock_on && c >= 'a' && c <= 'z') c -= 32;
    else if (capslock_on && c >= 'A' && c <= 'Z') c += 32;

    if (ctrl_held && c >= 'a' && c <= 'z') c = (char)(c - 'a' + 1);
    else if (ctrl_held && c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 1);

    if (c == 3) { signal_ctrl_c(); return; }

    if (c != 0) {
        if (terminal_is_open())
            terminal_key_input(c);
        else
            kb_buffer_push(c);
    }
}

/* Inject one newly-pressed USB HID boot-protocol key. Uses the report's
 * modifier byte directly (NOT the PS/2 shift_held/ctrl_held latches), so USB
 * and PS/2 input coexist without clobbering each other's modifier state. The
 * caller (the HID report parser) supplies only freshly-pressed usages and
 * filters error/rollover usages (0x01-0x03). Routes to the same Ctrl-C signal
 * / terminal / kb_buffer path as the PS/2 driver. ISR-context-safe
 * (kb_buffer_push + terminal_key_input take their own locks). */
void keyboard_inject_hid_key(uint8_t usage, uint8_t hid_modifiers)
{
    char c;
    int shift = (hid_modifiers & (HID_MOD_LSHIFT | HID_MOD_RSHIFT)) ? 1 : 0;
    int ctrl  = (hid_modifiers & (HID_MOD_LCTRL | HID_MOD_RCTRL)) ? 1 : 0;

    /* Caps Lock (HID usage 0x39) toggles the shared latch on each press, like
     * the PS/2 path; it produces no character. */
    if (usage == 0x39) {
        capslock_on = !capslock_on;
        return;
    }

    if (usage >= HID_USAGE_MAX)
        return;
    c = shift ? hid_shifted[usage] : hid_normal[usage];
    if (c == 0)
        return;

    /* CapsLock toggles letter case (HID reports carry no caps state, so reuse
     * the shared toggle the PS/2 path maintains -- a benign single-byte read). */
    if (capslock_on && c >= 'a' && c <= 'z') c = (char)(c - 32);
    else if (capslock_on && c >= 'A' && c <= 'Z') c = (char)(c + 32);

    if (ctrl && c >= 'a' && c <= 'z') c = (char)(c - 'a' + 1);
    else if (ctrl && c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 1);

    if (c == 3) { signal_ctrl_c(); return; }

    if (terminal_is_open())
        terminal_key_input(c);
    else
        kb_buffer_push(c);
}

void keyboard_reset_state(void)
{
    /* Single critical section over the buffer indices AND the modifier /
     * prefix scalars. Splitting them was racy: an IRQ between the drain
     * and the flag clear could push a fresh character into the just-drained
     * buffer (kb_buffer_push runs under kb_lock so that part is safe) OR
     * latch a modifier / E0 prefix that the lockless second half then
     * clobbers, leaving the IRQ handler with mismatched expectations on
     * the next byte. spin_lock_irqsave masks local IRQs so the hardware
     * IRQ1 handler cannot race this CPU at all; the lock itself handles
     * cross-CPU mutual exclusion. Codex [H] quality review. */
    uint64_t flags;
    spin_lock_irqsave(&kb_lock, &flags);
    kb_head     = 0;
    kb_tail     = 0;
    shift_held  = 0;
    ctrl_held   = 0;
    alt_held    = 0;
    capslock_on = 0;
    e0_prefix   = 0;
    spin_unlock_irqrestore(&kb_lock, flags);
}
