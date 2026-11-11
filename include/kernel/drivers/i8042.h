/* ============================================================================
 * i8042.h -- the legacy PS/2 controller's ports, status bits and lines
 *
 * The single definition of the 8042 register interface shared by the PS/2
 * keyboard driver, the PS/2 mouse driver and the boot console confirmation
 * prompt (boot_confirm.c). The prompt makes a security decision from these
 * bits (the AUX bit keeps a mouse byte from being read as a 'y'), so the
 * three must never hold separate copies that could drift.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

#define I8042_DATA_PORT    0x60  /* read: output buffer; write: device data */
#define I8042_STATUS_PORT  0x64  /* read: status register */
#define I8042_CMD_PORT     0x64  /* write: controller command */

#define I8042_STATUS_OBF   0x01  /* output buffer full: a byte is waiting */
#define I8042_STATUS_IBF   0x02  /* input buffer full: do not write yet */
#define I8042_STATUS_AUX   0x20  /* the waiting byte came from the mouse */

/* A status read of 0xFF is the floating bus of an absent controller. */
#define I8042_STATUS_ABSENT 0xFF

#define I8042_BREAK_CODE   0x80  /* scancode set 1: set on key release */
#define I8042_EXTENDED_PREFIX 0xE0 /* scancode set 1: an extended key follows */

/* The ISA lines are IRQ_KEYBOARD and IRQ_MOUSE in kernel/drivers/pic.h. */

/* Does a status-register read show a controller? Callers check
 * acpi_hw_reduced() first: a hardware-reduced platform declares no legacy
 * fixed hardware, so no 8042 port is read there at all. */
static inline int i8042_status_says_present(uint8_t status)
{
    return status != I8042_STATUS_ABSENT;
}
