/* ============================================================================
 * storvsc.h — Hyper-V Synthetic SCSI Storage Driver (StorVSC)
 *
 * Clean-room implementation from the public Hyper-V Top-Level Functional
 * Specification (TLFS). NO code derived from Linux hv_storvsc.c (GPL).
 *
 * The StorVSC driver communicates with the host's Storage VSP over a VMBus
 * channel using VSTOR_PACKET messages containing SCSI CDBs.
 *
 * Reference: https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/tlfs
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* ---- Storage VSP Channel GUID ---- */

/* BA6163D9-04A1-4D29-B605-72E2FFB1DC7F */
#define HV_GUID_STORVSC_CHANNEL     { 0xBA6163D9, 0x04A1, 0x4D29, \
    { 0xB6, 0x05, 0x72, 0xE2, 0xFF, 0xB1, 0xDC, 0x7F } }

/* ---- StorVSC Protocol Versions ---- */

/* Protocol version format: major.minor as 16.16 */
#define STORVSC_VERSION_WIN10       0x00060002  /* Windows 10+ */
#define STORVSC_VERSION_WIN8_1      0x00060001  /* Windows 8.1 */
#define STORVSC_VERSION_WIN8        0x00050001  /* Windows 8 */

/* ---- VSTOR_PACKET operations ---- */

#define VSTOR_OPERATION_COMPLETE_IO             1
#define VSTOR_OPERATION_REMOVE_DEVICE           2
#define VSTOR_OPERATION_EXECUTE_SRB             3
#define VSTOR_OPERATION_RESET_LUN               4
#define VSTOR_OPERATION_RESET_ADAPTER           5
#define VSTOR_OPERATION_RESET_BUS               6
#define VSTOR_OPERATION_BEGIN_INITIALIZATION     7
#define VSTOR_OPERATION_END_INITIALIZATION       8
#define VSTOR_OPERATION_QUERY_PROTOCOL_VERSION   9
#define VSTOR_OPERATION_QUERY_PROPERTIES        10
#define VSTOR_OPERATION_ENUMERATE_BUS           11
#define VSTOR_OPERATION_FCHBA_DATA              12
#define VSTOR_OPERATION_CREATE_SUB_CHANNELS     13

/* ---- VSTOR_PACKET flags ---- */

#define VSTOR_FLAG_REQUEST_COMPLETION           0x01

/* ---- VSTOR_PACKET status ---- */

#define VSTOR_STATUS_SUCCESS                    0x00000000
#define VSTOR_STATUS_ERROR                      0x00000001

/* ---- SCSI CDB opcodes ---- */

#define SCSI_INQUIRY                            0x12
#define SCSI_READ_CAPACITY_16                   0x9E
#define SCSI_READ_16                            0x88
#define SCSI_WRITE_16                           0x8A
#define SCSI_TEST_UNIT_READY                    0x00

/* SCSI READ_CAPACITY_16 service action */
#define SCSI_SAI_READ_CAPACITY_16               0x10

/* SCSI INQUIRY peripheral qualifier (bits 7:5 of byte 0) */
#define SCSI_PQ_CONNECTED           0   /* device connected and present */
#define SCSI_PQ_DISCONNECTED        1   /* device supported but not connected */
#define SCSI_PQ_NOT_SUPPORTED       3   /* target does not support LUN */

/* ---- SCSI Request Block (SRB) ---- */

/* Simplified SRB structure for the VMBus VSTOR protocol.
 * Only the fields actually used in the protocol exchange are included. */
struct vstor_srb {
    uint16_t length;            /* sizeof(vstor_srb) */
    uint8_t  srb_status;        /* completion status */
    uint8_t  scsi_status;       /* SCSI completion status */

    uint8_t  target_id;         /* SCSI target (always 0 for storvsc) */
    uint8_t  path_id;           /* SCSI path (always 0) */
    uint8_t  lun;               /* LUN (always 0) */
    uint8_t  cdb_length;        /* length of CDB (6, 10, 12, or 16) */

    uint32_t data_transfer_length;  /* bytes to transfer */
    uint8_t  data_in;           /* 0 = write, 1 = read */
    uint8_t  reserved[3];

    uint8_t  cdb[16];           /* SCSI Command Descriptor Block */
    uint8_t  sense_data[20];    /* sense data on error */
} __attribute__((packed));

/* ---- VSTOR_PACKET ---- */

/* The main protocol message exchanged over the VMBus ring buffer.
 * Contains operation type, flags, status, and an embedded SRB for
 * SCSI command operations. */
struct vstor_packet {
    uint32_t operation;         /* VSTOR_OPERATION_* */
    uint32_t flags;             /* VSTOR_FLAG_* */
    uint32_t status;            /* VSTOR_STATUS_* */

    union {
        /* For QUERY_PROTOCOL_VERSION */
        struct {
            uint32_t major_minor;       /* version to negotiate */
            uint16_t revision;          /* revision */
            uint16_t reserved;
        } version;

        /* For QUERY_PROPERTIES */
        struct {
            uint16_t path_id;
            uint8_t  target_id;
            uint8_t  max_channel_count;
            uint8_t  max_targets;       /* max target IDs per path (0 = use default 1) */
            uint8_t  max_luns;          /* max LUNs per target (0 = use default 1) */
            uint16_t max_transfer_bytes_lo;
            uint32_t max_transfer_bytes_hi;
            uint32_t reserved2;
        } properties;

        /* For EXECUTE_SRB / COMPLETE_IO */
        struct vstor_srb srb;
    };
} __attribute__((packed));

/* ---- StorVSC device table (supports up to 64 devices per controller) ---- */

#define STORVSC_MAX_DEVICES     64

struct storvsc_disk_info {
    uint64_t sector_count;
    uint32_t sector_size;
    uint8_t  target_id;
    uint8_t  lun;
    int      active;            /* 1 = device present and registered */
};

/* ---- Public API ---- */

/* Initialize the StorVSC driver: find storage channel, open it,
 * negotiate protocol, enumerate all LUNs, and register each as a blkdev.
 * Returns 0 on success, -1 on failure (or if not on Hyper-V). */
int storvsc_init(void);
