# Kernel Core

The kernel core provides the foundational infrastructure: framebuffer console,
descriptor tables, interrupt handling, timers, and keyboard input.

## Framebuffer Console

The kernel uses a **linear framebuffer** provided by UEFI/GRUB via the Multiboot2
framebuffer tag. There is no VGA text mode — all text is drawn pixel-by-pixel.

### Key Files

| File | Purpose |
|------|---------|
| `src/kernel/drivers/framebuffer.c` | Pixel framebuffer driver |
| `src/kernel/printk.c` | Kernel printf (`%d/%u/%x/%p/%s/%c`) |
| `src/desktop/font.c` | PSF bitmap font renderer |

### Font

- **Format:** PSF (PC Screen Font) bitmap
- **Size:** 8×16 pixels per glyph
- **Glyphs:** 95 printable ASCII characters (0x20–0x7E)
- **Rendering:** Each character drawn as a 8×16 block of pixels

### Console Features

| Function | Description |
|----------|-------------|
| `fb_putchar(c)` | Draw a single character at cursor position |
| `fb_write(str, len)` | Write a string |
| `fb_clear()` | Clear the screen |
| `fb_set_color(fg, bg)` | Set foreground/background colors |
| `printk(fmt, ...)` | Kernel printf → framebuffer + serial COM1 |

The console supports automatic scrolling when text reaches the bottom of the screen.

## Global Descriptor Table (GDT)

### Key Files

| File | Purpose |
|------|---------|
| `src/kernel/gdt.c` | GDT setup and TSS |
| `src/boot/gdt_asm.asm` | `gdt_flush()` — reloads segment registers |

### Segments

| Index | Selector | Description |
|-------|----------|-------------|
| 0 | `0x00` | Null descriptor |
| 1 | `0x08` | Kernel code (64-bit, DPL 0) |
| 2 | `0x10` | Kernel data (DPL 0) |
| 3 | `0x18` | User code (64-bit, DPL 3) |
| 4 | `0x20` | User data (DPL 3) |
| 5 | `0x28` | TSS (Task State Segment) |

### Task State Segment (TSS)

The TSS provides the kernel stack pointer (`RSP0`) for ring 3 → ring 0 transitions.
Located at `0x10F040`. When a user-mode program triggers a syscall or exception,
the CPU loads `RSP0` from the TSS to switch to the kernel stack.

## Interrupt Descriptor Table (IDT)

### Key Files

| File | Purpose |
|------|---------|
| `src/kernel/idt.c` | IDT setup and ISR dispatcher |
| `src/boot/isr_stubs.asm` | Assembly stubs for all 256 interrupt vectors |

### Layout

| Vectors | Type | Description |
|---------|------|-------------|
| 0–31 | CPU exceptions | Divide-by-zero, page fault, GPF, etc. |
| 32–47 | Hardware IRQs | PIT, keyboard, serial, mouse, NIC, etc. |
| 48–255 | Software | Syscall (0x80), available for IPI |

### Exception Handler

Unhandled CPU exceptions trigger a **KERNEL PANIC** screen displaying:
- Exception name and number
- Error code (if applicable)
- Register dump (RAX–R15, RIP, RSP, RFLAGS, CR2)
- The system halts after displaying the panic

### Handler Registration

```c
/* Register a handler for any interrupt vector */
idt_register_handler(vector, handler_function);
```

## Programmable Interrupt Controller (PIC)

### Key Files

| File | Purpose |
|------|---------|
| `src/kernel/drivers/pic.c` | 8259 PIC initialization and management |

### Remapping

The PIC is remapped to avoid conflicts with CPU exceptions:

| PIC | Default | Remapped | IRQ Lines |
|-----|---------|----------|-----------|
| Master (PIC1) | 0–7 | **32–39** | Timer, Keyboard, Cascade, COM2, COM1, LPT2, Floppy, LPT1 |
| Slave (PIC2) | 8–15 | **40–47** | RTC, Free, Free, Free, Mouse, FPU, ATA1, ATA2 |

### API

| Function | Description |
|----------|-------------|
| `pic_init()` | Remap both PICs, mask all IRQs |
| `pic_send_eoi(irq)` | Acknowledge end-of-interrupt |
| `pic_unmask(irq)` | Enable a specific IRQ line |
| `pic_mask(irq)` | Disable a specific IRQ line |

## Programmable Interval Timer (PIT)

### Key Files

| File | Purpose |
|------|---------|
| `src/kernel/drivers/pit.c` | PIT timer (channel 0, IRQ 0) |

### Configuration

| Property | Value |
|----------|-------|
| Frequency | 100 Hz |
| Divisor | 11931 (`1193180 / 100`) |
| IRQ | 0 (vector 32) |
| Mode | Rate generator (mode 2) |

### API

| Function | Description |
|----------|-------------|
| `pit_init()` | Program channel 0 at 100 Hz, register IRQ 0 |
| `sleep_ms(ms)` | Busy-wait using HLT + tick count |
| `uptime()` | Seconds since boot |
| `pit_get_ticks()` | Raw tick count |

The PIT drives the preemptive scheduler — each tick triggers `schedule()`.

## PS/2 Keyboard Driver

### Key Files

| File | Purpose |
|------|---------|
| `src/kernel/drivers/keyboard.c` | PS/2 keyboard driver (IRQ 1) |

### How It Works

1. **IRQ 1** fires on keypress → handler reads scan code from port `0x60`
2. Scan code is translated via a **US QWERTY lookup table**
3. Modifier keys (Shift, Caps Lock, Ctrl, Alt) are tracked as state flags
4. The resulting ASCII character is pushed into a **256-byte circular buffer**
5. `keyboard_getchar()` blocks until a character is available
6. `keyboard_trygetchar()` returns immediately (0 if empty)

### Supported Keys

- All printable ASCII (letters, numbers, symbols)
- Shift (uppercase + shifted symbols)
- Caps Lock (toggle)
- Backspace, Enter, Tab
- Arrow keys (as escape sequences)
- Ctrl, Alt (modifier state tracked)
