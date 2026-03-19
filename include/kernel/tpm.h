/* ============================================================================
 * tpm.h — TPM Measured Boot interface
 *
 * Parses the TCG event log passed from the bootloader and exposes TPM
 * availability, version, and boot event summary to the kernel.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* Initialize TPM subsystem — parse event log from boot_info. */
void tpm_init(void);

/* Returns 1 if a TPM was detected during boot. */
int tpm_available(void);

/* Returns TPM version: 0 = none, 1 = 1.2, 2 = 2.0. */
int tpm_version(void);

/* Returns the number of measured boot events in the event log. */
uint32_t tpm_event_count(void);
