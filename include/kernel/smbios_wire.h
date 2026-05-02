/* SPDX-License-Identifier: MIT */
/* SMBIOS entry-point wire-format constants. DMTF DSP0134.
 *
 * Single source of truth for entry-point lengths; consumed by
 * src/kernel/smbios.c (parser) and src/kernel/firmware_tables.c
 * (catalog validator). Splitting these between the two files would
 * leak future-spec drift on the next SMBIOS revision. */

#ifndef KERNEL_SMBIOS_WIRE_H
#define KERNEL_SMBIOS_WIRE_H

/* SMBIOS 3.x 64-bit entry point: fixed 24-byte record. */
#define SMBIOS3_EP_LEN          0x18u

/* SMBIOS 2.x 32-bit entry point: 30 bytes (rev 2.1) up to 31 bytes (rev 2.4+). */
#define SMBIOS2_EP_LEN_MIN      0x1Eu
#define SMBIOS2_EP_LEN_MAX      0x1Fu

#endif /* KERNEL_SMBIOS_WIRE_H */
