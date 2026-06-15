/* ============================================================================
 * scsi.h -- Shared SCSI sense keys (SPC-4 4.5.6)
 *
 * Single source of truth for the SCSI sense-key constants used by every
 * SCSI-over-transport driver (AHCI SATA, USB MSC BOT). Reference: SCSI Primary
 * Commands (SPC-4) 4.5.6 "Sense key and additional sense code definitions".
 * ============================================================================ */

#pragma once

#define SCSI_SK_NO_SENSE        0x00
#define SCSI_SK_RECOVERED       0x01   /* Recovered error (success with warning) */
#define SCSI_SK_NOT_READY       0x02
#define SCSI_SK_MEDIUM_ERROR    0x03
#define SCSI_SK_HARDWARE_ERROR  0x04
#define SCSI_SK_ILLEGAL_REQUEST 0x05
#define SCSI_SK_UNIT_ATTENTION  0x06
#define SCSI_SK_DATA_PROTECT    0x07
#define SCSI_SK_BLANK_CHECK     0x08
#define SCSI_SK_ABORTED_COMMAND 0x0B
