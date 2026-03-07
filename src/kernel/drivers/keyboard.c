/* ============================================================================
 * keyboard.c — PS/2 Keyboard driver
 *
 * Handles IRQ 1 (interrupt vector 33 after PIC remap).
 * Reads scan codes from port 0x60, translates to ASCII using a
 * US QWERTY lookup table, and pushes characters into a circular buffer.
 *
 * Supports: Shift (left/right), Caps Lock, Ctrl, Alt modifier keys.
 * ============================================================================ */

#include "kernel/drivers/keyboard.h"
#include "kernel/idt.h"
#include "kernel/drivers/pic.h"
#include "kernel/printk.h"

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
static volatile uint32_t kb_head = 0;  /* write position */
static volatile uint32_t kb_tail = 0;  /* read position */

static void kb_buffer_push(char c)
{
    uint32_t next = (kb_head + 1) % KB_BUFFER_SIZE;
    if (next != kb_tail) {  /* don't overwrite unread data */
        kb_buffer[kb_head] = c;
        kb_head = next;
    }
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

/* --- IRQ 1 handler --- */
static uint64_t keyboard_irq_handler(struct interrupt_frame *frame)
{
    uint8_t scancode;
    char c;

    /* Read the scan code from the keyboard data port */
    scancode = inb(KB_DATA_PORT);

    /* Handle 0xE0 prefix (extended scan codes for arrow keys etc.) */
    if (scancode == 0xE0) {
        e0_prefix = 1;
        goto done;
    }

    if (e0_prefix) {
        e0_prefix = 0;
        /* Only handle key presses (bit 7 clear) */
        if (!(scancode & 0x80)) {
            switch (scancode) {
            case 0x48: kb_buffer_push((char)KEY_UP);    break;  /* Up arrow */
            case 0x50: kb_buffer_push((char)KEY_DOWN);  break;  /* Down arrow */
            case 0x4B: kb_buffer_push((char)KEY_LEFT);  break;  /* Left arrow */
            case 0x4D: kb_buffer_push((char)KEY_RIGHT); break;  /* Right arrow */
            default: break;
            }
        }
        goto done;
    }

    /* Handle modifier key presses and releases */
    switch (scancode) {
    case SC_LSHIFT_PRESS:
    case SC_RSHIFT_PRESS:
        shift_held = 1;
        goto done;
    case SC_LSHIFT_RELEASE:
    case SC_RSHIFT_RELEASE:
        shift_held = 0;
        goto done;
    case SC_LCTRL_PRESS:
        ctrl_held = 1;
        goto done;
    case SC_LCTRL_RELEASE:
        ctrl_held = 0;
        goto done;
    case SC_LALT_PRESS:
        alt_held = 1;
        goto done;
    case SC_LALT_RELEASE:
        alt_held = 0;
        goto done;
    case SC_CAPSLOCK:
        capslock_on = !capslock_on;
        goto done;
    default:
        break;
    }

    /* Ignore key releases (bit 7 set) */
    if (scancode & 0x80)
        goto done;

    /* Ignore out-of-range scan codes */
    if (scancode >= 0x59)
        goto done;

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

    /* Push printable/control characters into the buffer */
    if (c != 0)
        kb_buffer_push(c);

done:
    (void)alt_held;  /* suppress unused warning (reserved for future use) */
    pic_send_eoi(IRQ_KEYBOARD);
    return (uint64_t)frame;
}

void keyboard_init(void)
{
    /* Flush any pending data in the keyboard buffer */
    while (inb(KB_STATUS_PORT) & 0x01)
        inb(KB_DATA_PORT);

    /* Register IRQ 1 handler (interrupt vector 33) */
    idt_register_handler(33, keyboard_irq_handler);

    /* Unmask IRQ 1 on the PIC */
    pic_unmask_irq(IRQ_KEYBOARD);

    printk("[OK] PS/2 keyboard initialized (US QWERTY)\n");
}

char keyboard_getchar(void)
{
    char c;

    /* Block until a character is available */
    while (kb_head == kb_tail)
        __asm__ volatile ("hlt");   /* sleep until next interrupt */

    c = kb_buffer[kb_tail];
    kb_tail = (kb_tail + 1) % KB_BUFFER_SIZE;
    return c;
}

char keyboard_trygetchar(void)
{
    char c;

    if (kb_head == kb_tail)
        return 0;

    c = kb_buffer[kb_tail];
    kb_tail = (kb_tail + 1) % KB_BUFFER_SIZE;
    return c;
}
